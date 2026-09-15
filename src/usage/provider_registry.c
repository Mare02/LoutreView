#include "usage_provider.h"
#include <strings.h>
#include <string.h>

static const UsageProvider providers[] = {
    {USAGE_PROVIDER_CODEX, "Codex", codex_usage_parse},
    {USAGE_PROVIDER_CLAUDE_CODE, "Claude Code", claude_code_usage_from_statusline},
    {USAGE_PROVIDER_GEMINI_CLI, "Gemini CLI", gemini_cli_usage_parse}
};

size_t usage_provider_count(void) { return sizeof(providers) / sizeof(providers[0]); }

const UsageProvider *usage_provider_at(size_t index) {
    return index < usage_provider_count() ? &providers[index] : NULL;
}

const UsageProvider *usage_provider_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < usage_provider_count(); i++)
        if (!strcasecmp(name, providers[i].name)) return &providers[i];
    if (!strcasecmp(name, "codex")) return &providers[USAGE_PROVIDER_CODEX];
    if (!strcasecmp(name, "claude") || !strcasecmp(name, "claude-code"))
        return &providers[USAGE_PROVIDER_CLAUDE_CODE];
    if (!strcasecmp(name, "gemini") || !strcasecmp(name, "gemini-cli"))
        return &providers[USAGE_PROVIDER_GEMINI_CLI];
    return NULL;
}

bool usage_provider_parse(UsageProviderKind kind, const char *input, size_t length,
                          ProviderUsage *output) {
    const UsageProvider *provider = NULL;
    for (size_t i = 0; i < usage_provider_count(); i++)
        if (providers[i].kind == kind) provider = &providers[i];
    return provider && provider->parse(input, length, output);
}
