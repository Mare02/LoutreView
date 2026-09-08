#include <errno.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.0.0"
#define DEFAULT_LIMIT 12
#define MIN_INTERVAL_MS 250
#define MAX_CPU_CORES 128

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_CYAN "\033[38;5;81m"
#define ANSI_TEAL "\033[38;5;45m"
#define ANSI_GREEN "\033[38;5;114m"
#define ANSI_AMBER "\033[38;5;221m"
#define ANSI_RED "\033[38;5;203m"
#define ANSI_SLATE "\033[38;5;246m"
#define ANSI_ALT_SCREEN "\033[?1049h"
#define ANSI_MAIN_SCREEN "\033[?1049l"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"
#define ANSI_MOUSE_ON "\033[?1000h\033[?1006h"
#define ANSI_MOUSE_OFF "\033[?1006l\033[?1000l"

typedef enum { SORT_CPU, SORT_MEM, SORT_PID, SORT_NAME } SortMode;

typedef struct {
    unsigned long long user, system, idle, nice;
} CpuTicks;

typedef struct {
    pid_t pid;
    char name[PROC_PIDPATHINFO_MAXSIZE];
    unsigned long long resident;
    unsigned long long virtual_size;
    unsigned long long cpu_time;
    int threads;
    double cpu_percent;
} Process;

typedef struct {
    Process *items;
    size_t count;
} ProcessList;

typedef struct {
    bool available;
    int percent;
    bool charging;
    bool charged;
    int time_remaining_minutes;
    char state[16];
} BatteryInfo;

typedef struct {
    unsigned long long memory_total;
    unsigned long long memory_used;
    unsigned long long pressure;
    unsigned long long disk_total;
    unsigned long long disk_used;
    double loads[3];
    double uptime;
    BatteryInfo battery;
} SystemMetrics;

typedef struct {
    CpuTicks ticks;
    CpuTicks core_ticks[MAX_CPU_CORES];
    size_t core_count;
    double timestamp;
    ProcessList processes;
} Snapshot;

typedef struct {
    int interval_ms;
    int limit;
    bool once;
    bool json;
    bool compact;
    bool no_color;
    SortMode sort;
} Options;

static volatile sig_atomic_t running = 1;

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static const char *format_bytes(unsigned long long bytes, char *buffer, size_t size) {
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

static const char *format_compact_bytes(unsigned long long bytes, char *buffer, size_t size) {
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

static bool read_cpu_ticks(CpuTicks *out, CpuTicks *cores, size_t *core_count) {
    natural_t cpu_count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_count = 0;
    kern_return_t result = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                                               &cpu_count, &info, &info_count);
    if (result != KERN_SUCCESS || info == NULL) return false;

    memset(out, 0, sizeof(*out));
    *core_count = cpu_count < MAX_CPU_CORES ? cpu_count : MAX_CPU_CORES;
    for (natural_t i = 0; i < cpu_count; i++) {
        processor_cpu_load_info_t cpu =
            (processor_cpu_load_info_t)(info + i * CPU_STATE_MAX);
        out->user += cpu->cpu_ticks[CPU_STATE_USER];
        out->system += cpu->cpu_ticks[CPU_STATE_SYSTEM];
        out->idle += cpu->cpu_ticks[CPU_STATE_IDLE];
        out->nice += cpu->cpu_ticks[CPU_STATE_NICE];
        if (i < *core_count) {
            cores[i] = (CpuTicks){
                .user = cpu->cpu_ticks[CPU_STATE_USER],
                .system = cpu->cpu_ticks[CPU_STATE_SYSTEM],
                .idle = cpu->cpu_ticks[CPU_STATE_IDLE],
                .nice = cpu->cpu_ticks[CPU_STATE_NICE],
            };
        }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)info_count * sizeof(integer_t));
    return true;
}

static double cpu_usage(const CpuTicks *before, const CpuTicks *after) {
    unsigned long long previous = before->user + before->system + before->idle + before->nice;
    unsigned long long current = after->user + after->system + after->idle + after->nice;
    unsigned long long busy_before = before->user + before->system + before->nice;
    unsigned long long busy_after = after->user + after->system + after->nice;
    if (current <= previous) return 0.0;
    return 100.0 * (double)(busy_after - busy_before) / (double)(current - previous);
}

