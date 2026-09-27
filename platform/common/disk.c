#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>

static unsigned long long scaled_blocks(unsigned long long blocks, unsigned long long block_size) {
    if (block_size && blocks > ULLONG_MAX / block_size) return ULLONG_MAX;
    return blocks * block_size;
}

bool platform_disk_mount_add(DiskMountList *list, const char *mount_point) {
    if (!list || !mount_point || !*mount_point) return false;
    if (list->count >= MAX_DISK_MOUNTS) {
        list->truncated = true;
        list->partial = true;
        return false;
    }
    struct statvfs stats;
    if (statvfs(mount_point, &stats) != 0) {
        list->partial = true;
        return false;
    }
    size_t length = strlen(mount_point);
    if (length >= sizeof(list->items[0].mount_point)) {
        list->partial = true;
        return false;
    }
    unsigned long long block = stats.f_frsize ? (unsigned long long)stats.f_frsize :
                               (unsigned long long)stats.f_bsize;
    unsigned long long total = scaled_blocks((unsigned long long)stats.f_blocks, block);
    unsigned long long available = scaled_blocks((unsigned long long)stats.f_bavail, block);
    if (available > total) available = total;
    unsigned long long used = total - available;
    DiskMount *mount = &list->items[list->count++];
    memcpy(mount->mount_point, mount_point, length + 1);
    mount->total_bytes = total;
    mount->used_bytes = used;
    mount->available_bytes = available;
    mount->usage_percent = total ? 100.0 * (double)used / (double)total : 0.0;
    return true;
}

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
