#!/bin/sh
set -eu
PATH=/bin:/usr/bin
export PATH

SERVICE=vram-swap-ublk.service
HELPER=/usr/local/libexec/vram-swap/vram-swap-ublk-sleep.sh

fail()
{
    echo "INTEGRATION_FAIL $*" >&2
    /bin/busybox poweroff -f
    exit 1
}

# This gate must precede every systemd, sysfs, cgroup, and poweroff action.
/usr/bin/guest-check positive >/dev/null 2>&1 || exit 1
case " $(cat /proc/cmdline) " in
    *' vram_isolated_guest=1 '*) ;;
    *) exit 1 ;;
esac

show()
{
    systemctl show "$SERVICE" --property="$1" --value
}

check_active()
{
    state=$(show ActiveState) || fail systemd_show_active
    pid=$(show MainPID) || fail systemd_show_pid
    [ "$state" = active ] && case "$pid" in ''|*[!0-9]*|0) false ;; *) true ;; esac || fail "service_not_active state=$state pid=$pid"
}

check_swap()
{
    grep -F '/dev/ublkb0' /proc/swaps >/dev/null || fail swap_missing
}

check_inactive()
{
    state=$(show ActiveState) || fail systemd_show_inactive
    pid=$(show MainPID) || fail systemd_show_pid
    result=$(show Result) || fail systemd_show_result
    [ "$state" = inactive ] && [ "$pid" = 0 ] && [ "$result" = success ] || fail "service_stop state=$state pid=$pid result=$result"
    for device in /sys/class/block/ublkb* /dev/ublkb* /dev/ublkc*; do
        [ ! -e "$device" ] || fail "ublk_node_remains=$device"
    done
    if grep -F '/dev/ublkb0' /proc/swaps >/dev/null; then fail swap_remains; fi
}

check_active_swap()
{
    check_active
    check_swap
}

check_notify()
{
    type=$(show Type) || fail systemd_show_type
    [ "$type" = notify ] || fail "service_type=$type"
}

check_notify
check_active_swap

if ! grep -F ' /sys/fs/cgroup cgroup2 ' /proc/mounts >/dev/null; then
    mkdir -p /sys/fs/cgroup || fail cgroup_root
    mount -t cgroup2 none /sys/fs/cgroup || fail cgroup_mount
fi
if ! grep -qw memory /sys/fs/cgroup/cgroup.controllers; then fail memory_controller_unavailable; fi
if ! grep -qw memory /sys/fs/cgroup/cgroup.subtree_control; then
    echo +memory > /sys/fs/cgroup/cgroup.subtree_control || fail memory_controller_enable
fi
mkdir /sys/fs/cgroup/pageout || fail pageout_cgroup

[ ! -e /tmp/pageout-ready ] && [ ! -e /tmp/pageout-resume ] || fail pageout_marker_exists
pressure=$(cat /swap-pressure-mib) || fail swap_pressure_value
VRAM_PAGEOUT_BARRIER=1 /bin/sh -c 'echo $$ > /sys/fs/cgroup/pageout/cgroup.procs; exec /usr/bin/swap-pressure "$1"' sh "$pressure" &
helper=$!
i=0
while [ ! -f /tmp/pageout-ready ]; do
    kill -0 "$helper" 2>/dev/null || fail pageout_helper_exited
    [ "$i" -lt 200 ] || fail pageout_timeout
    sleep 0.1
    i=$((i + 1))
done
kill -0 "$helper" 2>/dev/null || fail pageout_helper_exited
echo INTEGRATION_SYSTEMD_SWAPPED

if [ "$(cat /sleep-hook-failure)" = 1 ]; then
    old_pid=$(show MainPID) || fail failed_hook_old_pid
    dmesg > /tmp/sleep-dmesg-before || fail failed_hook_dmesg_before
    before=$(grep -c 'PM: suspend entry' /tmp/sleep-dmesg-before || true)
    if systemctl start suspend.target; then fail failed_hook_allowed_sleep; fi
    dmesg > /tmp/sleep-dmesg-after || fail failed_hook_dmesg_after
    after=$(grep -c 'PM: suspend entry' /tmp/sleep-dmesg-after || true)
    [ "$before" = "$after" ] || fail failed_hook_kernel_slept
    printf '\n%s\n' INTEGRATION_SLEEP_ENTRY_COUNT_UNCHANGED
    nvidia_ordering=$(cat /nvidia-ordering) || fail nvidia_ordering_flag_missing
    case "$nvidia_ordering" in 0|1) ;; *) fail nvidia_ordering_flag_invalid ;; esac
    if [ "$nvidia_ordering" = 1 ]; then
        [ "$(systemctl show vram-swap-ublk-suspend.service --property=Result --value)" = exit-code ] || fail failed_hook_not_executed
        [ "$(systemctl show nvidia-suspend.service --property=LoadState --value)" = loaded ] || fail nvidia_fixture_missing
        case " $(systemctl show nvidia-suspend.service --property=Requires --value) " in
            *' vram-swap-ublk-suspend.service '*) ;;
            *) fail nvidia_required_hook_missing ;;
        esac
        if systemctl start nvidia-suspend.service; then fail failed_hook_allowed_nvidia; fi
        [ ! -e /run/nvidia-ordering-pre-entered ] || fail nvidia_action_entered
        [ "$(systemctl show nvidia-suspend.service --property=ExecMainStartTimestampMonotonic --value)" = 0 ] || fail nvidia_action_started
        printf '\n%s\n' INTEGRATION_NVIDIA_FAILURE_BLOCKED_PASS
    fi
    check_active_swap
    [ "$(show MainPID)" = "$old_pid" ] || fail failed_hook_changed_target
    touch /tmp/pageout-resume || fail failed_hook_resume
    wait "$helper" || fail failed_hook_data
    systemctl stop "$SERVICE" || fail failed_hook_final_stop
    check_inactive
    echo INTEGRATION_SLEEP_FAILURE_BLOCKED_PASS
    /bin/busybox poweroff -f
    exit 0
fi
if [ "$(cat /kernel-suspend)" = 1 ]; then
    old_pid=$(show MainPID) || fail suspend_old_pid
    echo INTEGRATION_KERNEL_SUSPEND_BEGIN
    systemctl start suspend.target || fail guest_suspend
    i=0
    while :; do
        state=$(show ActiveState) || fail resume_state
        pid=$(show MainPID) || fail resume_pid
        [ "$state" != active ] || [ "$pid" = "$old_pid" ] || break
        [ "$i" -lt 300 ] || fail resume_service_timeout
        sleep 0.1; i=$((i + 1))
    done
    check_active_swap
    echo INTEGRATION_KERNEL_RESUMED
else
    systemctl stop "$SERVICE" || fail service_stop_command
    check_inactive
fi
touch /tmp/pageout-resume || fail pageout_resume
wait "$helper" || fail pageout_data_check
if [ "$(cat /kernel-suspend)" != 1 ]; then check_inactive; fi
echo INTEGRATION_SYSTEMD_EVACUATED

if [ "$(cat /kernel-suspend)" != 1 ]; then
    systemctl start "$SERVICE" || fail service_start_command
    check_active_swap
    "$HELPER" pre || fail sleep_pre
    check_inactive
    "$HELPER" post || fail sleep_post
    check_active_swap
fi
systemctl stop "$SERVICE" || fail final_service_stop
check_inactive
echo INTEGRATION_SYSTEMD_LIFECYCLE_PASS
/bin/busybox poweroff -f
