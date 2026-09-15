#include "sampler.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    CpuTicks a = {.user=10, .system=20, .idle=70};
    CpuTicks b = {.user=20, .system=30, .idle=150};
    assert(fabs(cpu_usage(&a, &b) - 20) < .001);
    assert(isnan(cpu_usage(&b, &a)));
    assert(isnan(cpu_usage(&a, &a)));
    Process old[] = {{.pid=1, .start_id=100, .cpu_time=1000000000}};
    Process next[] = {{.pid=1, .start_id=100, .cpu_time=3000000000}};
    ProcessList before = {.items=old, .count=1}, after = {.items=next, .count=1};
    sample_process_usage(&after, &before, 1);
    assert(fabs(next[0].cpu_percent - 200) < .001); /* one busy core = 100% */
    next[0].start_id = 101;
    sample_process_usage(&after, &before, 1);
    assert(!isfinite(next[0].cpu_percent) || next[0].cpu_percent == 0);

    Process unordered[] = {{.pid=1, .cpu_percent=NAN}, {.pid=2, .cpu_percent=20},
                           {.pid=3, .cpu_percent=90}, {.pid=4, .cpu_percent=NAN}};
    ProcessList order = {.items=unordered, .count=4};
    sort_processes(&order, SORT_CPU);
    assert(unordered[0].pid == 3 && unordered[1].pid == 2);
    assert(isnan(unordered[2].cpu_percent) && isnan(unordered[3].cpu_percent));
    next[0].start_id = 100; next[0].cpu_time = 0;
    sample_process_usage(&after, &before, 1);
    assert(!isfinite(next[0].cpu_percent) || next[0].cpu_percent == 0);
    sample_process_usage(&after, &before, 0);
    assert(!isfinite(next[0].cpu_percent) || next[0].cpu_percent == 0);

    Snapshot previous = {.core_count=2, .core_ids={0,2}, .core_ticks={a,a}};
    Snapshot current = {.core_count=2, .core_ids={2,4}, .core_ticks={b,b}};
    double cores[MAX_CPU_CORES];
    assert(sample_core_usage(&previous, &current, cores) == 2);
    assert(fabs(cores[0] - 20) < .001 && isnan(cores[1]));
    current.cpu_status = METRIC_ERROR;
    assert(isnan(sample_cpu_usage(&previous, &current)));

    NetworkSnapshot n1 = {.count=1}, n2 = {.count=1};
    strcpy(n1.items[0].name, "eth0"); strcpy(n2.items[0].name, "eth0");
    n1.items[0].received = 100; n1.items[0].transmitted = 200;
    n2.items[0].received = 500; n2.items[0].transmitted = 800;
    sample_network_usage(&n2, &n1, 2);
    assert(n2.items[0].receive_rate == 200 && n2.items[0].transmit_rate == 300);
    n2.items[0].received = 0;
    sample_network_usage(&n2, &n1, 2);
    assert(!isfinite(n2.items[0].receive_rate) || n2.items[0].receive_rate == 0);
    strcpy(n2.items[0].name, "eth1");
    sample_network_usage(&n2, &n1, 2);
    assert(!isfinite(n2.items[0].transmit_rate) || n2.items[0].transmit_rate == 0);
    puts("Shared sampler tests passed");
    return 0;
}
