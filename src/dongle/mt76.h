/* Talking to the Xbox Wireless Adapter's MediaTek MT76 chip.
 *
 * Registers are read and written with vendor control requests; commands for
 * the chip's microcontroller, firmware included, go out on bulk endpoint 04
 * behind a four-byte header. Ported from medusalix/xow and dlundqvist/xone
 * (both GPL-2.0-or-later); register numbers from OpenWrt mt76 (ISC).
 */
#ifndef FGG_MT76_H
#define FGG_MT76_H

#include "usb/ugen.h"

#include <stdint.h>

/* Set in a register address to reach the chip's configuration space, which
 * uses the CFG pair of vendor requests instead of the MULTI pair. */
#define MT_VEND_TYPE_CFG 0x40000000u

/* Registers used so far. */
#define MT_RF_PATCH                     0x0130
#define MT_FCE_DMA_ADDR                 0x0230
#define MT_FCE_DMA_LEN                  0x0234
#define MT_FCE_PSE_CTRL                 0x0800
#define MT_TX_CPU_FROM_FCE_BASE_PTR     0x09a0
#define MT_TX_CPU_FROM_FCE_MAX_COUNT    0x09a4
#define MT_TX_CPU_FROM_FCE_CPU_DESC_IDX 0x09a8
#define MT_FCE_PDMA_GLOBAL_CONF         0x09c4
#define MT_FCE_SKIP_FS                  0x0a6c
#define MT_USB_U3DMA_CFG                0x9018
#define MT_USB_DMA_CFG_RX_BULK_EN       (1u << 22)
#define MT_USB_DMA_CFG_TX_BULK_EN       (1u << 23)

/* The bulk OUT endpoint for commands and firmware, and the slot it uses. */
#define MT76_EP_OUT    0x04
#define MT76_SLOT_CMD  0

/* Largest command payload: a firmware chunk. The slot carries it plus the
 * header, up to three bytes of padding and the four-byte trailer. */
#define MT76_CMD_MAX   0x3800
#define MT76_CMD_SLOT_SIZE (4 + MT76_CMD_MAX + 4 + 4)

typedef struct {
    ugen *u;
    int   err;     /* errno of the last failed request, 0 if none */
    int   trace;   /* log every register access */
} mt76;

int mt76_rr(mt76 *m, uint32_t addr, uint32_t *val);
int mt76_wr(mt76 *m, uint32_t addr, uint32_t val);

/* Reads `addr` until (value & mask) == want, up to 50 times 10-20 ms apart
 * as xone does. Returns 1 on a match; `last` receives the final value. */
int mt76_poll(mt76 *m, uint32_t addr, uint32_t mask, uint32_t want,
              uint32_t *last);

/* The DEV_MODE vendor request: 0x01 resets the firmware, 0x12 starts it
 * (loads the interrupt vector block). */
int mt76_dev_mode(mt76 *m, unsigned mode);

/* Sends a command to the chip's microcontroller and waits for the write. */
int mt76_send_command(mt76 *m, unsigned cmd, const void *payload, unsigned len);

/* Whether the last failure means the device has left the bus, as it does
 * briefly once its firmware starts. */
int mt76_gone(const mt76 *m);

#endif
