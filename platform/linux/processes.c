#include "linux.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool linux_parse_process(const char *line, long ticks, long page_size, time_t boot, Process *out) {
    if (ticks <= 0 || page_size <= 0) return false;
    *out = (Process){0};
    char *end;
    errno = 0;
    long pid = strtol(line, &end, 10);
    if (errno || pid <= 0 || pid > INT_MAX || *end != ' ' || end[1] != '(') return false;
    const char *name = end + 2, *close = strrchr(name, ')');
    if (!close || close[1] != ' ' || !close[2] || close[3] != ' ') return false;
    size_t length = (size_t)(close - name);
    if (length >= sizeof(out->name)) length = sizeof(out->name) - 1;
    memcpy(out->name, name, length);
    out->pid = (pid_t)pid;
    const char *p = close + 4; /* field 4; state (field 3) skipped */
    unsigned long long user = 0, system = 0, start = 0, vsize = 0, rss = 0, threads = 0;
    for (int field = 4; field <= 24; field++) {
        while (*p == ' ') p++;
        bool negative = *p == '-';
        const char *digits = negative ? p + 1 : p;
        if (*digits < '0' || *digits > '9') return false;
        errno = 0;
        unsigned long long value = strtoull(digits, &end, 10);
        if (errno || (*end && *end != ' ' && *end != '\n')) return false;
        if (field == 14 || field == 15 || field == 20 || field == 22 || field == 23 || field == 24) {
            if (negative) return false;
            if (field == 14) user = value;
            if (field == 15) system = value;
            if (field == 20) threads = value;
            if (field == 22) start = value;
            if (field == 23) vsize = value;
            if (field == 24) rss = value;
        }
        p = end;
    }
    if (user > ULLONG_MAX - system || threads > INT_MAX || rss > ULLONG_MAX / (unsigned long)page_size) return false;
    unsigned long long sum = user + system;
    /* Quotient/remainder avoids overflowing ticks * 1e9 on long-lived processes. */
    if (sum / (unsigned long)ticks > ULLONG_MAX / 1000000000ULL) return false;
    unsigned long long ns = (sum / (unsigned long)ticks) * 1000000000ULL;
    unsigned long long remainder =
        (unsigned long long)((long double)(sum % (unsigned long)ticks) * 1e9L / ticks);
    if (remainder > ULLONG_MAX - ns) return false;
    out->cpu_time = ns + remainder;
    out->start_id = start;
    unsigned long long since_boot = start / (unsigned long)ticks;
    if (boot > 0 && since_boot > (unsigned long long)LLONG_MAX - (unsigned long long)boot) return false;
    out->start_time = boot > 0 ? boot + (time_t)since_boot : 0;
    out->resident = rss * (unsigned long)page_size;
    out->virtual_size = vsize;
    out->threads = (int)threads;
    return true;
}

ProcessList platform_processes(void) {
    ProcessList list = {0};
    long ticks = sysconf(_SC_CLK_TCK), pages = sysconf(_SC_PAGESIZE);
    if (ticks <= 0 || pages <= 0) { list.status = METRIC_ERROR; return list; }
    DIR *directory = opendir("/proc");
    if (!directory) { list.status = linux_errno_status(); return list; }
    time_t boot = 0;
    FILE *stat = fopen("/proc/stat", "r");
    if (stat) {
        char line[1024];
        while (fgets(line, sizeof(line), stat)) {
            unsigned long long value;
            if (sscanf(line, "btime %llu", &value) == 1) { boot = (time_t)value; break; }
        }
        fclose(stat);
    }
    struct dirent *entry;
    size_t capacity = 0;
    while (true) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            if (errno) list.status = linux_errno_status();
            break;
        }
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid > INT_MAX) continue;
        /* Hold the proc directory so a reused PID cannot redirect exe reads to
         * a replacement process after stat has been sampled. */
        int process_fd = openat(dirfd(directory), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (process_fd < 0) continue;
        int stat_fd = openat(process_fd, "stat", O_RDONLY | O_CLOEXEC);
        if (stat_fd < 0) { close(process_fd); continue; }
        FILE *file = fdopen(stat_fd, "r");
        if (!file) { close(stat_fd); close(process_fd); continue; }
        char line[8192];
        size_t size = fread(line, 1, sizeof(line) - 1, file);
        bool read_ok = size > 0 && size < sizeof(line) - 1 && !ferror(file);
        line[size] = '\0';
        fclose(file);
        Process process;
        if (!read_ok || !linux_parse_process(line, ticks, pages, boot, &process)) { close(process_fd); continue; }
        ssize_t length = readlinkat(process_fd, "exe", process.path, sizeof(process.path) - 1);
        close(process_fd);
        if (length >= 0) process.path[length] = '\0';
        if (list.count == capacity) {
            size_t next = capacity ? capacity * 2 : 128;
            if (next > SIZE_MAX / sizeof(Process)) { list.status = METRIC_ERROR; break; }
            Process *items = realloc(list.items, next * sizeof(*items));
            if (!items) { list.status = METRIC_ERROR; break; }
            list.items = items;
            capacity = next;
        }
        list.items[list.count++] = process;
    }
    closedir(directory);
    return list;
}
