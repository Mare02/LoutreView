#include "usage_provider.h"
#include "../usage/usage_json.h"

bool gemini_cli_usage_parse(const char *input, size_t length, ProviderUsage *output) {
    /* /stats model is interactive; consume only an explicitly produced JSON
     * bridge/cache payload, never terminal text or Gemini credentials. */
    return usage_parse_bridge_json(input, length, "Gemini CLI", output);
}
