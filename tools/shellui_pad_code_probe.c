/* Read only: copy the already-loaded AddDevice code for offline analysis. */
#include "log.h"
#include "shell_call.h"

#include <ps5/kernel.h>
#include <stdint.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH STATE_DIR "/shellui-pad-code-probe.log"
#define CODE_PATH STATE_DIR "/shellui-pad-adddevice.bin"
#define SYSTEM_AUTHID 0x4800000000010003ull

uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

static int raise_creds(uint64_t *authid, uint8_t caps[16])
{
    static const uint8_t all[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    pid_t me = getpid();

    *authid = kernel_get_ucred_authid(me);
    return *authid != 0 && kernel_get_ucred_caps(me, caps) == 0 &&
           kernel_set_ucred_authid(me, SYSTEM_AUTHID) == 0 &&
           kernel_set_ucred_caps(me, all) == 0;
}

int main(void)
{
    int result;
    uint64_t authid;
    uint8_t caps[16];

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense ShellUI AddDevice code probe -- read-only");
    if (!raise_creds(&authid, caps)) {
        log_line("could not temporarily raise this probe's credentials");
        return 1;
    }
    result = shellui_dump_pad_adddevice_code(CODE_PATH);
    kernel_set_ucred_authid(getpid(), authid);
    kernel_set_ucred_caps(getpid(), caps);
    log_line("copy %s (result %d)", result == 0 ? "complete" : "failed", result);
    return result != 0;
}
