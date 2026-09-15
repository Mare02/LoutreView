#ifndef LOUTRE_PLATFORM_H
#define LOUTRE_PLATFORM_H
#include "model.h"

/* Exactly one backend is linked at build time. No OS headers leak into the core.
 * Outputs are initialized by collectors; owned arrays are released with free().
 * Partial process inventories are permitted when individual PIDs disappear. */
const char *platform_name(void);
MetricStatus platform_cpu(Snapshot *out);
MetricStatus platform_memory(SystemMetrics *out);
double platform_uptime(void);
bool platform_battery(BatteryInfo *out);
ProcessList platform_processes(void);
NetworkSnapshot platform_networks(void);
StartupList platform_startup(void);

/* Shared POSIX disk implementation. */
MetricStatus platform_disk(SystemMetrics *out);
#endif
