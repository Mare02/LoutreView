#ifndef LOUTRE_USAGE_H
#define LOUTRE_USAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define USAGE_PROVIDER_MAX 32
#define USAGE_WINDOW_NAME_MAX 32
#define USAGE_MAX_WINDOWS 8
#define USAGE_MAX_PROVIDERS 3

typedef struct {
    char name[USAGE_WINDOW_NAME_MAX];
    double used_percent;
    double remaining_percent;
    time_t resets_at;
    unsigned long long used_tokens;
    unsigned long long quota_tokens;
    bool has_percent;
    bool has_tokens;
    bool has_reset;
} UsageWindow;

typedef struct {
    char provider[USAGE_PROVIDER_MAX];
    UsageWindow windows[USAGE_MAX_WINDOWS];
    size_t window_count;
    time_t collected_at;
    bool available;
} ProviderUsage;

typedef struct {
    ProviderUsage providers[USAGE_MAX_PROVIDERS];
    size_t count;
} UsageSnapshot;

void usage_init(ProviderUsage *usage, const char *provider);
void usage_window_init(UsageWindow *window, const char *name);
bool usage_window_normalize(UsageWindow *window);
const UsageWindow *usage_find_window(const ProviderUsage *usage, const char *name);
UsageSnapshot collect_usage(void);

#endif
