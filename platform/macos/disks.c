#include "platform.h"
#include <errno.h>
#include <sys/mount.h>

DiskMountList platform_disks(void) {
    DiskMountList list = { .status = METRIC_UNAVAILABLE };
    struct statfs *mounts = NULL;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0 || !mounts) {
        list.status = errno == EACCES || errno == EPERM ? METRIC_PERMISSION : METRIC_ERROR;
        return list;
    }

    for (int i = 0; i < count; i++) {
        if (!platform_disk_mount_add(&list, mounts[i].f_mntonname) &&
            list.count >= MAX_DISK_MOUNTS) break;
    }
    list.status = list.count ? METRIC_OK : METRIC_ERROR;
    return list;
}
