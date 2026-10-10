/* probe_bt -- first contact with the console's own Bluetooth chip.
 *
 * The PS5's Bluetooth chip is a USB device (/dev/ugen0.2) holding two HCI
 * controllers. The system uses one of them for the DualSense; HearBridge-PS5
 * found that the first one can be used beside the system's driver, without
 * detaching it -- detaching it takes the DualSense down too. This probe does
 * what HearBridge does to get started, and nothing more:
 *
 *   1. listens on the first controller's event and data endpoints for a
 *      while, sending nothing, and logs what arrives;
 *   2. sends four read-only commands (version, address, LE features, LE
 *      buffer size) and logs the replies.
 *
 * It never sends HCI Reset or any command that changes the controller's
 * state, never touches the second controller's interfaces, and stops at once
 * if the chip reports a hardware error or a vendor fault event, leaving a
 * marker that keeps later Bluetooth code off until it is deleted.
 */
#include "log.h"
#include "usb/ugen.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR    "/data/fgg-xsense"
#define LOG_PATH     STATE_DIR "/probe-bt.log"
#define DISABLED     STATE_DIR "/ble.disabled"
#define BT_NODE      "/dev/ugen0.2"

#define LISTEN_SECONDS  30
#define REPLY_TIMEOUT_MS 1000

/* Reads in flight. Fewer than HearBridge keeps: this only needs to see
 * traffic, not win most of it. */
#define EVT_READS 8
#define ACL_READS 4
#define SLOTS     (EVT_READS + ACL_READS)
#define EVT_BUF   260
#define ACL_BUF   1028

/* HCI events. */
#define EV_CMD_COMPLETE 0x0E
#define EV_CMD_STATUS   0x0F
#define EV_HW_ERROR     0x10
#define EV_VENDOR       0xFF

/* Interface class of a Bluetooth controller. */
#define IFACE_BT 0xE0

typedef struct {
    int     number;
    uint8_t evt_ep, in_ep, out_ep, spare_out_ep;
} hci_iface;

typedef struct {
    long events, acl, errors, cmd_complete, other;
    int  dumped, faulted;
} counts;

/* The read-only commands this probe may send. Nothing else is ever sent. */
static const struct {
    uint16_t    op;
    const char *name;
} g_allowed[] = {
    { 0x1001, "Read Local Version Information" },
    { 0x1009, "Read BD_ADDR" },
    { 0x2003, "LE Read Local Supported Features" },
    { 0x2002, "LE Read Buffer Size" },
};

static void hex(const char *what, const unsigned char *p, uint32_t len)
{
    char buf[3 * 40 + 1];
    uint32_t i, n = len < 40 ? len : 40;

    for (i = 0; i < n; i++) snprintf(buf + 3 * i, 4, "%02X ", p[i]);
    buf[3 * n] = '\0';
    log_line("    %s (%u bytes): %s%s", what, len, buf, len > n ? "..." : "");
}

/* The first HCI interface (E0/01/01, alternate setting 0) with an interrupt
 * IN, a bulk IN and a bulk OUT endpoint (HearBridge usb_hci_desc.c). */
static int find_first_hci(const unsigned char *d, int len, hci_iface *f)
{
    int pos = 0, inside = 0;

    memset(f, 0, sizeof *f);
    f->number = -1;
    while (pos + 2 <= len) {
        int blen = d[pos], type = d[pos + 1];

        if (blen < 2 || pos + blen > len) break;
        if (type == 4 && blen >= 9) {
            if (inside && f->evt_ep && f->in_ep && f->out_ep) return 1;
            inside = d[pos + 3] == 0 && d[pos + 5] == IFACE_BT &&
                     d[pos + 6] == 0x01 && d[pos + 7] == 0x01;
            if (inside) {
                memset(f, 0, sizeof *f);
                f->number = d[pos + 2];
            }
        } else if (type == 5 && blen >= 7 && inside) {
            uint8_t addr = d[pos + 2];
            unsigned t = d[pos + 3] & 3u;
            int in = (addr & 0x80) != 0;

            if (t == 3 && in && !f->evt_ep)        f->evt_ep = addr;
            else if (t == 2 && in && !f->in_ep)    f->in_ep = addr;
            else if (t == 2 && !in && !f->out_ep)  f->out_ep = addr;
            else if (t == 2 && !in)                f->spare_out_ep = addr;
        }
        pos += blen;
    }
    return inside && f->evt_ep && f->in_ep && f->out_ep;
}

