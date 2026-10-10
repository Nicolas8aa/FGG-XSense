/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from MegaCadeDev/YetAnotherControllerEnabler src/pad_assign.c
 * (find_pid, pt_io, resolve_sym, code_cave, pt_call3, call_remote),
 * GPL-3.0-or-later. */
#include "shell_call.h"
#include "log.h"

#include <machine/reg.h>
#include <ps5/kernel.h>
#include <ps5/nid.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How long one call may take before it is abandoned, in milliseconds. */
#define CALL_TIMEOUT_MS 2000

static int sys_ptrace(int request, pid_t pid, caddr_t addr, int data)
{
    return (int)__syscall(SYS_ptrace, request, pid, addr, data);
}

/* The lowest pid whose thread name is `name`, from the kernel's process
 * table (layout as YACE reads it on the PS5). */
static pid_t find_pid(const char *name)
{
    int mib[4] = { 1, 14, 8, 0 };
    size_t size = 0;
    uint8_t *buf, *ptr;
    pid_t found = -1;

    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return -1;
    buf = malloc(size);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
        free(buf);
        return -1;
    }
    for (ptr = buf; ptr + 8 < buf + size;) {
        int stride = *(int *)ptr;
        pid_t pid;
        const char *tdname;

        if (stride < 64 || stride > 4096 || ptr + stride > buf + size) break;
        pid = *(pid_t *)(ptr + 72);
        tdname = (const char *)(ptr + 447);
        if (pid > 0 && strcmp(tdname, name) == 0 && (found < 0 || pid < found))
            found = pid;
        ptr += stride;
    }
    free(buf);
    return found;
}

static int pt_io(pid_t pid, int op, intptr_t remote, void *local, size_t len)
{
    struct ptrace_io_desc iod;

    iod.piod_op = op;
    iod.piod_offs = (void *)remote;
    iod.piod_addr = local;
    iod.piod_len = len;
    return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static intptr_t resolve_sym(pid_t pid, const char *lib, const char *sym)
{
    uint32_t handle = 0;
    intptr_t addr;
    char sprx[64], nid[12];

    if (kernel_dynlib_handle(pid, lib, &handle) != 0 || handle == 0) {
        snprintf(sprx, sizeof sprx, "%s.sprx", lib);
        if (kernel_dynlib_handle(pid, sprx, &handle) != 0) return 0;
    }
    addr = kernel_dynlib_dlsym(pid, handle, sym);
    if (!addr) {
        nid_encode(sym, nid);
        addr = kernel_dynlib_resolve(pid, handle, nid);
    }
    return addr;
}

/* A byte the call can return to and trap on: the library's init routine,
 * which has long since run. */
static intptr_t code_cave(pid_t pid, const char *lib)
{
    uint32_t handle = 0;
    intptr_t addr;
    char sprx[64];

    if (kernel_dynlib_handle(pid, lib, &handle) != 0 || handle == 0) {
        snprintf(sprx, sizeof sprx, "%s.sprx", lib);
        if (kernel_dynlib_handle(pid, sprx, &handle) != 0 || handle == 0) return 0;
    }
    addr = kernel_dynlib_init_addr(pid, handle);
    if (!addr) addr = kernel_dynlib_fini_addr(pid, handle);
    return addr;
}

/* Calls fn(a1, a2, a3) inside the stopped process, returning to `trap`.
 * Every register is put back afterwards. */
static int64_t pt_call3(pid_t pid, intptr_t fn, intptr_t trap, uint64_t a1,
                        uint64_t a2, uint64_t a3, int *ok)
{
    struct reg regs, saved;
    intptr_t new_rsp;
    int status, waited = 0, trapped = 0;

    *ok = 0;
    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) return -1;
    saved = regs;
    new_rsp = (regs.r_rsp - 256) & ~(intptr_t)0xf;
    if (pt_io(pid, PIOD_WRITE_D, new_rsp, &trap, sizeof trap) != 0) return -1;
    regs.r_rsp = new_rsp;
    regs.r_rip = fn;
    regs.r_rdi = (int64_t)a1;
    regs.r_rsi = (int64_t)a2;
    regs.r_rdx = (int64_t)a3;
    regs.r_rcx = regs.r_r8 = regs.r_r9 = 0;
    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) != 0) return -1;
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0) != 0) {
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    while (waited < CALL_TIMEOUT_MS) {
        int got = waitpid(pid, &status, WNOHANG), sig;

        if (got < 0) break;
        if (got == 0) {
            usleep(1000);
            waited++;
            continue;
        }
        if (!WIFSTOPPED(status)) break;
        sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            trapped = 1;
            break;
        }
        sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, sig == SIGCHLD ? 0 : sig);
    }
    if (!trapped) {
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) {
        sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    *ok = 1;
    return (int64_t)regs.r_rax;
}

