#include <errno.h>
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

static bool get_disk(unsigned long long *total, unsigned long long *used) {
    struct statfs stats;
    if (statfs("/", &stats) != 0) return false;
    *total = (unsigned long long)stats.f_blocks * stats.f_bsize;
    *used = (unsigned long long)(stats.f_blocks - stats.f_bavail) * stats.f_bsize;
    return true;
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
    unsigned long long memory_total = 0, memory_used = 0, pressure = 0, disk_total = 0, disk_used = 0;
    get_memory(&memory_total, &memory_used, &pressure);
    get_disk(&disk_total, &disk_used);
    double loads[3] = {0}; getloadavg(loads, 3);
    printf("{\"cpu\":{\"usage_percent\":%.1f,\"load\":[%.2f,%.2f,%.2f]},", cpu, loads[0], loads[1], loads[2]);
    printf("\"memory\":{\"total_bytes\":%llu,\"used_bytes\":%llu,\"pressure_bytes\":%llu},", memory_total, memory_used, pressure);
    double uptime = get_uptime();
    printf("\"disk\":{\"total_bytes\":%llu,\"used_bytes\":%llu},\"uptime_seconds\":", disk_total, disk_used);
    if (uptime < 0) fputs("null", stdout); else printf("%.0f", uptime);
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

static void print_screen(const Snapshot *snapshot, double cpu, const Options *options,
                         const double *core_usage, size_t core_count,
                         bool clear, bool color) {
    unsigned long long memory_total = 0, memory_used = 0, pressure = 0, disk_total = 0, disk_used = 0;
    get_memory(&memory_total, &memory_used, &pressure);
    get_disk(&disk_total, &disk_used);
    double loads[3] = {0}; getloadavg(loads, 3);
    char memory_used_text[24], memory_total_text[24], pressure_text[24], disk_used_text[24], disk_total_text[24], uptime[32];
    format_bytes(memory_used, memory_used_text, sizeof(memory_used_text));
    format_bytes(memory_total, memory_total_text, sizeof(memory_total_text));
    format_bytes(pressure, pressure_text, sizeof(pressure_text));
    format_bytes(disk_used, disk_used_text, sizeof(disk_used_text));
    format_bytes(disk_total, disk_total_text, sizeof(disk_total_text));
    format_uptime(get_uptime(), uptime, sizeof(uptime));
    double memory_percent = memory_total ? 100.0 * (double)memory_used / memory_total : 0.0;
    double disk_percent = disk_total ? 100.0 * (double)disk_used / disk_total : 0.0;
    int width = terminal_width();
    int bar_width = width >= 100 ? 28 : width >= 78 ? 18 : 10;
    bool roomy_metrics = width >= 100;
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
    printf("  Load %.2f · %.2f · %.2f\n", loads[0], loads[1], loads[2]);
    if (roomy_metrics) putchar('\n');

    if (color) fputs(ANSI_TEAL ANSI_BOLD, stdout);
    printf("MEM  %5.1f%% ", memory_percent);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(memory_percent, bar_width, color);
    printf("  %s / %s  %spressure %s%s\n", memory_used_text, memory_total_text,
           color ? ANSI_DIM : "", pressure_text, color ? ANSI_RESET : "");
    if (roomy_metrics) putchar('\n');

    if (color) fputs(ANSI_AMBER ANSI_BOLD, stdout);
    printf("DISK %5.1f%% ", disk_percent);
    if (color) fputs(ANSI_RESET, stdout);
    print_bar(disk_percent, bar_width, color);
    printf("  %s / %s  %suptime %s%s\n", disk_used_text, disk_total_text,
           color ? ANSI_DIM : "", uptime, color ? ANSI_RESET : "");

    print_core_grid(core_usage, core_count, width, color);

    if (color) fputs(ANSI_BOLD, stdout);
    printf("  PID    CPU%%     MEM  THR  PROCESS");
    if (color) fputs(ANSI_RESET, stdout);
    printf("  %s(sorted by %s)%s\n", color ? ANSI_DIM : "",
           options->sort == SORT_CPU ? "CPU" : options->sort == SORT_MEM ? "memory" : options->sort == SORT_PID ? "PID" : "name",
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
