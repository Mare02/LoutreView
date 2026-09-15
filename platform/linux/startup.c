#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "startup_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

enum { OUTPUT_CAP = 1024 * 1024, ITEM_CAP = 4096, BATCH = 64,
       COMMAND_MS = 1000, TOTAL_MS = 6000, DESKTOP_CAP = 65536 };

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
                if ((size_t)got >= capacity - used) {
                    result = LINUX_STARTUP_COMMAND_LIMIT; break;
                }
                memcpy(output + used, chunk, (size_t)got);
                used += (size_t)got;
                output[used] = '\0';
            } else if (got == 0) eof = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                result = LINUX_STARTUP_COMMAND_ERROR; break;
            }
            if (got > 0) continue; /* deadline checked even for continuous output */
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

static void copy(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len); dst[len] = '\0';
}

static void incomplete(StartupList *list, MetricStatus status) {
    list->partial = true;
    if (list->status == METRIC_OK || status == METRIC_ERROR) list->status = status;
}

static StartupItem *append(StartupList *list, StartupKind kind, const char *name) {
    if (list->count >= ITEM_CAP) { incomplete(list, METRIC_ERROR); return NULL; }
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 32;
        StartupItem *items = realloc(list->items, cap * sizeof(*items));
        if (!items) { incomplete(list, METRIC_ERROR); return NULL; }
        list->items = items; list->capacity = cap;
    }
    StartupItem *item = &list->items[list->count++];
    *item = (StartupItem){ .kind = kind, .cpu_percent = NAN };
    copy(item->name, sizeof(item->name), name);
    copy(item->owner, sizeof(item->owner), kind == STARTUP_SYSTEM_SERVICE ? "system" : "user");
    copy(item->state, sizeof(item->state), "unknown");
    return item;
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
    /* static/indirect/generated/alias do not establish startup enablement. */
    item->enabled_known = false; item->enabled = false;
    if (!strcmp(state, "enabled") || !strcmp(state, "enabled-runtime")) {
        item->enabled_known = true; item->enabled = true;
    } else if (!strcmp(state, "disabled") || !strcmp(state, "masked") ||
               !strcmp(state, "masked-runtime")) item->enabled_known = true;
}

static bool command(const LinuxStartupOptions *o, StartupList *list,
                    const char *const argv[], char *out, long long deadline) {
    long long remaining = deadline - milliseconds();
    if (remaining <= 0) { incomplete(list, METRIC_UNAVAILABLE); return false; }
    unsigned ms = remaining < COMMAND_MS ? (unsigned)remaining : COMMAND_MS;
    out[0] = '\0';
    LinuxStartupCommandResult r = o->runner(argv, out, OUTPUT_CAP, ms, o->context);
    if (r != LINUX_STARTUP_COMMAND_OK) {
        incomplete(list, r == LINUX_STARTUP_COMMAND_ERROR || r == LINUX_STARTUP_COMMAND_LIMIT
                        ? METRIC_ERROR : METRIC_UNAVAILABLE);
        return false;
    }
    if (!memchr(out, '\0', OUTPUT_CAP)) { incomplete(list, METRIC_ERROR); return false; }
    return true;
}

static void inventory(StartupList *list, StartupKind kind, char *text, bool files) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *fields = NULL;
        char *name = strtok_r(line, " \t\r", &fields);
        if (!name || !service_name(name)) { incomplete(list, METRIC_ERROR); continue; }
        StartupItem *item = service(list, kind, name);
        if (!item) item = append(list, kind, name);
        if (!item) return;
        if (files) {
            char *state = strtok_r(NULL, " \t\r", &fields);
            if (state) enabled_state(item, state);
            else incomplete(list, METRIC_ERROR);
        }
    }
}

static bool number(const char *s, unsigned long long *out) {
    if (!*s || *s == '-' || *s == '+') return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end) return false;
    *out = v; return true;
}

typedef struct {
    char id[256], active[32], sub[32], enabled[32], fragment[LOUTRE_PATH_MAX];
    unsigned long long pid, memory;
    bool have_pid, have_memory;
} Properties;