int shell_call(const char *process, const char *lib, const char *sym,
               uint64_t a1, uint64_t a2, int64_t *out)
{
    uint8_t original = 0x90, breakpoint = 0xcc;
    intptr_t fn, trap;
    pid_t pid;
    int ok;

    pid = find_pid(process);
    if (pid < 0) return -1;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -2;
    if (waitpid(pid, NULL, 0) < 0) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -3;
    }
    fn = resolve_sym(pid, lib, sym);
    trap = code_cave(pid, lib);
    if (!fn || !trap) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -4;
    }
    kernel_set_vmem_protection(pid, trap, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (pt_io(pid, PIOD_READ_D, trap, &original, 1) != 0) original = 0x90;
    pt_io(pid, PIOD_WRITE_D, trap, &breakpoint, 1);
    *out = pt_call3(pid, fn, trap, a1, a2, 0, &ok);
    pt_io(pid, PIOD_WRITE_D, trap, &original, 1);
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return ok ? 0 : -5;
}

int shellui_press_ps(int32_t handle, const void *data, size_t len)
{
    uint8_t original = 0x90, breakpoint = 0xcc;
    intptr_t fn, trap, data_addr;
    struct reg regs;
    pid_t pid;
    int frame, ok, result = -1;
    int64_t call_result;

    if (handle <= 0 || !data || len != 120) return -1;
    pid = find_pid("SceShellUI");
    if (pid < 0) return -2;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -3;
    if (waitpid(pid, NULL, 0) < 0) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -4;
    }
    fn = resolve_sym(pid, "libScePad", "scePadVirtualDeviceInsertData");
    trap = code_cave(pid, "libScePad");
    if (!fn || !trap || sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0)
        goto detach;
    kernel_set_vmem_protection(pid, trap, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (pt_io(pid, PIOD_READ_D, trap, &original, 1) != 0 ||
        pt_io(pid, PIOD_WRITE_D, trap, &breakpoint, 1) != 0)
        goto detach;

    /* Keep the input clear of both the call's temporary stack frame and the
     * x86-64 red zone.  YACE uses the same placement. */
    data_addr = (regs.r_rsp - 4096) & ~(intptr_t)0xf;
    if (pt_io(pid, PIOD_WRITE_D, data_addr, (void *)data, len) != 0)
        goto restore;
    for (frame = 0; frame < 12; frame++) {
        call_result = pt_call3(pid, fn, trap, (uint64_t)(uint32_t)handle,
                               (uint64_t)data_addr, 0, &ok);
        if (!ok || call_result != 0)
            goto restore;
    }

    /* `data` is a mutable local report in the caller.  Its release version
     * differs only in the first four bytes, so write a zero button word over
     * the remote copy then deliver one final frame. */
    {
        uint32_t released = 0;
        if (pt_io(pid, PIOD_WRITE_D, data_addr, &released, sizeof released) != 0)
            goto restore;
        call_result = pt_call3(pid, fn, trap, (uint64_t)(uint32_t)handle,
                               (uint64_t)data_addr, 0, &ok);
        if (!ok || call_result != 0)
            goto restore;
    }
    result = 0;

restore:
    pt_io(pid, PIOD_WRITE_D, trap, &original, 1);
detach:
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return result;
}

/* ---- read-only ShellUI pad-information probe --------------------------- */

#define SHELL_PAD_INFO_BYTES 256

static void shell_dump_info(const char *what, const unsigned char *data, unsigned n)
{
    char line[3 * 16 + 1];
    unsigned i, offset;

    for (offset = 0; offset < n; offset += 16) {
        unsigned len = n - offset < 16 ? n - offset : 16;
        for (i = 0; i < len; i++)
            snprintf(line + 3 * i, 4, "%02X ", data[offset + i]);
        line[3 * len] = '\0';
        log_line("    ShellUI pad info %s +%02x: %s", what, offset, line);
    }
}

