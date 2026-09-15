#ifndef LOUTRE_SAMPLER_H
#define LOUTRE_SAMPLER_H
#include "model.h"
double now_seconds(void);
/* Invalid/reset counters and unmatched cores return NAN, serialized as null. */
double cpu_usage(const CpuTicks *before, const CpuTicks *after);
double sample_cpu_usage(const Snapshot *before, const Snapshot *after);
size_t sample_core_usage(const Snapshot *before, const Snapshot *after, double *out);
void sample_process_usage(ProcessList *current, const ProcessList *previous, double elapsed);
void sample_network_usage(NetworkSnapshot *current, const NetworkSnapshot *previous, double elapsed);
Snapshot collect_snapshot(const Snapshot *previous);
NetworkSnapshot collect_networks(const NetworkSnapshot *previous, double elapsed);
SystemMetrics collect_system_metrics(void);
void free_snapshot(Snapshot *snapshot);
void sort_processes(ProcessList *list, SortMode sort);
#endif
