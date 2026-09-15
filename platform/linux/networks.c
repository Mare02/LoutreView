#include "linux.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

bool linux_parse_network(const char *line, NetworkInterface *out) {
    const char *colon = strrchr(line, ':');
    if (!colon) return false;
    while (isspace((unsigned char)*line)) line++;
    size_t length = (size_t)(colon - line);
    if (!length || length >= sizeof(out->name)) return false;
    *out = (NetworkInterface){0};
    memcpy(out->name, line, length);
    const char *p = colon + 1;
    for (int field = 0; field < 16; field++) {
        while (isspace((unsigned char)*p)) p++;
        if (*p < '0' || *p > '9') return false;
        char *end;
        errno = 0;
        unsigned long long value = strtoull(p, &end, 10);
        if (errno || (*end && !isspace((unsigned char)*end))) return false;
        if (!field) out->received = value;
        if (field == 8) out->transmitted = value;
        p = end;
    }
    return true;
}

NetworkSnapshot platform_networks(void) {
    NetworkSnapshot result = {0};
    FILE *file = fopen("/proc/net/dev", "r");
    if (!file) { result.status = linux_errno_status(); return result; }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        NetworkInterface item;
        if (!linux_parse_network(line, &item)) continue;
        if (result.count == MAX_NETWORK_INTERFACES) { result.truncated = true; continue; }
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/net/%s/flags", item.name);
        FILE *flags = fopen(path, "r");
        if (flags) {
            unsigned value = 0;
            if (fscanf(flags, "%x", &value) == 1) item.up = (value & IFF_UP) != 0;
            fclose(flags);
        }
        result.items[result.count++] = item;
    }
    if (ferror(file)) result.status = METRIC_ERROR;
    fclose(file);
    return result;
}
