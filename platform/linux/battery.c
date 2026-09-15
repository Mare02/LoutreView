#include "linux.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool read_text(const char *root, const char *name, const char *key, char *out, size_t size) {
    char path[LOUTRE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s/%s", root, name, key);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = fgets(out, (int)size, file) != NULL;
    fclose(file);
    if (ok) out[strcspn(out, "\r\n")] = '\0';
    return ok;
}

static bool read_number(const char *root, const char *name, const char *key, double *value) {
    char text[128], *end;
    if (!read_text(root, name, key, text, sizeof(text)) || text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno || end == text || *end) return false;
    *value = (double)number;
    return true;
}

static bool read_current(const char *root, const char *name, double *value) {
    char text[128], *end;
    if (!read_text(root, name, "current_now", text, sizeof(text))) return false;
    errno = 0;
    long long current = strtoll(text, &end, 10);
    if (errno || end == text || *end) return false;
    *value = fabs((double)current);
    return true;
}

bool linux_read_battery(const char *root, BatteryInfo *out) {
    *out = (BatteryInfo){ .time_remaining_minutes = -1 };
    DIR *dir = opendir(root);
    if (!dir) return false;
    struct dirent *entry;
    char chosen[256] = {0};
    /* Deterministic first readable system battery; do not mix charge/energy units. */
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (chosen[0] && strcmp(chosen, entry->d_name) <= 0) continue;
        char type[64], status[64], scope[64];
        if (!read_text(root, entry->d_name, "type", type, sizeof(type)) || strcmp(type, "Battery")) continue;
        if (read_text(root, entry->d_name, "scope", scope, sizeof(scope)) && !strcmp(scope, "Device")) continue;
        double present;
        if (read_number(root, entry->d_name, "present", &present) && present == 0) continue;
        double capacity = -1, current = 0, full = 0, rate = 0;
        bool energy = read_number(root, entry->d_name, "energy_now", &current) &&
                      read_number(root, entry->d_name, "energy_full", &full) && full > 0;
        bool have_units = energy;
        if (!energy) have_units = read_number(root, entry->d_name, "charge_now", &current) &&
                                  read_number(root, entry->d_name, "charge_full", &full) && full > 0;
        if (!read_number(root, entry->d_name, "capacity", &capacity)) {
            if (!have_units) continue;
            capacity = current / full * 100;
        }
        if (capacity < 0 || capacity > 100) continue;
        BatteryInfo item = { .available = true, .percent = (int)(capacity + .5), .time_remaining_minutes = -1 };
        snprintf(item.state, sizeof(item.state), "unknown");
        if (read_text(root, entry->d_name, "status", status, sizeof(status))) {
            item.charging = !strcmp(status, "Charging");
            item.charged = !strcmp(status, "Full");
            if (item.charging) strcpy(item.state, "charging");
            else if (item.charged) strcpy(item.state, "charged");
            else if (!strcmp(status, "Discharging")) strcpy(item.state, "discharging");
            else if (!strcmp(status, "Not charging")) strcpy(item.state, "on power");
        }
        if (have_units && (item.charging || !strcmp(item.state, "discharging")) &&
            (energy ? read_number(root, entry->d_name, "power_now", &rate) :
                      read_current(root, entry->d_name, &rate)) && rate > 0) {
            double remaining = item.charging ? full - current : current;
            double minutes = remaining / rate * 60;
            if (minutes >= 0 && minutes <= INT_MAX) item.time_remaining_minutes = (int)minutes;
        }
        snprintf(chosen, sizeof(chosen), "%s", entry->d_name);
        *out = item;
    }
    closedir(dir);
    return out->available;
}

bool platform_battery(BatteryInfo *out) { return linux_read_battery("/sys/class/power_supply", out); }
