/* Verify each QEMU fixture inside the guest before invoking the target. */
#include <cpuid.h>
#include <stdio.h>
#include <string.h>
#include "ublk-guest-guard.h"

int main(int argc, char **argv)
{
    unsigned a, b, c, d;
    char vendor[256], product[256], cmdline[8192];
    if (argc != 2 || !__get_cpuid(1, &a, &b, &c, &d) ||
        ublk_guard_read("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor)) ||
        ublk_guard_read("/sys/class/dmi/id/product_name", product, sizeof(product)) ||
        ublk_guard_read("/proc/cmdline", cmdline, sizeof(cmdline))) return 1;
    int hypervisor = !!(c & (1U << 31));
    int dmi = !strcmp(vendor, "QEMU") || !strcmp(vendor, "KVM") ||
              !strcmp(product, "QEMU") || !strcmp(product, "KVM");
    int marker = 0;
    for (char *p = strtok(cmdline, " \t\r\n"); p; p = strtok(NULL, " \t\r\n"))
        if (!strcmp(p, "vram_isolated_guest=1")) marker = 1;
    printf("FIXTURE mode=%s hypervisor=%d dmi=%d marker=%d vendor=%s product=%s\n",
           argv[1], hypervisor, dmi, marker, vendor, product);
    if (!strcmp(argv[1], "positive")) return !(hypervisor && dmi && marker);
    if (!strcmp(argv[1], "no-marker")) return !(hypervisor && dmi && !marker);
    if (!strcmp(argv[1], "no-hypervisor")) return !(!hypervisor && dmi && marker);
    if (!strcmp(argv[1], "other-dmi")) return !(hypervisor && !dmi && marker);
    return 1;
}
