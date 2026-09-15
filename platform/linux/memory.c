#include "linux.h"
#include <limits.h>
#include <math.h>
#include <string.h>

bool linux_parse_memory(FILE *file, SystemMetrics *out) {
    char line[256];
    unsigned long long total = 0, available = 0;
    bool have_total = false, have_available = false;
    while (fgets(line, sizeof(line), file)) {
        char key[64], unit[16];
        unsigned long long value;
        if (sscanf(line, "%63s %llu %15s", key, &value, unit) != 3) continue;
        if (strcmp(unit, "kB") || value > ULLONG_MAX / 1024) continue;
        if (!strcmp(key, "MemTotal:")) { total = value * 1024; have_total = true; }
        if (!strcmp(key, "MemAvailable:")) { available = value * 1024; have_available = true; }
    }
    if (ferror(file) || !have_total || !have_available || !total || available > total) return false;
    out->memory_total = total;
    out->memory_used = total - available;
    return true;
}

MetricStatus platform_memory(SystemMetrics *out) {
    out->memory_total = out->memory_used = out->pressure = 0;
    out->pressure_available = out->pressure_percent_available = false;
    out->pressure_source = NULL;
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return out->memory_status = linux_errno_status();
    bool ok = linux_parse_memory(file, out);
    fclose(file);
    file = fopen("/proc/pressure/memory", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            double percent;
            if (sscanf(line, "some avg10=%lf", &percent) == 1 &&
                isfinite(percent) && percent >= 0 && percent <= 100) {
                out->pressure_percent = percent;
                out->pressure_percent_available = true;
                out->pressure_source = "linux_psi_some_avg10";
                break;
            }
        }
        fclose(file);
    }
    return out->memory_status = ok ? METRIC_OK : METRIC_ERROR;
}
