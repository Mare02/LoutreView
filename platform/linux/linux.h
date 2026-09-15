#ifndef LOUTRE_LINUX_H
#define LOUTRE_LINUX_H
#include "platform.h"
#include <stdio.h>
bool linux_parse_cpu(const char *line, CpuTicks *out, unsigned *id, bool *aggregate);
bool linux_parse_process(const char *line, long ticks, long page_size, time_t boot, Process *out);
bool linux_parse_memory(FILE *file, SystemMetrics *out);
bool linux_parse_network(const char *line, NetworkInterface *out);
bool linux_read_battery(const char *root, BatteryInfo *out);
MetricStatus linux_collect_cpu(const char *stat_path, Snapshot *out);
MetricStatus linux_collect_memory(const char *meminfo_path, const char *pressure_path,
                                  SystemMetrics *out);
ProcessList linux_collect_processes(const char *proc_root, long ticks, long page_size);
NetworkSnapshot linux_collect_networks(const char *dev_path, const char *sys_net_root);
MetricStatus linux_errno_status(void);
#endif
