#include "ui.h"
#include "format.h"
#include "sampler.h"
#include <math.h>
#include <stdio.h>

void json_string(const char *value) {
    putchar('"');
    if (value) for (const unsigned char *p = (const unsigned char *)value; *p;) {
        size_t width = text_utf8_width(p);
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p++); }
        else if (*p < 0x20 || !width) { printf("\\u%04x", *p++); }
        else { fwrite(p, 1, width, stdout); p += width; }
    }
    putchar('"');
}

static void number(double value, int precision) {
    if (isfinite(value)) printf("%.*f", precision, value);
    else fputs("null", stdout);
}

static void bytes(unsigned long long value, bool available) {
    if (available) printf("%llu", value); else fputs("null", stdout);
}

void print_json(const Snapshot *snapshot, double cpu, const Options *options) {
    SystemMetrics metrics = collect_system_metrics();
    fputs("{\"cpu\":{\"usage_percent\":", stdout);
    number(snapshot->cpu_status == METRIC_OK ? cpu : NAN, 1);
    fputs(",\"load\":", stdout);
    if (metrics.load_status == METRIC_OK) {
        putchar('[');
        for (int i = 0; i < 3; i++) { if (i) putchar(','); number(metrics.loads[i], 2); }
        putchar(']');
    } else fputs("null", stdout);
    fputs("},\"memory\":{\"total_bytes\":", stdout);
    bytes(metrics.memory_total, metrics.memory_status == METRIC_OK);
    fputs(",\"used_bytes\":", stdout); bytes(metrics.memory_used, metrics.memory_status == METRIC_OK);
    fputs(",\"pressure_bytes\":", stdout); bytes(metrics.pressure, metrics.pressure_available);
    fputs(",\"pressure_source\":", stdout);
    if (metrics.pressure_source) json_string(metrics.pressure_source); else fputs("null", stdout);
    fputs(",\"pressure_percent\":", stdout);
    number(metrics.pressure_percent_available ? metrics.pressure_percent : NAN, 2);
    fputs("},\"disk\":{\"total_bytes\":", stdout); bytes(metrics.disk_total, metrics.disk_status == METRIC_OK);
    fputs(",\"used_bytes\":", stdout); bytes(metrics.disk_used, metrics.disk_status == METRIC_OK);
    fputs("},\"uptime_seconds\":", stdout); number(metrics.uptime >= 0 ? metrics.uptime : NAN, 0);
    fputs(",\"battery\":", stdout);
    if (!metrics.battery.available) fputs("null", stdout);
    else {
        printf("{\"percent\":%d,\"state\":", metrics.battery.percent);
        json_string(metrics.battery.state);
        fputs(",\"time_remaining_minutes\":", stdout);
        if (metrics.battery.time_remaining_minutes < 0) fputs("null", stdout);
        else printf("%d", metrics.battery.time_remaining_minutes);
        putchar('}');
    }
    fputs(",\"processes\":", stdout);
    if (snapshot->processes.status != METRIC_OK) { puts("null}"); return; }
    putchar('[');
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    for (size_t i = 0; i < count; i++) {
        const Process *p = &snapshot->processes.items[i];
        if (i) putchar(',');
        printf("{\"pid\":%d,\"name\":", p->pid); json_string(p->name);
        fputs(",\"cpu_percent\":", stdout); number(p->cpu_percent, 1);
        printf(",\"memory_bytes\":%llu,\"threads\":%d}", p->resident, p->threads);
    }
    puts("]}");
}

void print_startup_json(const StartupList *list) {
    fputs("{\"status\":", stdout); json_string(metric_status_name(list->status));
    printf(",\"partial\":%s,\"items\":[", list->partial ? "true" : "false");
    for (size_t i = 0; i < list->count; i++) {
        const StartupItem *item = &list->items[i];
        if (i) putchar(',');
        fputs("{\"name\":", stdout); json_string(item->name);
        fputs(",\"kind\":", stdout); json_string(startup_kind_name(item->kind));
        fputs(",\"pid\":", stdout);
        if (item->running_known) printf("%d", item->pid); else fputs("null", stdout);
        printf(",\"running\":%s,\"enabled\":%s,\"state\":",
               item->running_known ? (item->running ? "true" : "false") : "null",
               item->enabled_known ? (item->enabled ? "true" : "false") : "null");
        if (item->state[0]) json_string(item->state); else fputs("null", stdout);
        fputs(",\"memory_bytes\":", stdout); bytes(item->resident, item->memory_known);
        fputs(",\"cpu_percent\":", stdout); number(item->cpu_percent, 1);
        fputs(",\"path\":", stdout); json_string(item->path);
        fputs(",\"owner\":", stdout); json_string(item->owner);
        printf(",\"path_missing\":%s,\"last_start\":", item->path_missing ? "true" : "false");
        char start[32]; format_start_time(item->start_time, start, sizeof(start));
        if (item->start_time) json_string(start); else fputs("null", stdout);
        putchar('}');
    }
    puts("]}");
}