static void disable(const char *why)
{
    FILE *f = fopen(DISABLED, "w");

    if (f) {
        fprintf(f, "%s\n", why);
        fclose(f);
    }
    log_line("  FAULT: %s -- stopping; Bluetooth work stays off until %s is "
             "deleted", why, DISABLED);
    notify("Bluetooth probe: chip fault - if the DualSense stops, restart the console");
}

/* Handles one completed read. Returns 0 if the chip reported a fault. */
static int handle(ugen_completion *c, ugen *u, counts *n, uint16_t want_op,
                  unsigned char *reply, int *reply_len)
{
    const unsigned char *p = ugen_slot_buf(u, c->slot);

    if (c->status != 0) {
        n->errors++;
        return 1;
    }
    if (c->frames == 0 || c->len == 0) return 1;

    if (c->slot >= EVT_READS) {
        n->acl++;
        if (n->dumped < 12) {
            n->dumped++;
            hex("ACL", p, c->len);
        }
        return 1;
    }

    n->events++;
    if (c->len >= 2 && (p[0] == EV_HW_ERROR || p[0] == EV_VENDOR)) {
        hex("event", p, c->len);
        disable(p[0] == EV_HW_ERROR ? "hardware error event" : "vendor event");
        n->faulted = 1;
        return 0;
    }

    if (c->len >= 6 && p[0] == EV_CMD_COMPLETE) {
        uint16_t op = (uint16_t)(p[3] | (p[4] << 8));
        n->cmd_complete++;
        if (want_op && op == want_op && *reply_len == 0) {
            *reply_len = (int)(c->len < 64 ? c->len : 64);
            memcpy(reply, p, (size_t)*reply_len);
            return 1;
        }
    } else {
        n->other++;
    }

    if (n->dumped < 12) {
        n->dumped++;
        hex("event", p, c->len);
    }
    return 1;
}

/* Re-arms every idle read and handles whatever has completed. */
static int pump(ugen *u, counts *n, uint16_t want_op, unsigned char *reply,
                int *reply_len)
{
    ugen_completion c;
    int i;

    while (ugen_fs_complete(u, &c))
        if (!handle(&c, u, n, want_op, reply, reply_len)) return 0;

    for (i = 0; i < SLOTS; i++)
        ugen_fs_start(u, i, i < EVT_READS ? EVT_BUF : ACL_BUF);
    return 1;
}

static void report_reply(uint16_t op, const unsigned char *r, int len)
{
    uint8_t status = len >= 6 ? r[5] : 0xFF;

    hex("reply", r, (uint32_t)len);
    if (status != 0) {
        log_line("  -> status %02X (not success)", status);
        return;
    }
    if (op == 0x1001 && len >= 14)
        log_line("  -> HCI version %u rev %04X, LMP version %u, manufacturer %04X, "
                 "subversion %04X", r[6], r[7] | (r[8] << 8), r[9],
                 r[10] | (r[11] << 8), r[12] | (r[13] << 8));
    else if (op == 0x1009 && len >= 12)
        log_line("  -> controller address %02x:%02x:%02x:%02x:%02x:%02x",
                 r[11], r[10], r[9], r[8], r[7], r[6]);
    else if (op == 0x2003 && len >= 14)
        log_line("  -> LE features %02X %02X %02X %02X %02X %02X %02X %02X",
                 r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13]);
    else if (op == 0x2002 && len >= 9)
        log_line("  -> LE ACL buffers: %u x %u bytes", r[8], r[6] | (r[7] << 8));
}

