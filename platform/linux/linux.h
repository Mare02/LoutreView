#ifndef LOUTRE_LINUX_H
#define LOUTRE_LINUX_H
#include "platform.h"
#include <stdio.h>
bool linux_parse_cpu(const char *line, CpuTicks *out, unsigned *id, bool *aggregate);
bool linux_parse_process(const char *line, long ticks, long page_size, time_t boot, Process *out);
bool linux_parse_memory(FILE *file, SystemMetrics *out);
bool linux_parse_network(const char *line, NetworkInterface *out);
bool linux_read_battery(const char *root, BatteryInfo *out);
MetricStatus linux_errno_status(void);
#endif
