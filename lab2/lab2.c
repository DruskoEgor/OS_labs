#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>

#define MAX_THREADS 4096U
#define MAX_EXPERIMENTS 32U
#define BENCH_REPEATS 5U
#define DEFAULT_SEED 1ULL
#define WORKER_STACK_SIZE (256U * 1024U)

typedef struct {
    const int64_t *input;
    int64_t *result;
    int64_t *partial;
    size_t k;
    size_t n;
    size_t begin;
    size_t end;
    size_t worker_index;
    int by_arrays;
} worker_args_t;

static void write_all(int fd, const char *text, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        done += (size_t)n;
    }
}

static int str_equal(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0;
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

static void say(const char *s) {
    write_all(STDOUT_FILENO, s, str_len(s));
}

static void say_err(const char *s) {
    write_all(STDERR_FILENO, s, str_len(s));
}

static void append_char(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos + 1 < cap) buf[(*pos)++] = c;
}

static void append_text(char *buf, size_t cap, size_t *pos, const char *s) {
    size_t i = 0;
    while (s[i] != '\0') append_char(buf, cap, pos, s[i++]);
}

static void append_u64(char *buf, size_t cap, size_t *pos, uint64_t value) {
    char tmp[32];
    size_t count = 0;
    do {
        tmp[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0 && count < sizeof(tmp));
    while (count > 0) append_char(buf, cap, pos, tmp[--count]);
}

static void append_i64(char *buf, size_t cap, size_t *pos, int64_t value) {
    if (value < 0) {
        append_char(buf, cap, pos, '-');
        /* Не отрицаем INT64_MIN напрямую, чтобы избежать переполнения. */
        append_u64(buf, cap, pos, (uint64_t)(-(value + 1)) + 1ULL);
    } else {
        append_u64(buf, cap, pos, (uint64_t)value);
    }
}

/* Значение задано в тысячных долях единицы; здесь это микросекунды для вывода миллисекунд. */
static void append_fixed3(char *buf, size_t cap, size_t *pos, uint64_t value) {
    append_u64(buf, cap, pos, value / 1000ULL);
    append_char(buf, cap, pos, '.');
    uint64_t frac = value % 1000ULL;
    append_char(buf, cap, pos, (char)('0' + frac / 100ULL));
    append_char(buf, cap, pos, (char)('0' + (frac / 10ULL) % 10ULL));
    append_char(buf, cap, pos, (char)('0' + frac % 10ULL));
}

static void append_fixed6(char *buf, size_t cap, size_t *pos, uint64_t value) {
    append_u64(buf, cap, pos, value / 1000000ULL);
    append_char(buf, cap, pos, '.');
    uint64_t frac = value % 1000000ULL;
    append_char(buf, cap, pos, (char)('0' + frac / 100000ULL));
    append_char(buf, cap, pos, (char)('0' + (frac / 10000ULL) % 10ULL));
    append_char(buf, cap, pos, (char)('0' + (frac / 1000ULL) % 10ULL));
    append_char(buf, cap, pos, (char)('0' + (frac / 100ULL) % 10ULL));
    append_char(buf, cap, pos, (char)('0' + (frac / 10ULL) % 10ULL));
    append_char(buf, cap, pos, (char)('0' + frac % 10ULL));
}

static void append_fixed9(char *buf, size_t cap, size_t *pos, uint64_t value) {
    append_u64(buf, cap, pos, value / 1000000000ULL);
    append_char(buf, cap, pos, '.');
    uint64_t frac = value % 1000000000ULL;
    uint64_t divisor = 100000000ULL;
    for (size_t i = 0; i < 9; ++i) {
        append_char(buf, cap, pos, (char)('0' + (frac / divisor) % 10ULL));
        divisor /= 10ULL;
    }
}

static void emit_line(int fd, const char *line, size_t len) {
    write_all(fd, line, len);
}

static int parse_u64(const char *s, uint64_t *out) {
    if (s == NULL || s[0] == '\0') return 0;
    uint64_t value = 0;
    size_t i = 0;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        uint64_t digit = (uint64_t)(s[i] - '0');
        if (value > (UINT64_MAX - digit) / 10ULL) return 0;
        value = value * 10ULL + digit;
    }
    *out = value;
    return 1;
}

