#!/bin/sh
set -eu

PROGRESS_FILE=/run/vram-swap-ublk-sleep-in-progress
RESTORE_FILE=/run/vram-swap-ublk-restore
SERVICE=vram-swap-ublk.service

exec 9>/run/nbd-vram-transition.lock
flock -x 9

case "${1:-}" in
    pre)
        touch "$PROGRESS_FILE"
        state=$(systemctl show "$SERVICE" --property=ActiveState --value)
        case "$state" in
            active|activating|reloading|deactivating)
                touch "$RESTORE_FILE"
                systemctl stop "$SERVICE"
                result=$(systemctl show "$SERVICE" --property=Result --value)
                [ "$result" = success ] || { echo "swap stop failed: $result" >&2; exit 1; }
                ;;
            inactive|failed) ;;
            *) echo "unknown swap service state: $state" >&2; exit 1 ;;
        esac
        pid=$(systemctl show "$SERVICE" --property=MainPID --value)
        [ "$pid" = 0 ] || { echo "swap daemon remains; refusing sleep" >&2; exit 1; }
        # shortcut: reject every remaining ublk disk until ownership tracking is available.
        for device in /sys/class/block/ublkb*; do
            [ ! -e "$device" ] || { echo "ublk disk remains; refusing sleep: $device" >&2; exit 1; }
        done
        ;;
    post)
        rm -f "$PROGRESS_FILE"
        if [ -e "$RESTORE_FILE" ]; then
            systemctl start "$SERVICE"
            state=$(systemctl show "$SERVICE" --property=ActiveState --value)
            [ "$state" = active ] || { echo "swap service did not start: $state" >&2; exit 1; }
            rm -f "$RESTORE_FILE"
        fi
        ;;
    *) echo "usage: $0 {pre|post}" >&2; exit 1 ;;
esac
