#define _POSIX_C_SOURCE 200809L
#include "usage_provider.h"
#include "usage_json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define USAGE_CACHE_MAX_BYTES 65536
#define USAGE_PATH_MAX 4096

static bool read_file(const char *path, char **contents, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size < 0 || size > USAGE_CACHE_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return false;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return false; }
    size_t used = fread(buffer, 1, (size_t)size, file);
    bool ok = used == (size_t)size && ferror(file) == 0;
    fclose(file);
    if (!ok) { free(buffer); return false; }
    buffer[used] = '\0'; *contents = buffer; *length = used;
    return true;
}

bool usage_cache_read(const char *path, ProviderUsage *output) {
    char *contents = NULL; size_t length = 0;
    if (!read_file(path, &contents, &length)) return false;
    bool ok = usage_parse_bridge_json(contents, length, "", output);
    free(contents);
    return ok;
}

bool usage_cache_read_provider(UsageProviderKind kind, const char *path,
                               ProviderUsage *output) {
    char *contents = NULL; size_t length = 0;
    if (!read_file(path, &contents, &length)) return false;
    bool ok = usage_provider_parse(kind, contents, length, output);
    if (!ok && kind == USAGE_PROVIDER_CLAUDE_CODE)
        ok = usage_parse_bridge_json(contents, length, "Claude Code", output);
    free(contents);
    return ok;
}

static bool ensure_parent(const char *path) {
    char parent[USAGE_PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(parent)) return false;
    memcpy(parent, path, length + 1);
    char *slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = '\0';
    for (char *cursor = parent + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(parent, 0700) != 0 && errno != EEXIST) return false;
        *cursor = '/';
    }
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

bool usage_cache_write(const char *path, const ProviderUsage *usage) {
    if (!path || !usage || !usage->provider[0] || !ensure_parent(path)) return false;
    char temporary[USAGE_PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (length <= 0 || (size_t)length >= sizeof(temporary)) return false;
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary); return false; }
    fprintf(file, "{\"provider\":\"%s\",\"windows\":[", usage->provider);
    for (size_t i = 0; i < usage->window_count; i++) {
        const UsageWindow *window = &usage->windows[i];
        if (i) fputc(',', file);
        fprintf(file, "{\"name\":\"%s\"", window->name);
        if (window->has_percent) fprintf(file, ",\"used_percent\":%.6f", window->used_percent);
        if (window->has_tokens) fprintf(file, ",\"used_tokens\":%llu,\"quota_tokens\":%llu",
                                        window->used_tokens, window->quota_tokens);
        if (window->has_reset) fprintf(file, ",\"resets_at\":%lld", (long long)window->resets_at);
        fputc('}', file);
    }
    fputs("]}\n", file);
    bool ok = fflush(file) == 0 && fsync(fd) == 0 && fclose(file) == 0;
    if (!ok) { unlink(temporary); return false; }
    if (rename(temporary, path) != 0) { unlink(temporary); return false; }
    chmod(path, 0600);
    return true;
}