static void *map_memory(size_t bytes) {
    if (bytes == 0) bytes = 1;
    return mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void unmap_memory(void *ptr, size_t bytes) {
    if (ptr != MAP_FAILED && ptr != NULL) {
        if (bytes == 0) bytes = 1;
        (void)munmap(ptr, bytes);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int write_number_line(int fd, int64_t value) {
    char buf[64];
    size_t pos = 0;
    append_i64(buf, sizeof(buf), &pos, value);
    append_char(buf, sizeof(buf), &pos, '\n');
    size_t done = 0;
    while (done < pos) {
        ssize_t n = write(fd, buf + done, pos - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static void sequential_sum(const int64_t *input, int64_t *result,
                           size_t k, size_t n) {
    for (size_t j = 0; j < n; ++j) {
        int64_t total = 0;
        for (size_t a = 0; a < k; ++a) total += input[a * n + j];
        result[j] = total;
    }
}

static void *worker_main(void *opaque) {
    worker_args_t *a = (worker_args_t *)opaque;
    if (!a->by_arrays) {
        for (size_t j = a->begin; j < a->end; ++j) {
            int64_t total = 0;
            for (size_t row = 0; row < a->k; ++row)
                total += a->input[row * a->n + j];
            a->result[j] = total;
        }
    } else {
        int64_t *private_result = a->partial + a->worker_index * a->n;
        for (size_t j = 0; j < a->n; ++j) {
            int64_t total = 0;
            for (size_t row = a->begin; row < a->end; ++row)
                total += a->input[row * a->n + j];
            private_result[j] = total;
        }
    }
    return NULL;
}

/* Эквивалент floor(total * part / parts) без переполнения произведения total * part. */
static size_t partition_at(size_t total, size_t part, size_t parts) {
    size_t quotient = total / parts;
    size_t remainder = total % parts;
    return quotient * part + (remainder * part) / parts;
}

static int parallel_sum(const int64_t *input, int64_t *result,
                        size_t k, size_t n, size_t requested,
                        uint64_t *elapsed_ns, size_t *actual,
                        const char **strategy, int *error_code) {
    size_t workers = requested;
    int by_arrays = 0;

    /* Выбираем измерение, в котором больше независимой работы. Каждый элемент
       результата вычисляется независимо; если N мало, а K больше, распараллеливаем
       обработку входных массивов и затем складываем частичные результаты потоков. */
    if (workers <= n) {
        by_arrays = 0;
    } else if (k > n) {
        by_arrays = 1;
        if (workers > k) workers = k;
    } else {
        by_arrays = 0;
        workers = n;
    }
    if (workers == 0) workers = 1;
    if (workers > MAX_THREADS) workers = MAX_THREADS;

    uint64_t begin_ns = now_ns();
    size_t ids_bytes = workers * sizeof(pthread_t);
    size_t args_bytes = workers * sizeof(worker_args_t);
    pthread_t *threads = (pthread_t *)map_memory(ids_bytes);
    worker_args_t *args = (worker_args_t *)map_memory(args_bytes);
    int64_t *partial = NULL;
    size_t partial_bytes = 0;

    if (threads == MAP_FAILED || args == MAP_FAILED) {
        unmap_memory(threads, ids_bytes);
        unmap_memory(args, args_bytes);
        *error_code = ENOMEM;
        return 0;
    }
    if (by_arrays) {
        if (n != 0 && workers > SIZE_MAX / n / sizeof(int64_t)) {
            unmap_memory(threads, ids_bytes);
            unmap_memory(args, args_bytes);
            *error_code = ENOMEM;
            return 0;
        }
        partial_bytes = workers * n * sizeof(int64_t);
        partial = (int64_t *)map_memory(partial_bytes);
        if (partial == MAP_FAILED) {
            unmap_memory(threads, ids_bytes);
            unmap_memory(args, args_bytes);
            *error_code = ENOMEM;
            return 0;
        }
    }

    pthread_attr_t attr;
    int attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok && pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE) != 0) {
        /* Если уменьшенный размер стека не поддерживается, используем атрибуты по умолчанию. */
        (void)pthread_attr_destroy(&attr);
        attr_ok = 0;
    }

    size_t created = 0;
    int create_error = 0;
    for (size_t i = 0; i < workers; ++i) {
        args[i].input = input;
        args[i].result = result;
        args[i].partial = partial;
        args[i].k = k;
        args[i].n = n;
        args[i].worker_index = i;
        args[i].by_arrays = by_arrays;
        if (by_arrays) {
            args[i].begin = partition_at(k, i, workers);
            args[i].end = partition_at(k, i + 1, workers);
        } else {
            args[i].begin = partition_at(n, i, workers);
            args[i].end = partition_at(n, i + 1, workers);
        }
        int rc = pthread_create(&threads[i], attr_ok ? &attr : NULL,
                                worker_main, &args[i]);
        if (rc != 0) {
            create_error = rc;
            break;
        }
        ++created;
    }
    for (size_t i = 0; i < created; ++i) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0 && create_error == 0) create_error = rc;
    }
    if (attr_ok) (void)pthread_attr_destroy(&attr);

    if (create_error == 0 && by_arrays) {
        for (size_t j = 0; j < n; ++j) {
            int64_t total = 0;
            for (size_t i = 0; i < workers; ++i)
                total += partial[i * n + j];
            result[j] = total;
        }
    }
    uint64_t finish_ns = now_ns();

    unmap_memory(threads, ids_bytes);
    unmap_memory(args, args_bytes);
    unmap_memory(partial, partial_bytes);

    if (create_error != 0) {
        *error_code = create_error;
        return 0;
    }
    *elapsed_ns = finish_ns >= begin_ns ? finish_ns - begin_ns : 0;
    *actual = workers;
    *strategy = by_arrays ? "по массивам" : "по элементам результата";
    *error_code = 0;
    return 1;
}

static uint64_t median_u64(uint64_t *values, size_t count) {
    /* Для небольшого фиксированного числа замеров достаточно сортировки вставками. */
    for (size_t i = 1; i < count; ++i) {
        uint64_t value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
    return count == 0 ? 0 : values[count / 2];
}

static int results_equal(const int64_t *a, const int64_t *b, size_t n,
                         size_t *bad_index) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            *bad_index = i;
            return 0;
        }
    }
    return 1;
}

