#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "linux.h"
#include "platform.h"
#include <errno.h>
#include <mntent.h>
#include <stdio.h>

DiskMountList platform_disks(void) {
    DiskMountList list = { .status = METRIC_UNAVAILABLE };
    FILE *mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) {
        list.status = linux_errno_status();
        return list;
    }

    struct mntent *entry;
    while ((entry = getmntent(mounts)) != NULL) {
        if (!platform_disk_mount_add(&list, entry->mnt_dir) && list.count >= MAX_DISK_MOUNTS)
            break;
    }
    if (ferror(mounts)) list.partial = true;
    endmntent(mounts);
    list.status = list.count ? METRIC_OK : METRIC_ERROR;
    return list;
}
