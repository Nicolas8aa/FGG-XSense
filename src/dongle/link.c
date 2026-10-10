/* SPDX-License-Identifier: GPL-3.0-or-later
 * Follows dlundqvist/xone transport/dongle.c (message, frame, pairing and
 * client handling), (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/link.h"
#include "gip/gip_proto.h"
#include "log.h"
#include "pads.h"

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

/* Controllers admitted while pairing was open, one address per line. Outside
 * the pairing window only these may associate: otherwise any device in
 * radio range could connect and send input to the console. */
#define KNOWN_PATH "/data/fgg-xsense/controllers.txt"
#define KNOWN_MAX  16

/* GIP messages logged in full per controller while the link is new. */
#define DUMP_PER_CLIENT 16

/* Button changes logged per controller, to check every input arrives. */
#define CHANGES_LOGGED 200

/* An announce repeats until the controller is answered; answer at most this
 * often so a burst of announces is not met with a burst of start-ups. */
#define INIT_INTERVAL_SECONDS 1

/* Data frames to a client go on the data queue (xone XONE_DONGLE_QUEUE_DATA). */
#define QUEUE_DATA 0x00

/* 802.11 frame control for data sent from the access point. */
#define FC_QOS_DATA_FROMDS 0x0288

#define REPORT_SECONDS 60

/* A controller silent this long after connecting is dropped so it
 * reconnects, at most this many times in a row. */
#define SILENT_SECONDS 10
#define SILENT_RETRIES 3

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

    /* A single failed write has been seen right after the radio came up;
     * one retry covers it. */
    if (!mt76_radio_set_pairing(l->m, l->r, on) &&
        !mt76_radio_set_pairing(l->m, l->r, on)) {
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

static uint8_t g_known[KNOWN_MAX][6];
static int g_nknown = -1;   /* -1 until loaded */

static void load_known(void)
{
    char line[64];
    FILE *f;

    g_nknown = 0;
    f = fopen(KNOWN_PATH, "r");
    if (!f) return;
    while (g_nknown < KNOWN_MAX && fgets(line, sizeof line, f)) {
        unsigned a[6];
        int i;
        if (sscanf(line, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3],
                   &a[4], &a[5]) != 6)
            continue;
        for (i = 0; i < 6; i++) g_known[g_nknown][i] = (uint8_t)a[i];
        g_nknown++;
    }
    fclose(f);
}

static int is_known(const uint8_t *addr)
{
    int i;
    if (g_nknown < 0) load_known();
    for (i = 0; i < g_nknown; i++)
        if (memcmp(g_known[i], addr, 6) == 0) return 1;
    return 0;
}

static void remember(const uint8_t *addr)
{
    FILE *f;
    int i;

    if (is_known(addr)) return;

    /* The oldest entry gives way once the list is full. */
    if (g_nknown == KNOWN_MAX) {
        memmove(g_known[0], g_known[1], sizeof g_known[0] * (KNOWN_MAX - 1));
        g_nknown--;
    }
    memcpy(g_known[g_nknown++], addr, 6);

    f = fopen(KNOWN_PATH, "w");
    if (!f) {
        log_line("  adapter: could not save %s", KNOWN_PATH);
        return;
    }
    for (i = 0; i < g_nknown; i++)
        fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x\n", g_known[i][0],
                g_known[i][1], g_known[i][2], g_known[i][3], g_known[i][4],
                g_known[i][5]);
    fclose(f);
}

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

    if (!l->pairing && !is_known(addr)) {
        l->refused++;
        if (l->refused <= 5)
            log_line("  adapter: refused %02x:%02x:%02x:%02x:%02x:%02x -- not "
                     "paired with this adapter (press its button to pair)",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return;
    }

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
    l->clients[wcid - 1].pad = -1;
    l->clients[wcid - 1].battery_level = -1;
    l->clients[wcid - 1].battery_type = -1;
    memcpy(l->clients[wcid - 1].addr, addr, 6);
    l->nclients++;
    remember(addr);
    led(l);

    log_line("  adapter: controller %02x:%02x:%02x:%02x:%02x:%02x connected "
             "as client %d", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5], wcid);
    l->clients[wcid - 1].connected_at = time(NULL);
    notify("FGG-XSense: controller connected to the adapter");
}

