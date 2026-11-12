#!/bin/sh
set -eu
PATH=/bin:/usr/bin
export PATH
/usr/bin/guest-check positive >/dev/null 2>&1 || exit 1
case " $(cat /proc/cmdline) " in
    *' vram_isolated_guest=1 '*) ;;
    *) exit 1 ;;
esac
case "${1:-}" in
    pre)
        test "$(systemctl show vram-swap-ublk.service --property=MainPID --value)" = 0
        test ! -b /dev/ublkb0
        /bin/busybox rmmod nvkvm_guest
        echo INTEGRATION_NVKVM_QUIESCED
        ;;
    post)
        /bin/busybox insmod /modules/nvkvm.ko
        test -c /dev/nvidiactl && test -c /dev/nvidia0 && test -c /dev/nvidia-uvm
        echo INTEGRATION_NVKVM_RELOADED
        ;;
    *) exit 1 ;;
esac