static void apply_properties(StartupList *list, StartupKind kind, const Properties *p) {
    StartupItem *item = service(list, kind, p->id);
    if (!item) return;
    if (*p->enabled) enabled_state(item, p->enabled);
    copy(item->path, sizeof(item->path), p->fragment);
    item->path_missing = *p->fragment && access(p->fragment, F_OK) != 0;
    if (p->have_pid && p->pid <= INT_MAX) item->pid = (pid_t)p->pid;
    if (p->have_memory && p->memory != ULLONG_MAX) {
        item->resident = p->memory;
        item->memory_known = true; /* systemd MemoryCurrent is service-wide */
    }
    copy(item->state, sizeof(item->state), *p->sub ? p->sub : (*p->active ? p->active : "unknown"));
    if (!strcmp(p->active, "inactive") || !strcmp(p->active, "failed")) {
        item->running_known = true; item->running = false;
    } else if (!strcmp(p->active, "active") || !strcmp(p->active, "activating") ||
               !strcmp(p->active, "deactivating") || !strcmp(p->active, "reloading")) {
        if ((p->have_pid && p->pid > 0 && p->pid <= INT_MAX) || !strcmp(p->sub, "running")) {
            item->running_known = true; item->running = true;
        } else if (!strcmp(p->sub, "exited") || !strcmp(p->sub, "dead")) {
            item->running_known = true; item->running = false;
        }
    }
}

static void properties(StartupList *list, StartupKind kind, char *text) {
    Properties p = {0};
    char *line = text;
    /* Unlike strtok, preserve blank lines separating machine property records. */
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (!*line) { apply_properties(list, kind, &p); p = (Properties){0}; }
        else {
            char *equal = strchr(line, '=');
            if (equal) {
                *equal++ = '\0';
                if (!strcmp(line, "Id")) copy(p.id, sizeof(p.id), equal);
                else if (!strcmp(line, "ActiveState")) copy(p.active, sizeof(p.active), equal);
                else if (!strcmp(line, "SubState")) copy(p.sub, sizeof(p.sub), equal);
                else if (!strcmp(line, "UnitFileState")) copy(p.enabled, sizeof(p.enabled), equal);
                else if (!strcmp(line, "FragmentPath")) copy(p.fragment, sizeof(p.fragment), equal);
                else if (!strcmp(line, "MainPID")) p.have_pid = number(equal, &p.pid);
                else if (!strcmp(line, "MemoryCurrent")) p.have_memory = number(equal, &p.memory);
            } else incomplete(list, METRIC_ERROR);
        }
        line = next;
    }
    apply_properties(list, kind, &p);
}

static void systemd(const LinuxStartupOptions *o, StartupList *list,
                    StartupKind kind, char *out, long long deadline) {
    const char *scope = kind == STARTUP_SYSTEM_SERVICE ? "--system" : "--user";
    size_t begin = list->count;
    const char *files[] = {o->systemctl, scope, "--no-pager", "--no-legend", "--plain", "--full",
                          "list-unit-files", "--type=service", NULL};
    if (command(o, list, files, out, deadline)) inventory(list, kind, out, true);
    const char *units[] = {o->systemctl, scope, "--no-pager", "--no-legend", "--plain", "--full",
                          "list-units", "--all", "--type=service", NULL};
    if (command(o, list, units, out, deadline)) inventory(list, kind, out, false);
    size_t end = list->count;
    for (size_t i = begin; i < end; i += BATCH) {
        const char *args[BATCH + 8] = {o->systemctl, scope, "--no-pager", "show",
            "--property=Id,ActiveState,SubState,UnitFileState,MainPID,FragmentPath,MemoryCurrent",
            "--"};
        size_t n = 6;
        for (size_t j = i; j < end && j < i + BATCH; j++) args[n++] = list->items[j].name;
        args[n] = NULL;
        if (command(o, list, args, out, deadline)) properties(list, kind, out);
        for (size_t j = i; j < end && j < i + BATCH; j++)
            if (!list->items[j].running_known) incomplete(list, METRIC_UNAVAILABLE);
    }
}

/* Desktop string unescaping precedes Exec quoting. */
static bool unescape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    while (*s) {
        char c = *s++;
        if (c == '\\') {
            c = *s++;
            if (!c) return false;
            switch (c) {
                case 's': c = ' '; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': break;
                default: return false;
            }
        }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
    out[n] = '\0'; return true;
}

bool linux_startup_exec_token(const char *value, char *out, size_t capacity) {
    char decoded[LOUTRE_PATH_MAX];
    if (!value || !out || !capacity) return false;
    out[0] = '\0';
    if (!unescape(value, decoded, sizeof(decoded))) return false;
    const char *s = decoded;
    while (*s == ' ' || *s == '\t') s++;
    bool quoted = *s == '"', closed = !quoted;
    if (quoted) s++;
    size_t n = 0;
    while (*s) {
        char c = *s++;
        if (quoted && c == '"') { closed = true; break; }
        if (!quoted && (c == ' ' || c == '\t')) break;
        if (quoted && c == '\\') {
            c = *s++;
            if (!c || !strchr("\"`$\\", c)) return false;
        } else if ((!quoted && strchr("\n\r\"'\\><~|&;$*?#()`", c)) ||
                   (quoted && strchr("$`", c))) return false;
        if (c == '=' || (c == '%' && *s != '%')) return false;
        if (c == '%') s++;
        if (n + 1 >= capacity) return false;
        out[n++] = c;
    }
    if (!closed || !n || (quoted && *s && *s != ' ' && *s != '\t')) return false;
    out[n] = '\0'; return true;
}