static int save_result(const char *path, const int64_t *result, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (!write_number_line(fd, result[i])) {
            (void)close(fd);
            return 0;
        }
    }
    if (close(fd) != 0) return 0;
    return 1;
}

static int append_thread(size_t *list, size_t *count, size_t value) {
    if (value == 0 || value > MAX_THREADS || *count >= MAX_EXPERIMENTS) return 0;
    for (size_t i = 0; i < *count; ++i) if (list[i] == value) return 1;
    list[(*count)++] = value;
    return 1;
}

static int parse_thread_list(const char *s, size_t *list, size_t *count) {
    *count = 0;
    size_t start = 0;
    size_t len = str_len(s);
    for (size_t i = 0; i <= len; ++i) {
        if (s[i] == ',' || s[i] == '\0') {
            if (i == start) return 0;
            char tmp[32];
            size_t l = i - start;
            if (l >= sizeof(tmp)) return 0;
            for (size_t j = 0; j < l; ++j) tmp[j] = s[start + j];
            tmp[l] = '\0';
            uint64_t value = 0;
            if (!parse_u64(tmp, &value) || value == 0 || value > MAX_THREADS) return 0;
            if (!append_thread(list, count, (size_t)value)) return 0;
            start = i + 1;
        }
    }
    return *count > 0;
}

static size_t max_size(size_t a, size_t b) { return a > b ? a : b; }

