#include "format.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_container_cpu(const void *left_value, const void *right_value) {
    const DockerContainer *left = left_value;
    const DockerContainer *right = right_value;
    bool left_ok = left->stats_status == METRIC_OK && isfinite(left->cpu_percent);
    bool right_ok = right->stats_status == METRIC_OK && isfinite(right->cpu_percent);
    if (left_ok != right_ok) return left_ok ? -1 : 1;
    if (left_ok && left->cpu_percent != right->cpu_percent)
        return left->cpu_percent > right->cpu_percent ? -1 : 1;
    return strcmp(left->name, right->name);
}

static int display_width(const char *text) {
    int columns = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; columns++) {
        size_t length = text_utf8_width(p);
        p += length ? length : 1;
    }
    return columns;
}

static void print_field(const char *value, int width) {
    if (width <= 0) return;
    print_safe_text(value, width);
    int used = display_width(value);
    if (used < width) print_spaces(width - used);
}

static void format_memory(const DockerContainer *container, char *out, size_t capacity) {
    if (container->stats_status != METRIC_OK) {
        snprintf(out, capacity, "n/a");
        return;
    }
    char used[16], limit[16];
    format_compact_bytes(container->memory_used, used, sizeof(used));
    format_compact_bytes(container->memory_limit, limit, sizeof(limit));
    snprintf(out, capacity, "%s/%s", used, limit);
}

static void print_unavailable(const DockerSnapshot *snapshot) {
    if (snapshot->message[0]) {
        printf("  %s\n", snapshot->message);
    } else if (snapshot->status == METRIC_PERMISSION) {
        puts("  Docker socket access denied.");
    } else if (snapshot->status == METRIC_ERROR) {
        puts("  Could not read a valid response from the Docker engine.");
    } else {
        puts("  Docker engine unavailable (local socket not found or daemon stopped).");
    }
}

void print_docker_screen(const DockerSnapshot *snapshot, bool clear, bool color) {
    int width = terminal_width();
    int height = terminal_height();
    if (clear) fputs(ANSI_CLEAR_SCREEN, stdout);
    print_view_header("DOCKER", width, color);
    if (!snapshot || snapshot->status != METRIC_OK) {
        DockerSnapshot unavailable = { .status = METRIC_UNAVAILABLE };
        print_unavailable(snapshot ? snapshot : &unavailable);
        return;
    }
    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    printf("  %zu running container%s\n", snapshot->count, snapshot->count == 1 ? "" : "s");
    if (color) fputs(ANSI_RESET, stdout);
    if (snapshot->count == 0) {
        puts("  No running containers.");
        return;
    }

    DockerContainer ordered[MAX_DOCKER_CONTAINERS];
    size_t count = snapshot->count;
    if (count > MAX_DOCKER_CONTAINERS) count = MAX_DOCKER_CONTAINERS;
    memcpy(ordered, snapshot->items, count * sizeof(ordered[0]));
    qsort(ordered, count, sizeof(ordered[0]), compare_container_cpu);
    bool wide = width >= 108;
    if (wide) {
        if (color) fputs(ANSI_DIM, stdout);
        puts("  NAME                 IMAGE                         CPU       MEM USED/LIMIT       NET RX/s   NET TX/s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    size_t row_limit = height > (wide ? 5 : 6) ? (size_t)(height - (wide ? 5 : 6)) : count;
    if (!wide) row_limit /= 2;
    if (row_limit > count) row_limit = count;
    for (size_t i = 0; i < row_limit; i++) {
        DockerContainer *container = &ordered[i];
        char memory[40], receive[16], transmit[16];
        format_memory(container, memory, sizeof(memory));
        format_rate(container->stats_status == METRIC_OK ? container->receive_rate : NAN,
                    receive, sizeof(receive));
        format_rate(container->stats_status == METRIC_OK ? container->transmit_rate : NAN,
                    transmit, sizeof(transmit));
        if (wide) {
            fputs("  ", stdout); print_field(container->name, 20); putchar(' ');
            print_field(container->image, 28); putchar(' ');
            if (container->stats_status == METRIC_OK) print_percent(container->cpu_percent, 6, 1, true);
            else printf("%7s", "n/a");
            putchar(' '); print_field(memory, 20);
            printf(" %9s %9s\n", receive, transmit);
        } else {
            fputs("  ", stdout); print_safe_text(container->name, width > 4 ? width - 4 : 1);
            fputs("  ", stdout); print_safe_text(container->image, width > 4 ? width - 4 : 1);
            putchar('\n');
            if (container->stats_status == METRIC_OK) {
                printf("    CPU "); print_percent(container->cpu_percent, 0, 1, true);
            } else fputs("    CPU n/a", stdout);
            printf("  MEM %s  ↓%s/s ↑%s/s\n", memory, receive, transmit);
        }
    }
    if (row_limit < count) {
        if (color) fputs(ANSI_DIM, stdout);
        printf("  + %zu more container%s below terminal height\n", count - row_limit,
               count - row_limit == 1 ? "" : "s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    if (snapshot->truncated) {
        if (color) fputs(ANSI_DIM, stdout);
        puts("  Container list truncated at 128 entries.");
        if (color) fputs(ANSI_RESET, stdout);
    }
}
