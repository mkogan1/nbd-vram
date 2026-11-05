#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ublk-guest-guard.h"

struct ublksrv_ctrl_dev;

int ublksrv_ctrl_stop_dev(struct ublksrv_ctrl_dev *dev)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned int calls;
    static int authorized;
    unsigned int call;

    pthread_mutex_lock(&lock);
    if (!calls) authorized = ublk_guest_guard() == 0;
    call = ++calls;
    pthread_mutex_unlock(&lock);

    if (!authorized) return -EPERM;

    const char *lost = getenv("VRAM_TEST_STOP_LOST_REPLY");
    int lose_reply = lost && !strcmp(lost, "1");
    if (call == 1 && !lose_reply) {
        fprintf(stderr, "UBLK_STOP_FAULT synthetic=1 call=1\n");
        return -EIO;
    }

    typedef int (*stop_fn)(struct ublksrv_ctrl_dev *);
    stop_fn real_stop = (stop_fn)dlsym(RTLD_NEXT, "ublksrv_ctrl_stop_dev");
    if (!real_stop) {
        fprintf(stderr, "UBLK_STOP_FAULT: dlsym failed: %s\n", dlerror());
        return -ENOSYS;
    }
    if (call > 1) fprintf(stderr, "UBLK_STOP_FAULT_RETRY call=%u\n", call);
    int result = real_stop(dev);
    if (call == 1 && lose_reply && result >= 0) {
        fprintf(stderr, "UBLK_STOP_REPLY_LOST synthetic=1 kernel_result=%d\n", result);
        return -EIO;
    }
    return result;
}