static bool get_memory(unsigned long long *total, unsigned long long *used,
                       unsigned long long *pressure) {
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    size_t length = sizeof(*total);
    if (sysctl(mib, 2, total, &length, NULL, 0) != 0) return false;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        return false;
    }
    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);
    unsigned long long available_pages = (unsigned long long)vm.free_count + vm.inactive_count;
    unsigned long long wired_pages = vm.wire_count;
    *used = *total > available_pages * page_size ? *total - available_pages * page_size : 0;
    *pressure = (wired_pages + vm.compressor_page_count) * (unsigned long long)page_size;
    return true;
}

static double get_uptime(void) {
    struct timeval boot;
    size_t size = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &size, NULL, 0) != 0) return -1;
    return difftime(time(NULL), boot.tv_sec);
}

static void format_uptime(double seconds, char *buffer, size_t size) {
    if (seconds < 0) {
        snprintf(buffer, size, "unavailable");
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

static void format_duration_minutes(int minutes, char *buffer, size_t size) {
    if (minutes >= 60) snprintf(buffer, size, "%dh %dm", minutes / 60, minutes % 60);
    else snprintf(buffer, size, "%dm", minutes);
}

static bool cf_number_to_int(CFTypeRef value, int *out) {
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, out);
}

static bool cf_boolean_value(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
}

static bool cf_string_equal(CFTypeRef value, const char *expected) {
    char text[64];
    return value && CFGetTypeID(value) == CFStringGetTypeID() &&
           CFStringGetCString((CFStringRef)value, text, sizeof(text), kCFStringEncodingUTF8) &&
           strcmp(text, expected) == 0;
}

static bool get_battery(BatteryInfo *out) {
    *out = (BatteryInfo){ .time_remaining_minutes = -1 };
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    if (!sources) {
        CFRelease(info);
        return false;
    }

    for (CFIndex i = 0; i < CFArrayGetCount(sources); i++) {
        CFTypeRef source = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef description = IOPSGetPowerSourceDescription(info, source);
        if (!description || !cf_string_equal(CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)),
                                             kIOPSInternalBatteryType)) {
            continue;
        }

        int current = 0, maximum = 0;
        if (!cf_number_to_int(CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)), &current) ||
            !cf_number_to_int(CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)), &maximum) ||
            current < 0 || maximum <= 0) {
            continue;
        }
        out->available = true;
        out->percent = (int)(100.0 * current / maximum + 0.5);
        if (out->percent > 100) out->percent = 100;
        out->charging = cf_boolean_value(CFDictionaryGetValue(description, CFSTR(kIOPSIsChargingKey)));
        out->charged = cf_boolean_value(CFDictionaryGetValue(description, CFSTR(kIOPSIsChargedKey)));

        CFTypeRef power_state = CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey));
        if (out->charged) snprintf(out->state, sizeof(out->state), "charged");
        else if (out->charging) snprintf(out->state, sizeof(out->state), "charging");
        else if (cf_string_equal(power_state, kIOPSBatteryPowerValue)) snprintf(out->state, sizeof(out->state), "discharging");
        else if (cf_string_equal(power_state, kIOPSACPowerValue)) snprintf(out->state, sizeof(out->state), "on power");
        else snprintf(out->state, sizeof(out->state), "unknown");

        if (out->charging || cf_string_equal(power_state, kIOPSBatteryPowerValue)) {
            CFStringRef time_key = out->charging ? CFSTR(kIOPSTimeToFullChargeKey) : CFSTR(kIOPSTimeToEmptyKey);
            cf_number_to_int(CFDictionaryGetValue(description, time_key), &out->time_remaining_minutes);
        }
        break;
    }
    CFRelease(sources);
    CFRelease(info);
    return out->available;
}

static bool get_disk(unsigned long long *total, unsigned long long *used) {
    struct statfs stats;
    if (statfs("/", &stats) != 0) return false;
    *total = (unsigned long long)stats.f_blocks * stats.f_bsize;
    *used = (unsigned long long)(stats.f_blocks - stats.f_bavail) * stats.f_bsize;
    return true;
}

static SystemMetrics collect_system_metrics(void) {
    SystemMetrics metrics = {0};
    get_memory(&metrics.memory_total, &metrics.memory_used, &metrics.pressure);
    get_disk(&metrics.disk_total, &metrics.disk_used);
    get_battery(&metrics.battery);
    getloadavg(metrics.loads, 3);
    metrics.uptime = get_uptime();
    return metrics;
}