void shellui_pad_info_probe(const int32_t users[4], int32_t known_handle)
{
    uint8_t original = 0x90, breakpoint = 0xcc;
    unsigned char data[SHELL_PAD_INFO_BYTES];
    int32_t candidates[7] = { -1, -1, -1, -1, -1, 1, 0x10000000 };
    intptr_t get_handle, info, ext_info, trap, data_addr;
    struct reg regs;
    pid_t pid;
    int i, j, count = 0, type, index, ok;

    if (users) {
        for (i = 0; i < 4; i++) {
            if (users[i] < 0) continue;
            for (j = 0; j < count && candidates[j] != users[i]; j++) {}
            if (j == count) candidates[count++] = users[i];
        }
    }
    for (i = 4; i < 7; i++) {
        for (j = 0; j < count && candidates[j] != candidates[i]; j++) {}
        if (j == count) candidates[count++] = candidates[i];
    }
    pid = find_pid("SceShellUI");
    if (pid < 0) {
        log_line("  vpad diag: ShellUI pad probe: SceShellUI not found");
        return;
    }
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) {
        log_line("  vpad diag: ShellUI pad probe: could not attach");
        return;
    }
    if (waitpid(pid, NULL, 0) < 0) {
        log_line("  vpad diag: ShellUI pad probe: attach wait failed");
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return;
    }
    get_handle = resolve_sym(pid, "libScePad", "scePadGetHandle");
    info = resolve_sym(pid, "libScePad", "scePadGetControllerInformation");
    ext_info = resolve_sym(pid, "libScePad", "scePadGetExtControllerInformation");
    trap = code_cave(pid, "libScePad");
    if (!get_handle || !info || !ext_info || !trap ||
        sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) {
        log_line("  vpad diag: ShellUI pad probe: required Pad exports unavailable");
        goto detach;
    }
    kernel_set_vmem_protection(pid, trap, 16, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (pt_io(pid, PIOD_READ_D, trap, &original, 1) != 0 ||
        pt_io(pid, PIOD_WRITE_D, trap, &breakpoint, 1) != 0) {
        log_line("  vpad diag: ShellUI pad probe: could not set temporary trap");
        goto detach;
    }

    /* Keep output clear of each remote call frame and the ABI red zone. */
    data_addr = (regs.r_rsp - 4096) & ~(intptr_t)0xf;
    log_line("  vpad diag: ShellUI public pad-information probe begins (read-only)");
    if (known_handle > 0) {
        unsigned char basic[SHELL_PAD_INFO_BYTES];
        int32_t r1, r2;

        memset(data, 0xA5, sizeof data);
        if (pt_io(pid, PIOD_WRITE_D, data_addr, data, sizeof data) == 0) {
            r1 = (int32_t)pt_call3(pid, info, trap, (uint64_t)(uint32_t)known_handle,
                                   (uint64_t)data_addr, 0, &ok);
            if (ok && r1 == 0 &&
                pt_io(pid, PIOD_READ_D, data_addr, basic, sizeof basic) != 0)
                r1 = -1;
            memset(data, 0xA5, sizeof data);
            if (pt_io(pid, PIOD_WRITE_D, data_addr, data, sizeof data) == 0) {
                r2 = (int32_t)pt_call3(pid, ext_info, trap,
                                       (uint64_t)(uint32_t)known_handle,
                                       (uint64_t)data_addr, 0, &ok);
                if (!ok) r2 = -1;
                log_line("  vpad diag: ShellUI known handle %#x -> info %#x ext %#x",
                         (unsigned)known_handle, (unsigned)r1, (unsigned)r2);
                if (r1 == 0) shell_dump_info("info", basic, 64);
                if (r2 == 0 &&
                    pt_io(pid, PIOD_READ_D, data_addr, data, sizeof data) == 0)
                    shell_dump_info("ext ", data, 96);
            }
        }
    }
    /* One-shot probes provide an exact PSS/CIM handle.  Do not fall back to
     * port enumeration there: some unavailable ports take the full remote
     * call timeout and provide no additional read-only information. */
    if (!users) goto complete;
    for (i = 0; i < count; i++) {
        for (type = 0; type <= 3; type++) {
            for (index = 0; index < 4; index++) {
                unsigned char basic[SHELL_PAD_INFO_BYTES];
                int32_t handle, r1, r2;

                handle = (int32_t)pt_call3(pid, get_handle, trap,
                                            (uint64_t)(uint32_t)users[i],
                                            (uint64_t)(uint32_t)type,
                                            (uint64_t)(uint32_t)index, &ok);
                if (!ok || handle <= 0) continue;
                memset(data, 0xA5, sizeof data);
                if (pt_io(pid, PIOD_WRITE_D, data_addr, data, sizeof data) != 0) continue;
                r1 = (int32_t)pt_call3(pid, info, trap, (uint64_t)(uint32_t)handle,
                                       (uint64_t)data_addr, 0, &ok);
                if (!ok) continue;
                if (r1 == 0 && pt_io(pid, PIOD_READ_D, data_addr, basic, sizeof basic) != 0)
                    r1 = -1;
                memset(data, 0xA5, sizeof data);
                if (pt_io(pid, PIOD_WRITE_D, data_addr, data, sizeof data) != 0) continue;
                r2 = (int32_t)pt_call3(pid, ext_info, trap, (uint64_t)(uint32_t)handle,
                                       (uint64_t)data_addr, 0, &ok);
                if (!ok) continue;
                log_line("  vpad diag: ShellUI user %#x type %d index %d handle %#x -> "
                         "info %#x ext %#x", (unsigned)users[i], type, index,
                         (unsigned)handle, (unsigned)r1, (unsigned)r2);
                if (r1 == 0) shell_dump_info("info", basic, 64);
                if (r2 == 0 && pt_io(pid, PIOD_READ_D, data_addr, data, sizeof data) == 0)
                    shell_dump_info("ext ", data, 96);
            }
        }
    }
complete:
    log_line("  vpad diag: ShellUI public pad-information probe complete");
    pt_io(pid, PIOD_WRITE_D, trap, &original, 1);
detach:
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
}

