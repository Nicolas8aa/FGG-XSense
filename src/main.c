/* FGG-XSense -- use an Xbox-protocol controller on a PlayStation 5.
 *
 * A PS5 enumerates a GIP gamepad and then ignores it entirely: no light, no
 * input, not even a complaint. This payload picks it up and republishes it to
 * the system as an ordinary PlayStation pad. Nothing in the system software is
 * patched, nothing is installed, and the virtual pads are removed on exit.
 *
 *   main.c            the lock, finding controllers, stopping
 *   pads.c            connected controllers -> one virtual pad each
 *   vpad.c            publishes a virtual pad to the kernel
 *   gip/gip_proto.c   the Xbox wire protocol
 *   gip/gip_wired.c   a controller on a USB cable, one thread each
 *   usb/ugen.c        USB device nodes and transfer slots
 *   usb/usb_enum.c    finding devices without two backends sharing one
 *
 * Resident: it waits for controllers, serves them while connected and waits
 * again, so it can be autoloaded before one is plugged in. Holding View+Menu
 * on any controller stops it.
 *
 * The PS button cannot be sent through a virtual pad -- see the README.
 */
#include "config.h"
#include "gip/gip_wired.h"
#include "lock.h"
#include "log.h"
#include "pads.h"
#include "usb/usb_enum.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH  STATE_DIR "/xsense.log"

/* How often to look for newly connected controllers. */
#define SCAN_SECONDS 3

/* How often the main loop wakes, in microseconds. */
#define TICK_US 100000

/* One thread per connected device. */
typedef struct {
    int          active;
    volatile int done;
    pthread_t    thread;
    char         node[USB_NODE_LEN];
} backend;

static backend g_backends[PADS_MAX];

static void *wired_thread(void *arg)
{
    backend *b = arg;

    gip_wired_run(b->node);
    b->done = 1;
    return NULL;
}

/* Collects threads whose device has gone, freeing their node and slot. */
static void reap(void)
{
    int i;

    for (i = 0; i < PADS_MAX; i++) {
        if (g_backends[i].active && g_backends[i].done) {
            pthread_join(g_backends[i].thread, NULL);
            usb_disown(g_backends[i].node);
            g_backends[i].active = 0;
        }
    }
}

/* Starts a thread for every newly connected wired controller. Returns how
 * many were started. */
static int scan(void)
{
    int i, started = 0;

    for (i = 0; i < PADS_MAX; i++) {
        backend *b = &g_backends[i];

        if (b->active) continue;
        if (!usb_find_and_own(gip_wired_match, b->node)) break;

        b->done = 0;
        if (pthread_create(&b->thread, NULL, wired_thread, b) != 0) {
            log_line("  %s: could not start a thread", b->node);
            usb_disown(b->node);
            break;
        }
        b->active = 1;
        started++;
    }
    return started;
}

static int any_active(void)
{
    int i;
    for (i = 0; i < PADS_MAX; i++)
        if (g_backends[i].active) return 1;
    return 0;
}

int main(void)
{
    time_t last_scan = 0, last_beat = 0;
    int waiting = 0, i;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;

    log_line("========================================");
    log_line("FGG-XSense -- Xbox controller on PlayStation 5");

    if (!lock_take()) {
        log_line("  another copy is already running -- exiting");
        notify("FGG-XSense: already running");
        log_close();
        return 1;
    }

    config_load(CONFIG_PATH);
    pads_set_mirror(config_int("mirror", 0));
    if (config_int("mirror", 0))
        log_line("  mirror=1: every controller is published twice");

    notify("FGG-XSense: waiting for an Xbox controller");

    for (;;) {
        time_t now = time(NULL);

        if (now != last_beat) {
            last_beat = now;
            lock_refresh();
        }

        if (pads_tick(now)) {
            log_line("  View+Menu held -- stopping");
            break;
        }

        if (now - last_scan >= SCAN_SECONDS) {
            last_scan = now;
            reap();
            if (scan() > 0) {
                waiting = 0;
            } else if (!any_active() && !waiting) {
                log_line("  no Xbox-protocol controller connected -- waiting");
                waiting = 1;
            }
        }

        usleep(TICK_US);
    }

    /* Backends notice the stop within a read or two and remove their pads. */
    pads_request_stop();
    for (i = 0; i < PADS_MAX; i++) {
        if (g_backends[i].active) {
            pthread_join(g_backends[i].thread, NULL);
            usb_disown(g_backends[i].node);
            g_backends[i].active = 0;
        }
    }

    notify("FGG-XSense: stopped");
    lock_release();
    log_close();
    return 0;
}
