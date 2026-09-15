#define _POSIX_C_SOURCE 200809L
#include "buffer.h"
#include "platform.h"
#include "startup_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void linux_startup_incomplete(StartupList *list, MetricStatus status) {
    list->partial = true;
    if (list->status == METRIC_OK || status == METRIC_ERROR) list->status = status;
}

StartupItem *linux_startup_append(StartupList *list, StartupKind kind, const char *name) {
    if (list->count >= LINUX_STARTUP_ITEM_CAP) {
        linux_startup_incomplete(list, METRIC_ERROR);
        return NULL;
    }
    if (!buffer_reserve((void **)&list->items, &list->capacity, list->count + 1,
                        sizeof(*list->items), 32, LINUX_STARTUP_ITEM_CAP)) {
        linux_startup_incomplete(list, METRIC_ERROR);
        return NULL;
    }
    StartupItem *item = &list->items[list->count++];
    *item = (StartupItem){ .kind = kind, .cpu_percent = NAN };
    (void)buffer_copy(item->name, sizeof(item->name), name);
    (void)buffer_copy(item->owner, sizeof(item->owner),
                      kind == STARTUP_SYSTEM_SERVICE ? "system" : "user");
    (void)buffer_copy(item->state, sizeof(item->state), "unknown");
    return item;
}

StartupList linux_startup_collect(const LinuxStartupOptions *options) {
    StartupList list = { .status = METRIC_OK };
    if (!options) { linux_startup_incomplete(&list, METRIC_ERROR); return list; }
    LinuxStartupOptions configured = *options;
    if (!configured.runner) configured.runner = linux_startup_run;
    char *output = NULL;
    if (!buffer_calloc((void **)&output, LINUX_STARTUP_OUTPUT_CAP, sizeof(*output)))
        output = NULL;
    if (output && configured.systemctl && configured.systemctl[0] == '/') {
        long long deadline = milliseconds() + LINUX_STARTUP_TOTAL_MS;
        linux_systemd_collect(&configured, &list, STARTUP_SYSTEM_SERVICE, output, deadline);
        linux_systemd_collect(&configured, &list, STARTUP_USER_SERVICE, output, deadline);
    } else linux_startup_incomplete(&list, output ? METRIC_UNAVAILABLE : METRIC_ERROR);
    free(output);
    linux_xdg_autostart_collect(&configured, &list);
    return list;
}

StartupList platform_startup(void) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char fallback[LOUTRE_PATH_MAX] = "";
    if (!config || config[0] != '/') {
        if (home && home[0] == '/') {
            int length = snprintf(fallback, sizeof(fallback), "%s/.config", home);
            if (length < 0 || (size_t)length >= sizeof(fallback)) fallback[0] = '\0';
        }
        config = fallback;
    }
    const char *dirs = getenv("XDG_CONFIG_DIRS");
    LinuxStartupOptions options = {
        .systemctl = access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl",
        .config_home = config,
        .config_dirs = dirs && *dirs ? dirs : "/etc/xdg",
        .desktop = getenv("XDG_CURRENT_DESKTOP"),
        .search_path = getenv("PATH"),
    };
    StartupList list = linux_startup_collect(&options);
    if (!*config) linux_startup_incomplete(&list, METRIC_UNAVAILABLE);
    return list;
}