static unsigned long long process_cpu_time(const struct proc_taskinfo *task) {
    return task->pti_total_user + task->pti_total_system;
}

static ProcessList collect_processes(const Snapshot *previous, double elapsed) {
    ProcessList list = {0};
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return list;
    pid_t *pids = malloc((size_t)bytes);
    if (!pids) return list;
    int actual = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);
    if (actual <= 0) { free(pids); return list; }

    size_t capacity = (size_t)actual / sizeof(pid_t);
    list.items = calloc(capacity, sizeof(Process));
    if (!list.items) { free(pids); return list; }

    for (int i = 0; i < actual / (int)sizeof(pid_t); i++) {
        if (pids[i] <= 0) continue;
        struct proc_taskinfo task;
        int received = proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &task, sizeof(task));
        if (received != sizeof(task)) continue;

        Process *process = &list.items[list.count++];
        process->pid = pids[i];
        process->resident = task.pti_resident_size;
        process->virtual_size = task.pti_virtual_size;
        process->threads = task.pti_threadnum;
        process->cpu_time = process_cpu_time(&task);
        if (proc_name(pids[i], process->name, sizeof(process->name)) <= 0) {
            snprintf(process->name, sizeof(process->name), "<unknown>");
        }
        if (previous && elapsed > 0) {
            for (size_t j = 0; j < previous->processes.count; j++) {
                const Process *old = &previous->processes.items[j];
                if (old->pid == process->pid && process->cpu_time >= old->cpu_time) {
                    process->cpu_percent = 100.0 * ((double)(process->cpu_time - old->cpu_time) / 1e9) / elapsed;
                    break;
                }
            }
        }
    }
    free(pids);
    return list;
}

static void free_snapshot(Snapshot *snapshot) {
    free(snapshot->processes.items);
    snapshot->processes.items = NULL;
    snapshot->processes.count = 0;
}

static int compare_cpu(const void *a, const void *b) {
    const Process *left = a, *right = b;
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

static void sort_processes(ProcessList *list, SortMode sort) {
    int (*compare)(const void *, const void *) = compare_cpu;
    if (sort == SORT_MEM) compare = compare_mem;
    else if (sort == SORT_PID) compare = compare_pid;
    else if (sort == SORT_NAME) compare = compare_name;
    qsort(list->items, list->count, sizeof(Process), compare);
}

static int terminal_width(void) {
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0) return window.ws_col;
    return 100;
}

static int terminal_height(void) {
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_row > 0) return window.ws_row;
    return 0;
}

static const char *status_color(double percent, bool color) {
    if (!color) return "";
    if (percent >= 90.0) return ANSI_RED;
    if (percent >= 70.0) return ANSI_AMBER;
    return ANSI_GREEN;
}

static void print_bar(double percent, int width, bool color) {
    int filled = (int)((percent / 100.0) * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    fputs(status_color(percent, color), stdout);
    for (int i = 0; i < filled; i++) fputs("█", stdout);
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = filled; i < width; i++) fputs("░", stdout);
    if (color) fputs(ANSI_RESET, stdout);
}

static void print_core_grid(const double *cores, size_t count, int width, bool color) {
    int columns = width >= 92 ? 3 : width >= 62 ? 2 : 1;
    const int meter_width = 6;

    putchar('\n');
    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    fputs("CPU CORES", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    for (size_t start = 0; start < count; start += (size_t)columns) {
        for (int column = 0; column < columns && start + (size_t)column < count; column++) {
            size_t index = start + (size_t)column;
            if (color) fputs(ANSI_SLATE, stdout);
            printf("  CORE %02zu ", index + 1);
            if (color) fputs(ANSI_RESET, stdout);
            print_bar(cores[index], meter_width, color);
            printf(" %4.1f%%", cores[index]);
            if (column + 1 < columns && start + (size_t)column + 1 < count) fputs("    ", stdout);
        }
        putchar('\n');
    }
    putchar('\n');
}

static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p >= 0x20) putchar(*p);
    }
    putchar('"');
}

