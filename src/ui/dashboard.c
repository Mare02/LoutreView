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

static void print_core_grid(const double *cores, size_t count, const TerminalLayout *layout,
                            bool color) {
    int columns = layout->dashboard_core_columns;
    const int meter_width = 6;

    putchar('\n');
    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
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

static void print_compact_process_header(bool color) {
    if (color) fputs(ANSI_BOLD, stdout);
    printf("  %-5s  %5s  %6s  %3s  %s", "PID", "CPU%", "MEM", "THR", "PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
}

static void print_compact_process_row(const Process *process, int name_width, bool color) {
    char resident[16];
    const char *process_color = process->cpu_percent >= 70.0 ? ANSI_RED :
                                process->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
    format_compact_bytes(process->resident, resident, sizeof(resident));
    printf("  %-5d  ", process->pid);
    if (color) fputs(process_color, stdout);
    print_percent(process->cpu_percent, 5, 1, false);
    if (color) fputs(ANSI_RESET, stdout);
    printf("  %6s  %3d  ", resident, process->threads);
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
    const int name_width = width - left_width - 29;
    const size_t process_count = snapshot->processes.count < (size_t)process_rows ?
                                 snapshot->processes.count : (size_t)process_rows;

    putchar('\n');
    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
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
            print_compact_process_header(color);
        } else if (row - 1 < process_count) {
            const Process *process = &snapshot->processes.items[row - 1];
            print_compact_process_row(process, name_width, color);
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
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else if (layout->compact_full_metrics) {
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available && layout->compact_battery) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else {
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        putchar(' '); print_percent(cpu, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        putchar(' '); print_percent(memory_percent, 0, 0, true); fputs("  ", stdout);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        putchar(' '); print_percent(disk_percent, 0, 0, true);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    }

    if (metrics.load_status == METRIC_OK) printf("load %.2f · %.2f · %.2f", metrics.loads[0], metrics.loads[1], metrics.loads[2]); else fputs("load n/a", stdout);
    if (layout->compact_uptime) printf("  · up %s", uptime);
    if (layout->compact_pressure) printf("  · pressure %s", pressure);
    putchar('\n');

    bool show_memory = layout->compact_process_memory;
    if (show_memory) print_compact_process_header(color);
    else {
        if (color) fputs(ANSI_BOLD, stdout);
        printf("  %-5s  %5s  %s", "PID", "CPU%", "PROCESS");
        if (color) fputs(ANSI_RESET, stdout);
    }
    putchar('\n');
    if (snapshot->processes.status != METRIC_OK) puts("  Processes n/a");
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    int name_width = layout->width - (show_memory ? 29 : 17);
    if (name_width < 1) name_width = 1;
    for (size_t i = 0; i < count; i++) {
        const Process *process = &snapshot->processes.items[i];
        if (show_memory) {
            print_compact_process_row(process, name_width, color);
        } else {
            const char *process_color = process->cpu_percent >= 70.0 ? ANSI_RED :
                                        process->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
            printf("  %-5d  ", process->pid);
            if (color) fputs(process_color, stdout);
            print_percent(process->cpu_percent, 5, 1, false);
            if (color) fputs(ANSI_RESET, stdout);
            fputs("  ", stdout);
            print_process_name(process->name, name_width);
        }
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

    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("CPU  ", stdout); print_percent(cpu, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cpu, layout->dashboard_bar_width, color);
    if (metrics.load_status == METRIC_OK) printf("  Load %.2f · %.2f · %.2f\n", metrics.loads[0], metrics.loads[1], metrics.loads[2]); else puts("  Load n/a");

    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    fputs("MEM  ", stdout); print_percent(memory_percent, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(memory_percent, layout->dashboard_bar_width, color);
    printf("  %s / %s  %spressure %s%s\n", memory_used_text, memory_total_text,
           color ? ANSI_DIM : "", pressure_text, color ? ANSI_RESET : "");

    if (color) fputs(ANSI_AMBER ANSI_BOLD, stdout);
    fputs("DISK ", stdout); print_percent(disk_percent, 5, 1, true); putchar(' ');
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(disk_percent, layout->dashboard_bar_width, color);
    printf("  %s / %s  %suptime %s%s\n", disk_used_text, disk_total_text,
           color ? ANSI_DIM : "", uptime, color ? ANSI_RESET : "");
    if (metrics.battery.available) {
        if (color) fputs(ANSI_AMBER ANSI_BOLD, stdout);
        printf("BAT  %5d%% ", metrics.battery.percent);
        if (color) fputs(ANSI_RESET, stdout);
        print_bar((double)metrics.battery.percent, layout->dashboard_bar_width, color);
        printf("  %s", metrics.battery.state);
        if (metrics.battery.time_remaining_minutes >= 0) {
            char battery_time[24];
            format_duration_minutes(metrics.battery.time_remaining_minutes, battery_time, sizeof(battery_time));
            printf(" · %s", battery_time);
        }
        putchar('\n');
    }

    size_t process_count = snapshot->processes.count < (size_t)options->limit ?
                           snapshot->processes.count : (size_t)options->limit;
    if (!metrics.battery.available) puts("BAT    n/a");
    if (layout_uses_dashboard_side_panel(layout, core_count, process_count)) {
        print_wide_short_panel(snapshot, core_usage, core_count, layout->width,
                               layout->dashboard_side_core_columns,
                               layout_dashboard_side_process_rows(layout), color);
        fflush(stdout);
        return;
    }

    print_core_grid(core_usage, core_count, layout, color);

    if (color) fputs(ANSI_BOLD, stdout);
    printf("  PID    CPU%%     MEM  THR  PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  %s(sorted by %s)%s\n", color ? ANSI_DIM : "", sort_name(options->sort),
           color ? ANSI_RESET : "");
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  ─────  ─────  ──────  ───  ");
    for (int i = 0; i < layout->width - 33; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
    if (snapshot->processes.status != METRIC_OK) puts("  Processes n/a");
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    for (size_t i = 0; i < count; i++) {
        const Process *p = &snapshot->processes.items[i];
        char resident[24]; format_bytes(p->resident, resident, sizeof(resident));
        const char *process_color = p->cpu_percent >= 70.0 ? ANSI_RED : p->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
        printf("  %-5d  ", p->pid);
        if (color) fputs(process_color, stdout);
        print_percent(p->cpu_percent, 5, 1, false);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  %6s  %3d  ", resident, p->threads); print_safe_text(p->name, 48); putchar('\n');
    }
    fflush(stdout);
}
