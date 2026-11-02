/* VM-only watchdog: locked memory keeps diagnostics available under pressure. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

static void copy_file(const char *path)
{
    char buf[2048];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) (void)write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && (!*argv[1] || strspn(argv[1], "0123456789") != strlen(argv[1]) || strlen(argv[1]) > 10))) return 2;
    struct rlimit limit = { RLIM_INFINITY, RLIM_INFINITY };
    if (setrlimit(RLIMIT_MEMLOCK, &limit) || mlockall(MCL_CURRENT | MCL_FUTURE)) return 1;
    struct timespec delay = { 30, 0 };
    nanosleep(&delay, NULL);
    static const char marker[] = "\nBSELF_WATCHDOG_30_SECONDS\n";
    (void)write(STDOUT_FILENO, marker, sizeof(marker) - 1);
    copy_file("/proc/swaps");
    copy_file("/proc/pressure/memory");
    if (argc == 2) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/task", argv[1]);
        DIR *tasks = opendir(path);
        if (tasks) {
            struct dirent *entry;
            while ((entry = readdir(tasks))) {
                if (!*entry->d_name || strspn(entry->d_name, "0123456789") != strlen(entry->d_name) || strlen(entry->d_name) > 10) continue;
                printf("WATCH_TARGET_THREAD pid=%s tid=%s\n", argv[1], entry->d_name);
                fflush(stdout);
                snprintf(path, sizeof(path), "/proc/%s/task/%.10s/stack", argv[1], entry->d_name);
                copy_file(path);
                snprintf(path, sizeof(path), "/proc/%s/task/%.10s/wchan", argv[1], entry->d_name);
                copy_file(path);
                (void)write(STDOUT_FILENO, "\n", 1);
            }
            closedir(tasks);
        }
    }
    int fd = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { (void)write(fd, "w", 1); close(fd); }
    return 0;
}