static int auto_thread_list(size_t *list, size_t *count, size_t cores) {
    *count = 0;
    if (cores == 0) cores = 1;
    if (cores > MAX_THREADS - 3U) {
        say_err("Ошибка: слишком много логических CPU для авто-плана.\n");
        say_err("Выберите явный список потоков через --bench.\n");
        return 0;
    }

    /* Подбираем до четырёх разных значений ниже числа CPU.
       При малом числе процессоров их может быть меньше четырёх. */
    if (cores > 1) (void)append_thread(list, count, 1);
    if (cores > 2) (void)append_thread(list, count, max_size(2, cores / 4));
    if (cores > 3) (void)append_thread(list, count, max_size(3, cores / 2));
    if (cores > 1) (void)append_thread(list, count, cores - 1);
    for (size_t v = 1; *count < 4 && v < cores; ++v)
        (void)append_thread(list, count, v);

    /* Даже при малом числе CPU проводим доступные сравнения. */
    (void)append_thread(list, count, cores);

    size_t high[3];
    if (cores <= 512U) {
        high[0] = max_size(16U, cores * 2U);
        high[1] = max_size(128U, cores * 4U);
        high[2] = max_size(1024U, cores * 8U);
    } else {
        /* Выбираем три значения выше cores, вплоть до MAX_THREADS. */
        size_t span = MAX_THREADS - cores;
        high[0] = cores + span / 3U;
        high[1] = cores + (span * 2U) / 3U;
        high[2] = MAX_THREADS;
    }
    for (size_t i = 0; i < 3; ++i)
        (void)append_thread(list, count, high[i]);

    if (cores < 5U) {
        say_err("Предупреждение: доступно меньше пяти логических CPU.\n");
        say_err("Нельзя выбрать четыре значения ниже числа CPU.\n");
        say_err("Запускаю все доступные значения и три больших.\n");
    }
    return *count >= 4U;
}

static void make_timing_row(int fd, size_t requested, size_t actual,
                            uint64_t elapsed_ns, uint64_t seq_ns,
                            const char *strategy) {
    char b[512]; size_t p = 0;
    uint64_t ms_milli = elapsed_ns / 1000ULL; /* Наносекунды -> тысячные доли миллисекунды. */
    uint64_t speed_x1m = elapsed_ns == 0 ? 0 : (seq_ns * 1000000ULL + elapsed_ns / 2ULL) / elapsed_ns;
    uint64_t efficiency_x1e9 = actual == 0 ? 0 : (speed_x1m * 1000ULL + actual / 2ULL) / actual;
    append_text(b, sizeof(b), &p, "Запрошено: "); append_u64(b, sizeof(b), &p, requested);
    append_text(b, sizeof(b), &p, ", фактически: "); append_u64(b, sizeof(b), &p, actual);
    append_text(b, sizeof(b), &p, " | "); append_fixed3(b, sizeof(b), &p, ms_milli);
    append_text(b, sizeof(b), &p, " | "); append_fixed6(b, sizeof(b), &p, speed_x1m);
    append_text(b, sizeof(b), &p, " | "); append_fixed9(b, sizeof(b), &p, efficiency_x1e9);
    append_text(b, sizeof(b), &p, " | "); append_text(b, sizeof(b), &p, strategy);
    append_char(b, sizeof(b), &p, '\n');
    emit_line(fd, b, p);
}

static int write_csv_header(int fd) {
    const char h[] = "arrays,length,requested_threads,actual_threads,"
                     "parallel_ms,speedup,efficiency,sequential_ms,strategy\n";
    size_t done = 0;
    while (done < sizeof(h) - 1) {
        ssize_t n = write(fd, h + done, sizeof(h) - 1 - done);
        if (n < 0) { if (errno == EINTR) continue; return 0; }
        if (n == 0) return 0;
        done += (size_t)n;
    }
    return 1;
}