static void remove_client(dlink *l, int wcid, const char *why)
{
    link_client *c;

    if (wcid < 1 || wcid > MT76_MAX_CLIENTS) return;
    c = &l->clients[wcid - 1];
    if (!c->used) return;

    log_line("  adapter: client %d (%02x:%02x:%02x:%02x:%02x:%02x) gone: %s, "
             "after %ld frame(s), %ld input report(s), %ld acknowledged, %ld "
             "send failure(s)", wcid, c->addr[0], c->addr[1], c->addr[2],
             c->addr[3], c->addr[4], c->addr[5], why, c->frames, c->inputs,
             c->acked, c->sent_failed);

    if (c->pad >= 0) pads_detach(c->pad);
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

/* Sends one GIP message to a client: an 8-byte queue header, a TXWI asking
 * for acknowledgement, a QoS data header from us, two bytes of padding, then
 * the message (xone xone_dongle_prep_packet). */
static int send_gip(dlink *l, int wcid, const unsigned char *gip, int len)
{
    unsigned char b[8 + 20 + 26 + 2 + 64];
    unsigned char *txwi = b + 8, *h = b + 28;
    link_client *c = &l->clients[wcid - 1];
    uint32_t w;

    if (len < 0 || len > 64) return 0;
    memset(b, 0, sizeof b);

    b[2] = QUEUE_DATA;
    b[3] = (unsigned char)(wcid - 1);

    w = FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY, 5) |              /* 4 us */
        (FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM) << 16);
    txwi[0] = (unsigned char)w;
    txwi[1] = (unsigned char)(w >> 8);
    txwi[2] = (unsigned char)(w >> 16);
    txwi[3] = (unsigned char)(w >> 24);
    txwi[4] = MT_TXWI_ACK_CTL_REQ;
    txwi[5] = (unsigned char)(wcid - 1);
    txwi[6] = (unsigned char)(26 + len);

    h[0] = FC_QOS_DATA_FROMDS & 0xFF;
    h[1] = FC_QOS_DATA_FROMDS >> 8;
    h[2] = 144;                          /* duration, us */
    memcpy(h + 4, c->addr, 6);
    memcpy(h + 10, l->r->address, 6);
    memcpy(h + 16, l->r->address, 6);

    memcpy(b + 56, gip, (size_t)len);

    if (!mt76_send_command(l->m, 0, b, 56 + (unsigned)len)) {
        c->sent_failed++;
        return 0;
    }
    return 1;
}

/* Power on, authenticate, light the guide button: the start-up sequence the
 * wired path already sends, in reply to the controller's announce. */
static void start_client(dlink *l, int wcid)
{
    link_client *c = &l->clients[wcid - 1];
    unsigned char msg[GIP_HEADER_SIZE + 8];
    int i, len;

    for (i = 0; i < gip_init_count; i++) {
        len = gip_build(msg, (int)sizeof msg, gip_init_seq[i].cmd,
                        (unsigned)(++c->seq & 0xFF), gip_init_seq[i].payload,
                        gip_init_seq[i].len);
        send_gip(l, wcid, msg, len);
    }
    c->last_init = time(NULL);
}

static void client_input(dlink *l, int wcid, const pad_state *st)
{
    link_client *c = &l->clients[wcid - 1];
    char id[24];

    c->inputs++;

    if (c->pad < 0) {
        snprintf(id, sizeof id, "%02x:%02x:%02x:%02x:%02x:%02x", c->addr[0],
                 c->addr[1], c->addr[2], c->addr[3], c->addr[4], c->addr[5]);
        c->pad = pads_attach("wireless", id);
    }
    if (c->pad >= 0) pads_update(c->pad, st);

    if (st->buttons != c->last_buttons && c->changes_logged < CHANGES_LOGGED) {
        c->changes_logged++;
        log_line("    client %d input: buttons %04X  LT %4u RT %4u  "
                 "L %6d,%6d  R %6d,%6d", wcid, (unsigned)st->buttons,
                 st->lt, st->rt, st->lx, st->ly, st->rx, st->ry);
    }
    c->last_buttons = st->buttons;
}

