/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portions ported from dlundqvist/xone transport/mt76.c and medusalix/xow
 * dongle/mt76.cpp, (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/mt76.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define MT_VEND_DEV_MODE    0x01
#define MT_VEND_MULTI_WRITE 0x06
#define MT_VEND_MULTI_READ  0x07
#define MT_VEND_WRITE_CFG   0x46
#define MT_VEND_READ_CFG    0x47

#define REQ_VENDOR_IN  0xC0
#define REQ_VENDOR_OUT 0x40

/* Command header (xone mt76_defs.h MT_MCU_MSG_*). */
#define MT_MCU_MSG_TYPE_CMD (1u << 30)
#define MT_MCU_MSG_PORT(p)  (((uint32_t)(p) & 0x7) << 27)
#define MT_MCU_MSG_CMD(c)   (((uint32_t)(c) & 0x7F) << 20)
#define MT_CPU_TX_PORT      2

#define BULK_TIMEOUT_MS 1000
#define POLL_RETRIES    50

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

int mt76_rr(mt76 *m, uint32_t addr, uint32_t *val)
{
    unsigned req = MT_VEND_MULTI_READ;
    unsigned char d[4] = { 0 };

    if (addr & MT_VEND_TYPE_CFG) {
        req = MT_VEND_READ_CFG;
        addr &= ~MT_VEND_TYPE_CFG;
    }

    if (ugen_ctrl(m->u, REQ_VENDOR_IN, req, addr >> 16, addr & 0xFFFF,
                  d, sizeof d) != (int)sizeof d) {
        m->err = errno ? errno : EIO;
        if (m->trace) log_line("    rr %04X: failed (%d)", (unsigned)addr, m->err);
        return 0;
    }

    *val = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
           ((uint32_t)d[3] << 24);
    if (m->trace) log_line("    rr %04X = %08X", (unsigned)addr, (unsigned)*val);
    return 1;
}

int mt76_wr(mt76 *m, uint32_t addr, uint32_t val)
{
    unsigned req = MT_VEND_MULTI_WRITE;
    unsigned char d[4];

    if (addr & MT_VEND_TYPE_CFG) {
        req = MT_VEND_WRITE_CFG;
        addr &= ~MT_VEND_TYPE_CFG;
    }

    put_le32(d, val);
    if (m->trace) log_line("    wr %04X = %08X", (unsigned)addr, (unsigned)val);

    if (ugen_ctrl(m->u, REQ_VENDOR_OUT, req, addr >> 16, addr & 0xFFFF,
                  d, sizeof d) != (int)sizeof d) {
        m->err = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

int mt76_poll(mt76 *m, uint32_t addr, uint32_t mask, uint32_t want,
              uint32_t *last)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < POLL_RETRIES; i++) {
        if (!mt76_rr(m, addr, &v)) {
            if (mt76_gone(m)) break;
        } else if ((v & mask) == want) {
            if (last) *last = v;
            return 1;
        }
        usleep(15000);
    }
    if (last) *last = v;
    return 0;
}

int mt76_dev_mode(mt76 *m, unsigned mode)
{
    if (m->trace) log_line("    dev_mode %02X", mode);
    if (ugen_ctrl(m->u, REQ_VENDOR_OUT, MT_VEND_DEV_MODE, mode, 0, NULL, 0) < 0) {
        m->err = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

int mt76_send_command(mt76 *m, unsigned cmd, const void *payload, unsigned len)
{
    unsigned char *buf;
    unsigned padded, total;
    ugen_completion c;

    if (len > MT76_CMD_MAX) {
        m->err = EINVAL;
        return 0;
    }

    /* Header, payload padded to four bytes, then a four-byte zero trailer
     * (xone xone_mt76_prep_message). */
    padded = (len + 3) & ~3u;
    total = 4 + padded + 4;

    buf = ugen_slot_buf(m->u, MT76_SLOT_CMD);
    put_le32(buf, MT_MCU_MSG_TYPE_CMD | MT_MCU_MSG_PORT(MT_CPU_TX_PORT) |
                  MT_MCU_MSG_CMD(cmd) | padded);
    memcpy(buf + 4, payload, len);
    memset(buf + 4 + len, 0, total - 4 - len);

    if (!ugen_fs_start(m->u, MT76_SLOT_CMD, total)) {
        m->err = errno ? errno : EIO;
        return 0;
    }
    if (!ugen_fs_wait(m->u, MT76_SLOT_CMD, BULK_TIMEOUT_MS + 200, &c)) {
        m->err = ETIMEDOUT;
        return 0;
    }
    if (c.status != 0) {
        m->err = EIO;
        if (m->trace) log_line("    command %u: status %d", cmd, c.status);
        return 0;
    }
    return 1;
}

int mt76_gone(const mt76 *m)
{
    return m->err == ENXIO || m->err == ENODEV;
}
