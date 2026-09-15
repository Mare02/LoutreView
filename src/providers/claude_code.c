#include "usage_provider.h"
#include "../usage/usage_json.h"
#include <string.h>

bool claude_code_usage_from_statusline(const char *input, size_t length,
                                       ProviderUsage *output) {
    if (!input || !output) return false;
    usage_init(output, "Claude Code");
    const char *names[] = {"five_hour", "seven_day"};
    const char *labels[] = {"5-hour", "7-day"};
    for (size_t i = 0; i < 2; i++) {
        double used;
        /* Search within the named rate-limit object to avoid accepting
         * unrelated context-window percentages. */
        const char *marker = strstr(input, names[i]);
        if (!marker) continue;
        if (!usage_json_number(marker, length - (size_t)(marker - input),
                               "used_percentage", &used)) continue;
        UsageWindow *window = &output->windows[output->window_count];
        usage_window_init(window, labels[i]);
        window->used_percent = used; window->has_percent = true;
        double reset;
        if (usage_json_number(marker, length - (size_t)(marker - input),
                              "resets_at", &reset) && reset >= 0) {
            window->resets_at = (time_t)reset; window->has_reset = true;
        }
        if (usage_window_normalize(window)) output->window_count++;
    }
    output->collected_at = time(NULL);
    output->available = output->window_count > 0;
    return output->available;
}