static void print_json(const Snapshot *snapshot, double cpu, const Options *options) {
    SystemMetrics metrics = collect_system_metrics();
    printf("{\"cpu\":{\"usage_percent\":%.1f,\"load\":[%.2f,%.2f,%.2f]},", cpu,
           metrics.loads[0], metrics.loads[1], metrics.loads[2]);
    printf("\"memory\":{\"total_bytes\":%llu,\"used_bytes\":%llu,\"pressure_bytes\":%llu},",
           metrics.memory_total, metrics.memory_used, metrics.pressure);
    printf("\"disk\":{\"total_bytes\":%llu,\"used_bytes\":%llu},\"uptime_seconds\":",
           metrics.disk_total, metrics.disk_used);
    if (metrics.uptime < 0) fputs("null", stdout); else printf("%.0f", metrics.uptime);
    fputs(",\"battery\":", stdout);
    if (!metrics.battery.available) {
        fputs("null", stdout);
    } else {
        printf("{\"percent\":%d,\"state\":\"%s\",\"time_remaining_minutes\":",
               metrics.battery.percent, metrics.battery.state);
        if (metrics.battery.time_remaining_minutes < 0) fputs("null", stdout);
        else printf("%d", metrics.battery.time_remaining_minutes);
        putchar('}');
    }
    fputs(",\"processes\":[", stdout);
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    for (size_t i = 0; i < count; i++) {
        const Process *p = &snapshot->processes.items[i];
        if (i) putchar(',');
        printf("{\"pid\":%d,\"name\":", p->pid); json_string(p->name);
        printf(",\"cpu_percent\":%.1f,\"memory_bytes\":%llu,\"threads\":%d}", p->cpu_percent, p->resident, p->threads);
    }
    puts("]}");
}

static void print_process_name(const char *name, int width) {
    size_t length = strlen(name);
    if (width <= 0) return;
    if (length <= (size_t)width) {
        fputs(name, stdout);
    } else if (width <= 3) {
        printf("%.*s", width, name);
    } else {
        printf("%.*s...", width - 3, name);
    }
}

static const char *sort_name(SortMode sort) {
    if (sort == SORT_MEM) return "memory";
    if (sort == SORT_PID) return "PID";
    if (sort == SORT_NAME) return "name";
    return "CPU";
}

static void print_spaces(int count) {
    for (int i = 0; i < count; i++) putchar(' ');
}

static void print_wide_core_cell(const double *cores, size_t index, size_t count, bool color) {
    if (index >= count) return;
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  CORE %02zu ", index + 1);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cores[index], 6, color);
    printf(" %4.1f%%", cores[index]);
}

