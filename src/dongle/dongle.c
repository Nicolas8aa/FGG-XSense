#include "dongle/dongle.h"
#include "dongle/mt76.h"
#include "dongle/mt76_boot.h"
#include "dongle/mt76_fw.h"
#include "dongle/mt76_radio.h"
#include "log.h"
#include "pads.h"
#include "usb/ugen.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MS_VENDOR 0x045E

/* How often to check that the adapter is still there once running. */
#define WATCH_SECONDS 5

/* Reads kept in flight on each incoming endpoint (xone keeps 12 per
 * endpoint), and their sizes: microcontroller events are small, radio
 * frames may arrive aggregated. */
#define CMD_READS   4
#define WLAN_READS  8
#define CMD_BUF     0x654
#define WLAN_BUF    0x8400
#define SLOT_FIRST_READ (MT76_SLOT_CMD + 1)
#define SLOTS (SLOT_FIRST_READ + CMD_READS + WLAN_READS)

/* How many incoming messages to log in full, and how often to summarise. */
#define DUMP_FIRST     12
#define REPORT_SECONDS 60

/* Microcontroller event types (xone enum xone_mt76_event). */
#define EVT_BUTTON      0x04
#define EVT_CHANNELS    0x0a
#define EVT_PACKET_RX   0x0c
#define EVT_COREDUMP    0x0d
#define EVT_CLIENT_LOST 0x0e

static int g_trace;

void dongle_set_trace(int on) { g_trace = on; }

int dongle_match(unsigned vendor, unsigned product,
                 const unsigned char *cfg, int cfglen)
{
    (void)cfg;
    (void)cfglen;

    return vendor == MS_VENDOR &&
           (product == 0x02E6 || product == 0x02FE ||
            product == 0x02F9 || product == 0x091E);
}

/* Sleeps in short steps so a stop is noticed promptly. */
static void nap(int seconds)
{
    int i;
    for (i = 0; i < seconds * 10 && !pads_stopping(); i++) usleep(100000);
}

typedef struct {
    long messages, errors, wlan, events[16];
    int  dumped;
} rx_stats;

static void dump(const char *what, const unsigned char *p, uint32_t len)
{
    char hex[3 * 32 + 1];
    uint32_t i, n = len < 32 ? len : 32;

    for (i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02X ", p[i]);
    hex[3 * n] = '\0';
    log_line("    %s %u bytes: %s", what, len, hex);
}

/* Looks at one incoming message: its header says which port it came from
 * and, for the microcontroller's port, which event it is. */
static void handle_rx(const unsigned char *p, uint32_t len, int wlan_ep,
                      rx_stats *st)
{
    uint32_t info, port, evt;

    st->messages++;
    if (len < 8) return;

    info = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
    port = FIELD_GET(MT_RX_FCE_INFO_D_PORT, info);
    evt = FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, info);

    if (st->dumped < DUMP_FIRST) {
        st->dumped++;
        dump(wlan_ep ? "radio" : "event", p, len);
    }

    if (port == MT_WLAN_PORT) {
        st->wlan++;
        return;
    }
    if (port != MT_CPU_RX_PORT) return;

    st->events[evt & 0x0F]++;
    if (evt == EVT_BUTTON) {
        log_line("  adapter: its button was pressed");
        notify("FGG-XSense: adapter button pressed");
    } else if (evt == EVT_COREDUMP) {
        log_line("  adapter: firmware reported a crash (coredump event)");
    }
}

/* Keeps reads queued on both incoming endpoints and handles whatever
 * arrives, checking every few seconds that the adapter is still connected.
 * Returns when it is unplugged or the payload stops. */
