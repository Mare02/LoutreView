#include "usage_json.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static bool key_start(const char *p, const char *end, const char *key) {
    size_t n = strlen(key);
    return p + n + 2 <= end && p[0] == '"' && !strncmp(p + 1, key, n) &&
           p[n + 1] == '"';
}

static const char *find_key(const char *input, const char *end, const char *key) {
    for (const char *p = input; p < end; p++)
        if (key_start(p, end, key)) return p;
    return NULL;
}

bool usage_json_number(const char *input, size_t length, const char *key,
                       double *value) {
    if (!input || !key || !value) return false;
    const char *end = input + length;
    const char *p = find_key(input, end, key);
    if (!p) return false;
    p = skip_space(p + strlen(key) + 2, end);
    if (p >= end || *p++ != ':') return false;
    p = skip_space(p, end);
    if (p >= end || *p == 'n' || *p == '"' || *p == '{' || *p == '[') return false;
    char *parsed = NULL;
    errno = 0;
    double number = strtod(p, &parsed);
    if (parsed == p || errno == ERANGE || !isfinite(number)) return false;
    *value = number;
    return true;
}

static bool string_value(const char *input, const char *end, const char *key,
                         char *value, size_t capacity, const char **after) {
    const char *p = find_key(input, end, key);
    if (!p) return false;
    p = skip_space(p + strlen(key) + 2, end);
    if (p >= end || *p++ != ':') return false;
    p = skip_space(p, end);
    if (p >= end || *p++ != '"') return false;
    size_t used = 0;
    while (p < end && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20) return false;
        if (used + 1 >= capacity) return false;
        value[used++] = *p++;
    }
    if (p >= end) return false;
    value[used] = '\0';
    if (after) *after = p + 1;
    return true;
}

static bool number_in_object(const char *object, const char *end, const char *key,
                             double *value) {
    const char *p = find_key(object, end, key);
    if (!p) return false;
    return usage_json_number(p, (size_t)(end - p), key, value);
}

bool usage_parse_bridge_json(const char *input, size_t length, const char *provider,
                             ProviderUsage *output) {
    if (!input || !output || length == 0) return false;
    usage_init(output, provider);
    const char *end = input + length;
    const char *p = input;
    while ((p = find_key(p, end, "name")) != NULL && output->window_count < USAGE_MAX_WINDOWS) {
        const char *object_end = strchr(p, '}');
        if (!object_end || object_end > end) break;
        UsageWindow window;
        usage_window_init(&window, "");
        const char *after = NULL;
        if (!string_value(p, object_end, "name", window.name, sizeof(window.name), &after)) {
            p += 6; continue;
        }
        double number;
        if (number_in_object(after, object_end, "used_percent", &number)) {
            window.used_percent = number; window.has_percent = true;
        }
        if (number_in_object(after, object_end, "used_tokens", &number) && number >= 0) {
            window.used_tokens = (unsigned long long)number; window.has_tokens = true;
        }
        if (number_in_object(after, object_end, "quota_tokens", &number) && number >= 0) {
            window.quota_tokens = (unsigned long long)number; window.has_tokens = true;
        }
        if (number_in_object(after, object_end, "resets_at", &number) && number >= 0) {
            window.resets_at = (time_t)number; window.has_reset = true;
        }
        if (usage_window_normalize(&window)) output->windows[output->window_count++] = window;
        p = object_end + 1;
    }
    output->available = output->window_count > 0;
    return output->available;
}
