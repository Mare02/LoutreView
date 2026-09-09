#include <errno.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
#include <poll.h>
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
#include <termios.h>
#include <unistd.h>

#define VERSION "1.0.0"
#define DEFAULT_LIMIT 12
#define MIN_INTERVAL_MS 250
#define MAX_CPU_CORES 128
#define MAX_NETWORK_INTERFACES 64

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
typedef enum { VIEW_DASHBOARD, VIEW_NETWORKS } View;

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
    char name[IFNAMSIZ];
    bool up;
    unsigned long long received;
    unsigned long long transmitted;
    double receive_rate;
    double transmit_rate;
} NetworkInterface;

typedef struct {
    NetworkInterface items[MAX_NETWORK_INTERFACES];
    size_t count;
} NetworkSnapshot;

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
static struct termios original_terminal;
static bool terminal_configured = false;

static int terminal_width(void);
static int terminal_height(void);

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void restore_terminal(void) {
    if (terminal_configured) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_terminal);
        terminal_configured = false;
    }
}

static bool configure_terminal(void) {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &original_terminal) != 0) return false;

    struct termios terminal = original_terminal;
    terminal.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    terminal.c_cc[VMIN] = 0;
    terminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal) != 0) return false;
    terminal_configured = true;
    return true;
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

static NetworkSnapshot collect_networks(const NetworkSnapshot *previous, double elapsed) {
    NetworkSnapshot snapshot = {0};
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) return snapshot;

    for (struct ifaddrs *address = addresses; address && snapshot.count < MAX_NETWORK_INTERFACES;
         address = address->ifa_next) {
        if (!address->ifa_name || !address->ifa_data || !address->ifa_addr ||
            address->ifa_addr->sa_family != AF_LINK) continue;

        struct if_data *data = (struct if_data *)address->ifa_data;
        NetworkInterface *network = &snapshot.items[snapshot.count++];
        snprintf(network->name, sizeof(network->name), "%s", address->ifa_name);
        network->up = (address->ifa_flags & IFF_UP) != 0;
        network->received = data->ifi_ibytes;
        network->transmitted = data->ifi_obytes;

        if (previous && elapsed > 0) {
            for (size_t i = 0; i < previous->count; i++) {
                const NetworkInterface *old = &previous->items[i];
                if (strcmp(old->name, network->name) != 0) continue;
                if (network->received >= old->received) {
                    network->receive_rate = (double)(network->received - old->received) / elapsed;
                }
                if (network->transmitted >= old->transmitted) {
                    network->transmit_rate = (double)(network->transmitted - old->transmitted) / elapsed;
                }
                break;
            }
        }
    }
    freeifaddrs(addresses);
    return snapshot;
}

static void print_view_header(const char *view_name, int interval_ms, int width, bool color) {
    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("SYSVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    if (width < 80) {
        printf("  /  %s\n", view_name);
        if (color) fputs(ANSI_DIM, stdout);
        printf("1:dashboard · 2:networks   refresh %dms\n", interval_ms);
        if (color) fputs(ANSI_RESET, stdout);
    } else {
        printf("  /  %s   1:dashboard · 2:networks   refresh %dms\n",
               view_name, interval_ms);
    }
    if (color) fputs(ANSI_SLATE, stdout);
    for (int i = 0; i < width - 1; i++) fputs("─", stdout);
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');
}

static void print_compact_header(const char *view_name, int width, bool color) {
    if (color) fputs(ANSI_CYAN ANSI_BOLD, stdout);
    fputs("SYSVIEW", stdout);
    if (color) fputs(ANSI_RESET ANSI_DIM, stdout);
    if (width < 70) {
        printf("  /  %s\n", view_name);
        if (color) fputs(ANSI_DIM, stdout);
        fputs("1:dashboard · 2:networks\n", stdout);
        if (color) fputs(ANSI_RESET, stdout);
    } else {
        printf("  /  %s   1:dashboard · 2:networks\n", view_name);
    }
}