static bool executable(const char *name, const char *path) {
    struct stat st;
    if (!*name) return false;
    if (strchr(name, '/'))
        return name[0] == '/' && stat(name, &st) == 0 && S_ISREG(st.st_mode) && access(name, X_OK) == 0;
    const char *p = path ? path : "";
    do {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char full[LOUTRE_PATH_MAX];
        int count = len ? snprintf(full, sizeof(full), "%.*s/%s", (int)len, p, name)
                        : snprintf(full, sizeof(full), "./%s", name);
        if (count > 0 && (size_t)count < sizeof(full) && stat(full, &st) == 0 &&
            S_ISREG(st.st_mode) && access(full, X_OK) == 0) return true;
        if (!end) break;
        p = end + 1;
    } while (true);
    return false;
}

static bool desktops_match(const char *list, const char *current) {
    if (!current) return false;
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        for (const char *d = current; *d;) {
            const char *de = strchr(d, ':');
            size_t dl = de ? (size_t)(de - d) : strlen(d);
            if (len && len == dl && !memcmp(p, d, len)) return true;
            if (!de) break;
            d = de + 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = '\0';
    return s;
}

static void desktop_file(const LinuxStartupOptions *o, StartupList *list,
                         const char *path, const char *name, bool user) {
    StartupItem *item = append(list, STARTUP_DESKTOP_AUTOSTART, name);
    if (!item) return;
    copy(item->path, sizeof(item->path), path);
    copy(item->owner, sizeof(item->owner), user ? "user" : "system");
    /* Nonblocking open avoids hanging on a FIFO masquerading as a desktop file. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > DESKTOP_CAP) {
        if (fd >= 0) close(fd);
        copy(item->state, sizeof(item->state), "unreadable");
        incomplete(list, METRIC_ERROR); return;
    }
    char text[DESKTOP_CAP + 1];
    size_t used = 0;
    while (used < DESKTOP_CAP) {
        ssize_t got = read(fd, text + used, DESKTOP_CAP - used);
        if (got == 0) break;
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd); incomplete(list, METRIC_ERROR); return;
        }
        used += (size_t)got;
    }
    close(fd);
    if (used == DESKTOP_CAP || memchr(text, '\0', used)) { incomplete(list, METRIC_ERROR); return; }
    text[used] = '\0';
    char *exec = NULL, *try_exec = NULL, *only = NULL, *exclude = NULL, *type = NULL;
    bool section = false, hidden = false, valid = true, seen = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line = trim(line);
        if (!*line || *line == '#') continue;
        if (*line == '[') {
            section = !strcmp(line, "[Desktop Entry]");
            if (section && seen) valid = false;
            if (section) seen = true;
            continue;
        }
        if (!section) continue;
        char *equal = strchr(line, '=');
        if (!equal) { valid = false; continue; }
        *equal++ = '\0';
        char *key = trim(line), *value = trim(equal);
        if (!strcmp(key, "Hidden")) {
            hidden = !strcmp(value, "true");
            if (!hidden && strcmp(value, "false")) valid = false;
        } else if (!strcmp(key, "Name")) {
            if (!unescape(value, item->name, sizeof(item->name))) valid = false;
        } else if (!strcmp(key, "Exec")) exec = value;
        else if (!strcmp(key, "TryExec")) try_exec = value;
        else if (!strcmp(key, "OnlyShowIn")) only = value;
        else if (!strcmp(key, "NotShowIn")) exclude = value;
        else if (!strcmp(key, "Type")) type = value;
    }
    const char *state = "configured";
    item->enabled_known = true; item->enabled = false;
    char token[LOUTRE_PATH_MAX];
    if (hidden) state = "hidden";
    else if (!valid || !seen || !type || strcmp(type, "Application") || (only && exclude)) {
        state = "invalid"; incomplete(list, METRIC_ERROR);
    } else if ((only && !desktops_match(only, o->desktop)) ||
               (exclude && desktops_match(exclude, o->desktop))) state = "desktop-filtered";
    else if (try_exec && *try_exec &&
             (!unescape(try_exec, token, sizeof(token)) || !executable(token, o->search_path))) {
        state = "tryexec-missing"; item->path_missing = true;
    } else if (!exec || !linux_startup_exec_token(exec, token, sizeof(token))) {
        state = "exec-unknown"; item->enabled_known = false;
        incomplete(list, METRIC_UNAVAILABLE);
    } else {
        item->path_missing = !executable(token, o->search_path);
        item->enabled = true;
        if (item->path_missing) state = "exec-missing";
    }
    copy(item->state, sizeof(item->state), state);
    /* Presence/configuration never tells us whether a desktop entry is running. */
}

typedef struct { char (*names)[256]; size_t count; } DesktopSeen;

static void desktop_dir(const LinuxStartupOptions *o, StartupList *list,
                        DesktopSeen *seen, const char *root, bool user) {
    if (!root || root[0] != '/') return; /* XDG paths must be absolute. */
    char dirpath[LOUTRE_PATH_MAX];
    int len = snprintf(dirpath, sizeof(dirpath), "%s/autostart", root);
    if (len < 0 || (size_t)len >= sizeof(dirpath)) { incomplete(list, METRIC_ERROR); return; }
    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (errno != ENOENT && errno != ENOTDIR) incomplete(list, errno == EACCES ? METRIC_PERMISSION : METRIC_ERROR);
        return;
    }
    struct dirent *entry;
    while (true) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) incomplete(list, METRIC_ERROR); break; }
        size_t n = strlen(entry->d_name);
        if (n <= 8 || strcmp(entry->d_name + n - 8, ".desktop")) continue;
        bool duplicate = false;
        for (size_t i = 0; i < seen->count; i++)
            if (!strcmp(seen->names[i], entry->d_name)) { duplicate = true; break; }
        if (duplicate) continue;
        if (n >= 256 || seen->count == ITEM_CAP || list->count == ITEM_CAP) {
            incomplete(list, METRIC_ERROR); break;
        }
        /* Claim before parsing: unreadable/hidden user overrides also win. */
        copy(seen->names[seen->count++], 256, entry->d_name);
        char full[LOUTRE_PATH_MAX];
        len = snprintf(full, sizeof(full), "%s/%s", dirpath, entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(full)) { incomplete(list, METRIC_ERROR); continue; }
        desktop_file(o, list, full, entry->d_name, user);
    }
    closedir(dir);
}

