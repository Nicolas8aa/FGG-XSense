/* SPDX-License-Identifier: GPL-3.0-or-later
 * Follows dlundqvist/xone transport/dongle.c (message, frame, pairing and
 * client handling), (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/link.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The pairing window, as xone opens it. */
#define PAIRING_SECONDS 60

/* Holding the adapter's button repeats its event about once a second; one
 * hold is one request. */
#define BUTTON_REPEAT_SECONDS 3

/* Creating this file over FTP opens the pairing window. */
#define PAIR_TRIGGER "/data/fgg-xsense/pair"

/* GIP messages logged in full per controller while the link is new. */
#define DUMP_PER_CLIENT 16

#define REPORT_SECONDS 60

/* Microcontroller events (xone enum xone_mt76_event). */
#define EVT_BUTTON      0x04
#define EVT_CHANNELS    0x0a
#define EVT_PACKET_RX   0x0c
#define EVT_COREDUMP    0x0d
#define EVT_CLIENT_LOST 0x0e

/* 802.11 frame control, type and subtype bits only. */
#define FC_TYPE_MASK   0x00FC
#define FC_ASSOC_REQ   0x0000
#define FC_DISASSOC    0x00A0
#define FC_RESERVED    0x0070
#define FC_QOS_DATA    0x0088

#define WLAN_RESERVED     0x70
#define CLIENT_PAIR_REQ   0x01
#define CLIENT_ENCRYPTION 0x10

#define RXWI_SIZE 32

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void hex(const char *what, const unsigned char *p, uint32_t len)
{
    char buf[3 * 48 + 1];
    uint32_t i, n = len < 48 ? len : 48;

    for (i = 0; i < n; i++) snprintf(buf + 3 * i, 4, "%02X ", p[i]);
    buf[3 * n] = '\0';
    log_line("    %s (%u bytes): %s%s", what, len, buf, len > n ? "..." : "");
}

static void led(dlink *l)
{
    mt76_set_led(l->m, l->pairing ? MT76_LED_BLINK :
                       l->nclients ? MT76_LED_ON : MT76_LED_OFF);
}

void link_init(dlink *l, mt76 *m, mt76_radio *r)
{
    memset(l, 0, sizeof *l);
    l->m = m;
    l->r = r;
}

void link_set_pairing(dlink *l, int on, const char *why)
{
    if (on) l->pairing_until = time(NULL) + PAIRING_SECONDS;
    if (l->pairing == on) return;

    if (!mt76_radio_set_pairing(l->m, l->r, on)) {
        log_line("  adapter: could not %s pairing (error %d)",
                 on ? "open" : "close", l->m->err);
        return;
    }
    l->pairing = on;
    led(l);

    log_line("  adapter: pairing %s (%s)",
             on ? "open for 60 seconds" : "closed", why);
    if (on)
        notify("FGG-XSense: adapter pairing - hold the pair button on your controller");
}

/* ---- clients ----------------------------------------------------------- */

static int valid_unicast(const uint8_t *a)
{
    return !(a[0] & 0x01) && (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]);
}

static void associate(dlink *l, const uint8_t *addr)
{
    int i, wcid = 0;

    /* A controller already holding a slot is not given a second one. */
    for (i = 0; i < MT76_MAX_CLIENTS; i++)
        if (l->clients[i].used && memcmp(l->clients[i].addr, addr, 6) == 0)
            return;

    for (i = 0; i < MT76_MAX_CLIENTS && !wcid; i++)
        if (!l->clients[i].used) wcid = i + 1;
    if (!wcid) {
        log_line("  adapter: no free client slot");
        return;
    }

    if (!mt76_radio_associate(l->m, l->r, wcid, addr)) {
        log_line("  adapter: associating %02x:%02x:%02x:%02x:%02x:%02x failed "
                 "(error %d)", addr[0], addr[1], addr[2], addr[3], addr[4],
                 addr[5], l->m->err);
        return;
    }

    memset(&l->clients[wcid - 1], 0, sizeof l->clients[0]);
    l->clients[wcid - 1].used = 1;
    memcpy(l->clients[wcid - 1].addr, addr, 6);
    l->nclients++;
    led(l);

    log_line("  adapter: controller %02x:%02x:%02x:%02x:%02x:%02x connected "
             "as client %d", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5], wcid);
    notify("FGG-XSense: controller connected to the adapter");
}

static void remove_client(dlink *l, int wcid, const char *why)
{
    link_client *c;

    if (wcid < 1 || wcid > MT76_MAX_CLIENTS) return;
    c = &l->clients[wcid - 1];
    if (!c->used) return;

    log_line("  adapter: client %d (%02x:%02x:%02x:%02x:%02x:%02x) gone: %s, "
             "after %ld frame(s)", wcid, c->addr[0], c->addr[1], c->addr[2],
             c->addr[3], c->addr[4], c->addr[5], why, c->frames);

    mt76_radio_remove_client(l->m, wcid);
    memset(c, 0, sizeof *c);
    l->nclients--;
    led(l);
}

static void pair(dlink *l, const uint8_t *addr)
{
    log_line("  adapter: pairing request from %02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    if (!mt76_radio_pair_client(l->m, l->r, addr)) {
        log_line("  adapter: pairing reply failed (error %d)", l->m->err);
        return;
    }
    link_set_pairing(l, 0, "controller paired");
}

