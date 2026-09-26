#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "format.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>

static bool contains_case_insensitive(const char *text, const char *query) {
    size_t query_length = strlen(query);
    if (!query_length) return true;
    for (const char *position = text; *position; position++)
        if (strncasecmp(position, query, query_length) == 0) return true;
    return false;
}

static bool process_matches(const Process *process, const char *filter) {
    if (!filter[0]) return true;
    char pid[32];
    snprintf(pid, sizeof(pid), "%d", process->pid);
    return contains_case_insensitive(process->name, filter) || strstr(pid, filter) != NULL;
}

static void print_cell(const char *value, int width, bool right_aligned) {
    if (width <= 0) return;
    int length = (int)strlen(value);
    if (length > width) {
        if (width <= 3) print_safe_text(value, width);
        else { print_safe_text(value, width - 3); fputs("...", stdout); }
        return;
    }
    if (right_aligned) print_spaces(width - length);
    print_safe_text(value, -1);
    if (!right_aligned) print_spaces(width - length);
}

void print_process_screen(const ProcessList *processes, ProcessViewState *state,
                          bool clear, bool color) {
    int width = terminal_width();
    int height = terminal_height();
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
    print_view_header("PROCESSES", width, color);
    int header_rows = width < 80 ? 3 : 2;

    if (color) fputs(ANSI_DIM, stdout);
    fputs(ANSI_ERASE_LINE, stdout);
    printf("  %zu processes · SORT %s · FILTER ", processes->count, sort_name(state->sort));
    if (state->filter[0]) print_safe_text(state->filter, -1);
    else fputs("none", stdout);
    printf("%s\n", state->filtering ? "_" : "");
    int help_rows;
    if (width < 70) {
        puts("  SORT c CPU · m MEM · t THREADS");
        puts("       p PID · n NAME · / SEARCH");
        puts("       Enter done · Backspace clear · j/k scroll");
        help_rows = 3;
    } else if (width < 100) {
        puts("  SORT c CPU · m MEM · t THREADS · p PID · n NAME");
        puts("  / SEARCH name/PID · Enter done · Backspace clear · j/k scroll");
        help_rows = 2;
    } else {
        puts("  c CPU · m MEM · t THREADS · p PID · n NAME · / SEARCH · Enter done · Backspace clear · j/k scroll");
        help_rows = 1;
    }
    if (color) fputs(ANSI_RESET, stdout);

    if (processes->status != METRIC_OK) {
        fputs(ANSI_ERASE_LINE, stdout);
        printf("  Process metrics %s\n", metric_status_name(processes->status));
        fputs(ANSI_ERASE_TO_END, stdout);
        fflush(stdout);
        return;
    }

    const int pid_width = 8, cpu_width = 9, mem_width = 11, thread_width = 9;
    const int separators = 10;
    int name_width = width - pid_width - cpu_width - mem_width - thread_width - separators - 1;
    if (name_width < 8) name_width = 8;
    if (color) fputs(ANSI_BOLD, stdout);
    fputs(ANSI_ERASE_LINE, stdout);
    printf("  ");
    print_cell("PID", pid_width, true); printf("  ");
    print_cell("CPU%", cpu_width, true); printf("  ");
    print_cell("MEMORY", mem_width, true); printf("  ");
    print_cell("THREADS", thread_width, true); printf("  ");
    print_cell("PROCESS", name_width, false); putchar('\n');
    if (color) fputs(ANSI_RESET, stdout);

    size_t rendered = 0;
    int fixed_rows = header_rows + 1 + help_rows + 1 + 1;
    size_t max_rows = height > 0 ? (height > fixed_rows + 1 ? (size_t)(height - fixed_rows - 1) : 0) : DEFAULT_LIMIT;
    size_t matched = 0;
    for (size_t i = 0; i < processes->count; i++)
        if (process_matches(&processes->items[i], state->filter)) matched++;
    if (state->offset >= matched)
        state->offset = matched > max_rows ? matched - max_rows : 0;

    size_t seen = 0;
    for (size_t i = 0; i < processes->count; i++) {
        const Process *process = &processes->items[i];
        if (!process_matches(process, state->filter)) continue;
        if (seen++ < state->offset || rendered >= max_rows) continue;
        char pid[32], cpu[32], memory[32], threads[32];
        snprintf(pid, sizeof(pid), "%d", process->pid);
        if (isfinite(process->cpu_percent)) snprintf(cpu, sizeof(cpu), "%.1f%%", process->cpu_percent);
        else snprintf(cpu, sizeof(cpu), "n/a");
        format_compact_bytes(process->resident, memory, sizeof(memory));
        snprintf(threads, sizeof(threads), "%d", process->threads);
        fputs(ANSI_ERASE_LINE, stdout);
        printf("  ");
        print_cell(pid, pid_width, true); printf("  ");
        if (color) fputs(ANSI_BRAND, stdout);
        print_cell(cpu, cpu_width, true);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        if (color) fputs(ANSI_SAND, stdout);
        print_cell(memory, mem_width, true);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  ");
        print_cell(threads, thread_width, true); printf("  ");
        print_cell(process->name, name_width, false); putchar('\n');
        rendered++;
    }

    fputs(ANSI_ERASE_LINE, stdout);
    if (matched == 0) puts("  No matching processes.");
    else if (rendered == 0) puts("  No process rows fit; enlarge the terminal or filter the list.");
    else {
        size_t first = state->offset < matched ? state->offset + 1 : matched;
        size_t last = first + rendered;
        if (last > matched) last = matched;
        if (color) fputs(ANSI_DIM, stdout);
        printf("  Showing %zu-%zu of %zu · use j/k to scroll\n", first, last, matched);
        if (color) fputs(ANSI_RESET, stdout);
    }
    fputs(ANSI_ERASE_TO_END, stdout);
    fflush(stdout);
}
