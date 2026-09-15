#define _POSIX_C_SOURCE 200809L
#include "buffer.h"
#include "startup_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static long long milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

LinuxStartupCommandResult linux_startup_run(
    const char *const argv[], char *output, size_t capacity,
    unsigned timeout_ms, void *context) {
    (void)context;
    if (!argv || !argv[0] || argv[0][0] != '/' || !output || capacity < 2)
        return LINUX_STARTUP_COMMAND_ERROR;
    output[0] = '\0';
    long long start = milliseconds();
    if (start < 0) return LINUX_STARTUP_COMMAND_ERROR;
    if (access(argv[0], X_OK) != 0) return LINUX_STARTUP_COMMAND_UNAVAILABLE;
    size_t nenv = 0;
    while (environ[nenv]) nenv++;
    char **env = calloc(nenv + 4, sizeof(*env));
    if (!env) return LINUX_STARTUP_COMMAND_ERROR;
    size_t n = 0;
    for (size_t i = 0; i < nenv; i++) {
        if (strncmp(environ[i], "LC_ALL=", 7) &&
            strncmp(environ[i], "SYSTEMD_COLORS=", 15) &&
            strncmp(environ[i], "SYSTEMD_PAGER=", 14)) env[n++] = environ[i];
    }
    env[n++] = "LC_ALL=C";
    env[n++] = "SYSTEMD_COLORS=0";
    env[n++] = "SYSTEMD_PAGER=";
    int pipefd[2];
    if (pipe(pipefd) != 0) { free(env); return LINUX_STARTUP_COMMAND_ERROR; }
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) {
        close(pipefd[0]); close(pipefd[1]); free(env);
        return LINUX_STARTUP_COMMAND_ERROR;
    }
    pid_t child = fork();
    if (child == 0) {
        (void)setpgid(0, 0);
        if (dup2(nullfd, STDIN_FILENO) < 0 ||
            dup2(nullfd, STDERR_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (nullfd > STDERR_FILENO) close(nullfd);
        if (pipefd[0] > STDERR_FILENO) close(pipefd[0]);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        execve(argv[0], (char *const *)argv, env);
        _exit(errno == ENOENT ? 127 : 126);
    }
    free(env);
    close(nullfd); close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return LINUX_STARTUP_COMMAND_ERROR; }
    (void)setpgid(child, child);
    LinuxStartupCommandResult result = LINUX_STARTUP_COMMAND_OK;
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0)
        result = LINUX_STARTUP_COMMAND_ERROR;
    bool eof = false, reaped = false;
    size_t used = 0;
    int status = 0;
    while (result == LINUX_STARTUP_COMMAND_OK && !(eof && reaped)) {
        long long now = milliseconds();
        if (now < 0 || now - start >= timeout_ms) {
            result = LINUX_STARTUP_COMMAND_TIMEOUT; break;
        }
        if (!eof) {
            char chunk[4096];
            ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
            if (got > 0) {
                if (buffer_append(output, capacity, &used, chunk, (size_t)got) != BUFFER_OK) {
                    result = LINUX_STARTUP_COMMAND_LIMIT; break;
                }
            } else if (got == 0) eof = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                result = LINUX_STARTUP_COMMAND_ERROR; break;
            }
            if (got > 0) continue;
        }
        if (!reaped) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) reaped = true;
            else if (waited < 0 && errno != EINTR) {
                result = LINUX_STARTUP_COMMAND_ERROR; break;
            }
        }
        if (!(eof && reaped)) {
            int delay = (int)((long long)timeout_ms - (now - start));
            if (delay > 10) delay = 10;
            struct pollfd fd = { .fd = pipefd[0], .events = POLLIN };
            (void)poll(eof ? NULL : &fd, eof ? 0 : 1, delay);
        }
    }
    close(pipefd[0]);
    if (result != LINUX_STARTUP_COMMAND_OK) {
        (void)kill(-child, SIGKILL);
        if (!reaped) (void)kill(child, SIGKILL);
    }
    if (!reaped) {
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != child) result = LINUX_STARTUP_COMMAND_ERROR;
    }
    if (result == LINUX_STARTUP_COMMAND_OK &&
        (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
        result = WIFEXITED(status) && WEXITSTATUS(status) == 127
            ? LINUX_STARTUP_COMMAND_UNAVAILABLE : LINUX_STARTUP_COMMAND_ERROR;
    if (result != LINUX_STARTUP_COMMAND_OK) output[0] = '\0';
    return result;
}
