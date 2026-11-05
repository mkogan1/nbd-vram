#!/bin/sh
set -eu
PATH=/bin:/usr/bin
export PATH

# This guest-only guard must run before systemd, procfs, or runtime actions.
/usr/bin/guest-check positive >/dev/null 2>&1 || exit 1
case " $(cat /proc/cmdline) " in
    *' vram_isolated_guest=1 '*) ;;
    *) exit 1 ;;
esac
case " $(cat /proc/cmdline) " in
    *' integration_nvidia_ordering=1 '*) ;;
    *) exit 1 ;;
esac

case "${1:-}" in
    pre)
        /bin/busybox touch /run/nvidia-ordering-pre-entered
        echo INTEGRATION_NVIDIA_PRE_ENTERED
        test "$(systemctl show vram-swap-ublk.service --property=MainPID --value)" = 0
        if grep -E '/dev/ublk[b-c]' /proc/swaps >/dev/null; then exit 1; fi
        for device in /sys/class/block/ublkb* /dev/ublkb* /dev/ublkc*; do
            test ! -e "$device" || exit 1
        done
        /bin/busybox rm -f /run/nvidia-ordering-ready
        /bin/busybox touch /run/nvidia-ordering-was-suspended
        echo INTEGRATION_NVIDIA_PRE_EVACUATED
        ;;
    post)
        sleep 0.3
        /bin/busybox touch /run/nvidia-ordering-ready
        echo INTEGRATION_NVIDIA_RESUME_READY
        ;;
    main-check)
        if test -e /run/nvidia-ordering-was-suspended; then
            test -e /run/nvidia-ordering-ready || exit 1
        fi
        ;;
    *) exit 1 ;;
esac
