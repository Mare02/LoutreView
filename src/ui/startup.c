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

static void print_startup_clamped(const char *value, int width) {
    if (width <= 0) return;
    size_t length = strlen(value);
    if ((int)length <= width) {
        print_safe_text(value, -1);
    } else if (width <= 3) {
        print_safe_text(value, width);
    } else {
        print_safe_text(value, width - 3);
        fputs("...", stdout);
    }
}

static void print_startup_cell(const char *value, int width) {
    if (width <= 0) return;
    print_startup_clamped(value, width);
    int padding = width - (int)strlen(value);
    if (padding < 0) padding = 0;
    if ((int)strlen(value) > width && width > 3) padding = 0;
    print_spaces(padding);
}

void print_startup_report(const Options *options) {
    StartupList list = platform_startup();
    if (options->json) {
        print_startup_json(&list);
        free(list.items);
        return;
    }

    bool color = isatty(STDOUT_FILENO);
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    printf("LOUTREVIEW  /  STARTUP\n");
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    printf("%zu configured items · %s%s\n\n", list.count, metric_status_name(list.status), list.partial ? " (partial)" : "");
    int width = terminal_width();
    int name_width = width >= 120 ? 28 : width >= 90 ? 22 : 16;
    int type_width = 8;
    int status_width = 12;
    int memory_width = 8;
    int cpu_width = 7;
    int start_width = 17;
    int owner_width = width >= 90 ? 8 : 7;

    if (color) fputs(ANSI_BOLD, stdout);
    fputs("  ", stdout);
    print_startup_cell("NAME", name_width);
    fputs("  ", stdout);
    print_startup_cell("TYPE", type_width);
    fputs("  ", stdout);
    print_startup_cell("STATUS", status_width);
    fputs("  ", stdout);
    print_startup_cell("MEM", memory_width);
    fputs("  ", stdout);
    print_startup_cell("CPU", cpu_width);
    fputs("  ", stdout);
    print_startup_cell("LAST START", start_width);
    fputs("  ", stdout);
    print_startup_cell("OWNER", owner_width);
    fputs(" PATH\n", stdout);
    putchar('\n');
    if (color) fputs(ANSI_RESET, stdout);
    for (size_t i = 0; i < list.count; i++) {
        const StartupItem *item = &list.items[i];
        char memory[16];
        if (item->memory_known) format_bytes(item->resident, memory, sizeof(memory)); else snprintf(memory, sizeof(memory), "n/a");
        const char *status = item->state[0] ? item->state : item->running_known ? (item->running ? "running" : "inactive") : "n/a";
        const char *status_style = item->path_missing ? ANSI_RED : !item->running_known ? ANSI_SLATE : item->running ? ANSI_GREEN : ANSI_AMBER;
        const char *path = item->path[0] ? item->path : "-";
        char start[32];
        format_start_time(item->start_time, start, sizeof(start));
        printf("  ");
        print_startup_cell(item->name, name_width);
        fputs("  ", stdout);
        print_startup_cell(startup_kind_short_name(item->kind), type_width);
        fputs("  ", stdout);
        if (color) fputs(status_style, stdout);
        print_startup_cell(status, status_width);
        if (color) fputs(ANSI_RESET, stdout);
        fputs("  ", stdout);
        print_startup_cell(memory, memory_width);
        fputs("  ", stdout);
        char cpu[16];
        if (isfinite(item->cpu_percent)) snprintf(cpu, sizeof(cpu), "%.1f%%", item->cpu_percent); else snprintf(cpu, sizeof(cpu), "n/a");
        print_startup_cell(cpu, cpu_width);
        fputs("  ", stdout);
        print_startup_cell(start, start_width);
        fputs("  ", stdout);
        print_startup_cell(item->owner, owner_width);
        fputs(" ", stdout);
        print_safe_text(path, -1);
        putchar('\n');
        putchar('\n');
    }
    free(list.items);
}
