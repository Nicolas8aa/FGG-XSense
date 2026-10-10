/* The adapter's side of the radio link: what arrives, and who is connected.
 *
 * Incoming messages are either microcontroller events (a button press, a
 * client lost) or 802.11 frames from controllers: association and pairing
 * requests, and data frames whose payload is plain GIP. This keeps the table
 * of associated controllers and the pairing window, following xone's
 * transport/dongle.c.
 */
#ifndef FGG_LINK_H
#define FGG_LINK_H

#include "dongle/mt76.h"
#include "dongle/mt76_radio.h"

#include <time.h>

typedef struct {
    int     used;
    uint8_t addr[6];
    long    frames, inputs, acked, sent_failed;
    int     dumped;
    int     seq;            /* our GIP sequence number towards it */
    int     pad;            /* pad table slot, -1 until it sends input */
    time_t  last_init;      /* when the start-up sequence was last sent */
    uint32_t last_buttons;
    int     changes_logged;
    time_t  connected_at;
    int     silence_reported;
    int     battery_level, battery_type;
    long    rssi_sum, rssi_n;   /* signal readings since the last report */
    int     rssi_min, rssi_max;
} link_client;

typedef struct {
    mt76       *m;
    mt76_radio *r;

    int    pairing;
    time_t pairing_until, last_button;

    link_client clients[MT76_MAX_CLIENTS];   /* index = wcid - 1 */
    int         nclients;

    long messages, wlan, errors, ignored, refused, renumbered, events[16];
    int  silent_drops;   /* consecutive silent clients dropped */
} dlink;

void link_init(dlink *l, mt76 *m, mt76_radio *r);

/* Opens (for the standard 60 seconds) or closes the pairing window. */
void link_set_pairing(dlink *l, int on, const char *why);

/* Handles one incoming message from either endpoint. */
void link_handle(dlink *l, const unsigned char *p, uint32_t len);

/* Removes every controller: switching each off first when the payload is
 * stopping, not when the adapter has already gone. */
void link_close(dlink *l, int power_off);

/* Housekeeping: pairing timeout, the pairing trigger file, the periodic
 * summary. */
void link_tick(dlink *l, time_t now);

#endif
