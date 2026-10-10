#include "dongle/dongle.h"
#include "dongle/mt76.h"
#include "dongle/mt76_boot.h"
#include "dongle/mt76_fw.h"
#include "log.h"
#include "pads.h"
#include "usb/ugen.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define MS_VENDOR 0x045E

/* How often to check that the adapter is still there once running. */
#define WATCH_SECONDS 5

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
    char why[160];
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
    if (!ugen_fs_init(m.u, 1) ||
        !ugen_fs_open(m.u, MT76_SLOT_CMD, MT76_EP_OUT, MT76_CMD_SLOT_SIZE, 1000)) {
        log_line("  adapter %s: transfer setup failed: %s", node, strerror(errno));
        goto done;
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

    notify("FGG-XSense: wireless adapter ready (radio not yet enabled)");
    watch(&m, node);

done:
    mt76_fw_free(&fw);
    ugen_close(m.u);
    return NULL;
}
