#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "usage.h"
#include "usage_provider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void test_normalization(void) {
    UsageWindow window;
    usage_window_init(&window, "window");
    window.used_percent = 125.0;
    window.has_percent = true;
    assert(usage_window_normalize(&window));
    assert(window.used_percent == 100.0 && window.remaining_percent == 0.0);
}

static void test_provider_parsers(void) {
    const char claude[] = "{\"rate_limits\":{\"five_hour\":{\"used_percentage\":23.5,\"resets_at\":2000},\"seven_day\":{\"used_percentage\":41.2,\"resets_at\":3000}}}";
    ProviderUsage usage;
    assert(claude_code_usage_from_statusline(claude, strlen(claude), &usage));
    assert(usage.window_count == 2);
    assert(usage_find_window(&usage, "5-hour")->used_percent == 23.5);
    assert(usage_find_window(&usage, "7-day")->resets_at == 3000);

    const char bridge[] = "{\"windows\":[{\"name\":\"daily\",\"used_percent\":31,\"used_tokens\":31,\"quota_tokens\":100,\"resets_at\":4000}]}";
    assert(codex_usage_parse(bridge, strlen(bridge), &usage));
    assert(!strcmp(usage.provider, "Codex") && usage.window_count == 1);
    assert(usage.windows[0].used_tokens == 31 && usage.windows[0].quota_tokens == 100);
    assert(gemini_cli_usage_parse(bridge, strlen(bridge), &usage));
    assert(!strcmp(usage.provider, "Gemini CLI"));
}

static void test_cache(void) {
    char directory[] = "/tmp/loutre-usage-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/nested/claude.json", directory) > 0);
    ProviderUsage source;
    usage_init(&source, "Claude Code");
    usage_window_init(&source.windows[0], "5-hour");
    source.windows[0].used_percent = 12.5;
    source.windows[0].has_percent = true;
    source.windows[0].resets_at = time(NULL) + 60;
    source.windows[0].has_reset = true;
    source.window_count = 1;
    assert(usage_cache_write(path, &source));
    ProviderUsage loaded;
    assert(usage_cache_read_provider(USAGE_PROVIDER_CLAUDE_CODE, path, &loaded));
    assert(loaded.window_count == 1 && loaded.windows[0].used_percent == 12.5);
    assert(unlink(path) == 0);
    char nested[512];
    assert(snprintf(nested, sizeof(nested), "%s/nested", directory) > 0);
    assert(rmdir(nested) == 0 && rmdir(directory) == 0);
}

int main(void) {
    test_normalization();
    test_provider_parsers();
    test_cache();
    assert(usage_provider_count() == 3);
    puts("Usage tests passed");
    return 0;
}