/* ---- static libScePad inspection --------------------------------------- */

#define PAD_ADD_CODE_BYTES 8192
#define PAD_ADD_CODE_CHUNK 256
#define MBUS_SET_CODE_BYTES 32768

int shellui_dump_pad_adddevice_code(const char *path)
{
    unsigned char code[PAD_ADD_CODE_BYTES];
    intptr_t fn;
    pid_t pid;
    int fd, result = -1;
    size_t offset;

    if (!path || !*path) return -1;
    pid = find_pid("SceShellUI");
    if (pid < 0) return -2;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -3;
    if (waitpid(pid, NULL, 0) < 0) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -4;
    }
    fn = resolve_sym(pid, "libScePad", "scePadVirtualDeviceAddDevice");
    if (!fn) {
        log_line("  vpad diag: AddDevice export unavailable in ShellUI");
        goto detach;
    }
    for (offset = 0; offset < sizeof code; offset += PAD_ADD_CODE_CHUNK) {
        if (pt_io(pid, PIOD_READ_D, fn + (intptr_t)offset, code + offset,
                  PAD_ADD_CODE_CHUNK) != 0) {
            log_line("  vpad diag: could not read AddDevice code at +%#x (errno %d)",
                     (unsigned)offset, errno);
            goto detach;
        }
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) goto detach;
    for (offset = 0; offset < sizeof code;) {
        ssize_t n = write(fd, code + offset, sizeof code - offset);
        if (n <= 0) break;
        offset += (size_t)n;
    }
    close(fd);
    if (offset == sizeof code) {
        log_line("  vpad diag: copied %u code bytes from ShellUI libScePad AddDevice at %#llx",
                 (unsigned)sizeof code, (unsigned long long)fn);
        result = 0;
    }
detach:
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return result;
}

int shellui_mbus_symbol_probe(void)
{
    static const char *const names[] = {
        "sceMbusGetDeviceStatus",
        "sceMbusSetDeviceStatus",
        "sceMbusGetDeviceFunctionState",
        "sceMbusSetDeviceFunctionState",
        "sceMbusGetFunctionState",
        "sceMbusSetFunctionState",
        "sceMbusGetDeviceInfo",
        "sceMbusUpdateDeviceStatus",
        "sceMbusSetDeviceBatteryState",
    };
    pid_t pid;
    unsigned i, found = 0;

    pid = find_pid("SceShellUI");
    if (pid < 0) return -1;
    log_line("  vpad diag: ShellUI MBus symbol survey (read-only)");
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        intptr_t address = resolve_sym(pid, "libSceMbus", names[i]);
        if (address) {
            log_line("    %s = %#llx", names[i], (unsigned long long)address);
            found++;
        }
    }
    log_line("  vpad diag: ShellUI MBus symbol survey found %u candidate(s)", found);
    return (int)found;
}

