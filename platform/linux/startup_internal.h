#ifndef LOUTRE_LINUX_STARTUP_INTERNAL_H
#define LOUTRE_LINUX_STARTUP_INTERNAL_H

#include "model.h"

#include <stddef.h>

enum {
    LINUX_STARTUP_OUTPUT_CAP = 1024 * 1024,
    LINUX_STARTUP_ITEM_CAP = 4096,
    LINUX_STARTUP_SYSTEMD_BATCH = 64,
    LINUX_STARTUP_COMMAND_MS = 1000,
    LINUX_STARTUP_TOTAL_MS = 6000,
    LINUX_STARTUP_DESKTOP_CAP = 65536
};

/* Private test seams. A runner returns a complete, NUL-terminated response only
 * on success; callers discard failed/truncated output. No entry is executed. */
typedef enum {
    LINUX_STARTUP_COMMAND_OK,
    LINUX_STARTUP_COMMAND_UNAVAILABLE,
    LINUX_STARTUP_COMMAND_ERROR,
    LINUX_STARTUP_COMMAND_TIMEOUT,
    LINUX_STARTUP_COMMAND_LIMIT
} LinuxStartupCommandResult;
typedef LinuxStartupCommandResult (*LinuxStartupRunner)(
    const char *const argv[], char *output, size_t capacity,
    unsigned timeout_ms, void *context);

typedef struct {
    const char *systemctl;       /* absolute executable path */
    const char *config_home;     /* resolved XDG_CONFIG_HOME, absolute */
    const char *config_dirs;     /* colon-separated XDG_CONFIG_DIRS */
    const char *desktop;         /* colon-separated XDG_CURRENT_DESKTOP */
    const char *search_path;     /* PATH for executable existence checks */
    LinuxStartupRunner runner;
    void *context;
} LinuxStartupOptions;

StartupList linux_startup_collect(const LinuxStartupOptions *options);
void linux_startup_incomplete(StartupList *list, MetricStatus status);
StartupItem *linux_startup_append(StartupList *list, StartupKind kind, const char *name);
LinuxStartupCommandResult linux_startup_run(
    const char *const argv[], char *output, size_t capacity,
    unsigned timeout_ms, void *context);
/* Decode the executable token only, never expand variables or run a shell. */
bool linux_startup_exec_token(const char *value, char *out, size_t capacity);
void linux_systemd_collect(const LinuxStartupOptions *options, StartupList *list,
                          StartupKind kind, char *output, long long deadline);
void linux_xdg_autostart_collect(const LinuxStartupOptions *options, StartupList *list);

#endif
