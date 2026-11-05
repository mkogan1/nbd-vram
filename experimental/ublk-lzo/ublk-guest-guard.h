#ifndef UBLK_GUEST_GUARD_H
#define UBLK_GUEST_GUARD_H

#include <stdio.h>
#include <string.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

/* Returns 0 on success, -1 for CPUID, -2 for DMI, -3 for marker. */
static int ublk_guard_values(int hypervisor, const char *vendor,
                             const char *product, const char *cmdline)
{
    const char *p = cmdline;
    int marker = 0;
    if (!hypervisor)
        return -1;
    if (!vendor || !product)
        return -2;
    if (!cmdline)
        return -3;
    if (strcmp(vendor, "QEMU") && strcmp(vendor, "KVM") &&
        strcmp(product, "QEMU") && strcmp(product, "KVM"))
        return -2;
    while (*p) {
        size_t n;
        p += strspn(p, " \t\r\n");
        n = strcspn(p, " \t\r\n");
        if (n == sizeof("vram_isolated_guest=1") - 1 &&
            !memcmp(p, "vram_isolated_guest=1", n))
            marker = 1;
        p += n;
    }
    return marker ? 0 : -3;
}

static int ublk_guard_read(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n;
    int extra, failed;
    if (!f)
        return -1;
    n = fread(buf, 1, size - 1, f);
    extra = fgetc(f);
    failed = ferror(f) || extra != EOF || memchr(buf, '\0', n) != NULL;
    if (fclose(f) != 0 || failed || n == 0)
        return -1;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        --n;
    buf[n] = '\0';
    return n ? 0 : -1;
}

static int ublk_guest_guard(void)
{
    char vendor[256], product[256], cmdline[8192];
    unsigned int a, b, c, d;
#if defined(__i386__) || defined(__x86_64__)
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1U << 31)))
        return -1;
#else
    (void)a; (void)b; (void)c; (void)d;
    return -1;
#endif
    if (ublk_guard_read("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor)) ||
        ublk_guard_read("/sys/class/dmi/id/product_name", product, sizeof(product)))
        return -2;
    if (ublk_guard_read("/proc/cmdline", cmdline, sizeof(cmdline)))
        return -3;
    return ublk_guard_values(1, vendor, product, cmdline);
}
#endif
