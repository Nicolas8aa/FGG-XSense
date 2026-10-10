/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from MegaCadeDev/YetAnotherControllerEnabler src/pad_assign.c
 * (find_pid, pt_io, resolve_sym, code_cave, pt_call3, call_remote),
 * GPL-3.0-or-later. */
#include "shell_call.h"

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
