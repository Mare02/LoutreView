#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include <libproc.h>
#include <sys/proc_info.h>
#include <math.h>

static unsigned long long process_cpu_time(const struct proc_taskinfo *task) {
    return task->pti_total_user + task->pti_total_system;
}

ProcessList platform_processes(void) {
    ProcessList list = { .status = METRIC_UNAVAILABLE };
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
        struct proc_taskallinfo info;
        int received = proc_pidinfo(pids[i], PROC_PIDTASKALLINFO, 0, &info, sizeof(info));
        if (received != sizeof(info)) continue;
        const struct proc_taskinfo task = info.ptinfo;

        Process *process = &list.items[list.count++];
        process->pid = pids[i];
        process->resident = task.pti_resident_size;
        process->virtual_size = task.pti_virtual_size;
        process->threads = task.pti_threadnum;
        process->cpu_time = process_cpu_time(&task);
        process->cpu_percent = NAN;
        if (proc_name(pids[i], process->name, sizeof(process->name)) <= 0) {
            snprintf(process->name, sizeof(process->name), "<unknown>");
        }
        if (proc_pidpath(pids[i], process->path, sizeof(process->path)) <= 0) {
            process->path[0] = '\0';
        }
        process->start_time = (time_t)info.pbsd.pbi_start_tvsec;
        process->start_id = (unsigned long long)info.pbsd.pbi_start_tvsec * 1000000ULL + info.pbsd.pbi_start_tvusec;
    }
    free(pids);
    list.status = METRIC_OK;
    return list;
}
