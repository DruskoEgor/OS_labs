#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#define IO_BUFFER_SIZE 4096
#define STATUS_SYSTEM_ERROR 1
#define STATUS_DIVISION_BY_ZERO 2
#define STATUS_INVALID_INPUT 3
#define STATUS_ARITHMETIC_OVERFLOW 4

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, buffer + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void fail(int status, const char *message)
{
    (void)write_all(STDERR_FILENO, message, string_length(message));
    _exit(status);
}

static int is_separator(char character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\v' || character == '\f';
}

static int apply_token(unsigned long long magnitude, int sign, int digits,
                       int *has_result, int *result)
{
    long long signed_value;
    int number;

    if (digits == 0) {
        return STATUS_INVALID_INPUT;
    }

    signed_value = (long long)magnitude;
    if (sign < 0) {
        signed_value = -signed_value;
    }
    number = (int)signed_value;

    if (!*has_result) {
        *result = number;
        *has_result = 1;
        return 0;
    }

    if (number == 0) {
        return STATUS_DIVISION_BY_ZERO;
    }
    if (*result == INT_MIN && number == -1) {
        return STATUS_ARITHMETIC_OVERFLOW;
    }

    *result /= number;
    return 0;
}

static int print_integer(int value)
{
    char output[32];
    char reversed_digits[sizeof(output)];
    size_t output_length = 0;
    size_t digit_count = 0;
    unsigned long long magnitude;

    if (value < 0) {
        output[output_length++] = '-';
        magnitude = (unsigned long long)(-(long long)value);
    } else {
        magnitude = (unsigned long long)value;
    }

    do {
        reversed_digits[digit_count++] = (char)('0' + (magnitude % 10ULL));
        magnitude /= 10ULL;
    } while (magnitude != 0);

    while (digit_count > 0) {
        output[output_length++] = reversed_digits[--digit_count];
    }
    output[output_length++] = '\n';
    return write_all(STDOUT_FILENO, output, output_length);
}

static int finish_token(unsigned long long magnitude, int sign, int digits,
                        int *has_result, int *result)
{
    return apply_token(magnitude, sign, digits, has_result, result);
}

static void report_status(int status)
{
    if (status == STATUS_DIVISION_BY_ZERO) {
        fail(status, "child: деление на ноль; завершаю работу\n");
    }
    if (status == STATUS_INVALID_INPUT) {
        fail(status, "child: некорректная строка; ожидались целые числа\n");
    }
    if (status == STATUS_ARITHMETIC_OVERFLOW) {
        fail(status, "child: переполнение при целочисленном делении\n");
    }
    fail(STATUS_SYSTEM_ERROR, "child: ошибка ввода-вывода\n");
}

int main(void)
{
    char buffer[IO_BUFFER_SIZE];
    int token_active = 0;
    int token_sign = 1;
    int token_sign_seen = 0;
    int token_digits = 0;
    unsigned long long token_magnitude = 0;
    int has_result = 0;
    int line_result = 0;

    for (;;) {
        ssize_t amount = read(STDIN_FILENO, buffer, sizeof(buffer));
        size_t index;

        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail(STATUS_SYSTEM_ERROR, "child: ошибка чтения входных данных\n");
        }

        if (amount == 0) {
            if (token_active) {
                int status = finish_token(token_magnitude, token_sign, token_digits,
                                          &has_result, &line_result);
                if (status != 0) {
                    report_status(status);
                }
            }
            if (has_result && print_integer(line_result) < 0) {
                fail(STATUS_SYSTEM_ERROR, "child: ошибка записи результата\n");
            }
            return 0;
        }

        for (index = 0; index < (size_t)amount; ++index) {
            char character = buffer[index];

            if (character == '\n' || is_separator(character)) {
                if (token_active) {
                    int status = finish_token(token_magnitude, token_sign, token_digits,
                                              &has_result, &line_result);
                    if (status != 0) {
                        report_status(status);
                    }
                    token_active = 0;
                    token_sign = 1;
                    token_sign_seen = 0;
                    token_digits = 0;
                    token_magnitude = 0;
                }

                if (character == '\n') {
                    if (has_result && print_integer(line_result) < 0) {
                        fail(STATUS_SYSTEM_ERROR, "child: ошибка записи результата\n");
                    }
                    has_result = 0;
                    line_result = 0;
                }
                continue;
            }

            if (!token_active) {
                token_active = 1;
                token_sign = 1;
                token_sign_seen = 0;
                token_digits = 0;
                token_magnitude = 0;
            }

            if (character == '+' || character == '-') {
                if (token_digits != 0 || token_sign_seen) {
                    report_status(STATUS_INVALID_INPUT);
                }
                token_sign = (character == '-') ? -1 : 1;
                token_sign_seen = 1;
                continue;
            }

            if (character < '0' || character > '9') {
                report_status(STATUS_INVALID_INPUT);
            }

            {
                unsigned long long digit = (unsigned long long)(character - '0');
                unsigned long long limit = (token_sign < 0)
                    ? (unsigned long long)(-(long long)INT_MIN)
                    : (unsigned long long)INT_MAX;

                if (token_magnitude > (limit - digit) / 10ULL) {
                    report_status(STATUS_INVALID_INPUT);
                }
                token_magnitude = token_magnitude * 10ULL + digit;
                ++token_digits;
            }
        }
    }
}
