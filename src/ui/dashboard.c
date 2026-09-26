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

enum { PROCESS_MEMORY_WIDTH = 9 };

static size_t dashboard_process_count(const ProcessList *processes, const Options *options,
                                      size_t row_limit) {
    size_t limit = row_limit;
    if (options->limit_explicit && limit > (size_t)options->limit)
        limit = (size_t)options->limit;
    return processes->count < limit ? processes->count : limit;
}

static size_t dashboard_process_limit(const ProcessList *processes, const Options *options,
                                      int height, int row_limit) {
    if (height <= 0) return dashboard_process_count(processes, options, (size_t)options->limit);
    return dashboard_process_count(processes, options,
                                   row_limit > 0 ? (size_t)row_limit : 0);
}

static int dashboard_side_process_rows(const TerminalLayout *layout) {
    int header_rows = layout->width < 80 ? 3 : 2;
    int rows = layout->height - header_rows - 8;
    return rows > 0 ? rows : 0;
}

static void print_core_grid(const double *cores, size_t count, const TerminalLayout *layout,
                            bool color) {
    int columns = layout->dashboard_core_columns;
    const int meter_width = 6;

    putchar('\n');
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    fputs("CPU CORES", stdout);
    if (count == 0) fputs("  n/a", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    for (size_t start = 0; start < count; start += (size_t)columns) {
        for (int column = 0; column < columns && start + (size_t)column < count; column++) {
            size_t index = start + (size_t)column;
            if (color) fputs(ANSI_SLATE, stdout);
            printf("  CORE %02zu ", index + 1);
            if (color) fputs(ANSI_RESET, stdout);
            print_bar(cores[index], meter_width, color);
            putchar(' '); print_percent(cores[index], 4, 1, true);
            if (column + 1 < columns && start + (size_t)column + 1 < count) fputs("    ", stdout);
        }
        putchar('\n');
    }
    putchar('\n');
}

static void print_process_name(const char *name, int width) {
    size_t length = strlen(name);
    if (width <= 0) return;
    if (length <= (size_t)width) {
        print_safe_text(name, -1);
    } else if (width <= 3) {
        print_safe_text(name, width);
    } else {
        print_safe_text(name, width - 3); fputs("...", stdout);
    }
}

static void print_process_cpu(double cpu_percent) {
    char value[32];
    if (isfinite(cpu_percent)) snprintf(value, sizeof(value), "%.1f", cpu_percent);
    else snprintf(value, sizeof(value), "n/a");
    printf("%-5s", value);
}

static void print_compact_process_header(bool show_memory, bool color) {
    if (color) fputs(ANSI_SLATE ANSI_BOLD, stdout);
    printf("  %-5s", "PID");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    printf("%-5s", "CPU%");
    if (color) fputs(ANSI_RESET, stdout);
    if (show_memory) {
        printf("  ");
        if (color) fputs(ANSI_SAND ANSI_BOLD, stdout);
        printf("%-*s", PROCESS_MEMORY_WIDTH, "MEM");
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_CLAY ANSI_BOLD, stdout);
        printf("%-3s", "THR");
        if (color) fputs(ANSI_RESET, stdout);
    }
    printf("  ");
    if (color) fputs(ANSI_BOLD, stdout);
    fputs("PROCESS", stdout);
    if (color) fputs(ANSI_RESET, stdout);
}

static void print_compact_process_row(const Process *process, int name_width,
                                      bool show_memory, bool color) {
    char resident[16];
    format_compact_bytes(process->resident, resident, sizeof(resident));
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  %-5d", process->pid);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_BRAND, stdout);
    print_process_cpu(process->cpu_percent);
    if (color) fputs(ANSI_RESET, stdout);
    if (show_memory) {
        printf("  ");
        if (color) fputs(ANSI_SAND, stdout);
        printf("%-*s", PROCESS_MEMORY_WIDTH, resident);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_CLAY, stdout);
        printf("%-3d", process->threads);
        if (color) fputs(ANSI_RESET, stdout);
    }
    printf("  ");
    print_process_name(process->name, name_width);
}

