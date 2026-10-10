#include "dongle/dongle.h"
#include "dongle/mt76_fw.h"
#include "log.h"
#include "usb/ugen.h"

#define MS_VENDOR 0x045E

int dongle_match(unsigned vendor, unsigned product,
                 const unsigned char *cfg, int cfglen)
{
    (void)cfg;
    (void)cfglen;

    return vendor == MS_VENDOR &&
           (product == 0x02E6 || product == 0x02FE ||
            product == 0x02F9 || product == 0x091E);
}

void dongle_report(const char *node)
{
    unsigned vendor = 0, product = 0;
    char why[160];
    mt76_fw fw;
    ugen *u;

    u = ugen_open(node);
    if (!u || !ugen_ids(u, &vendor, &product)) {
        log_line("  adapter %s: could not read its identity", node);
        ugen_close(u);
        return;
    }
    ugen_close(u);

    log_line("  wireless adapter %s  %04X:%04X", node, vendor, product);

    if (!mt76_fw_load(product, &fw, why, (int)sizeof why)) {
        log_line("  adapter firmware: %s", why);
        notify("FGG-XSense: adapter found, firmware missing - see README");
        return;
    }

    log_line("  adapter firmware: checksum ok, %zu bytes: ILM %u + DLM %u, "
             "version %04X build %04X, built \"%s\"",
             fw.size, fw.ilm_len, fw.dlm_len, fw.fw_version, fw.build_version,
             fw.build_time);
    mt76_fw_free(&fw);
}
