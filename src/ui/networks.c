#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "format.h"
#include "ui.h"
#include "sampler.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>

static void safe_interface_names(NetworkSnapshot *snapshot) {
    for (size_t i = 0; i < snapshot->count; i++) {
        unsigned char *p = (unsigned char *)snapshot->items[i].name;
        for (; *p; p++) if (*p < 0x20 || *p >= 0x7f) *p = '?';
    }
}

static int compare_network_usage(const void *left_value, const void *right_value) {
    const NetworkInterface *left = left_value;
    const NetworkInterface *right = right_value;
    if (left->up != right->up) return right->up - left->up;

    double left_rate = left->receive_rate + left->transmit_rate;
    double right_rate = right->receive_rate + right->transmit_rate;
    if (isfinite(left_rate) != isfinite(right_rate)) return isfinite(left_rate) ? -1 : 1;
    if (!isfinite(left_rate)) return strcmp(left->name, right->name);
    if (left_rate != right_rate) return right_rate > left_rate ? 1 : -1;
    return strcmp(left->name, right->name);
}

void print_network_screen(const NetworkSnapshot *snapshot, const Options *options,
                                 bool clear, bool color) {
    if (snapshot->status != METRIC_OK) {
        if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
        print_compact_header("NETWORKS", terminal_width(), color);
        puts("  Network metrics n/a");
        return;
    }
    int width = terminal_width();
    int height = terminal_height();
    NetworkSnapshot ordered = *snapshot;
    safe_interface_names(&ordered);
    qsort(ordered.items, ordered.count, sizeof(ordered.items[0]), compare_network_usage);
    size_t active_count = 0;
    double receive_rate = 0.0;
    double transmit_rate = 0.0;
    for (size_t i = 0; i < snapshot->count; i++) {
        if (snapshot->items[i].up) active_count++;
        receive_rate += snapshot->items[i].receive_rate;
        transmit_rate += snapshot->items[i].transmit_rate;
    }
    char receive_total[16], transmit_total[16];
    format_rate(receive_rate, receive_total, sizeof(receive_total));
    format_rate(transmit_rate, transmit_total, sizeof(transmit_total));
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
    print_view_header("NETWORKS", options->interval_ms, width, color);

    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    printf("  TRAFFIC  %zu active · %zu total    ↓ RX %s/s    ↑ TX %s/s\n",
           active_count, snapshot->count, receive_total, transmit_total);
    if (color) fputs(ANSI_RESET ANSI_SLATE, stdout);
    fputs("  ─────────────────────────────────────────────────────────────────────────\n", stdout);
    if (color) fputs(ANSI_RESET, stdout);

    size_t max_rows = snapshot->count;
    if (height > 6 && max_rows > (size_t)(height - 6)) max_rows = (size_t)(height - 6);
    size_t rendered_count = 0;
    for (int show_active = 1; show_active >= 0 && rendered_count < max_rows; show_active--) {
        for (size_t i = 0; i < ordered.count && rendered_count < max_rows; i++) {
            const NetworkInterface *network = &ordered.items[i];
            if ((network->up ? 1 : 0) != show_active) continue;
        char receive_rate[16], transmit_rate[16], received[16], transmitted[16];
        format_rate(network->receive_rate, receive_rate, sizeof(receive_rate));
        format_rate(network->transmit_rate, transmit_rate, sizeof(transmit_rate));
        format_compact_bytes(network->received, received, sizeof(received));
        format_compact_bytes(network->transmitted, transmitted, sizeof(transmitted));
        const char *status_color = color ? (network->up ? ANSI_GREEN : ANSI_RED) : "";
        const char *muted = color ? ANSI_DIM : "";
        if (width >= 78) {
            printf("  %s●%s %-10s  %s%-4s%s  %s↓%7s/s  ↑%7s/s%s   in %-8s out %-8s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", muted,
                   receive_rate, transmit_rate, color ? ANSI_RESET : "", received, transmitted);
        } else {
            printf("  %s●%s %-8s %s%-4s%s  ↓%s/s ↑%s/s  in%s out%s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate,
                   transmit_rate, received, transmitted);
        }
            rendered_count++;
        }
    }
    if (rendered_count < ordered.count) {
        if (color) fputs(ANSI_DIM, stdout);
        printf("  + %zu more interface%s below terminal height\n",
               ordered.count - rendered_count, ordered.count - rendered_count == 1 ? "" : "s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    if (snapshot->count == 0) puts("  No network interfaces available.");
    if (snapshot->truncated) puts("  Interface list is partial.");
    fflush(stdout);
}

void print_compact_network_screen(const NetworkSnapshot *snapshot, bool clear, bool color) {
    if (snapshot->status != METRIC_OK) {
        if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
        print_compact_header("NETWORKS", terminal_width(), color);
        puts("  Network metrics n/a");
        return;
    }
    int width = terminal_width();
    int height = terminal_height();
    NetworkSnapshot ordered = *snapshot;
    safe_interface_names(&ordered);
    qsort(ordered.items, ordered.count, sizeof(ordered.items[0]), compare_network_usage);
    size_t active_count = 0;
    double receive_rate = 0.0;
    double transmit_rate = 0.0;
    for (size_t i = 0; i < ordered.count; i++) {
        if (ordered.items[i].up) active_count++;
        receive_rate += ordered.items[i].receive_rate;
        transmit_rate += ordered.items[i].transmit_rate;
    }
    char receive_total[16], transmit_total[16];
    format_rate(receive_rate, receive_total, sizeof(receive_total));
    format_rate(transmit_rate, transmit_total, sizeof(transmit_total));
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);

    print_compact_header("NETWORKS", width, color);
    if (color) fputs(ANSI_DIM, stdout);
    printf("  %zu/%zu up  ↓%s/s ↑%s/s\n", active_count, ordered.count,
           receive_total, transmit_total);
    if (color) fputs(ANSI_RESET, stdout);
    if (color) fputs(ANSI_SLATE, stdout);
    fputs("  ─────────────────────────────────────────────────────────────────────────\n", stdout);
    if (color) fputs(ANSI_RESET, stdout);

    size_t max_rows = ordered.count;
    if (height > 3 && max_rows > (size_t)(height - 3)) max_rows = (size_t)(height - 3);
    size_t rendered_count = 0;
    for (size_t i = 0; i < ordered.count && rendered_count < max_rows; i++) {
        const NetworkInterface *network = &ordered.items[i];
        char receive_rate_text[16], transmit_rate_text[16], received[16], transmitted[16];
        format_rate(network->receive_rate, receive_rate_text,
                             sizeof(receive_rate_text));
        format_rate(network->transmit_rate, transmit_rate_text,
                             sizeof(transmit_rate_text));
        format_compact_bytes(network->received, received, sizeof(received));
        format_compact_bytes(network->transmitted, transmitted, sizeof(transmitted));
        const char *status_color = color ? (network->up ? ANSI_GREEN : ANSI_RED) : "";
        if (width >= 72) {
            printf("  %s●%s %-8s %s%-4s%s ↓%7s/s ↑%7s/s  in %-7s out %-7s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate_text,
                   transmit_rate_text, received, transmitted);
        } else {
            printf("  %s●%s %-8s %s%-4s%s ↓%s/s ↑%s/s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate_text,
                   transmit_rate_text);
        }
        rendered_count++;
    }
    if (rendered_count < ordered.count) {
        if (color) fputs(ANSI_DIM, stdout);
        printf("  + %zu more interface%s\n", ordered.count - rendered_count,
               ordered.count - rendered_count == 1 ? "" : "s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    if (snapshot->truncated) puts("  Interface list is partial.");
    fflush(stdout);
}