static int write_csv_row(int fd, size_t k, size_t n,
                         size_t requested, size_t actual,
                         uint64_t elapsed_ns, uint64_t seq_ns,
                         const char *strategy) {
    char b[512]; size_t p = 0;
    append_u64(b, sizeof(b), &p, k); append_char(b, sizeof(b), &p, ',');
    append_u64(b, sizeof(b), &p, n); append_char(b, sizeof(b), &p, ',');
    append_u64(b, sizeof(b), &p, requested); append_char(b, sizeof(b), &p, ',');
    append_u64(b, sizeof(b), &p, actual); append_char(b, sizeof(b), &p, ',');
    append_fixed3(b, sizeof(b), &p, elapsed_ns / 1000ULL); append_char(b, sizeof(b), &p, ',');
    uint64_t speed = elapsed_ns ? (seq_ns * 1000000ULL + elapsed_ns / 2ULL) / elapsed_ns : 0;
    append_fixed6(b, sizeof(b), &p, speed); append_char(b, sizeof(b), &p, ',');
    uint64_t efficiency_x1e9 = actual ? (speed * 1000ULL + actual / 2ULL) / actual : 0;
    append_fixed9(b, sizeof(b), &p, efficiency_x1e9); append_char(b, sizeof(b), &p, ',');
    append_fixed3(b, sizeof(b), &p, seq_ns / 1000ULL); append_char(b, sizeof(b), &p, ',');
    append_text(b, sizeof(b), &p, strategy); append_char(b, sizeof(b), &p, '\n');
    size_t done = 0;
    while (done < p) {
        ssize_t written = write(fd, b + done, p - done);
        if (written < 0) { if (errno == EINTR) continue; return 0; }
        if (written == 0) return 0;
        done += (size_t)written;
    }
    return 1;
}

static void usage(void) {
    say_err("Использование:\n");
    say_err("  ./lab2 --arrays K --length N --threads T [--seed S] [--out result.txt]\n");
    say_err("  ./lab2 --arrays K --length N --bench auto|T1,T2,...\n");
    say_err("      [--seed S] [--out result.txt] [--csv timings.csv]\n");
    say_err("Пример: ./lab2 --arrays 8 --length 500000\n");
    say_err("      --bench 1,2,4,8,16,128,1024 --csv timings.csv\n");
}

