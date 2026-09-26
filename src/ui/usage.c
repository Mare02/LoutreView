#define _POSIX_C_SOURCE 200809L
#include "usage.h"
#include "usage_provider.h"
#include "format.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void print_reset(time_t reset) {
    time_t now = time(NULL);
    if (reset <= now) { fputs("now", stdout); return; }
    long seconds = (long)(reset - now);
    if (seconds < 3600) printf("%ldm", (seconds + 59) / 60);
    else if (seconds < 86400) printf("%ldh %ldm", seconds / 3600, (seconds % 3600 + 59) / 60);
    else printf("%ldd %ldh", seconds / 86400, (seconds % 86400) / 3600);
}

static void print_window(const UsageWindow *window, bool color) {
    printf("  %-10s ", window->name);
    if (window->has_percent) {
        if (color) fputs(status_color(window->used_percent, color), stdout);
        printf("%5.1f%% used", window->used_percent);
        if (color) fputs(ANSI_RESET, stdout);
    } else if (window->has_tokens) {
        printf("%llu / %llu tokens", window->used_tokens, window->quota_tokens);
    } else {
        fputs("n/a", stdout);
    }
    if (window->has_reset) {
        fputs("  resets ", stdout);
        print_reset(window->resets_at);
    }
    putchar('\n');
}

void print_usage_screen(bool clear, bool color) {
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
    else fputs(ANSI_HOME ANSI_ERASE_TO_END, stdout);
    print_compact_header("AI USAGE", terminal_width(), color);
    UsageSnapshot snapshot = collect_usage();
    if (snapshot.count == 0) {
        puts("\n  No installed AI CLI providers detected");
        fputs(ANSI_ERASE_TO_END, stdout);
        fflush(stdout);
        return;
    }
    for (size_t i = 0; i < snapshot.count; i++) {
        const ProviderUsage *usage = &snapshot.providers[i];
        if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
        printf("\n%s\n", usage->provider);
        if (color) fputs(ANSI_RESET, stdout);
        if (!usage || !usage->available || usage->window_count == 0) {
            puts("  Usage data unavailable");
            continue;
        }
        for (size_t window = 0; window < usage->window_count; window++)
            print_window(&usage->windows[window], color);
    }
    fputs(ANSI_ERASE_TO_END, stdout);
    fflush(stdout);
}
