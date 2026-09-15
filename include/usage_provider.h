#ifndef LOUTRE_USAGE_PROVIDER_H
#define LOUTRE_USAGE_PROVIDER_H

#include "usage.h"
#include <stddef.h>

typedef enum {
    USAGE_PROVIDER_CODEX,
    USAGE_PROVIDER_CLAUDE_CODE,
    USAGE_PROVIDER_GEMINI_CLI
} UsageProviderKind;

typedef bool (*UsageProviderParse)(const char *input, size_t length,
                                   ProviderUsage *output);

typedef struct {
    UsageProviderKind kind;
    const char *name;
    UsageProviderParse parse;
} UsageProvider;

size_t usage_provider_count(void);
const UsageProvider *usage_provider_at(size_t index);
const UsageProvider *usage_provider_find(const char *name);
bool usage_provider_parse(UsageProviderKind kind, const char *input,
                          size_t length, ProviderUsage *output);

bool usage_cache_read(const char *path, ProviderUsage *output);
bool usage_cache_write(const char *path, const ProviderUsage *usage);
bool usage_cache_read_provider(UsageProviderKind kind, const char *path,
                               ProviderUsage *output);
bool usage_cache_path(UsageProviderKind kind, char *out, size_t capacity);

/* Converts a Claude Code status-line payload into a cache-ready usage value. */
bool claude_code_usage_from_statusline(const char *input, size_t length,
                                       ProviderUsage *output);

/* Adapters intentionally accept only bridge/cache JSON, never credentials. */
bool codex_usage_parse(const char *input, size_t length, ProviderUsage *output);
bool codex_usage_collect(ProviderUsage *output);
bool gemini_cli_usage_parse(const char *input, size_t length,
                            ProviderUsage *output);

#endif