static void print_wide_short_panel(const Snapshot *snapshot, const double *cores, size_t core_count,
                                   const Options *options, int width, int process_rows, bool color) {
    const int left_width = width / 2;
    const size_t core_rows = (core_count + 1) / 2;
    const int name_width = width - left_width - 24;
    const size_t process_count = snapshot->processes.count < (size_t)process_rows ?
                                 snapshot->processes.count : (size_t)process_rows;

    putchar('\n');
    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    fputs("CPU CORES", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    print_spaces(left_width - 9);
    if (color) fputs(ANSI_BOLD, stdout);
    fputs("TOP PROCESSES", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    printf("  (sorted by %s)", sort_name(options->sort));
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    size_t rows = core_rows > process_count + 1 ? core_rows : process_count + 1;
    for (size_t row = 0; row < rows; row++) {
        size_t first_core = row * 2;
        if (first_core < core_count) {
            print_wide_core_cell(cores, first_core, core_count, color);
            print_spaces(4);
            if (first_core + 1 < core_count) {
                print_wide_core_cell(cores, first_core + 1, core_count, color);
            } else {
                print_spaces(22);
            }
        } else {
            print_spaces(48);
        }
        print_spaces(left_width - 48);

        if (row == 0) {
            if (color) fputs(ANSI_BOLD, stdout);
            printf("  %-5s  %5s  %6s  %s", "PID", "CPU%", "MEM", "PROCESS");
            if (color) fputs(ANSI_RESET, stdout);
        } else if (row - 1 < process_count) {
            const Process *process = &snapshot->processes.items[row - 1];
            char resident[16];
            const char *process_color = process->cpu_percent >= 70.0 ? ANSI_RED :
                                        process->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
            format_compact_bytes(process->resident, resident, sizeof(resident));
            printf("  %-5d  ", process->pid);
            if (color) fputs(process_color, stdout);
            printf("%5.1f", process->cpu_percent);
            if (color) fputs(ANSI_RESET, stdout);
            printf("  %6s  ", resident);
            print_process_name(process->name, name_width);
        }
        putchar('\n');
    }
}

static void print_compact_screen(const Snapshot *snapshot, double cpu, const Options *options,
                                 bool clear, bool color) {
    SystemMetrics metrics = collect_system_metrics();
    char memory_used[16], memory_total[16], disk_used[16], disk_total[16], pressure[16], uptime[32];
    format_compact_bytes(metrics.memory_used, memory_used, sizeof(memory_used));
    format_compact_bytes(metrics.memory_total, memory_total, sizeof(memory_total));
    format_compact_bytes(metrics.disk_used, disk_used, sizeof(disk_used));
    format_compact_bytes(metrics.disk_total, disk_total, sizeof(disk_total));
    format_compact_bytes(metrics.pressure, pressure, sizeof(pressure));
    format_uptime(metrics.uptime, uptime, sizeof(uptime));
    double memory_percent = metrics.memory_total ? 100.0 * (double)metrics.memory_used / metrics.memory_total : 0.0;
    double disk_percent = metrics.disk_total ? 100.0 * (double)metrics.disk_used / metrics.disk_total : 0.0;
    int width = terminal_width();
    if (clear) fputs("\033[H\033[2J", stdout);

    if (width >= 78) {
        printf("SYSVIEW  CPU %.0f%%  MEM %s/%s  DISK %s/%s", cpu, memory_used, memory_total,
               disk_used, disk_total);
        if (metrics.battery.available) printf("  BAT %d%%", metrics.battery.percent);
        putchar('\n');
    } else if (width >= 60) {
        printf("SYSVIEW  CPU %.0f%%  MEM %s/%s  DISK %s/%s", cpu, memory_used, memory_total,
               disk_used, disk_total);
        if (metrics.battery.available && width >= 70) printf("  BAT %d%%", metrics.battery.percent);
        putchar('\n');
    } else {
        printf("SYSVIEW  CPU %.0f%%  MEM %.0f%%  DISK %.0f%%", cpu, memory_percent, disk_percent);
        if (metrics.battery.available) printf("  BAT %d%%", metrics.battery.percent);
        putchar('\n');
    }

    printf("load %.2f · %.2f · %.2f", metrics.loads[0], metrics.loads[1], metrics.loads[2]);
    if (width >= 60) printf("  · up %s", uptime);
    if (width >= 78) printf("  · pressure %s", pressure);
    putchar('\n');

    bool show_memory = width >= 60;
    if (color) fputs(ANSI_BOLD, stdout);
    if (show_memory) printf("  %-5s  %5s  %6s  %s\n", "PID", "CPU%", "MEM", "PROCESS");
    else printf("  %-5s  %5s  %s\n", "PID", "CPU%", "PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    int name_width = width - (show_memory ? 24 : 17);
    if (name_width < 1) name_width = 1;
    for (size_t i = 0; i < count; i++) {
        const Process *process = &snapshot->processes.items[i];
        const char *process_color = process->cpu_percent >= 70.0 ? ANSI_RED :
                                    process->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
        printf("  %-5d  ", process->pid);
        if (color) fputs(process_color, stdout);
        printf("%5.1f", process->cpu_percent);
        if (color) fputs(ANSI_RESET, stdout);
        if (show_memory) {
            char resident[16];
            format_compact_bytes(process->resident, resident, sizeof(resident));
            printf("  %6s  ", resident);
        } else {
            fputs("  ", stdout);
        }
        print_process_name(process->name, name_width);
        putchar('\n');
    }
    fflush(stdout);
}

static void print_screen(const Snapshot *snapshot, double cpu, const Options *options,
                         const double *core_usage, size_t core_count,
                         bool clear, bool color) {
    SystemMetrics metrics = collect_system_metrics();
    char memory_used_text[24], memory_total_text[24], pressure_text[24], disk_used_text[24], disk_total_text[24], uptime[32];
    format_bytes(metrics.memory_used, memory_used_text, sizeof(memory_used_text));
    format_bytes(metrics.memory_total, memory_total_text, sizeof(memory_total_text));
    format_bytes(metrics.pressure, pressure_text, sizeof(pressure_text));
    format_bytes(metrics.disk_used, disk_used_text, sizeof(disk_used_text));
    format_bytes(metrics.disk_total, disk_total_text, sizeof(disk_total_text));
    format_uptime(metrics.uptime, uptime, sizeof(uptime));
    double memory_percent = metrics.memory_total ? 100.0 * (double)metrics.memory_used / metrics.memory_total : 0.0;
    double disk_percent = metrics.disk_total ? 100.0 * (double)metrics.disk_used / metrics.disk_total : 0.0;
    int width = terminal_width();
    int height = terminal_height();
    int bar_width = width >= 100 ? 28 : width >= 78 ? 18 : 10;
    if (clear) fputs("\033[H\033[2J", stdout);

    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("SYSVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    printf("  /  SYSTEM PULSE    %srefresh %dms%s    %sCtrl-C to quit%s\n",
           color ? ANSI_TEAL : "", options->interval_ms, color ? ANSI_RESET : "",
           color ? ANSI_DIM : "", color ? ANSI_RESET : "");
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = 0; i < width - 1; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    printf("CPU  %5.1f%% ", cpu);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cpu, bar_width, color);
    printf("  Load %.2f · %.2f · %.2f\n", metrics.loads[0], metrics.loads[1], metrics.loads[2]);

    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    printf("MEM  %5.1f%% ", memory_percent);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(memory_percent, bar_width, color);
    printf("  %s / %s  %spressure %s%s\n", memory_used_text, memory_total_text,
           color ? ANSI_DIM : "", pressure_text, color ? ANSI_RESET : "");

    if (color) fputs(ANSI_AMBER ANSI_BOLD, stdout);
    printf("DISK %5.1f%% ", disk_percent);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(disk_percent, bar_width, color);
    printf("  %s / %s  %suptime %s%s\n", disk_used_text, disk_total_text,
           color ? ANSI_DIM : "", uptime, color ? ANSI_RESET : "");
    if (metrics.battery.available) {
        if (color) fputs(ANSI_AMBER ANSI_BOLD, stdout);
        printf("BAT  %5d%% ", metrics.battery.percent);
        if (color) fputs(ANSI_RESET, stdout);
        print_bar((double)metrics.battery.percent, bar_width, color);
        printf("  %s", metrics.battery.state);
        if (metrics.battery.time_remaining_minutes >= 0) {
            char battery_time[24];
            format_duration_minutes(metrics.battery.time_remaining_minutes, battery_time, sizeof(battery_time));
            printf(" · %s", battery_time);
        }
        putchar('\n');
    }

    size_t standard_core_rows = (core_count + 2) / 3;
    size_t process_count = snapshot->processes.count < (size_t)options->limit ?
                           snapshot->processes.count : (size_t)options->limit;
    int metric_rows = 5 + (metrics.battery.available ? 1 : 0);
    int standard_rows = metric_rows + 3 + (int)standard_core_rows + 2 + (int)process_count;
    size_t side_core_rows = (core_count + 1) / 2;
    bool use_side_panel = width >= 96 && height > 0 && height < standard_rows &&
                          height >= metric_rows + 2 + (int)side_core_rows;
    if (use_side_panel) {
        int side_process_rows = height - metric_rows - 3;
        if (side_process_rows > 8) side_process_rows = 8;
        if (side_process_rows < 1) side_process_rows = 1;
        print_wide_short_panel(snapshot, core_usage, core_count, options, width, side_process_rows, color);
        fflush(stdout);
        return;
    }

    print_core_grid(core_usage, core_count, width, color);

    if (color) fputs(ANSI_BOLD, stdout);
    printf("  PID    CPU%%     MEM  THR  PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  %s(sorted by %s)%s\n", color ? ANSI_DIM : "", sort_name(options->sort),
           color ? ANSI_RESET : "");
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  ─────  ─────  ──────  ───  ");
    for (int i = 0; i < width - 33; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    for (size_t i = 0; i < count; i++) {
        const Process *p = &snapshot->processes.items[i];
        char resident[24]; format_bytes(p->resident, resident, sizeof(resident));
        const char *process_color = p->cpu_percent >= 70.0 ? ANSI_RED : p->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
        printf("  %-5d  ", p->pid);
        if (color) fputs(process_color, stdout);
        printf("%5.1f", p->cpu_percent);
        if (color) fputs(ANSI_RESET, stdout);
        printf("  %6s  %3d  %.48s\n", resident, p->threads, p->name);
    }
    fflush(stdout);
}

static void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage: sysview [options]\n\n"
        "Native macOS resource and process monitor.\n\n"
        "Options:\n"
        "  -i, --interval MS    Refresh interval (minimum %d; default 1000)\n"
        "  -n, --limit COUNT    Number of processes to display (default %d)\n"
        "  -s, --sort FIELD     Sort by cpu, mem, pid, or name (default cpu)\n"
        "      --compact        Use a dense live dashboard\n"
        "      --once           Print one report and exit\n"
        "      --json           Emit one JSON report and exit\n"
        "      --no-color       Disable terminal color\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n", MIN_INTERVAL_MS, DEFAULT_LIMIT);
}

static bool parse_positive(const char *value, int minimum, int *out) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < minimum || parsed > 3600000) return false;
    *out = (int)parsed;
    return true;
}

