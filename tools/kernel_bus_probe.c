/* Read-only snapshot of the kernel's published MBus device-list root. */
#include "log.h"

#include <ps5/kernel.h>

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH STATE_DIR "/kernel-bus-probe.log"
#define ROOT_PATH STATE_DIR "/kernel-bus-devices-root.bin"
#define LIST_PATH STATE_DIR "/kernel-bus-devices-list.bin"
#define SNAPSHOT_BYTES 4096

static int save_snapshot(const char *path, intptr_t address)
{
    unsigned char data[SNAPSHOT_BYTES];
    int fd;
    size_t off = 0;

    if (kernel_copyout(address, data, sizeof data) != 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -2;
    while (off < sizeof data) {
        ssize_t n = write(fd, data + off, sizeof data - off);
        if (n <= 0) {
            close(fd);
            return -3;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

int main(void)
{
    intptr_t root = KERNEL_ADDRESS_BUS_DATA_DEVICES;
    intptr_t first = (intptr_t)kernel_getlong(root);
    int r1, r2 = -1;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense kernel MBus root probe -- read-only");
    log_line("BUS_DATA_DEVICES %#llx -> %#llx", (unsigned long long)root,
             (unsigned long long)first);
    r1 = save_snapshot(ROOT_PATH, root);
    if (first) r2 = save_snapshot(LIST_PATH, first);
    log_line("root snapshot %d; first-record snapshot %d", r1, r2);
    return r1 != 0 || r2 != 0;
}
