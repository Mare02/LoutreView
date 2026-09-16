#include "usage_provider.h"
#include "../usage/usage_json.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern volatile sig_atomic_t running;

bool codex_usage_parse(const char *input, size_t length, ProviderUsage *output) {
    return usage_parse_bridge_json(input, length, "Codex", output);
}

static bool write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written;
        length -= (size_t)written;
    }
    return true;
}

static bool has_response_id(const char *output) {
    return strstr(output, "\"id\":2") != NULL ||
           strstr(output, "\"id\": 2") != NULL;
}

static bool read_app_server(char *output, size_t capacity) {
    int input_pipe[2];
    int output_pipe[2];
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) return false;

    pid_t child = fork();
    if (child < 0) {
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        return false;
    }
    if (child == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        execlp("codex", "codex", "app-server", "--stdio", (char *)NULL);
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    const char *request =
        "{\"method\":\"initialize\",\"id\":1,\"params\":{"
        "\"clientInfo\":{\"name\":\"loutre-view\",\"title\":\"LoutreView\","
        "\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":true}}}\n"
        "{\"method\":\"initialized\",\"params\":{}}\n"
        "{\"method\":\"account/rateLimits/read\",\"id\":2,\"params\":{}}\n";
    bool ok = write_all(input_pipe[1], request, strlen(request));

    size_t used = 0;
    output[0] = '\0';
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;
    while (ok && used + 1 < capacity) {
        if (!running) { ok = false; break; }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(output_pipe[0], &read_set);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        time_t seconds = deadline.tv_sec - now.tv_sec;
        long nanoseconds = deadline.tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
        if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) { ok = false; break; }
        struct timeval timeout = {
            .tv_sec = seconds,
            .tv_usec = nanoseconds / 1000
        };
        int ready = select(output_pipe[0] + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) {
            if (!running) { ok = false; break; }
            continue;
        }
        if (ready <= 0) { ok = false; break; }
        ssize_t count = read(output_pipe[0], output + used, capacity - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
        output[used] = '\0';
        if (has_response_id(output)) break;
    }
    close(input_pipe[1]);
    close(output_pipe[0]);
    kill(child, SIGTERM);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return ok && has_response_id(output);
}

static bool parse_bucket(const char *input, size_t length, const char *key,
                         const char *name, ProviderUsage *output) {
    char needle[32];
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written <= 0 || (size_t)written >= sizeof(needle)) return false;
    const char *bucket = strstr(input, needle);
    if (!bucket) return false;
    const char *end = strchr(bucket, '}');
    if (!end || end >= input + length) return false;

    double used_percent, duration, reset;
    if (!usage_json_number(bucket, (size_t)(end - bucket), "usedPercent",
                           &used_percent) ||
        !usage_json_number(bucket, (size_t)(end - bucket), "windowDurationMins",
                           &duration)) return false;
    char window_name[USAGE_WINDOW_NAME_MAX];
    if (duration == 300.0) snprintf(window_name, sizeof(window_name), "5h");
    else if (duration == 10080.0) snprintf(window_name, sizeof(window_name), "7d");
    else snprintf(window_name, sizeof(window_name), "%ldm", (long)duration);
    UsageWindow window;
    usage_window_init(&window, window_name[0] ? window_name : name);
    window.used_percent = used_percent;
    window.has_percent = true;
    if (usage_json_number(bucket, (size_t)(end - bucket), "resetsAt", &reset) &&
        reset >= 0) {
        window.resets_at = (time_t)reset;
        window.has_reset = true;
    }
    if (!usage_window_normalize(&window) ||
        output->window_count >= USAGE_MAX_WINDOWS) return false;
    output->windows[output->window_count++] = window;
    return true;
}

bool codex_usage_collect(ProviderUsage *output) {
    if (!output) return false;
    char response[65536];
    if (!read_app_server(response, sizeof(response))) return false;
    usage_init(output, "Codex");
    parse_bucket(response, strlen(response), "primary", "primary", output);
    parse_bucket(response, strlen(response), "secondary", "secondary", output);
    output->available = output->window_count > 0;
    return output->available;
}