static void client_battery(dlink *l, int wcid, const unsigned char *msg, int n)
{
    link_client *c = &l->clients[wcid - 1];
    int level, type;

    if (!gip_decode_battery(msg, n, &level, &type) ||
        (level == c->battery_level && type == c->battery_type))
        return;
    c->battery_level = level;
    c->battery_type = type;
    log_line("  adapter: client %d battery %s (%s)", wcid,
             gip_battery_level_name(level), gip_battery_type_name(type));
}

/* One radio frame's payload from a client: GIP messages back to back.
 * `rssi` is the frame's signal strength in dBm, or 0 if unknown. */
static void client_data(dlink *l, int wcid, const unsigned char *p, uint32_t len,
                        int rssi)
{
    link_client *c;
    char what[32];

    if (wcid < 1 || wcid > MT76_MAX_CLIENTS || !l->clients[wcid - 1].used) {
        l->ignored++;
        return;
    }
    c = &l->clients[wcid - 1];
    c->frames++;
    l->silent_drops = 0;

    if (rssi < 0) {
        if (c->rssi_n == 0 || rssi < c->rssi_min) c->rssi_min = rssi;
        if (c->rssi_n == 0 || rssi > c->rssi_max) c->rssi_max = rssi;
        c->rssi_sum += rssi;
        c->rssi_n++;
    }

    if (c->dumped < DUMP_PER_CLIENT) {
        c->dumped++;
        snprintf(what, sizeof what, "client %d GIP", wcid);
        hex(what, p, len);
    }

    while (len >= GIP_HEADER_SIZE) {
        uint32_t n = GIP_HEADER_SIZE + p[3];
        pad_state st;

        /* Long and chunked messages (length above 127) are not used by a
         * gamepad's input and are not decoded here. */
        if (p[3] & 0x80 || n > len) break;

        if (gip_wants_ack(p)) {
            unsigned char ack[GIP_HEADER_SIZE + 16];
            if (send_gip(l, wcid, ack, gip_build_ack(ack, (int)sizeof ack, p)))
                c->acked++;
        }

        switch (p[0]) {
        case GIP_CMD_ANNOUNCE:
            /* The vendor and product IDs sit at bytes 12-15 of an announce;
             * a shorter one is answered all the same, just not described. */
            if (time(NULL) - c->last_init >= INIT_INTERVAL_SECONDS) {
                if (n >= 16)
                    log_line("  adapter: client %d announced %02X%02X:%02X%02X "
                             "-- starting it", wcid, p[13], p[12], p[15], p[14]);
                else
                    log_line("  adapter: client %d announced -- starting it", wcid);
                start_client(l, wcid);
            }
            break;
        case GIP_CMD_STATUS:
            client_battery(l, wcid, p, (int)n);
            break;
        case GIP_CMD_AUTHENTICATE:
            log_line("  adapter: client %d sent an authentication message", wcid);
            break;
        default:
            if (gip_decode_input(p, (int)n, &st)) client_input(l, wcid, &st);
        }

        p += n;
        len -= n;
    }
}

/* A frame's signal strength: the stronger of the two antennas' readings in
 * the RX descriptor, as signed dBm. These are the raw readings; Linux's mt76
 * adds small per-chip calibration offsets, so they are close but not exact.
 * Returns 0 if neither reading is valid. */