int main(int argc, char **argv) {
    uint64_t k64 = 0, n64 = 0, threads64 = 0, seed = DEFAULT_SEED;
    const char *bench_arg = NULL;
    const char *out_path = "result.txt";
    const char *csv_path = NULL;
    int has_threads = 0;

    for (int i = 1; i < argc; ++i) {
        if ((argv[i][0] == '-') && argv[i][1] == '-') {
            if (i + 1 >= argc) { usage(); return 2; }
            const char *key = argv[i];
            const char *val = argv[++i];
            if (str_equal(key, "--arrays")) {
                if (!parse_u64(val, &k64)) {
                    say_err("Ошибка: некорректное число массивов.\n");
                    return 2;
                }
            } else if (str_equal(key, "--length")) {
                if (!parse_u64(val, &n64)) {
                    say_err("Ошибка: некорректная длина массива.\n");
                    return 2;
                }
            } else if (str_equal(key, "--threads")) {
                if (!parse_u64(val, &threads64)) {
                    say_err("Ошибка: некорректное число потоков.\n");
                    return 2;
                }
                has_threads = 1;
            } else if (str_equal(key, "--bench")) {
                bench_arg = val;
            } else if (str_equal(key, "--seed")) {
                if (!parse_u64(val, &seed)) {
                    say_err("Ошибка: некорректное значение seed.\n");
                    return 2;
                }
            } else if (str_equal(key, "--out")) {
                out_path = val;
            } else if (str_equal(key, "--csv")) {
                csv_path = val;
            } else {
                say_err("Ошибка: неизвестный параметр.\n"); usage(); return 2;
            }
        } else {
            usage(); return 2;
        }
    }

    if (k64 == 0 || n64 == 0 || k64 > SIZE_MAX || n64 > SIZE_MAX) {
        say_err("Ошибка: K и N должны быть положительными.\n");
        usage();
        return 2;
    }
    if ((has_threads && bench_arg != NULL) || (!has_threads && bench_arg == NULL)) {
        say_err("Ошибка: укажите --threads T или --bench со списком/auto.\n");
        usage();
        return 2;
    }
    if (has_threads && (threads64 == 0 || threads64 > MAX_THREADS)) {
        say_err("Ошибка: число потоков должно быть от 1 до 4096.\n");
        return 2;
    }

    size_t k = (size_t)k64, n = (size_t)n64;
    /* Каждый элемент лежит в [-100, 100], поэтому сумма помещается в int64_t. */
    if (k64 > (uint64_t)(INT64_MAX / 100)) {
        say_err("Ошибка: слишком много массивов для int64_t.\n");
        return 2;
    }
    if (k > SIZE_MAX / n || k * n > SIZE_MAX / sizeof(int64_t) ||
        n > SIZE_MAX / sizeof(int64_t)) {
        say_err("Ошибка: размеры массивов слишком велики.\n");
        return 2;
    }
    size_t input_count = k * n;
    size_t input_bytes = input_count * sizeof(int64_t);
    size_t result_bytes = n * sizeof(int64_t);
    int64_t *input = (int64_t *)map_memory(input_bytes);
    int64_t *seq_result = (int64_t *)map_memory(result_bytes);
    int64_t *par_result = (int64_t *)map_memory(result_bytes);
    if (input == MAP_FAILED || seq_result == MAP_FAILED || par_result == MAP_FAILED) {
        say_err("Ошибка: мало памяти. Уменьшите K или N.\n");
        unmap_memory(input, input_bytes);
        unmap_memory(seq_result, result_bytes);
        unmap_memory(par_result, result_bytes);
        return 1;
    }

    uint64_t rng = seed;
    for (size_t i = 0; i < input_count; ++i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        input[i] = (int64_t)((rng >> 33) % 201ULL) - 100;
    }

    size_t cores = 1;
    long core_val = sysconf(_SC_NPROCESSORS_ONLN);
    if (core_val > 0) cores = (size_t)core_val;

    size_t thread_list[MAX_EXPERIMENTS]; size_t thread_count = 0;
    if (has_threads) {
        thread_list[thread_count++] = (size_t)threads64;
    } else if (str_equal(bench_arg, "auto")) {
        if (!auto_thread_list(thread_list, &thread_count, cores)) {
            unmap_memory(input, input_bytes);
            unmap_memory(seq_result, result_bytes);
            unmap_memory(par_result, result_bytes);
            return 2;
        }
    } else if (!parse_thread_list(bench_arg, thread_list, &thread_count)) {
        say_err("Ошибка: список потоков должен иметь вид 1,2,4,8.\n");
        say_err("Допускается не более 4096 потоков.\n");
        unmap_memory(input, input_bytes);
        unmap_memory(seq_result, result_bytes);
        unmap_memory(par_result, result_bytes);
        return 2;
    }

    char line[256]; size_t lp = 0;
    append_text(line, sizeof(line), &lp,
                "Вариант 8: сложение K массивов одинаковой длины.\n");
    append_text(line, sizeof(line), &lp, "Логических процессоров в системе: ");
    append_u64(line, sizeof(line), &lp, cores); append_char(line, sizeof(line), &lp, '\n');
    append_text(line, sizeof(line), &lp, "K = "); append_u64(line, sizeof(line), &lp, k);
    append_text(line, sizeof(line), &lp, ", N = "); append_u64(line, sizeof(line), &lp, n);
    append_text(line, sizeof(line), &lp, ", элементов входа = ");
    append_u64(line, sizeof(line), &lp, input_count);
    append_char(line, sizeof(line), &lp, '\n');
    emit_line(STDOUT_FILENO, line, lp);

    uint64_t seq_samples[BENCH_REPEATS];
    for (size_t r = 0; r < BENCH_REPEATS; ++r) {
        uint64_t seq_begin = now_ns();
        sequential_sum(input, seq_result, k, n);
        uint64_t seq_end = now_ns();
        seq_samples[r] = seq_end >= seq_begin ? seq_end - seq_begin : 0;
    }
    uint64_t seq_ns = median_u64(seq_samples, BENCH_REPEATS);
    say("Время последовательной версии, медиана 5 запусков (мс): ");
    char tbuf[64]; size_t tp = 0;
    append_fixed3(tbuf, sizeof(tbuf), &tp, seq_ns / 1000ULL);
    append_char(tbuf, sizeof(tbuf), &tp, '\n');
    emit_line(STDOUT_FILENO, tbuf, tp);
    say("\nПараллельные эксперименты (медиана 5 запусков):\n");
    say("Потоки (запрошено/факт.) | мс | Ускорение | ");
    say("Эффективность | Стратегия\n");

    int csv_fd = -1;
    if (csv_path != NULL) {
        csv_fd = open(csv_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (csv_fd < 0) {
            say_err("Ошибка: не удалось создать CSV-файл.\n");
            unmap_memory(input, input_bytes);
            unmap_memory(seq_result, result_bytes);
            unmap_memory(par_result, result_bytes);
            return 1;
        }
        if (!write_csv_header(csv_fd)) {
            say_err("Ошибка: не удалось записать заголовок CSV.\n");
            (void)close(csv_fd);
            unmap_memory(input, input_bytes);
            unmap_memory(seq_result, result_bytes);
            unmap_memory(par_result, result_bytes);
            return 1;
        }
    }

    size_t successes = 0;
    for (size_t i = 0; i < thread_count; ++i) {
        uint64_t samples[BENCH_REPEATS];
        size_t actual = 0;
        const char *strategy = "неизвестно";
        int errcode = 0;
        int failed = 0;

        for (size_t r = 0; r < BENCH_REPEATS; ++r) {
            size_t this_actual = 0;
            const char *this_strategy = "неизвестно";
            if (!parallel_sum(input, par_result, k, n, thread_list[i],
                              &samples[r], &this_actual, &this_strategy,
                              &errcode)) {
                failed = 1;
                break;
            }
            if (r == 0) {
                actual = this_actual;
                strategy = this_strategy;
            }
            size_t bad = 0;
            if (!results_equal(seq_result, par_result, n, &bad)) {
                say_err("Ошибка: результаты двух версий различаются.\n");
                if (csv_fd >= 0) (void)close(csv_fd);
                unmap_memory(input, input_bytes);
                unmap_memory(seq_result, result_bytes);
                unmap_memory(par_result, result_bytes);
                return 1;
            }
        }

        if (failed) {
            char eb[192]; size_t ep = 0;
            append_text(eb, sizeof(eb), &ep, "Ошибка: эксперимент с ");
            append_u64(eb, sizeof(eb), &ep, thread_list[i]);
            append_text(eb, sizeof(eb), &ep, " потоков не запущен, код: ");
            append_u64(eb, sizeof(eb), &ep, (uint64_t)errcode);
            append_text(eb, sizeof(eb), &ep, "). Попробуйте меньше потоков.\n");
            emit_line(STDERR_FILENO, eb, ep);
            /* При частично созданном наборе потоков часть par_result могла быть перезаписана. */
            for (size_t j = 0; j < n; ++j) par_result[j] = seq_result[j];
            continue;
        }

        uint64_t elapsed = median_u64(samples, BENCH_REPEATS);
        make_timing_row(STDOUT_FILENO, thread_list[i], actual, elapsed, seq_ns, strategy);
        if (csv_fd >= 0 &&
            !write_csv_row(csv_fd, k, n, thread_list[i], actual,
                           elapsed, seq_ns, strategy)) {
            say_err("Ошибка: не удалось записать строку CSV.\n");
            (void)close(csv_fd);
            unmap_memory(input, input_bytes);
            unmap_memory(seq_result, result_bytes);
            unmap_memory(par_result, result_bytes);
            return 1;
        }
        ++successes;
    }
    if (csv_fd >= 0 && close(csv_fd) != 0)
        say_err("Предупреждение: ошибка при закрытии CSV-файла.\n");

    if (successes == 0) {
        say_err("Ошибка: не выполнено ни одного эксперимента.\n");
        unmap_memory(input, input_bytes);
        unmap_memory(seq_result, result_bytes);
        unmap_memory(par_result, result_bytes);
        return 1;
    }
    if (!save_result(out_path, par_result, n)) {
        say_err("Ошибка: не удалось сохранить результат.\n");
        unmap_memory(input, input_bytes);
        unmap_memory(seq_result, result_bytes);
        unmap_memory(par_result, result_bytes);
        return 1;
    }
    say("\nПроверка: результаты двух версий совпадают.\n");
    say("Результат записан в файл: ");
    say(out_path);
    say("\n");
    if (csv_path != NULL) {
        say("Таблица измерений CSV сохранена в файл: ");
        say(csv_path);
        say("\n");
    }

    unmap_memory(input, input_bytes);
    unmap_memory(seq_result, result_bytes);
    unmap_memory(par_result, result_bytes);
    return 0;
}
