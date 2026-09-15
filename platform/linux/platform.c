#include "linux.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *platform_name(void) { return "linux"; }

MetricStatus linux_errno_status(void) {
    if (errno == EACCES || errno == EPERM) return METRIC_PERMISSION;
    if (errno == ENOENT || errno == ENOTDIR) return METRIC_UNAVAILABLE;
    return METRIC_ERROR;
}

bool linux_parse_cpu(const char *line, CpuTicks *out, unsigned *id, bool *aggregate) {
    if (strncmp(line, "cpu", 3)) return false;
    const char *p = line + 3;
    *aggregate = *p == ' ' || *p == '\t';
    *id = 0;
    if (!*aggregate) {
        if (*p < '0' || *p > '9') return false;
        char *end;
        errno = 0;
        unsigned long n = strtoul(p, &end, 10);
        if (errno || n > UINT_MAX || (*end != ' ' && *end != '\t')) return false;
        *id = (unsigned)n;
        p = end;
    }
    unsigned long long values[8] = {0};
    size_t count = 0;
    while (count < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        if (*p < '0' || *p > '9') return false;
        char *end;
        errno = 0;
        values[count++] = strtoull(p, &end, 10);
        if (errno || (*end && *end != ' ' && *end != '\t' && *end != '\n')) return false;
        p = end;
    }
    if (count < 4 || ULLONG_MAX - values[2] < values[5] ||
        ULLONG_MAX - values[2] - values[5] < values[6] ||
        ULLONG_MAX - values[2] - values[5] - values[6] < values[7] ||
        ULLONG_MAX - values[3] < values[4]) return false;
    *out = (CpuTicks){ .user = values[0], .nice = values[1],
        .system = values[2] + values[5] + values[6] + values[7],
        .idle = values[3] + values[4] };
    return true;
}

MetricStatus linux_collect_cpu(const char *stat_path, Snapshot *out) {
    out->ticks = (CpuTicks){0};
    out->core_count = 0;
    FILE *file = fopen(stat_path, "r");
    if (!file) return out->cpu_status = linux_errno_status();
    char *line = NULL;
    size_t size = 0;
    bool found = false;
    while (getline(&line, &size, file) >= 0) {
        CpuTicks ticks;
        unsigned id;
        bool aggregate;
        if (!linux_parse_cpu(line, &ticks, &id, &aggregate)) continue;
        if (aggregate) { out->ticks = ticks; found = true; }
        else if (out->core_count < MAX_CPU_CORES) {
            out->core_ids[out->core_count] = id;
            out->core_ticks[out->core_count++] = ticks;
        }
    }
    bool failed = ferror(file);
    free(line);
    fclose(file);
    return out->cpu_status = found && !failed ? METRIC_OK : METRIC_ERROR;
}

MetricStatus platform_cpu(Snapshot *out) { return linux_collect_cpu("/proc/stat", out); }

double platform_uptime(void) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) return -1;
    double uptime = -1;
    if (fscanf(file, "%lf", &uptime) != 1 || !isfinite(uptime) || uptime < 0) uptime = -1;
    fclose(file);
    return uptime;
}