static void run_radio(mt76 *m, const char *node)
{
    time_t last_check = time(NULL), last_note = last_check;
    rx_stats st;
    int i;

    memset(&st, 0, sizeof st);

    while (!pads_stopping()) {
        ugen_completion c;
        time_t now;
        int busy = 0;

        for (i = SLOT_FIRST_READ; i < SLOTS; i++) {
            uint32_t size = i < SLOT_FIRST_READ + CMD_READS ? CMD_BUF : WLAN_BUF;
            ugen_fs_start(m->u, i, size);
        }

        while (ugen_fs_complete(m->u, &c)) {
            busy = 1;
            if (c.slot < SLOT_FIRST_READ) continue;
            if (c.status != 0) {
                st.errors++;
                continue;
            }
            if (c.frames > 0)
                handle_rx(ugen_slot_buf(m->u, c.slot), c.len,
                          c.slot >= SLOT_FIRST_READ + CMD_READS, &st);
        }

        now = time(NULL);
        if (now - last_check >= WATCH_SECONDS) {
            uint32_t v;
            last_check = now;
            if (access(node, F_OK) != 0 ||
                (!mt76_rr(m, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &v) && mt76_gone(m))) {
                log_line("  adapter %s: unplugged", node);
                return;
            }
        }
        if (now - last_note >= REPORT_SECONDS) {
            last_note = now;
            log_line("  adapter: %ld message(s), %ld radio frame(s), %ld transfer "
                     "error(s); events button %ld, channels %ld, packet %ld, "
                     "client lost %ld", st.messages, st.wlan, st.errors,
                     st.events[EVT_BUTTON], st.events[EVT_CHANNELS],
                     st.events[EVT_PACKET_RX], st.events[EVT_CLIENT_LOST]);
        }

        if (!busy) usleep(2000);
    }
}

/* Holds the adapter, checking every few seconds that it is still connected.
 * Returns when it is unplugged or the payload stops. */
static void watch(mt76 *m, const char *node)
{
    while (!pads_stopping()) {
        uint32_t v;

        nap(WATCH_SECONDS);
        if (pads_stopping()) break;

        if (access(node, F_OK) != 0 ||
            (!mt76_rr(m, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &v) && mt76_gone(m))) {
            log_line("  adapter %s: unplugged", node);
            return;
        }
    }
}

void *dongle_run(void *arg)
{
    const char *node = arg;
    unsigned vendor = 0, product = 0;
    mt76_boot_result r;
    mt76_radio radio;
    char why[160];
    int i;
    mt76_fw fw;
    mt76 m;

    memset(&m, 0, sizeof m);
    memset(&fw, 0, sizeof fw);
    m.trace = g_trace;

    m.u = ugen_open(node);
    if (!m.u || !ugen_ids(m.u, &vendor, &product)) {
        log_line("  adapter %s: could not open it (%s)", node, strerror(errno));
        goto done;
    }
    log_line("  wireless adapter %s  %04X:%04X", node, vendor, product);

    if (!mt76_fw_load(product, &fw, why, (int)sizeof why)) {
        log_line("  adapter firmware: %s", why);
        notify("FGG-XSense: adapter found, firmware missing - see README");
        /* Held anyway, so it is not rediscovered and reported every scan. */
        watch(&m, node);
        goto done;
    }
    log_line("  adapter firmware: checksum ok, ILM %u + DLM %u, built \"%s\"",
             fw.ilm_len, fw.dlm_len, fw.build_time);

    ugen_set_config(m.u, 0);
    ugen_claim(m.u, 0);
    if (!ugen_fs_init(m.u, SLOTS) ||
        !ugen_fs_open(m.u, MT76_SLOT_CMD, MT76_EP_OUT, MT76_CMD_SLOT_SIZE, 1000)) {
        log_line("  adapter %s: transfer setup failed: %s", node, strerror(errno));
        goto done;
    }
    for (i = SLOT_FIRST_READ; i < SLOTS; i++) {
        int cmd = i < SLOT_FIRST_READ + CMD_READS;
        /* Reads never time out: they wait for the adapter to send. */
        if (!ugen_fs_open(m.u, i, cmd ? MT76_EP_IN_CMD : MT76_EP_IN_WLAN,
                          cmd ? CMD_BUF : WLAN_BUF, 0)) {
            log_line("  adapter %s: read slot %d: %s", node, i, strerror(errno));
            goto done;
        }
    }

    r = mt76_boot(&m, &fw);
    log_line("  adapter %s: %s", node, mt76_boot_name(r));

    if (r == MT76_BOOT_GONE) {
        /* Expected on some adapters: it re-enumerates, possibly on another
         * node, and the next scan finds it with the firmware warm. */
        log_line("  adapter %s: waiting for it to reappear", node);
        goto done;
    }
    if (r == MT76_BOOT_FAILED) {
        notify("FGG-XSense: adapter firmware did not start - see the log");
        watch(&m, node);
        goto done;
    }

    if (!mt76_radio_init(&m, &radio)) {
        notify("FGG-XSense: adapter radio did not start - see the log");
        watch(&m, node);
        goto done;
    }

    notify("FGG-XSense: wireless adapter radio on");
    run_radio(&m, node);

done:
    mt76_fw_free(&fw);
    ugen_close(m.u);
    return NULL;
}