static int compare_network_usage(const void *left_value, const void *right_value) {
    const NetworkInterface *left = left_value;
    const NetworkInterface *right = right_value;
    if (left->up != right->up) return right->up - left->up;

    double left_rate = left->receive_rate + left->transmit_rate;
    double right_rate = right->receive_rate + right->transmit_rate;
    if (left_rate != right_rate) return right_rate > left_rate ? 1 : -1;
    return strcmp(left->name, right->name);
}

static void print_network_screen(const NetworkSnapshot *snapshot, const Options *options,
                                 bool clear, bool color) {
    int width = terminal_width();
    int height = terminal_height();
    NetworkSnapshot ordered = *snapshot;
    qsort(ordered.items, ordered.count, sizeof(ordered.items[0]), compare_network_usage);
    size_t active_count = 0;
    double receive_rate = 0.0;
    double transmit_rate = 0.0;
    for (size_t i = 0; i < snapshot->count; i++) {
        if (snapshot->items[i].up) active_count++;
        receive_rate += snapshot->items[i].receive_rate;
        transmit_rate += snapshot->items[i].transmit_rate;
    }
    char receive_total[16], transmit_total[16];
    format_compact_bytes((unsigned long long)receive_rate, receive_total, sizeof(receive_total));
    format_compact_bytes((unsigned long long)transmit_rate, transmit_total, sizeof(transmit_total));
    if (clear) fputs("\033[H\033[2J", stdout);
    print_view_header("NETWORKS", options->interval_ms, width, color);

    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    printf("  TRAFFIC  %zu active · %zu total    ↓ RX %s/s    ↑ TX %s/s\n",
           active_count, snapshot->count, receive_total, transmit_total);
    if (color) fputs(ANSI_RESET ANSI_SLATE, stdout);
    fputs("  ─────────────────────────────────────────────────────────────────────────\n", stdout);
    if (color) fputs(ANSI_RESET, stdout);

    size_t max_rows = snapshot->count;
    if (height > 6 && max_rows > (size_t)(height - 6)) max_rows = (size_t)(height - 6);
    size_t rendered_count = 0;
    for (int show_active = 1; show_active >= 0 && rendered_count < max_rows; show_active--) {
        for (size_t i = 0; i < ordered.count && rendered_count < max_rows; i++) {
            const NetworkInterface *network = &ordered.items[i];
            if ((network->up ? 1 : 0) != show_active) continue;
        char receive_rate[16], transmit_rate[16], received[16], transmitted[16];
        format_compact_bytes((unsigned long long)network->receive_rate, receive_rate, sizeof(receive_rate));
        format_compact_bytes((unsigned long long)network->transmit_rate, transmit_rate, sizeof(transmit_rate));
        format_compact_bytes(network->received, received, sizeof(received));
        format_compact_bytes(network->transmitted, transmitted, sizeof(transmitted));
        const char *status_color = color ? (network->up ? ANSI_GREEN : ANSI_RED) : "";
        const char *muted = color ? ANSI_DIM : "";
        if (width >= 78) {
            printf("  %s●%s %-10s  %s%-4s%s  %s↓%7s/s  ↑%7s/s%s   in %-8s out %-8s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", muted,
                   receive_rate, transmit_rate, color ? ANSI_RESET : "", received, transmitted);
        } else {
            printf("  %s●%s %-8s %s%-4s%s  ↓%s/s ↑%s/s  in%s out%s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate,
                   transmit_rate, received, transmitted);
        }
            rendered_count++;
        }
    }
    if (rendered_count < ordered.count) {
        if (color) fputs(ANSI_DIM, stdout);
        printf("  + %zu more interface%s below terminal height\n",
               ordered.count - rendered_count, ordered.count - rendered_count == 1 ? "" : "s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    if (snapshot->count == 0) puts("  No network interfaces available.");
    fflush(stdout);
}

static void print_compact_network_screen(const NetworkSnapshot *snapshot, bool clear, bool color) {
    int width = terminal_width();
    int height = terminal_height();
    NetworkSnapshot ordered = *snapshot;
    qsort(ordered.items, ordered.count, sizeof(ordered.items[0]), compare_network_usage);
    size_t active_count = 0;
    double receive_rate = 0.0;
    double transmit_rate = 0.0;
    for (size_t i = 0; i < ordered.count; i++) {
        if (ordered.items[i].up) active_count++;
        receive_rate += ordered.items[i].receive_rate;
        transmit_rate += ordered.items[i].transmit_rate;
    }
    char receive_total[16], transmit_total[16];
    format_compact_bytes((unsigned long long)receive_rate, receive_total, sizeof(receive_total));
    format_compact_bytes((unsigned long long)transmit_rate, transmit_total, sizeof(transmit_total));
    if (clear) fputs("\033[H\033[2J", stdout);

    print_compact_header("NETWORKS", width, color);
    if (color) fputs(ANSI_DIM, stdout);
    printf("  %zu/%zu up  ↓%s/s ↑%s/s\n", active_count, ordered.count,
           receive_total, transmit_total);
    if (color) fputs(ANSI_RESET, stdout);
    if (color) fputs(ANSI_SLATE, stdout);
    fputs("  ─────────────────────────────────────────────────────────────────────────\n", stdout);
    if (color) fputs(ANSI_RESET, stdout);

    size_t max_rows = ordered.count;
    if (height > 3 && max_rows > (size_t)(height - 3)) max_rows = (size_t)(height - 3);
    size_t rendered_count = 0;
    for (size_t i = 0; i < ordered.count && rendered_count < max_rows; i++) {
        const NetworkInterface *network = &ordered.items[i];
        char receive_rate_text[16], transmit_rate_text[16], received[16], transmitted[16];
        format_compact_bytes((unsigned long long)network->receive_rate, receive_rate_text,
                             sizeof(receive_rate_text));
        format_compact_bytes((unsigned long long)network->transmit_rate, transmit_rate_text,
                             sizeof(transmit_rate_text));
        format_compact_bytes(network->received, received, sizeof(received));
        format_compact_bytes(network->transmitted, transmitted, sizeof(transmitted));
        const char *status_color = color ? (network->up ? ANSI_GREEN : ANSI_RED) : "";
        if (width >= 72) {
            printf("  %s●%s %-8s %s%-4s%s ↓%7s/s ↑%7s/s  in %-7s out %-7s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate_text,
                   transmit_rate_text, received, transmitted);
        } else {
            printf("  %s●%s %-8s %s%-4s%s ↓%s/s ↑%s/s\n",
                   status_color, color ? ANSI_RESET : "", network->name, status_color,
                   network->up ? "UP" : "DOWN", color ? ANSI_RESET : "", receive_rate_text,
                   transmit_rate_text);
        }
        rendered_count++;
    }
    if (rendered_count < ordered.count) {
        if (color) fputs(ANSI_DIM, stdout);
        printf("  + %zu more interface%s\n", ordered.count - rendered_count,
               ordered.count - rendered_count == 1 ? "" : "s");
        if (color) fputs(ANSI_RESET, stdout);
    }
    fflush(stdout);
}

static View handle_input(View current) {
    unsigned char input[32];
    ssize_t received = read(STDIN_FILENO, input, sizeof(input));
    if (received <= 0) return current;
    for (ssize_t i = 0; i < received; i++) {
        if (input[i] == '1') {
            current = VIEW_DASHBOARD;
        } else if (input[i] == '2' || input[i] == 'n' || input[i] == 'N') {
            current = VIEW_NETWORKS;
        } else if (input[i] == '\t') {
            current = current == VIEW_DASHBOARD ? VIEW_NETWORKS : VIEW_DASHBOARD;
        }
    }
    return current;
}

static View wait_for_input(View current, int timeout_ms) {
    struct pollfd descriptor = { .fd = STDIN_FILENO, .events = POLLIN };
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR && running);
    if (result > 0 && (descriptor.revents & POLLIN)) return handle_input(current);
    return current;
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

static void print_compact_process_header(bool color) {
    if (color) fputs(ANSI_BOLD, stdout);
    printf("  %-5s  %5s  %6s  %s", "PID", "CPU%", "MEM", "PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
}

static void print_compact_process_row(const Process *process, int name_width, bool color) {
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

static void print_wide_core_cell(const double *cores, size_t index, size_t count, bool color) {
    if (index >= count) return;
    if (color) fputs(ANSI_SLATE, stdout);
    printf("  CORE %02zu ", index + 1);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(cores[index], 6, color);
    printf(" %4.1f%%", cores[index]);
}

static void print_wide_short_panel(const Snapshot *snapshot, const double *cores, size_t core_count,
                                   int width, int core_columns, int process_rows, bool color) {
    const int core_panel_width = core_columns == 1 ? 22 : 48;
    const int left_width = core_columns == 1 ? 26 : 50;
    const size_t core_rows = (core_count + (size_t)core_columns - 1) / (size_t)core_columns;
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
    if (color) fputs(ANSI_RESET, stdout);
    putchar('\n');

    size_t rows = core_rows > process_count + 1 ? core_rows : process_count + 1;
    for (size_t row = 0; row < rows; row++) {
        for (int column = 0; column < core_columns; column++) {
            size_t index = row * (size_t)core_columns + (size_t)column;
            if (index < core_count) print_wide_core_cell(cores, index, core_count, color);
            else print_spaces(22);
            if (column + 1 < core_columns) print_spaces(4);
        }
        print_spaces(left_width - core_panel_width);

        if (row == 0) {
            print_compact_process_header(color);
        } else if (row - 1 < process_count) {
            const Process *process = &snapshot->processes.items[row - 1];
            print_compact_process_row(process, name_width, color);
        }
        putchar('\n');
    }
}

static void print_compact_metric_label(const char *label, const char *accent, bool color) {
    if (color) {
        fputs(accent, stdout);
        fputs(ANSI_BOLD, stdout);
    }
    fputs(label, stdout);
    if (color) fputs(ANSI_RESET, stdout);
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

    print_compact_header("DASHBOARD", width, color);

    if (width >= 78) {
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        printf(" %.0f%%  ", cpu);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else if (width >= 60) {
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        printf(" %.0f%%  ", cpu);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        printf(" %s/%s  ", memory_used, memory_total);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        printf(" %s/%s", disk_used, disk_total);
        if (metrics.battery.available && width >= 70) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    } else {
        print_compact_metric_label("CPU", ANSI_CYAN, color);
        printf(" %.0f%%  ", cpu);
        print_compact_metric_label("MEM", ANSI_TEAL, color);
        printf(" %.0f%%  ", memory_percent);
        print_compact_metric_label("DISK", ANSI_AMBER, color);
        printf(" %.0f%%", disk_percent);
        if (metrics.battery.available) {
            printf("  ");
            print_compact_metric_label("BAT", ANSI_AMBER, color);
            printf(" %d%%", metrics.battery.percent);
        }
        putchar('\n');
    }

    printf("load %.2f · %.2f · %.2f", metrics.loads[0], metrics.loads[1], metrics.loads[2]);
    if (width >= 60) printf("  · up %s", uptime);
    if (width >= 78) printf("  · pressure %s", pressure);
    putchar('\n');

    bool show_memory = width >= 60;
    if (show_memory) print_compact_process_header(color);
    else {
        if (color) fputs(ANSI_BOLD, stdout);
        printf("  %-5s  %5s  %s", "PID", "CPU%", "PROCESS");
        if (color) fputs(ANSI_RESET, stdout);
    }
    putchar('\n');
    size_t count = snapshot->processes.count < (size_t)options->limit ? snapshot->processes.count : (size_t)options->limit;
    int name_width = width - (show_memory ? 24 : 17);
    if (name_width < 1) name_width = 1;
    for (size_t i = 0; i < count; i++) {
        const Process *process = &snapshot->processes.items[i];
        if (show_memory) {
            print_compact_process_row(process, name_width, color);
        } else {
            const char *process_color = process->cpu_percent >= 70.0 ? ANSI_RED :
                                        process->cpu_percent >= 25.0 ? ANSI_AMBER : ANSI_RESET;
            printf("  %-5d  ", process->pid);
            if (color) fputs(process_color, stdout);
            printf("%5.1f", process->cpu_percent);
            if (color) fputs(ANSI_RESET, stdout);
            fputs("  ", stdout);
            print_process_name(process->name, name_width);
        }
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

    print_view_header("DASHBOARD", options->interval_ms, width, color);

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

    int standard_core_columns = width >= 92 ? 3 : width >= 62 ? 2 : 1;
    size_t standard_core_rows = (core_count + (size_t)standard_core_columns - 1) /
                                (size_t)standard_core_columns;
    size_t process_count = snapshot->processes.count < (size_t)options->limit ?
                           snapshot->processes.count : (size_t)options->limit;
    int metric_rows = 5 + (metrics.battery.available ? 1 : 0);
    int standard_rows = metric_rows + 3 + (int)standard_core_rows + 2 + (int)process_count;
    int side_core_columns = width >= 78 ? 2 : 1;
    size_t side_core_rows = (core_count + (size_t)side_core_columns - 1) /
                            (size_t)side_core_columns;
    bool use_side_panel = width >= 62 && height > 0 && height < standard_rows &&
                          height >= metric_rows + 2 + (int)side_core_rows;
    if (use_side_panel) {
        int side_process_rows = height - metric_rows - 3;
        if (side_process_rows > 8) side_process_rows = 8;
        if (side_process_rows < 1) side_process_rows = 1;
        print_wide_short_panel(snapshot, core_usage, core_count, width, side_core_columns,
                               side_process_rows, color);
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
        configure_terminal();
        fputs(ANSI_ALT_SCREEN ANSI_HIDE_CURSOR ANSI_MOUSE_ON, stdout);
        fflush(stdout);
    }
    View view = VIEW_DASHBOARD;
    NetworkSnapshot previous_networks = {0};
    bool have_previous_networks = false;
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
        NetworkSnapshot networks = collect_networks(have_previous_networks ? &previous_networks : NULL,
                                                    elapsed);
        if (options.json) print_json(&current, cpu, &options);
        else if (view == VIEW_NETWORKS && options.compact) {
            print_compact_network_screen(&networks, interactive, color);
        }
        else if (view == VIEW_NETWORKS) print_network_screen(&networks, &options, interactive, color);
        else if (options.compact) print_compact_screen(&current, cpu, &options, interactive, color);
        else print_screen(&current, cpu, &options, core_usage, core_count, interactive, color);
        free_snapshot(&previous);
        previous = current;
        previous_networks = networks;
        have_previous_networks = true;
        if (options.once) break;
        if (interactive) view = wait_for_input(view, options.interval_ms);
        else {
            struct timespec delay = { .tv_sec = options.interval_ms / 1000,
                                      .tv_nsec = (long)(options.interval_ms % 1000) * 1000000L };
            nanosleep(&delay, NULL);
        }
    }
    free_snapshot(&previous);
    if (interactive) {
        fputs(ANSI_RESET ANSI_MOUSE_OFF ANSI_SHOW_CURSOR ANSI_MAIN_SCREEN, stdout);
        fflush(stdout);
        restore_terminal();
    }
    return 0;
}