int main(void)
{
    unsigned char cfg[4096];
    hci_iface f;
    counts n;
    unsigned vid = 0, pid = 0;
    time_t until;
    ugen *u;
    int len, i, ok = 1;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense Bluetooth probe -- listen, then read-only commands");
    memset(&n, 0, sizeof n);

    if (access(DISABLED, F_OK) == 0) {
        log_line("  %s exists -- a previous run saw a fault; delete it to probe again",
                 DISABLED);
        notify("Bluetooth probe: disabled after an earlier fault");
        log_close();
        return 1;
    }

    u = ugen_open(BT_NODE);
    if (!u) {
        log_line("  %s: %s", BT_NODE, strerror(errno));
        log_close();
        return 1;
    }
    ugen_ids(u, &vid, &pid);
    len = ugen_config_desc(u, cfg, (int)sizeof cfg);
    if (!find_first_hci(cfg, len, &f)) {
        log_line("  %s (%04X:%04X): no usable HCI interface", BT_NODE, vid, pid);
        ugen_close(u);
        log_close();
        return 1;
    }
    log_line("  %s %04X:%04X: HCI interface %d, events %02X, data in %02X, "
             "out %02X / %02X", BT_NODE, vid, pid, f.number, f.evt_ep, f.in_ep,
             f.out_ep, f.spare_out_ep);

    /* No configuration change and no claim: the system's driver stays
     * attached, as HearBridge does. Reads never time out. */
    if (!ugen_fs_init(u, SLOTS)) {
        log_line("  FS_INIT: %s", strerror(errno));
        ugen_close(u);
        log_close();
        return 1;
    }
    for (i = 0; i < SLOTS; i++) {
        int evt = i < EVT_READS;
        if (!ugen_fs_open(u, i, evt ? f.evt_ep : f.in_ep,
                          evt ? EVT_BUF : ACL_BUF, 0)) {
            log_line("  FS_OPEN slot %d: %s", i, strerror(errno));
            ugen_close(u);
            log_close();
            return 1;
        }
    }

    /* 1. Listen. */
    log_line("  listening for %d seconds, sending nothing", LISTEN_SECONDS);
    until = time(NULL) + LISTEN_SECONDS;
    while (ok && time(NULL) < until) {
        ok = pump(u, &n, 0, NULL, &len);
        usleep(2000);
    }
    log_line("  heard: %ld event(s) (%ld command complete), %ld ACL packet(s), "
             "%ld transfer error(s)", n.events, n.cmd_complete, n.acl, n.errors);

    /* 2. Read-only commands, each answered by a Command Complete. The system
     * also has reads queued, so a reply may go to it instead; one retry. */
    for (i = 0; ok && i < (int)(sizeof g_allowed / sizeof g_allowed[0]); i++) {
        uint16_t op = g_allowed[i].op;
        unsigned char pkt[3] = { (unsigned char)op, (unsigned char)(op >> 8), 0 };
        unsigned char reply[64];
        int attempt, reply_len = 0;

        for (attempt = 1; ok && attempt <= 2 && reply_len == 0; attempt++) {
            int waited;

            log_line("  command %04X %s%s", op, g_allowed[i].name,
                     attempt > 1 ? " (again)" : "");
            if (ugen_ctrl(u, 0x20, 0, 0, (unsigned)f.number, pkt, sizeof pkt) < 0) {
                log_line("  -> could not send: %s", strerror(errno));
                break;
            }
            for (waited = 0; ok && reply_len == 0 && waited < REPLY_TIMEOUT_MS;
                 waited += 2) {
                ok = pump(u, &n, op, reply, &reply_len);
                usleep(2000);
            }
        }
        if (reply_len) report_reply(op, reply, reply_len);
        else if (ok)   log_line("  -> no reply seen (the system may have taken it)");
    }

    log_line("  total: %ld event(s), %ld ACL packet(s), %ld transfer error(s)%s",
             n.events, n.acl, n.errors, n.faulted ? ", FAULT" : "");
    if (!n.faulted)
        notify("Bluetooth probe done - is the DualSense still working?");

    ugen_close(u);
    log_close();
    return n.faulted ? 1 : 0;
}
