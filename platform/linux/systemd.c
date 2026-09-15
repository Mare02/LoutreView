#define _POSIX_C_SOURCE 200809L
#include "buffer.h"
#include "startup_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool service_name(const char *name) {
    size_t n = strlen(name);
    if (n <= 8 || n >= 256 || strcmp(name + n - 8, ".service")) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && !strchr("_.:@-\\", c)) return false;
    }
    return name[0] != '-';
}

static StartupItem *service(StartupList *list, StartupKind kind, const char *name) {
    for (size_t i = 0; i < list->count; i++)
        if (list->items[i].kind == kind && !strcmp(list->items[i].name, name))
            return &list->items[i];
    return NULL;
}

static void enabled_state(StartupItem *item, const char *state) {
    item->enabled_known = false; item->enabled = false;
    if (!strcmp(state, "enabled") || !strcmp(state, "enabled-runtime")) {
        item->enabled_known = true; item->enabled = true;
    } else if (!strcmp(state, "disabled") || !strcmp(state, "masked") ||
               !strcmp(state, "masked-runtime")) item->enabled_known = true;
}

static bool command(const LinuxStartupOptions *options, StartupList *list,
                    const char *const argv[], char *output, long long deadline) {
    long long remaining = deadline - milliseconds();
    if (remaining <= 0) { linux_startup_incomplete(list, METRIC_UNAVAILABLE); return false; }
    unsigned ms = remaining < LINUX_STARTUP_COMMAND_MS ? (unsigned)remaining : LINUX_STARTUP_COMMAND_MS;
    output[0] = '\0';
    LinuxStartupCommandResult result = options->runner(
        argv, output, LINUX_STARTUP_OUTPUT_CAP, ms, options->context);
    if (result != LINUX_STARTUP_COMMAND_OK) {
        linux_startup_incomplete(list,
            result == LINUX_STARTUP_COMMAND_ERROR || result == LINUX_STARTUP_COMMAND_LIMIT
                ? METRIC_ERROR : METRIC_UNAVAILABLE);
        return false;
    }
    if (!memchr(output, '\0', LINUX_STARTUP_OUTPUT_CAP)) {
        linux_startup_incomplete(list, METRIC_ERROR); return false;
    }
    return true;
}

static void inventory(StartupList *list, StartupKind kind, char *text, bool files) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *fields = NULL;
        char *name = strtok_r(line, " \t\r", &fields);
        if (!name || !service_name(name)) { linux_startup_incomplete(list, METRIC_ERROR); continue; }
        StartupItem *item = service(list, kind, name);
        if (!item) item = linux_startup_append(list, kind, name);
        if (!item) return;
        if (files) {
            char *state = strtok_r(NULL, " \t\r", &fields);
            if (state) enabled_state(item, state);
            else linux_startup_incomplete(list, METRIC_ERROR);
        }
    }
}

static bool number(const char *s, unsigned long long *out) {
    if (!*s || *s == '-' || *s == '+') return false;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno || *end) return false;
    *out = value; return true;
}

typedef struct {
    char id[256], active[32], sub[32], enabled[32], fragment[LOUTRE_PATH_MAX];
    unsigned long long pid, memory;
    bool have_pid, have_memory;
} Properties;

static void apply_properties(StartupList *list, StartupKind kind, const Properties *properties) {
    StartupItem *item = service(list, kind, properties->id);
    if (!item) return;
    if (*properties->enabled) enabled_state(item, properties->enabled);
    (void)buffer_copy(item->path, sizeof(item->path), properties->fragment);
    item->path_missing = *properties->fragment && access(properties->fragment, F_OK) != 0;
    if (properties->have_pid && properties->pid <= INT_MAX) item->pid = (pid_t)properties->pid;
    if (properties->have_memory && properties->memory != ULLONG_MAX) {
        item->resident = properties->memory;
        item->memory_known = true;
    }
    (void)buffer_copy(item->state, sizeof(item->state),
                      *properties->sub ? properties->sub : (*properties->active ? properties->active : "unknown"));
    if (!strcmp(properties->active, "inactive") || !strcmp(properties->active, "failed")) {
        item->running_known = true; item->running = false;
    } else if (!strcmp(properties->active, "active") || !strcmp(properties->active, "activating") ||
               !strcmp(properties->active, "deactivating") || !strcmp(properties->active, "reloading")) {
        if ((properties->have_pid && properties->pid > 0 && properties->pid <= INT_MAX) || !strcmp(properties->sub, "running")) {
            item->running_known = true; item->running = true;
        } else if (!strcmp(properties->sub, "exited") || !strcmp(properties->sub, "dead")) {
            item->running_known = true; item->running = false;
        }
    }
}

static void properties(StartupList *list, StartupKind kind, char *text) {
    Properties parsed = {0};
    char *line = text;
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (!*line) { apply_properties(list, kind, &parsed); parsed = (Properties){0}; }
        else {
            char *equal = strchr(line, '=');
            if (equal) {
                *equal++ = '\0';
                if (!strcmp(line, "Id")) (void)buffer_copy(parsed.id, sizeof(parsed.id), equal);
                else if (!strcmp(line, "ActiveState")) (void)buffer_copy(parsed.active, sizeof(parsed.active), equal);
                else if (!strcmp(line, "SubState")) (void)buffer_copy(parsed.sub, sizeof(parsed.sub), equal);
                else if (!strcmp(line, "UnitFileState")) (void)buffer_copy(parsed.enabled, sizeof(parsed.enabled), equal);
                else if (!strcmp(line, "FragmentPath")) (void)buffer_copy(parsed.fragment, sizeof(parsed.fragment), equal);
                else if (!strcmp(line, "MainPID")) parsed.have_pid = number(equal, &parsed.pid);
                else if (!strcmp(line, "MemoryCurrent")) parsed.have_memory = number(equal, &parsed.memory);
            } else linux_startup_incomplete(list, METRIC_ERROR);
        }
        line = next;
    }
    apply_properties(list, kind, &parsed);
}

void linux_systemd_collect(const LinuxStartupOptions *options, StartupList *list,
                          StartupKind kind, char *output, long long deadline) {
    const char *scope = kind == STARTUP_SYSTEM_SERVICE ? "--system" : "--user";
    size_t begin = list->count;
    const char *files[] = {options->systemctl, scope, "--no-pager", "--no-legend", "--plain", "--full",
                          "list-unit-files", "--type=service", NULL};
    if (command(options, list, files, output, deadline)) inventory(list, kind, output, true);
    const char *units[] = {options->systemctl, scope, "--no-pager", "--no-legend", "--plain", "--full",
                          "list-units", "--all", "--type=service", NULL};
    if (command(options, list, units, output, deadline)) inventory(list, kind, output, false);
    size_t end = list->count;
    for (size_t i = begin; i < end; i += LINUX_STARTUP_SYSTEMD_BATCH) {
        const char *args[LINUX_STARTUP_SYSTEMD_BATCH + 8] = {options->systemctl, scope, "--no-pager", "show",
            "--property=Id,ActiveState,SubState,UnitFileState,MainPID,FragmentPath,MemoryCurrent", "--"};
        size_t n = 6;
        for (size_t j = i; j < end && j < i + LINUX_STARTUP_SYSTEMD_BATCH; j++) args[n++] = list->items[j].name;
        args[n] = NULL;
        if (command(options, list, args, output, deadline)) properties(list, kind, output);
        for (size_t j = i; j < end && j < i + LINUX_STARTUP_SYSTEMD_BATCH; j++)
            if (!list->items[j].running_known) linux_startup_incomplete(list, METRIC_UNAVAILABLE);
    }
}
