#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>

#include "ublksrv.h"
#include "ublk-guest-guard.h"

int ublksrv_ctrl_set_params(struct ublksrv_ctrl_dev *dev,
                            struct ublk_params *params)
{
    (void)dev;
    (void)params;
    if (ublk_guest_guard() != 0)
        return -EPERM;
    fprintf(stderr, "UBLK_START_FAULT synthetic=1 stage=set_params\n");
    return -EIO;
}

int ublksrv_ctrl_stop_dev(struct ublksrv_ctrl_dev *dev)
{
    if (ublk_guest_guard() != 0)
        return -EPERM;

    fprintf(stderr, "UBLK_START_FAULT_STOP_FORWARD\n");
    typedef int (*stop_fn)(struct ublksrv_ctrl_dev *);
    stop_fn real_stop = (stop_fn)dlsym(RTLD_NEXT, "ublksrv_ctrl_stop_dev");
    if (!real_stop) {
        fprintf(stderr, "UBLK_START_FAULT_STOP_FORWARD: dlsym failed: %s\n",
                dlerror());
        return -ENOSYS;
    }
    return real_stop(dev);
}