static void print_wide_core_cell(const double *cores, size_t index, size_t count, bool color) {
    if (index >= count) return;
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  CORE %02zu ", index + 1);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cores[index], 6, color);
    putchar(' '); print_percent(cores[index], 4, 1, true);
}

static void print_wide_short_panel(const Snapshot *snapshot, const double *cores, size_t core_count,
                                   int width, int core_columns, int process_rows, bool color) {
    const int core_panel_width = core_columns == 1 ? 22 : 48;
    const int left_width = core_columns == 1 ? 26 : 50;
    const size_t core_rows = (core_count + (size_t)core_columns - 1) / (size_t)core_columns;
    const int name_width = width - left_width - 32;
    const size_t process_count = snapshot->processes.count < (size_t)process_rows ?
                                 snapshot->processes.count : (size_t)process_rows;

    putchar('\n');
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    fputs("CPU CORES", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    print_spaces(left_width - 9);
    if (color) fputs(ANSI_BOLD, stdout);
    fputs("TOP PROCESSES", stdout);
    if (snapshot->processes.status != METRIC_OK) fputs("  n/a", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    size_t rows = core_rows > process_count + 1 ? core_rows : process_count + 1;
    for (size_t row = 0; row < rows; row++) {
        for (int column = 0; column < core_columns; column++) {
            size_t index = row * (size_t)core_columns + (size_t)column;
            if (index < core_count) print_wide_core_cell(cores, index, core_count, color);
            else print_spaces(22);
            if (column + 1 < core_columns) print_spaces(4);
        }
        print_spaces(left_width - core_panel_width);

        if (row == 0) {
            print_compact_process_header(true, color);
        } else if (row - 1 < process_count) {
            const Process *process = &snapshot->processes.items[row - 1];
            print_compact_process_row(process, name_width, true, color);
        }
        putchar('\n');
    }
}

static void print_compact_metric_label(const char *label, const char *accent, bool color) {
    if (color) {
        fputs(accent, stdout);
        fputs(ANSI_BOLD, stdout);
    }
    fputs(label, stdout);
    if (color) fputs(ANSI_RESET, stdout);
}

void render_compact_dashboard(const Snapshot *snapshot, double cpu, const Options *options,
                              const TerminalLayout *layout, bool clear, bool color) {
    SystemMetrics metrics = collect_system_metrics();
    char memory_used[16], memory_total[16], disk_used[16], disk_total[16], pressure[16], uptime[32];
    format_compact_bytes(metrics.memory_used, memory_used, sizeof(memory_used));
    format_compact_bytes(metrics.memory_total, memory_total, sizeof(memory_total));
    format_compact_bytes(metrics.disk_used, disk_used, sizeof(disk_used));
    format_compact_bytes(metrics.disk_total, disk_total, sizeof(disk_total));
    format_pressure(&metrics, pressure, sizeof(pressure), true);
    format_uptime(metrics.uptime, uptime, sizeof(uptime));
    if (metrics.memory_status != METRIC_OK) {
        snprintf(memory_used, sizeof(memory_used), "n/a");
        snprintf(memory_total, sizeof(memory_total), "n/a");
    }
    if (metrics.disk_status != METRIC_OK) {
        snprintf(disk_used, sizeof(disk_used), "n/a");
        snprintf(disk_total, sizeof(disk_total), "n/a");
    }

    double memory_percent = metrics.memory_status == METRIC_OK && metrics.memory_total ? 100.0 * (double)metrics.memory_used / metrics.memory_total : NAN;
    double disk_percent = metrics.disk_status == METRIC_OK && metrics.disk_total ? 100.0 * (double)metrics.disk_used / metrics.disk_total : NAN;
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);

    print_compact_header("DASHBOARD", layout->width, color);

    if (layout->compact_pressure) {
        print_compact_metric_label("CPU", ANSI_BRAND, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_BRAND, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_BRAND, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_BRAND, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else if (layout->compact_full_metrics) {
        print_compact_metric_label("CPU", ANSI_BRAND, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_BRAND, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_BRAND, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available && layout->compact_battery) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_BRAND, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else {
        print_compact_metric_label("CPU", ANSI_BRAND, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_BRAND, color);
        putchar(' '); print_percent(memory_percent, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("DISK", ANSI_BRAND, color);
        putchar(' '); print_percent(disk_percent, 0, 0, true);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_BRAND, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    }

    if (metrics.load_status == METRIC_OK) printf("load %.2f · %.2f · %.2f", metrics.loads[0], metrics.loads[1], metrics.loads[2]); else fputs("load n/a", stdout);
    if (layout->compact_uptime) printf("  · up %s", uptime);
    if (layout->compact_pressure) printf("  · pressure %s", pressure);
    putchar('\n');

    bool show_memory = layout->compact_process_memory;
    print_compact_process_header(show_memory, color);
    putchar('\n');
    if (snapshot->processes.status != METRIC_OK) puts("  Processes n/a");
    int compact_header_rows = layout->width < 84 ? 2 : 1;
    int compact_fixed_rows = compact_header_rows + 3;
    int compact_process_rows = layout->height - compact_fixed_rows - 1;
    size_t count = dashboard_process_limit(&snapshot->processes, options, layout->height,
                                           compact_process_rows);
    int name_width = layout->width - (show_memory ? 32 : 17);
    if (name_width < 1) name_width = 1;
    for (size_t i = 0; i < count; i++) {
        const Process *process = &snapshot->processes.items[i];
        print_compact_process_row(process, name_width, show_memory, color);
        putchar('\n');
    }
    fflush(stdout);
}

void render_dashboard(const Snapshot *snapshot, double cpu, const Options *options,
                      const double *core_usage, size_t core_count,
                      const TerminalLayout *layout, bool clear, bool color) {
    SystemMetrics metrics = collect_system_metrics();
    char memory_used_text[24], memory_total_text[24], pressure_text[24], disk_used_text[24], disk_total_text[24], uptime[32];
    format_bytes(metrics.memory_used, memory_used_text, sizeof(memory_used_text));
    format_bytes(metrics.memory_total, memory_total_text, sizeof(memory_total_text));
    format_pressure(&metrics, pressure_text, sizeof(pressure_text), false);
    format_bytes(metrics.disk_used, disk_used_text, sizeof(disk_used_text));
    format_bytes(metrics.disk_total, disk_total_text, sizeof(disk_total_text));
    format_uptime(metrics.uptime, uptime, sizeof(uptime));
    if (metrics.memory_status != METRIC_OK) {
        snprintf(memory_used_text, sizeof(memory_used_text), "n/a");
        snprintf(memory_total_text, sizeof(memory_total_text), "n/a");
    }
    if (metrics.disk_status != METRIC_OK) {
        snprintf(disk_used_text, sizeof(disk_used_text), "n/a");
        snprintf(disk_total_text, sizeof(disk_total_text), "n/a");
    }

    double memory_percent = metrics.memory_status == METRIC_OK && metrics.memory_total ? 100.0 * (double)metrics.memory_used / metrics.memory_total : NAN;
    double disk_percent = metrics.disk_status == METRIC_OK && metrics.disk_total ? 100.0 * (double)metrics.disk_used / metrics.disk_total : NAN;
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);

    print_view_header("DASHBOARD", layout->width, color);

    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    fputs("CPU  ", stdout); print_percent(cpu, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cpu, layout->dashboard_bar_width, color);
    if (metrics.load_status == METRIC_OK) printf("  Load %.2f · %.2f · %.2f\n", metrics.loads[0], metrics.loads[1], metrics.loads[2]); else puts("  Load n/a");

    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    fputs("MEM  ", stdout); print_percent(memory_percent, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(memory_percent, layout->dashboard_bar_width, color);
    printf("  %s / %s  %spressure %s%s\n", memory_used_text, memory_total_text,
           color ? ANSI_DIM : "", pressure_text, color ? ANSI_RESET : "");

    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    fputs("DISK ", stdout); print_percent(disk_percent, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(disk_percent, layout->dashboard_bar_width, color);
    printf("  %s / %s  %suptime %s%s\n", disk_used_text, disk_total_text,
           color ? ANSI_DIM : "", uptime, color ? ANSI_RESET : "");
    if (metrics.battery.available) {
        if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
        printf("BAT  %5d%% ", metrics.battery.percent);
        if (color) fputs(ANSI_RESET, stdout);
        print_battery_bar((double)metrics.battery.percent, layout->dashboard_bar_width, color);
        printf("  %s", metrics.battery.state);
        if (metrics.battery.time_remaining_minutes >= 0) {
            char battery_time[24];
            format_duration_minutes(metrics.battery.time_remaining_minutes, battery_time, sizeof(battery_time));
            printf(" · %s", battery_time);
        }
        putchar('\n');
    }

    size_t side_panel_process_count = options->limit_explicit ?
        dashboard_process_count(&snapshot->processes, options, (size_t)options->limit) : 0;
    size_t process_count;
    if (!metrics.battery.available) puts("BAT    n/a");
    if (layout_uses_dashboard_side_panel(layout, core_count, side_panel_process_count)) {
        int process_rows = dashboard_side_process_rows(layout);
        if (options->limit_explicit && process_rows > options->limit)
            process_rows = options->limit;
        print_wide_short_panel(snapshot, core_usage, core_count, layout->width,
                               layout->dashboard_side_core_columns,
                               process_rows, color);
        fflush(stdout);
        return;
    }

    int header_rows = layout->width < 80 ? 3 : 2;
    size_t standard_core_rows = (core_count + (size_t)layout->dashboard_core_columns - 1) /
                                (size_t)layout->dashboard_core_columns;
    int fixed_rows = header_rows + 4 + (int)standard_core_rows + 5;
    int process_rows = layout->height - fixed_rows - 1;
    process_count = dashboard_process_limit(&snapshot->processes, options, layout->height,
                                            process_rows);

    print_core_grid(core_usage, core_count, layout, color);

    if (color) fputs(ANSI_SLATE ANSI_BOLD, stdout);
    printf("%-5s", "PID");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_BRAND ANSI_BOLD, stdout);
    printf("%-5s", "CPU%");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_SAND ANSI_BOLD, stdout);
    printf("%-*s", PROCESS_MEMORY_WIDTH, "MEM");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_CLAY ANSI_BOLD, stdout);
    printf("%-3s", "THR");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_BOLD, stdout);
    fputs("PROCESS", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  %s(sorted by %s)%s\n", color ? ANSI_DIM : "", sort_name(options->sort),
           color ? ANSI_RESET : "");
    if (color) fputs(ANSI_SLATE, stdout);
    fputs("─────", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_BRAND, stdout);
    fputs("─────", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_SAND, stdout);
    for (int i = 0; i < PROCESS_MEMORY_WIDTH; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_CLAY, stdout);
    fputs("───", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  ");
    if (color) fputs(ANSI_TAUPE, stdout);
    for (int i = 0; i < layout->width - (PROCESS_MEMORY_WIDTH + 25); i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
    if (snapshot->processes.status != METRIC_OK) puts("Processes n/a");
    size_t count = process_count;
    for (size_t i = 0; i < count; i++) {
        const Process *p = &snapshot->processes.items[i];
        char resident[24]; format_bytes(p->resident, resident, sizeof(resident));
        if (color) fputs(ANSI_SLATE, stdout);
        printf("%-5d", p->pid);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_BRAND, stdout);
        print_process_cpu(p->cpu_percent);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_SAND, stdout);
        printf("%-*s", PROCESS_MEMORY_WIDTH, resident);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_CLAY, stdout);
        printf("%-3d", p->threads);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        print_safe_text(p->name, 48);
        putchar('\n');
    }
    fflush(stdout);
}