/* ---- frames ------------------------------------------------------------ */

static void client_data(dlink *l, int wcid, const unsigned char *gip, uint32_t len)
{
    link_client *c;
    char what[32];

    if (wcid < 1 || wcid > MT76_MAX_CLIENTS || !l->clients[wcid - 1].used) {
        l->ignored++;
        return;
    }
    c = &l->clients[wcid - 1];
    c->frames++;

    if (c->dumped < DUMP_PER_CLIENT) {
        c->dumped++;
        snprintf(what, sizeof what, "client %d GIP", wcid);
        hex(what, gip, len);
    }
}

/* An 802.11 frame behind its RX descriptor (xone_dongle_process_wlan). */
static void wlan_frame(dlink *l, const unsigned char *p, uint32_t len)
{
    const unsigned char *f, *body, *addr2;
    uint32_t rxinfo, ctl, mpdu, hdr, pad, body_len;
    uint16_t fc;
    int wcid;

    if (len < RXWI_SIZE + 24) {
        l->ignored++;
        return;
    }
    rxinfo = le32(p);
    ctl = le32(p + 4);
    wcid = (int)FIELD_GET(MT_RXWI_CTL_WCID, ctl);
    mpdu = FIELD_GET(MT_RXWI_CTL_MPDU_LEN, ctl);

    f = p + RXWI_SIZE;
    len -= RXWI_SIZE;
    fc = (uint16_t)(f[0] | (f[1] << 8));

    switch ((fc >> 2) & 0x3) {
    case 0:  hdr = 24; break;                                  /* management */
    case 2:  hdr = 24 + ((fc & 0x0300) == 0x0300 ? 6 : 0) +    /* data */
                   ((fc & 0x0080) ? 2 : 0);
             break;
    default: l->ignored++; return;                             /* control */
    }

    /* Two bytes of padding may follow the header; the MPDU length excludes
     * them. A frame claiming more than arrived -- typically the adapter's
     * own beacon coming back -- is dropped, as xone does. */
    pad = (rxinfo & MT_RXINFO_L2PAD) ? 2 : 0;
    if (mpdu < hdr || mpdu + pad > len) {
        l->ignored++;
        return;
    }
    body = f + hdr + pad;
    body_len = mpdu - hdr;
    addr2 = f + 10;

    switch (fc & FC_TYPE_MASK) {
    case FC_QOS_DATA:
        client_data(l, wcid, body, body_len);
        break;
    case FC_ASSOC_REQ:
        /* Channel scans can produce associations from bogus addresses. */
        if (valid_unicast(addr2)) associate(l, addr2);
        break;
    case FC_DISASSOC:
        remove_client(l, wcid, "disassociated");
        break;
    case FC_RESERVED:
        if (body_len < 2 || body[0] != WLAN_RESERVED) break;
        if (body[1] == CLIENT_PAIR_REQ) {
            pair(l, addr2);
        } else if (body[1] == CLIENT_ENCRYPTION) {
            log_line("  adapter: client %d asks for encryption -- not "
                     "supported yet", wcid);
        }
        break;
    default:
        l->ignored++;
    }
}

void link_handle(dlink *l, const unsigned char *p, uint32_t len)
{
    uint32_t info, port, evt;

    l->messages++;
    if (len < 8) return;

    info = le32(p);

    /* Responses to our own commands. */
    if (FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, info) == 0x01) return;

    port = FIELD_GET(MT_RX_FCE_INFO_D_PORT, info);
    p += 4;           /* header */
    len -= 8;         /* header and trailer */

    if (port == MT_WLAN_PORT) {
        l->wlan++;
        wlan_frame(l, p, len);
        return;
    }
    if (port != MT_CPU_RX_PORT) return;

    evt = FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, info);
    l->events[evt & 0x0F]++;

    switch (evt) {
    case EVT_BUTTON: {
        time_t now = time(NULL);
        if (now - l->last_button >= BUTTON_REPEAT_SECONDS)
            link_set_pairing(l, 1, "adapter button");
        l->last_button = now;
        break;
    }
    case EVT_PACKET_RX:
        l->wlan++;
        wlan_frame(l, p, len);
        break;
    case EVT_CLIENT_LOST:
        if (len >= 1) remove_client(l, p[0], "lost");
        break;
    case EVT_COREDUMP:
        log_line("  adapter: firmware reported a crash (coredump event)");
        break;
    }
}

void link_tick(dlink *l, time_t now)
{
    static time_t last_note;

    if (l->pairing && now >= l->pairing_until)
        link_set_pairing(l, 0, "timed out");

    if (access(PAIR_TRIGGER, F_OK) == 0) {
        unlink(PAIR_TRIGGER);
        link_set_pairing(l, 1, "pair file");
    }

    if (now - last_note >= REPORT_SECONDS) {
        last_note = now;
        log_line("  adapter: %ld message(s), %ld radio frame(s), %ld ignored, "
                 "%d client(s), %ld lost while sending; button %ld, lost %ld",
                 l->messages, l->wlan, l->ignored, l->nclients, l->m->dropped,
                 l->events[EVT_BUTTON], l->events[EVT_CLIENT_LOST]);
    }
}
