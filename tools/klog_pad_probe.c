/* Brief, read-only /dev/klog listener for the next physical-pad reconnect.
 * It copies only pad and battery-related ShellUI/MBus lines; it does not call
 * any Pad or MBus API and does not start or stop FGG-XSense. */
#include "log.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH STATE_DIR "/klog-pad-probe.log"
#define WINDOW_TICKS 600

int main(void)
{
    char chunk[1024], line[2048];
    size_t used = 0;
    int fd, tick;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense klog pad probe -- read-only, 60 seconds");
    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        log_line("could not open /dev/klog");
        log_close();
        return 1;
    }
    for (tick = 0; tick < WINDOW_TICKS; tick++) {
        ssize_t n = read(fd, chunk, sizeof chunk), i;

        if (n <= 0) {
            usleep(100000);
            continue;
        }
        for (i = 0; i < n; i++) {
            if (chunk[i] != '\n' && used + 1 < sizeof line) {
                line[used++] = chunk[i];
                continue;
            }
            line[used] = '\0';
            used = 0;
            if (strstr(line, "[SceShellUI][Pss]") || strstr(line, "[CIM]") ||
                strstr(line, "SCE_MBUS_EVENT") || strstr(line, "capabilityBattery") ||
                strstr(line, "Battery status") || strstr(line, "Battery level") ||
                strstr(line, "DEVICE_FUNCTION_CHANGED"))
                log_line("%s", line);
        }
    }
    close(fd);
    log_line("done");
    log_close();
    return 0;
}