static int frame_rssi(const unsigned char *rxwi)
{
    int a = (int8_t)rxwi[12], b = (int8_t)rxwi[13];

    if (a >= 0 && b >= 0) return 0;
    if (a >= 0) return b;
    if (b >= 0) return a;
    return a > b ? a : b;
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
        /* After the firmware restarts in place it can still number a
         * controller by its previous run's slot. The frame is then matched by
         * its sender's address -- but only if the firmware recognised the
         * sender as one of its stations at all; frames from unknown senders
         * are dropped. (The link is not encrypted, so the firmware's own
         * station match is by address too: this trusts nothing more.) */
        if (wcid < 1 || wcid > MT76_MAX_CLIENTS) {
            l->ignored++;
            break;
        }
        if (!l->clients[wcid - 1].used ||
            memcmp(l->clients[wcid - 1].addr, addr2, 6) != 0) {
            int i, found = 0;
            for (i = 0; i < MT76_MAX_CLIENTS && !found; i++)
                if (l->clients[i].used && memcmp(l->clients[i].addr, addr2, 6) == 0)
                    found = i + 1;
            if (found && !l->renumbered++)
                log_line("  adapter: data from client %d arrives as %d -- "
                         "matching it by address", found, wcid);
            wcid = found;
        }
        client_data(l, wcid, body, body_len, frame_rssi(p));
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

void link_close(dlink *l, int power_off)
{
    unsigned char msg[GIP_HEADER_SIZE + 4];
    int i;

    /* Switched off rather than left connected to nothing, as Windows and
     * xone do when the host goes away. */
    if (power_off) {
        for (i = 0; i < MT76_MAX_CLIENTS; i++) {
            link_client *c = &l->clients[i];
            if (!c->used) continue;
            send_gip(l, i + 1, msg,
                     gip_build_power_off(msg, (int)sizeof msg,
                                         (unsigned)(++c->seq & 0xFF)));
        }
        usleep(200000);   /* let the radio send them */
    }

    for (i = 0; i < MT76_MAX_CLIENTS; i++)
        if (l->clients[i].used)
            remove_client(l, i + 1, power_off ? "switched off" : "adapter gone");
}

void link_tick(dlink *l, time_t now)
{
    static time_t last_note;
    int i;

    /* A controller that associates and then stays silent is not usable.
     * Dropping it makes it associate again from scratch, which has been seen
     * to work where waiting did not. After a few tries in a row, the adapter
     * itself is the likely problem, and only a replug resets that. */
    for (i = 0; i < MT76_MAX_CLIENTS; i++) {
        link_client *c = &l->clients[i];
        if (!c->used || c->frames > 0 || now - c->connected_at < SILENT_SECONDS)
            continue;
        if (++l->silent_drops <= SILENT_RETRIES) {
            log_line("  adapter: client %d connected but sends nothing -- "
                     "dropping it so it reconnects", i + 1);
            remove_client(l, i + 1, "silent");
        } else if (!c->silence_reported) {
            c->silence_reported = 1;
            log_line("  adapter: client %d still sends nothing -- unplug and "
                     "replug the adapter", i + 1);
            notify("FGG-XSense: controller not responding - replug the adapter");
        }
    }

    if (l->pairing && now >= l->pairing_until)
        link_set_pairing(l, 0, "timed out");

    if (access(PAIR_TRIGGER, F_OK) == 0) {
        unlink(PAIR_TRIGGER);
        link_set_pairing(l, 1, "pair file");
    }

    if (now - last_note >= REPORT_SECONDS) {
        last_note = now;
        for (i = 0; i < MT76_MAX_CLIENTS; i++) {
            link_client *c = &l->clients[i];
            if (!c->used || c->rssi_n == 0) continue;
            log_line("  adapter: client %d signal about %ld dBm (weakest %d, "
                     "strongest %d) over %ld frames", i + 1,
                     c->rssi_sum / c->rssi_n, c->rssi_min, c->rssi_max, c->rssi_n);
            c->rssi_sum = c->rssi_n = 0;
        }
        log_line("  adapter: %ld message(s), %ld radio frame(s), %ld ignored, "
                 "%d client(s), %ld lost while sending; button %ld, lost %ld",
                 l->messages, l->wlan, l->ignored, l->nclients, l->m->dropped,
                 l->events[EVT_BUTTON], l->events[EVT_CLIENT_LOST]);
    }
}
