#ifndef LOUTRE_LINUX_STARTUP_INTERNAL_H
#define LOUTRE_LINUX_STARTUP_INTERNAL_H

#include "model.h"

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
LinuxStartupCommandResult linux_startup_run(
    const char *const argv[], char *output, size_t capacity,
    unsigned timeout_ms, void *context);
/* Decode the executable token only, never expand variables or run a shell. */
bool linux_startup_exec_token(const char *value, char *out, size_t capacity);

#endif
