/* Bringing up the adapter's radio once its firmware runs.
 *
 * Follows xone's xone_mt76_init_radio step for step: power, the register
 * table, crystal and radio calibration, the MAC address controllers expect,
 * picking a channel, and finally the beacon that lets controllers find the
 * adapter. Each step is logged, so a failure names the step that failed.
 */
#ifndef FGG_MT76_RADIO_H
#define FGG_MT76_RADIO_H

#include "dongle/mt76.h"

#define MT76_NUM_CHANNELS 12

typedef struct {
    uint8_t index;       /* channel number */
    uint8_t band;        /* power table column */
    uint8_t bandwidth;   /* MT_PHY_BW_* */
    uint8_t group;       /* 5 GHz calibration group */
    uint8_t scan;
    uint8_t power;
} mt76_channel;

typedef struct {
    uint16_t     chip_id;
    uint8_t      address[6];
    mt76_channel channels[MT76_NUM_CHANNELS];
    int          channel;   /* index into channels */
} mt76_radio;

/* Returns 0 if a step failed; the log says which. */
int mt76_radio_init(mt76 *m, mt76_radio *r);

/* Opens or closes the pairing window: the beacon advertises it, and the
 * receive filter is widened to let pairing requests in. */
int mt76_radio_set_pairing(mt76 *m, mt76_radio *r, int enable);

#endif
