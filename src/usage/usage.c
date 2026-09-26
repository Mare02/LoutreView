#include "usage.h"
#include "usage_provider.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define USAGE_PATH_MAX 4096

static ProviderUsage codex_live_usage;
static time_t codex_live_collected_at;
static bool codex_live_attempted;

static const char *provider_command(UsageProviderKind kind) {
    switch (kind) {
    case USAGE_PROVIDER_CODEX: return "codex";
    case USAGE_PROVIDER_CLAUDE_CODE: return "claude";
    case USAGE_PROVIDER_GEMINI_CLI: return "gemini";
    }
    return NULL;
}

static bool command_installed(const char *command) {
    if (!command || !*command) return false;
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    while (*path) {
        const char *separator = strchr(path, ':');
        size_t directory_length = separator ? (size_t)(separator - path) : strlen(path);
        char candidate[USAGE_PATH_MAX];
        int written;
        if (directory_length == 0)
            written = snprintf(candidate, sizeof(candidate), "%s", command);
        else
            written = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                               (int)directory_length, path, command);
        if (written > 0 && (size_t)written < sizeof(candidate) &&
            access(candidate, X_OK) == 0)
            return true;
        if (!separator) break;
        path = separator + 1;
    }
    return false;
}

static const char *usage_cache_root(void) {
    const char *override = getenv("LOUTREVIEW_USAGE_DIR");
    if (override && *override) return override;
    const char *state = getenv("XDG_STATE_HOME");
    if (state && *state) return state;
    const char *home = getenv("HOME");
    return home && *home ? home : NULL;
}

static bool usage_path(char *out, size_t capacity, const char *root, const char *file) {
    if (!root) return false;
    const char *override = getenv("LOUTREVIEW_USAGE_DIR");
    const char *state = getenv("XDG_STATE_HOME");
    int written;
    if (override && *override) written = snprintf(out, capacity, "%s/%s", root, file);
    else if (state && *state) written = snprintf(out, capacity, "%s/loutre-view/usage/%s", root, file);
    else written = snprintf(out, capacity, "%s/.local/state/loutre-view/usage/%s", root, file);
    return written > 0 && (size_t)written < capacity;
}

bool usage_cache_path(UsageProviderKind kind, char *out, size_t capacity) {
    const char *root = usage_cache_root();
    const char *files[] = {"codex.json", "claude-code.json", "gemini-cli.json"};
    if (kind < USAGE_PROVIDER_CODEX || kind > USAGE_PROVIDER_GEMINI_CLI) return false;
    return usage_path(out, capacity, root, files[kind]);
}

void usage_window_init(UsageWindow *window, const char *name) {
    if (!window) return;
    memset(window, 0, sizeof(*window));
    if (name) {
        strncpy(window->name, name, sizeof(window->name) - 1);
        window->name[sizeof(window->name) - 1] = '\0';
    }
}

void usage_init(ProviderUsage *usage, const char *provider) {
    if (!usage) return;
    memset(usage, 0, sizeof(*usage));
    if (provider) {
        strncpy(usage->provider, provider, sizeof(usage->provider) - 1);
        usage->provider[sizeof(usage->provider) - 1] = '\0';
    }
}

bool usage_window_normalize(UsageWindow *window) {
    if (!window || !window->name[0]) return false;
    if (window->has_percent) {
        if (!isfinite(window->used_percent)) return false;
        if (window->used_percent < 0.0) window->used_percent = 0.0;
        if (window->used_percent > 100.0) window->used_percent = 100.0;
        window->remaining_percent = 100.0 - window->used_percent;
    } else {
        window->remaining_percent = 0.0;
    }
    if (window->has_reset && window->resets_at < 0) {
        window->has_reset = false;
        window->resets_at = 0;
    }
    if (window->has_tokens && window->used_tokens > window->quota_tokens)
        window->used_tokens = window->quota_tokens;
    return window->has_percent || window->has_tokens || window->has_reset;
}

const UsageWindow *usage_find_window(const ProviderUsage *usage, const char *name) {
    if (!usage || !name) return NULL;
    for (size_t i = 0; i < usage->window_count; i++)
        if (!strcmp(usage->windows[i].name, name)) return &usage->windows[i];
    return NULL;
}

UsageSnapshot collect_usage(void) {
    UsageSnapshot snapshot = {0};
    for (size_t i = 0; i < 3; i++) {
        const UsageProvider *provider = usage_provider_at(i);
        if (!provider) continue;
        if (!command_installed(provider_command(provider->kind))) continue;
        ProviderUsage usage;
        usage_init(&usage, provider->name);
        snapshot.providers[snapshot.count++] = usage;
        char path[USAGE_PATH_MAX];
        bool live = false;
        if (provider->kind == USAGE_PROVIDER_CODEX) {
            time_t now = time(NULL);
            time_t retry_after = codex_live_usage.available ? 30 : 5;
            if (!codex_live_attempted || now - codex_live_collected_at >= retry_after) {
                codex_live_attempted = true;
                codex_live_collected_at = now;
                ProviderUsage updated;
                usage_init(&updated, provider->name);
                if (codex_usage_collect(&updated)) codex_live_usage = updated;
            }
            if (codex_live_usage.available) {
                usage = codex_live_usage;
                live = true;
            }
        }
        if (!live && usage_cache_path(provider->kind, path, sizeof(path)) &&
            usage_cache_read_provider(provider->kind, path, &usage))
            snapshot.providers[snapshot.count - 1] = usage;
        else if (live)
            snapshot.providers[snapshot.count - 1] = usage;
    }
    return snapshot;
}
