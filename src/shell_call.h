/* Calling a function inside one of the console's own processes.
 *
 * Some system calls are refused from a payload but accepted from the shell:
 * binding a virtual pad to a user over MBus is one. This attaches to the
 * named process with ptrace, runs one call there with the process's own
 * rights, puts everything back, and detaches -- the process is paused for the
 * duration of the call and is otherwise untouched.
 *
 * Ported from MegaCadeDev/YetAnotherControllerEnabler src/pad_assign.c,
 * GPL-3.0-or-later.
 */
#ifndef FGG_SHELL_CALL_H
#define FGG_SHELL_CALL_H

#include <stdint.h>
#include <stddef.h>

/* Runs `lib`'s `sym`(a1, a2) inside the process named `process` (e.g.
 * "SceShellUI"). Returns 0 and the function's return value in `out` if the
 * call was made, or a negative number if it could not be. */
int shell_call(const char *process, const char *lib, const char *sym,
               uint64_t a1, uint64_t a2, int64_t *out);

/* Injects a pressed then released ScePadData report into a virtual pad from
 * SceShellUI itself.  The assignment screen deliberately ignores reports
 * from a payload while the new pad is pending, but accepts this one.  `data`
 * must be a 120-byte ScePadData whose button word contains SCE_PS; it is
 * copied into ShellUI's stack and never retained.  Returns 0 only after both
 * reports were accepted by scePadVirtualDeviceInsertData. */
int shellui_press_ps(int32_t handle, const void *data, size_t len);

/* Enumerates ShellUI's existing pad handles and reads the public controller
 * information records into temporary ShellUI stack storage.  It only logs
 * the results and never opens, binds, or feeds a device. */
void shellui_pad_info_probe(const int32_t users[4], int32_t known_handle);

/* Copies the loaded code bytes of scePadVirtualDeviceAddDevice from ShellUI
 * into `path` for offline disassembly.  No function is invoked in ShellUI;
 * the process is only briefly stopped while ptrace reads its executable map.
 * Returns 0 on success. */
int shellui_dump_pad_adddevice_code(const char *path);

/* Logs which likely MBus battery/status exports are present in ShellUI.  It
 * resolves symbols only and never invokes one. */
int shellui_mbus_symbol_probe(void);

/* Copies the loaded MBus device-function setter implementation for offline
 * disassembly.  The setter itself is never invoked. */
int shellui_dump_mbus_setfunction_code(const char *path);

/* Waits for one normal ShellUI call to the MBus device-function setter and
 * logs its private request bytes.  The temporary breakpoint is restored
 * before the intercepted call resumes; no MBus call is synthesized. */
int shellui_capture_mbus_setfunction_request(void);

#endif
