#include "platform.h"
#include <errno.h>
#include <sys/statvfs.h>

MetricStatus platform_disk(SystemMetrics *out) {
    out->disk_total = out->disk_used = 0;
    out->disk_status = METRIC_UNAVAILABLE;
    struct statvfs stats;
    if (statvfs("/", &stats) != 0) {
        out->disk_status = errno == EACCES || errno == EPERM ? METRIC_PERMISSION : METRIC_ERROR;
        return out->disk_status;
    }
    unsigned long long block = stats.f_frsize ? stats.f_frsize : stats.f_bsize;
    out->disk_total = (unsigned long long)stats.f_blocks * block;
    out->disk_used = stats.f_blocks >= stats.f_bavail ?
        (unsigned long long)(stats.f_blocks - stats.f_bavail) * block : 0;
    return out->disk_status = METRIC_OK;
}
