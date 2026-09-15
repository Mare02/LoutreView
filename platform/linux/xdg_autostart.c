#define _POSIX_C_SOURCE 200809L
#include "startup_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool unescape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    while (*s) {
        char c = *s++;
        if (c == '\\') {
            c = *s++;
            if (!c) return false;
            switch (c) {
                case 's': c = ' '; break; case 'n': c = '\n'; break;
                case 't': c = '\t'; break; case 'r': c = '\r'; break;
                case '\\': break; default: return false;
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
        if (quoted && c == '\\') { c = *s++; if (!c || !strchr("\"`$\\", c)) return false; }
        else if ((!quoted && strchr("\n\r\"'\\><~|&;$*?#()`", c)) ||
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
    if (strchr(name, '/')) return name[0] == '/' && stat(name, &st) == 0 &&
        S_ISREG(st.st_mode) && access(name, X_OK) == 0;
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
        const char *end = strchr(p, ';'); size_t len = end ? (size_t)(end - p) : strlen(p);
        for (const char *d = current; *d;) {
            const char *de = strchr(d, ':'); size_t dl = de ? (size_t)(de - d) : strlen(d);
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
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

static void desktop_file(const LinuxStartupOptions *options, StartupList *list,
                         const char *path, const char *name, bool user) {
    StartupItem *item = linux_startup_append(list, STARTUP_DESKTOP_AUTOSTART, name);
    if (!item) return;
    linux_startup_copy(item->path, sizeof(item->path), path);
    linux_startup_copy(item->owner, sizeof(item->owner), user ? "user" : "system");
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > LINUX_STARTUP_DESKTOP_CAP) {
        if (fd >= 0) close(fd);
        linux_startup_copy(item->state, sizeof(item->state), "unreadable");
        linux_startup_incomplete(list, METRIC_ERROR); return;
    }
    char text[LINUX_STARTUP_DESKTOP_CAP + 1]; size_t used = 0;
    while (used < LINUX_STARTUP_DESKTOP_CAP) {
        ssize_t got = read(fd, text + used, LINUX_STARTUP_DESKTOP_CAP - used);
        if (got == 0) break;
        if (got < 0) { if (errno == EINTR) continue; close(fd); linux_startup_incomplete(list, METRIC_ERROR); return; }
        used += (size_t)got;
    }
    close(fd);
    if (used == LINUX_STARTUP_DESKTOP_CAP || memchr(text, '\0', used)) { linux_startup_incomplete(list, METRIC_ERROR); return; }
    text[used] = '\0';
    char *exec = NULL, *try_exec = NULL, *only = NULL, *exclude = NULL, *type = NULL;
    bool section = false, hidden = false, valid = true, seen = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line = trim(line);
        if (!*line || *line == '#') continue;
        if (*line == '[') { section = !strcmp(line, "[Desktop Entry]"); if (section && seen) valid = false; if (section) seen = true; continue; }
        if (!section) continue;
        char *equal = strchr(line, '=');
        if (!equal) { valid = false; continue; }
        *equal++ = '\0'; char *key = trim(line), *value = trim(equal);
        if (!strcmp(key, "Hidden")) { hidden = !strcmp(value, "true"); if (!hidden && strcmp(value, "false")) valid = false; }
        else if (!strcmp(key, "Name")) { if (!unescape(value, item->name, sizeof(item->name))) valid = false; }
        else if (!strcmp(key, "Exec")) exec = value;
        else if (!strcmp(key, "TryExec")) try_exec = value;
        else if (!strcmp(key, "OnlyShowIn")) only = value;
        else if (!strcmp(key, "NotShowIn")) exclude = value;
        else if (!strcmp(key, "Type")) type = value;
    }
    const char *state = "configured"; item->enabled_known = true; item->enabled = false;
    char token[LOUTRE_PATH_MAX];
    if (hidden) state = "hidden";
    else if (!valid || !seen || !type || strcmp(type, "Application") || (only && exclude)) { state = "invalid"; linux_startup_incomplete(list, METRIC_ERROR); }
    else if ((only && !desktops_match(only, options->desktop)) || (exclude && desktops_match(exclude, options->desktop))) state = "desktop-filtered";
    else if (try_exec && *try_exec && (!unescape(try_exec, token, sizeof(token)) || !executable(token, options->search_path))) { state = "tryexec-missing"; item->path_missing = true; }
    else if (!exec || !linux_startup_exec_token(exec, token, sizeof(token))) { state = "exec-unknown"; item->enabled_known = false; linux_startup_incomplete(list, METRIC_UNAVAILABLE); }
    else { item->path_missing = !executable(token, options->search_path); item->enabled = true; if (item->path_missing) state = "exec-missing"; }
    linux_startup_copy(item->state, sizeof(item->state), state);
}

typedef struct { char (*names)[256]; size_t count; } DesktopSeen;

static void desktop_dir(const LinuxStartupOptions *options, StartupList *list,
                        DesktopSeen *seen, const char *root, bool user) {
    if (!root || root[0] != '/') return;
    char dirpath[LOUTRE_PATH_MAX]; int len = snprintf(dirpath, sizeof(dirpath), "%s/autostart", root);
    if (len < 0 || (size_t)len >= sizeof(dirpath)) { linux_startup_incomplete(list, METRIC_ERROR); return; }
    DIR *dir = opendir(dirpath);
    if (!dir) { if (errno != ENOENT && errno != ENOTDIR) linux_startup_incomplete(list, errno == EACCES ? METRIC_PERMISSION : METRIC_ERROR); return; }
    while (true) {
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) linux_startup_incomplete(list, METRIC_ERROR); break; }
        size_t n = strlen(entry->d_name);
        if (n <= 8 || strcmp(entry->d_name + n - 8, ".desktop")) continue;
        bool duplicate = false;
        for (size_t i = 0; i < seen->count; i++) if (!strcmp(seen->names[i], entry->d_name)) { duplicate = true; break; }
        if (duplicate) continue;
        if (n >= 256 || seen->count == LINUX_STARTUP_ITEM_CAP || list->count == LINUX_STARTUP_ITEM_CAP) { linux_startup_incomplete(list, METRIC_ERROR); break; }
        linux_startup_copy(seen->names[seen->count++], 256, entry->d_name);
        char full[LOUTRE_PATH_MAX]; len = snprintf(full, sizeof(full), "%s/%s", dirpath, entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(full)) { linux_startup_incomplete(list, METRIC_ERROR); continue; }
        desktop_file(options, list, full, entry->d_name, user);
    }
    closedir(dir);
}

void linux_xdg_autostart_collect(const LinuxStartupOptions *options, StartupList *list) {
    DesktopSeen seen = { .names = calloc(LINUX_STARTUP_ITEM_CAP, sizeof(*seen.names)) };
    if (!seen.names) { linux_startup_incomplete(list, METRIC_ERROR); return; }
    desktop_dir(options, list, &seen, options->config_home, true);
    char *dirs = strdup(options->config_dirs ? options->config_dirs : "/etc/xdg");
    if (!dirs) linux_startup_incomplete(list, METRIC_ERROR);
    else {
        char *save = NULL;
        for (char *dir = strtok_r(dirs, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) desktop_dir(options, list, &seen, dir, false);
        free(dirs);
    }
    free(seen.names);
}