StartupList linux_startup_collect(const LinuxStartupOptions *options) {
    StartupList list = { .status = METRIC_OK };
    if (!options) { incomplete(&list, METRIC_ERROR); return list; }
    LinuxStartupOptions o = *options;
    if (!o.runner) o.runner = linux_startup_run;
    char *out = malloc(OUTPUT_CAP);
    if (out && o.systemctl && o.systemctl[0] == '/') {
        long long deadline = milliseconds() + TOTAL_MS;
        systemd(&o, &list, STARTUP_SYSTEM_SERVICE, out, deadline);
        systemd(&o, &list, STARTUP_USER_SERVICE, out, deadline);
    } else incomplete(&list, out ? METRIC_UNAVAILABLE : METRIC_ERROR);
    free(out);
    DesktopSeen seen = { .names = calloc(ITEM_CAP, sizeof(*seen.names)) };
    if (!seen.names) { incomplete(&list, METRIC_ERROR); return list; }
    desktop_dir(&o, &list, &seen, o.config_home, true);
    char *dirs = strdup(o.config_dirs ? o.config_dirs : "/etc/xdg");
    if (!dirs) incomplete(&list, METRIC_ERROR);
    else {
        char *save = NULL;
        for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
            desktop_dir(&o, &list, &seen, dir, false);
        free(dirs);
    }
    free(seen.names);
    return list;
}

StartupList platform_startup(void) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char fallback[LOUTRE_PATH_MAX] = "";
    if (!config || config[0] != '/') {
        if (home && home[0] == '/') {
            int len = snprintf(fallback, sizeof(fallback), "%s/.config", home);
            if (len < 0 || (size_t)len >= sizeof(fallback)) fallback[0] = '\0';
        }
        config = fallback;
    }
    const char *dirs = getenv("XDG_CONFIG_DIRS");
    LinuxStartupOptions o = {
        .systemctl = access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl",
        .config_home = config,
        .config_dirs = dirs && *dirs ? dirs : "/etc/xdg",
        .desktop = getenv("XDG_CURRENT_DESKTOP"),
        .search_path = getenv("PATH"),
    };
    StartupList list = linux_startup_collect(&o);
    if (!*config) incomplete(&list, METRIC_UNAVAILABLE);
    return list;
}
