#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "format.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

static void print_mount_rows(const DiskMountList *disks, int width, int height,
                             bool color, bool compact) {
    int fixed = view_header_rows("DISKS", width, compact) + 3;
    size_t max_rows = height > fixed ? (size_t)(height - fixed) : 0;
    size_t count = disks->count < max_rows ? disks->count : max_rows;
    int path_width = width - (compact ? 43 : 60);
    if (path_width < 12) path_width = 12;
    if (path_width > 52) path_width = 52;

    if (color) fputs(ANSI_BOLD, stdout);
    fputs("  ", stdout);
    printf("%-*s", path_width, "MOUNT POINT");
    if (compact) {
        printf("  %11s  %10s  %6s\n", "USED/TOTAL", "AVAILABLE", "USE");
    } else {
        printf("  %11s  %11s  %11s  %6s\n", "TOTAL", "USED", "AVAILABLE", "USE");
    }
    if (color) fputs(ANSI_RESET ANSI_SLATE, stdout);
    fputs("  ─────────────────────────────────────────────────────────────────────────\n", stdout);
    if (color) fputs(ANSI_RESET, stdout);

    for (size_t i = 0; i < count; i++) {
        const DiskMount *mount = &disks->items[i];
        char total[16], used[16], available[16];
        format_compact_bytes(mount->total_bytes, total, sizeof(total));
        format_compact_bytes(mount->used_bytes, used, sizeof(used));
        format_compact_bytes(mount->available_bytes, available, sizeof(available));
        fputs("  ", stdout);
        print_safe_text(mount->mount_point, path_width);
        int path_length = (int)strlen(mount->mount_point);
        print_spaces(path_length < path_width ? path_width - path_length : 0);
        if (compact) printf("  %5s/%-5s  %10s  %5.1f%%\n", used, total, available,
                            mount->usage_percent);
        else printf("  %11s  %11s  %11s  %5.1f%%\n", total, used, available,
                    mount->usage_percent);
    }
    if (count < disks->count)
        printf("  + %zu more mount%s below terminal height\n", disks->count - count,
               disks->count - count == 1 ? "" : "s");
    if (disks->count == 0) puts("  No mounted filesystems available.");
    if (disks->truncated) puts("  Mount list is truncated at the supported limit.");
    else if (disks->partial) puts("  Some mounted filesystems could not be read.");
}

void print_disk_screen(const DiskMountList *disks, bool clear, bool color, bool compact) {
    int width = terminal_width();
    int height = terminal_height();
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
    if (compact) print_compact_header("DISKS", width, color);
    else print_view_header("DISKS", width, color);
    if (disks->status != METRIC_OK) {
        printf("  Disk metrics %s\n", metric_status_name(disks->status));
        fflush(stdout);
        return;
    }
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    printf("  %zu mounted filesystem%s%s\n", disks->count,
           disks->count == 1 ? "" : "s", disks->partial ? " · partial inventory" : "");
    if (color) fputs(ANSI_RESET, stdout);
    print_mount_rows(disks, width, height, color, compact);
    fflush(stdout);
}
