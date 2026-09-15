#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
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

double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return NAN;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void free_snapshot(Snapshot *snapshot) {
    free(snapshot->processes.items);
    snapshot->processes.items = NULL;
    snapshot->processes.count = 0;
}

static int compare_cpu(const void *a, const void *b) {
    const Process *left = a, *right = b;
    if (isfinite(left->cpu_percent) != isfinite(right->cpu_percent)) return isfinite(left->cpu_percent) ? -1 : 1;
    if (!isfinite(left->cpu_percent)) return (left->pid > right->pid) - (left->pid < right->pid);
    return (right->cpu_percent > left->cpu_percent) - (right->cpu_percent < left->cpu_percent);
}
static int compare_mem(const void *a, const void *b) {
    const Process *left = a, *right = b;
    return (right->resident > left->resident) - (right->resident < left->resident);
}
static int compare_pid(const void *a, const void *b) {
    const Process *left = a, *right = b;
    return (left->pid > right->pid) - (left->pid < right->pid);
}
static int compare_name(const void *a, const void *b) {
    return strcasecmp(((const Process *)a)->name, ((const Process *)b)->name);
}

void sort_processes(ProcessList *list, SortMode sort) {
    int (*compare)(const void *, const void *) = compare_cpu;
    if (sort == SORT_MEM) compare = compare_mem;
    else if (sort == SORT_PID) compare = compare_pid;
    else if (sort == SORT_NAME) compare = compare_name;
    if (list->count > 1) qsort(list->items, list->count, sizeof(Process), compare);
}

double cpu_usage(const CpuTicks *before, const CpuTicks *after) {
    if (!before || !after || after->user < before->user ||
        after->system < before->system || after->nice < before->nice ||
        after->idle < before->idle) return NAN;
    double busy = (double)(after->user - before->user) +
                  (double)(after->system - before->system) +
                  (double)(after->nice - before->nice);
    double total = busy + (double)(after->idle - before->idle);
    return total > 0 ? 100.0 * busy / total : NAN;
}

static bool same_cores(const Snapshot *before, const Snapshot *after) {
    if (before->core_count > MAX_CPU_CORES || after->core_count > MAX_CPU_CORES) return false;
    if (before->core_count != after->core_count) return false;
    for (size_t i = 0; i < after->core_count; i++) {
        bool found = false;
        for (size_t j = 0; j < before->core_count; j++)
            if (after->core_ids[i] == before->core_ids[j]) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

double sample_cpu_usage(const Snapshot *before, const Snapshot *after) {
    if (!before || !after || before->cpu_status != METRIC_OK ||
        after->cpu_status != METRIC_OK || !same_cores(before, after)) return NAN;
    return cpu_usage(&before->ticks, &after->ticks);
}

size_t sample_core_usage(const Snapshot *before, const Snapshot *after, double *out) {
    if (!after || !out) return 0;
    size_t count = after->core_count < MAX_CPU_CORES ? after->core_count : MAX_CPU_CORES;
    for (size_t i = 0; i < count; i++) {
        out[i] = NAN;
        if (!before || before->cpu_status != METRIC_OK || after->cpu_status != METRIC_OK) continue;
        for (size_t j = 0; j < before->core_count && j < MAX_CPU_CORES; j++) {
            if (before->core_ids[j] != after->core_ids[i]) continue;
            out[i] = cpu_usage(&before->core_ticks[j], &after->core_ticks[i]);
            break;
        }
    }
    return count;
}

void sample_process_usage(ProcessList *current, const ProcessList *previous, double elapsed) {
    for (size_t i = 0; i < current->count; i++) {
        Process *p = &current->items[i];
        p->cpu_percent = NAN;
        if (!previous || previous->status != METRIC_OK || current->status != METRIC_OK ||
            !isfinite(elapsed) || elapsed <= 0 || !p->start_id) continue;
        for (size_t j = 0; j < previous->count; j++) {
            const Process *old = &previous->items[j];
            if (old->pid != p->pid || old->start_id != p->start_id) continue;
            if (p->cpu_time >= old->cpu_time)
                p->cpu_percent = 100.0 * ((double)(p->cpu_time - old->cpu_time) / 1e9) / elapsed;
            break;
        }
    }
}

void sample_network_usage(NetworkSnapshot *current, const NetworkSnapshot *previous, double elapsed) {
    for (size_t i = 0; i < current->count; i++) {
        NetworkInterface *net = &current->items[i];
        net->receive_rate = net->transmit_rate = NAN;
        if (!previous || previous->status != METRIC_OK || current->status != METRIC_OK ||
            !isfinite(elapsed) || elapsed <= 0) continue;
        for (size_t j = 0; j < previous->count; j++) {
            const NetworkInterface *old = &previous->items[j];
            if (strcmp(old->name, net->name)) continue;
            if (net->received >= old->received) net->receive_rate = (double)(net->received - old->received) / elapsed;
            if (net->transmitted >= old->transmitted) net->transmit_rate = (double)(net->transmitted - old->transmitted) / elapsed;
            break;
        }
    }
}

Snapshot collect_snapshot(const Snapshot *previous) {
    Snapshot result = { .cpu_status = METRIC_UNAVAILABLE,
                        .processes = { .status = METRIC_UNAVAILABLE } };
    result.cpu_status = platform_cpu(&result);
    result.timestamp = now_seconds();
    result.processes = platform_processes();
    sample_process_usage(&result.processes, previous ? &previous->processes : NULL,
                         previous ? result.timestamp - previous->timestamp : 0);
    return result;
}

NetworkSnapshot collect_networks(const NetworkSnapshot *previous, double elapsed) {
    NetworkSnapshot result = platform_networks();
    sample_network_usage(&result, previous, elapsed);
    return result;
}

SystemMetrics collect_system_metrics(void) {
    SystemMetrics result = { .memory_status = METRIC_UNAVAILABLE,
        .disk_status = METRIC_UNAVAILABLE, .load_status = METRIC_UNAVAILABLE, .uptime = -1,
        .battery = { .time_remaining_minutes = -1 } };
    result.memory_status = platform_memory(&result);
    result.disk_status = platform_disk(&result);
    platform_battery(&result.battery);
    result.load_status = getloadavg(result.loads, 3) == 3 ? METRIC_OK : METRIC_UNAVAILABLE;
    result.uptime = platform_uptime();
    return result;
}