static int parse_args(int argc, char **argv, Options *options) {
    *options = (Options){ .interval_ms = 1000, .limit = DEFAULT_LIMIT, .sort = SORT_CPU };
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { print_usage(stdout); exit(0); }
        if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) { puts("sysview " VERSION); exit(0); }
        if (!strcmp(arg, "--once")) { options->once = true; continue; }
        if (!strcmp(arg, "--json")) { options->json = true; options->once = true; continue; }
        if (!strcmp(arg, "--compact")) { options->compact = true; continue; }
        if (!strcmp(arg, "--no-color")) { options->no_color = true; continue; }
        if (!strcmp(arg, "-i") || !strcmp(arg, "--interval")) {
            if (++i >= argc || !parse_positive(argv[i], MIN_INTERVAL_MS, &options->interval_ms)) return -1;
            continue;
        }
        if (!strcmp(arg, "-n") || !strcmp(arg, "--limit")) {
            if (++i >= argc || !parse_positive(argv[i], 1, &options->limit)) return -1;
            continue;
        }
        if (!strcmp(arg, "-s") || !strcmp(arg, "--sort")) {
            if (++i >= argc) return -1;
            if (!strcmp(argv[i], "cpu")) options->sort = SORT_CPU;
            else if (!strcmp(argv[i], "mem")) options->sort = SORT_MEM;
            else if (!strcmp(argv[i], "pid")) options->sort = SORT_PID;
            else if (!strcmp(argv[i], "name")) options->sort = SORT_NAME;
            else return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Options options;
    if (parse_args(argc, argv, &options) != 0) { print_usage(stderr); return 2; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    Snapshot previous = {0};
    read_cpu_ticks(&previous.ticks, previous.core_ticks, &previous.core_count);
    previous.timestamp = now_seconds();
    previous.processes = collect_processes(NULL, 0);
    usleep(200000);

    bool interactive = isatty(STDOUT_FILENO) && !options.once && !options.json;
    bool color = interactive && !options.no_color && getenv("NO_COLOR") == NULL;
    if (interactive) {
        fputs(ANSI_ALT_SCREEN ANSI_HIDE_CURSOR ANSI_MOUSE_ON, stdout);
        fflush(stdout);
    }
    while (running) {
        Snapshot current = {0};
        read_cpu_ticks(&current.ticks, current.core_ticks, &current.core_count);
        current.timestamp = now_seconds();
        double elapsed = current.timestamp - previous.timestamp;
        current.processes = collect_processes(&previous, elapsed);
        sort_processes(&current.processes, options.sort);
        double cpu = cpu_usage(&previous.ticks, &current.ticks);
        double core_usage[MAX_CPU_CORES] = {0};
        size_t core_count = previous.core_count < current.core_count ? previous.core_count : current.core_count;
        for (size_t i = 0; i < core_count; i++) {
            core_usage[i] = cpu_usage(&previous.core_ticks[i], &current.core_ticks[i]);
        }
        if (options.json) print_json(&current, cpu, &options);
        else if (options.compact) print_compact_screen(&current, cpu, &options, interactive, color);
        else print_screen(&current, cpu, &options, core_usage, core_count, interactive, color);
        free_snapshot(&previous);
        previous = current;
        if (options.once) break;

        struct timespec delay = { .tv_sec = options.interval_ms / 1000,
                                  .tv_nsec = (long)(options.interval_ms % 1000) * 1000000L };
        nanosleep(&delay, NULL);
    }
    free_snapshot(&previous);
    if (interactive) {
        fputs(ANSI_RESET ANSI_MOUSE_OFF ANSI_SHOW_CURSOR ANSI_MAIN_SCREEN, stdout);
        fflush(stdout);
    }
    return 0;
}