int shellui_dump_mbus_setfunction_code(const char *path)
{
    unsigned char code[MBUS_SET_CODE_BYTES];
    intptr_t fn;
    pid_t pid;
    int fd, result = -1;
    size_t offset;

    if (!path || !*path) return -1;
    pid = find_pid("SceShellUI");
    if (pid < 0) return -2;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -3;
    if (waitpid(pid, NULL, 0) < 0) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -4;
    }
    fn = resolve_sym(pid, "libSceMbus", "sceMbusSetDeviceFunctionState");
    if (!fn) {
        log_line("  vpad diag: MBus device-function setter export unavailable");
        goto detach;
    }
    for (offset = 0; offset < sizeof code; offset += PAD_ADD_CODE_CHUNK)
        if (kernel_proc_copyout(pid, fn + (intptr_t)offset, code + offset,
                                PAD_ADD_CODE_CHUNK) != 0) {
            log_line("  vpad diag: could not read MBus setter code at +%#x (errno %d)",
                     (unsigned)offset, errno);
            goto detach;
        }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) goto detach;
    for (offset = 0; offset < sizeof code;) {
        ssize_t n = write(fd, code + offset, sizeof code - offset);
        if (n <= 0) break;
        offset += (size_t)n;
    }
    close(fd);
    if (offset == sizeof code) {
        log_line("  vpad diag: copied %u code bytes from ShellUI MBus SetDeviceFunctionState at %#llx",
                 (unsigned)sizeof code, (unsigned long long)fn);
        result = 0;
    }
detach:
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return result;
}

int shellui_capture_mbus_setfunction_request(void)
{
    enum { REQUEST_BYTES = 128, WAIT_MS = 60000 };
    uint8_t original, breakpoint = 0xcc;
    unsigned char request[REQUEST_BYTES];
    intptr_t fn;
    struct reg regs;
    pid_t pid;
    int old_prot, status, waited, result = -1;

    pid = find_pid("SceShellUI");
    if (pid < 0) return -1;
    if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0) return -2;
    if (waitpid(pid, NULL, 0) < 0) {
        sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
        return -3;
    }
    fn = resolve_sym(pid, "libSceMbus", "sceMbusSetDeviceFunctionState");
    if (!fn) goto detach;
    old_prot = kernel_get_vmem_protection(pid, fn, 1);
    if (old_prot < 0 || kernel_set_vmem_protection(pid, fn, 1,
                                                    PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto detach;
    original = kernel_proc_getchar(pid, fn);
    if (kernel_proc_setchar(pid, fn, (char)breakpoint) != 0) {
        kernel_set_vmem_protection(pid, fn, 1, old_prot);
        goto detach;
    }
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0) != 0) goto restore;
    for (waited = 0; waited < WAIT_MS; waited++) {
        int got = waitpid(pid, &status, WNOHANG);
        if (got < 0) goto restore;
        if (got == 0) {
            usleep(1000);
            continue;
        }
        if (!WIFSTOPPED(status)) goto restore;
        if (WSTOPSIG(status) != SIGTRAP) {
            sys_ptrace(PT_CONTINUE, pid, (caddr_t)1,
                       WSTOPSIG(status) == SIGCHLD ? 0 : WSTOPSIG(status));
            continue;
        }
        if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0) != 0) goto restore;
        if (kernel_proc_copyout(pid, (intptr_t)regs.r_rdi, request, sizeof request) != 0)
            goto restore;
        log_line("  vpad diag: intercepted normal MBus function request at %#llx",
                 (unsigned long long)regs.r_rdi);
        shell_dump_info("MBus request", request, sizeof request);
        regs.r_rip = fn; /* re-execute the original first instruction */
        if (kernel_proc_setchar(pid, fn, (char)original) != 0 ||
            sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) != 0)
            goto restore_prot;
        kernel_set_vmem_protection(pid, fn, 1, old_prot);
        result = 0;
        goto detach;
    }
    log_line("  vpad diag: no MBus device-function call arrived in %u seconds",
             (unsigned)(WAIT_MS / 1000));
restore:
    kernel_proc_setchar(pid, fn, (char)original);
restore_prot:
    kernel_set_vmem_protection(pid, fn, 1, old_prot);
detach:
    sys_ptrace(PT_DETACH, pid, (caddr_t)1, 0);
    return result;
}
