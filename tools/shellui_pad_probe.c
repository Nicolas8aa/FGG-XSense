/* One-shot read-only query of a ShellUI pad handle observed in the kernel log.
 * It neither starts nor stops FGG-XSense and makes no controller changes. */
#include "log.h"
#include "shell_call.h"

#include <ps5/kernel.h>

#include <string.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH STATE_DIR "/shellui-pad-probe.log"

#define SYSTEM_AUTHID 0x4800000000010003ull

static int raise_creds(uint64_t *authid, uint8_t caps[16])
{
    static const uint8_t all[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    pid_t me = getpid();

    *authid = kernel_get_ucred_authid(me);
    return *authid != 0 && kernel_get_ucred_caps(me, caps) == 0 &&
           kernel_set_ucred_authid(me, SYSTEM_AUTHID) == 0 &&
           kernel_set_ucred_caps(me, all) == 0;
}

int main(void)
{
    /* Current PSS "Open Pad" return for the physical DualSense. */
    const int32_t physical_handle = 0x038a0400;
    uint64_t authid;
    uint8_t caps[16];

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense ShellUI pad probe -- read-only");
    log_line("querying physical CIM padHandle %#x", (unsigned)physical_handle);
    if (raise_creds(&authid, caps)) {
        shellui_pad_info_probe(NULL, physical_handle);
        kernel_set_ucred_authid(getpid(), authid);
        kernel_set_ucred_caps(getpid(), caps);
    } else {
        log_line("could not raise this probe's temporary credentials");
    }
    log_line("done");
    log_close();
    return 0;
}
