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
#include <limits.h>

const char *format_bytes(unsigned long long bytes, char *buffer, size_t size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    snprintf(buffer, size, unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

const char *format_compact_bytes(unsigned long long bytes, char *buffer, size_t size) {
    static const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    snprintf(buffer, size, unit == 0 ? "%.0f%s" : "%.1f%s", value, units[unit]);
    return buffer;
}

void format_uptime(double seconds, char *buffer, size_t size) {
    if (!isfinite(seconds) || seconds < 0) {
        snprintf(buffer, size, "n/a");
        return;
    }
    unsigned long total = (unsigned long)seconds;
    unsigned long days = total / 86400;
    unsigned long hours = (total % 86400) / 3600;
    unsigned long minutes = (total % 3600) / 60;
    if (days) snprintf(buffer, size, "%lud %luh %lum", days, hours, minutes);
    else if (hours) snprintf(buffer, size, "%luh %lum", hours, minutes);
    else snprintf(buffer, size, "%lum", minutes);
}

void format_duration_minutes(int minutes, char *buffer, size_t size) {
    if (minutes >= 60) snprintf(buffer, size, "%dh %dm", minutes / 60, minutes % 60);
    else snprintf(buffer, size, "%dm", minutes);
}

const char *sort_name(SortMode sort) {
    if (sort == SORT_MEM) return "memory";
    if (sort == SORT_PID) return "PID";
    if (sort == SORT_NAME) return "name";
    return "CPU";
}

void print_spaces(int count) {
    for (int i = 0; i < count; i++) putchar(' ');
}

void format_start_time(time_t value, char *buffer, size_t size) {
    if (!value) { snprintf(buffer, size, "-"); return; }
    struct tm local;
    if (!localtime_r(&value, &local) || strftime(buffer, size, "%Y-%m-%d %H:%M", &local) == 0) {
        snprintf(buffer, size, "unknown");
    }
}

const char *metric_status_name(MetricStatus status) {
    switch (status) {
        case METRIC_OK: return "available";
        case METRIC_PERMISSION: return "permission_denied";
        case METRIC_ERROR: return "error";
        default: return "unavailable";
    }
}

const char *startup_kind_name(StartupKind kind) {
    switch (kind) {
        case STARTUP_AGENT: return "launch agent";
        case STARTUP_DAEMON: return "launch daemon";
        case STARTUP_LOGIN_ITEM: return "login item";
        case STARTUP_SYSTEM_SERVICE: return "system service";
        case STARTUP_USER_SERVICE: return "user service";
        case STARTUP_DESKTOP_AUTOSTART: return "desktop autostart";
        default: return "unknown";
    }
}

const char *startup_kind_short_name(StartupKind kind) {
    switch (kind) {
        case STARTUP_AGENT: return "agent";
        case STARTUP_DAEMON: return "daemon";
        case STARTUP_LOGIN_ITEM: return "login";
        case STARTUP_SYSTEM_SERVICE: return "system";
        case STARTUP_USER_SERVICE: return "user";
        case STARTUP_DESKTOP_AUTOSTART: return "autostart";
        default: return "unknown";
    }
}

void format_pressure(const SystemMetrics *metrics, char *buffer, size_t size, bool compact) {
    if (metrics->pressure_percent_available && isfinite(metrics->pressure_percent))
        snprintf(buffer, size, "%.2f%% PSI", metrics->pressure_percent);
    else if (metrics->pressure_available) {
        if (compact) format_compact_bytes(metrics->pressure, buffer, size);
        else format_bytes(metrics->pressure, buffer, size);
    } else snprintf(buffer, size, "n/a");
}

/* Reject overlong forms, surrogates, and code points outside Unicode. */
size_t text_utf8_width(const unsigned char *p) {
    if (*p < 0x80) return 1;
    size_t n = *p >= 0xc2 && *p <= 0xdf ? 2 :
               *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
    if (!n) return 0;
    for (size_t i = 1; i < n; i++) if (!p[i] || (p[i] & 0xc0) != 0x80) return 0;
    if ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0) ||
        (*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

void print_safe_text(const char *text, int max_columns) {
    int columns = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && (max_columns < 0 || columns < max_columns); columns++) {
        size_t n = text_utf8_width(p);
        if (!n || *p < 0x20 || *p == 0x7f || (n == 2 && p[0] == 0xc2 && p[1] < 0xa0)) {
            putchar('?'); p += n ? n : 1;
        } else { fwrite(p, 1, n, stdout); p += n; }
    }
}

void print_percent(double value, int width, int precision, bool suffix) {
    if (isfinite(value)) { printf("%*.*f", width, precision, value); if (suffix) putchar('%'); }
    else printf("%*s", width + (suffix ? 1 : 0), "n/a");
}

void format_rate(double value, char *buffer, size_t size) {
    if (!isfinite(value) || value < 0 || value >= (double)ULLONG_MAX) snprintf(buffer, size, "n/a");
    else format_compact_bytes((unsigned long long)value, buffer, size);
}
