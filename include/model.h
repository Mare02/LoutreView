#ifndef LOUTRE_MODEL_H
#define LOUTRE_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define LOUTRE_PATH_MAX 4096
#define MAX_CPU_CORES 128
#define MAX_NETWORK_INTERFACES 64
#define DEFAULT_LIMIT 12
#define MIN_INTERVAL_MS 250

typedef enum { METRIC_OK, METRIC_UNAVAILABLE, METRIC_PERMISSION, METRIC_ERROR } MetricStatus;
typedef enum { SORT_CPU, SORT_MEM, SORT_PID, SORT_NAME } SortMode;
typedef enum { VIEW_DASHBOARD, VIEW_NETWORKS, VIEW_USAGE } View;
/* Backend-normalized cumulative counters. Busy includes steal and IRQ on Linux;
 * idle includes iowait. Guest time is already included in user/nice. */
typedef struct { unsigned long long user, system, idle, nice; } CpuTicks;
typedef struct {
    pid_t pid;
    char name[LOUTRE_PATH_MAX], path[LOUTRE_PATH_MAX];
    unsigned long long resident, virtual_size, cpu_time; /* bytes, bytes, nanoseconds */
    unsigned long long start_id; /* precise backend identity; protects against PID reuse */
    int threads;
    double cpu_percent;
    time_t start_time;
} Process;
typedef struct { Process *items; size_t count; MetricStatus status; } ProcessList;
typedef struct {
    bool available;
    int percent;
    bool charging, charged;
    int time_remaining_minutes;
    char state[16];
} BatteryInfo;
typedef struct {
    unsigned long long memory_total, memory_used, pressure, disk_total, disk_used;
    double loads[3], uptime;
    BatteryInfo battery;
    MetricStatus memory_status, disk_status, load_status;
    bool pressure_available, pressure_percent_available;
    double pressure_percent;
    const char *pressure_source;
} SystemMetrics;
typedef struct {
    CpuTicks ticks, core_ticks[MAX_CPU_CORES];
    unsigned core_ids[MAX_CPU_CORES];
    size_t core_count;
    double timestamp;
    ProcessList processes;
    MetricStatus cpu_status;
} Snapshot;
typedef struct {
    char name[64];
    bool up;
    unsigned long long received, transmitted;
    double receive_rate, transmit_rate;
} NetworkInterface;
typedef struct {
    NetworkInterface items[MAX_NETWORK_INTERFACES];
    size_t count;
    MetricStatus status;
    bool truncated;
} NetworkSnapshot;
typedef struct {
    int interval_ms, limit;
    bool once, json, json_stream, include_usage, compact, startup, usage_ingest;
    char usage_provider[32];
    SortMode sort;
} Options;
typedef enum {
    STARTUP_AGENT, STARTUP_DAEMON, STARTUP_LOGIN_ITEM,
    STARTUP_SYSTEM_SERVICE, STARTUP_USER_SERVICE, STARTUP_DESKTOP_AUTOSTART
} StartupKind;
typedef struct {
    StartupKind kind;
    char name[256], path[LOUTRE_PATH_MAX], owner[64];
    pid_t pid;
    unsigned long long resident;
    double cpu_percent;
    time_t start_time;
    bool path_missing, memory_known;
    bool running_known, running, enabled_known, enabled;
    char state[32];
} StartupItem;
typedef struct {
    StartupItem *items;
    size_t count, capacity;
    MetricStatus status;
    bool partial;
} StartupList;
#endif
