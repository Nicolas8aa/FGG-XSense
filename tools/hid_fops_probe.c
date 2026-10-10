/* Read-only starting point for locating /dev/hid's kernel ioctl handler. */
#include "log.h"

#include <ps5/kernel.h>

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH STATE_DIR "/hid-fops-probe.log"
#define SNAPSHOT_PATH STATE_DIR "/hid-file-record.bin"
#define FOPS_PATH STATE_DIR "/hid-file-plus68.bin"
#define DATA_PATH STATE_DIR "/hid-file-plusb8.bin"
#define IOCTL_PATH STATE_DIR "/hid-ioctl-handler.bin"
#define SNAPSHOT_BYTES 512

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
    int fd, result;
    intptr_t file, plus68, plusb8, ioctl_handler;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense /dev/hid file-op probe -- read-only");
    fd = open("/dev/hid", O_RDONLY);
    if (fd < 0) {
        log_line("could not open /dev/hid");
        return 1;
    }
    file = kernel_get_proc_file(getpid(), fd);
    result = file ? save_snapshot(SNAPSHOT_PATH, file) : -4;
    plus68 = file ? (intptr_t)kernel_getlong(file + 0x68) : 0;
    plusb8 = file ? (intptr_t)kernel_getlong(file + 0xb8) : 0;
    if (result == 0 && plus68) result = save_snapshot(FOPS_PATH, plus68);
    if (result == 0 && plusb8) result = save_snapshot(DATA_PATH, plusb8);
    /* cdevsw: version/flags, then open, close, read, write, ioctl. */
    ioctl_handler = plusb8 ? (intptr_t)kernel_getlong(plusb8 + 0x28) : 0;
    if (result == 0 && ioctl_handler) result = save_snapshot(IOCTL_PATH, ioctl_handler);
    log_line("fd %d -> kernel file %#llx; snapshot %d", fd,
             (unsigned long long)file, result);
    log_line("file +0x68 -> %#llx; +0xb8 -> %#llx", (unsigned long long)plus68,
             (unsigned long long)plusb8);
    log_line("candidate cdevsw ioctl -> %#llx", (unsigned long long)ioctl_handler);
    close(fd);
    return result != 0;
}
