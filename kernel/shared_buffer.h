#ifndef SHARED_BUFFER_H
#define SHARED_BUFFER_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct shadowmap_region {
    __u64 target_va_start; /* игнорируется, используем 0 */
    __u32 target_pid;
    __u32 size;           /* игнорируется, используем фиксированное окно */
};

#define SHADOWMAP_DEVICE_NAME "shadowmap"
#define IOCTL_SHADOWMAP_REGISTER _IOW('k', 0, struct shadowmap_region)

#endif
