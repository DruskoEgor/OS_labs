#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define INPUT_PATH_CAPACITY 4096
#define IO_BUFFER_SIZE 4096

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

static void report_error(const char *message)
{
    (void)write_all(STDERR_FILENO, message, string_length(message));
}

static int read_input_path(char *path, size_t capacity)
{
    size_t length = 0;
    const char prompt[] = "Введите имя входного файла:\n";

    if (write_all(STDOUT_FILENO, prompt, sizeof(prompt) - 1) < 0) {
        report_error("parent: не удалось вывести приглашение\n");
        return -1;
    }

    for (;;) {
        char character;
        ssize_t result = read(STDIN_FILENO, &character, 1);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            report_error("parent: ошибка чтения имени файла\n");
            return -1;
        }
        if (result == 0) {
            if (length == 0) {
                report_error("parent: имя файла не введено\n");
                return -1;
            }
            break;
        }
        if (character == '\n') {
            break;
        }
        if (length + 1 >= capacity) {
            report_error("parent: слишком длинное имя файла\n");
            return -1;
        }
        path[length++] = character;
    }

    if (length > 0 && path[length - 1] == '\r') {
        --length;
    }
    if (length == 0) {
        report_error("parent: пустое имя файла\n");
        return -1;
    }
    path[length] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    char input_path[INPUT_PATH_CAPACITY];
    const char *child_path = (argc >= 2) ? argv[1] : "./child";
    int input_fd;
    int pipe_fd[2];
    pid_t child_pid;
    int read_failed = 0;

    if (read_input_path(input_path, sizeof(input_path)) < 0) {
        return 1;
    }

    input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) {
        report_error("parent: не удалось открыть входной файл\n");
        return 1;
    }

    if (pipe(pipe_fd) < 0) {
        report_error("parent: не удалось создать pipe\n");
        (void)close(input_fd);
        return 1;
    }

    child_pid = fork();
    if (child_pid < 0) {
        report_error("parent: ошибка fork\n");
        (void)close(input_fd);
        (void)close(pipe_fd[0]);
        (void)close(pipe_fd[1]);
        return 1;
    }

    if (child_pid == 0) {
        char *const child_argv[] = { (char *)child_path, NULL };

        if (close(pipe_fd[0]) < 0) {
            report_error("child launcher: ошибка close\n");
            _exit(126);
        }
        if (dup2(input_fd, STDIN_FILENO) < 0) {
            report_error("child launcher: ошибка перенаправления stdin\n");
            _exit(126);
        }
        if (dup2(pipe_fd[1], STDOUT_FILENO) < 0) {
            report_error("child launcher: ошибка перенаправления stdout\n");
            _exit(126);
        }
        if (input_fd != STDIN_FILENO && close(input_fd) < 0) {
            report_error("child launcher: ошибка закрытия файла\n");
            _exit(126);
        }
        if (pipe_fd[1] != STDOUT_FILENO && close(pipe_fd[1]) < 0) {
            report_error("child launcher: ошибка закрытия pipe\n");
            _exit(126);
        }

        execve(child_path, child_argv, environ);
        report_error("parent: не удалось выполнить программу child\n");
        _exit(127);
    }

    if (close(pipe_fd[1]) < 0) {
        report_error("parent: ошибка закрытия пишущего конца pipe\n");
        read_failed = 1;
    }
    if (close(input_fd) < 0) {
        report_error("parent: ошибка закрытия входного файла\n");
        read_failed = 1;
    }

    for (;;) {
        char buffer[IO_BUFFER_SIZE];
        ssize_t amount = read(pipe_fd[0], buffer, sizeof(buffer));

        if (amount == 0) {
            break;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            report_error("parent: ошибка чтения из pipe\n");
            read_failed = 1;
            break;
        }
        if (write_all(STDOUT_FILENO, buffer, (size_t)amount) < 0) {
            report_error("parent: ошибка вывода результата\n");
            read_failed = 1;
            break;
        }
    }

    if (close(pipe_fd[0]) < 0) {
        report_error("parent: ошибка закрытия читающего конца pipe\n");
        read_failed = 1;
    }

    for (;;) {
        int status;
        pid_t waited = waitpid(child_pid, &status, 0);
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            report_error("parent: ошибка waitpid\n");
            return 1;
        }

        if (read_failed) {
            return 1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        report_error("parent: дочерний процесс завершился аварийно\n");
        return 1;
    }
}
