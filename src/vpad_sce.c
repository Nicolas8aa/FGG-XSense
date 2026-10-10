/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from ps5-wireless-pads / PadBridge-PS5 src/ps5_vpad.c
 * (X-F1REBALL-X, AngelLopezTorres), GPL-3.0-or-later. */
#include "vpad_sce.h"
#include "log.h"
#include "vpad.h"
#include "shell_call.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- ScePadData: 120 bytes, as PS4/PS5 software sees it ----------------- */

typedef struct {
    uint16_t x, y;
    uint8_t  id;
    uint8_t  reserved[3];
} ScePadTouch;

typedef struct {
    uint8_t  touchNum;
    uint8_t  reserved[3];
    uint32_t reserved2;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
    uint8_t  padding[2];
    float    orientation[4];
    float    acceleration[3];
    float    angularVelocity[3];
    ScePadTouchData touchData;
    uint8_t  connected;
    uint64_t timestamp;
    uint8_t  extensionUnitData[16];
    uint8_t  connectedCount;
    uint8_t  reserved[2];
    uint8_t  deviceUniqueDataLen;
    uint8_t  deviceUniqueData[12];
} ScePadData;

_Static_assert(sizeof(ScePadData) == 120, "ScePadData must be 120 bytes");
_Static_assert(offsetof(ScePadData, timestamp) == 80, "timestamp offset");

/* ScePadData button bits. */
#define SCE_L3       0x00000002
#define SCE_R3       0x00000004
#define SCE_OPTIONS  0x00000008
#define SCE_UP       0x00000010
#define SCE_RIGHT    0x00000020
#define SCE_DOWN     0x00000040
#define SCE_LEFT     0x00000080
#define SCE_L2       0x00000100
#define SCE_R2       0x00000200
#define SCE_L1       0x00000400
#define SCE_R1       0x00000800
#define SCE_TRIANGLE 0x00001000
#define SCE_CIRCLE   0x00002000
#define SCE_CROSS    0x00004000
#define SCE_SQUARE   0x00008000
#define SCE_PS       0x00010000
#define SCE_TOUCHPAD 0x00100000

#define VIRTUAL_DEVICE_DUALSENSE 3
#define TRIGGER_DIGITAL 128

/* How long to wait for the kernel log to name the new device. */
#define IDENTIFY_MS 3000

/* AddDevice answers this when it has created the device but the console has
 * yet to assign it to a user: it brings up the "who is using this
 * controller" screen for it. The device exists all the same -- every call
 * creates one, so the call must never be repeated: retrying it on this code
 * filled the console with virtual pads until it refused more (0x803B0003)
 * and its login screen broke. Binding the device to the user over MBus is
 * what settles the assignment. */
#define ADD_PENDING 0x803B0006

/* ---- system interfaces, all resolved at runtime ------------------------- */

typedef int32_t (*fn_void)(void);
typedef int32_t (*fn_i32)(int32_t);
typedef int32_t (*fn_ptr)(void *);
typedef int32_t (*fn_add)(void *, int32_t);
typedef int32_t (*fn_insert)(int32_t, const void *);
typedef int32_t (*fn_bind)(uint64_t, int32_t);
typedef int32_t (*fn_users)(int32_t *);

static fn_add    p_add;
static fn_insert p_insert;
static fn_i32    p_delete;
static fn_bind   p_bind;
static fn_users  p_login_list, p_initial_user, p_foreground_user;

/* From the payload SDK. */
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

/* The credentials libScePad's virtual devices accept from a payload: those
 * YetAnotherControllerEnabler uses (PadBridge's 0x3800000000010003 also
 * creates the device on 13.60, but its MBus bind is refused). */
#define SYSTEM_AUTHID 0x4800000000010003ull

static uint64_t g_orig_authid;
static uint8_t  g_orig_caps[16];
static int      g_raised;
static int      g_ready;

/* Set after the first failure to add a pad: every AddDevice creates a
 * device, so after one goes wrong no further call is made this session. */
static int      g_failed;

/* Identification reads the kernel log, which has one reader at a time. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- credentials -------------------------------------------------------- */

static int set_creds(uint64_t authid, const uint8_t caps[16])
{
    pid_t me = getpid();
    uint8_t now[16];

    return kernel_set_ucred_authid(me, authid) == 0 &&
           kernel_set_ucred_caps(me, caps) == 0 &&
           kernel_get_ucred_authid(me) == authid &&
           kernel_get_ucred_caps(me, now) == 0 && memcmp(now, caps, 16) == 0;
}

static int raise_creds(void)
{
    static const uint8_t all[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    pid_t me = getpid();

    g_orig_authid = kernel_get_ucred_authid(me);
    if (!g_orig_authid || kernel_get_ucred_caps(me, g_orig_caps) != 0) {
        log_line("  vpad: cannot read our credentials");
        return 0;
    }
    if (!set_creds(SYSTEM_AUTHID, all)) {
        log_line("  vpad: credentials not raised; putting the old ones back");
        set_creds(g_orig_authid, g_orig_caps);
        return 0;
    }
    g_raised = 1;
    return 1;
}

void vpad_sce_shutdown(void)
{
    if (!g_raised) return;
    if (set_creds(g_orig_authid, g_orig_caps))
        log_line("  vpad: original credentials restored");
    else
        log_line("  vpad: could not put the original credentials back");
    g_raised = 0;
    g_ready = 0;
}

/* ---- set-up ------------------------------------------------------------- */

static void *sym(void *lib, const char *name, int *missing)
{
    void *p = lib ? dlsym(lib, name) : NULL;
    if (!p) {
        log_line("  vpad: %s not found", name);
        (*missing)++;
    }
    return p;
}

int vpad_sce_init(void)
{
    void *pad, *mbus, *users;
    fn_void pad_init;
    fn_i32 privilege;
    fn_ptr users_init;
    int missing = 0;
    int32_t r;

    if (!raise_creds()) return 0;

    users = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
    /* libScePad imports from libSceMbus: it must be loaded first, or
     * scePadInit can fault on an unresolved import. */
    mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!mbus) mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    pad = mbus ? dlopen("libScePad.sprx", RTLD_NOW | RTLD_GLOBAL) : NULL;
    if (!users || !mbus || !pad) {
        log_line("  vpad: system libraries unavailable (user %d, mbus %d, pad %d)",
                 users != NULL, mbus != NULL, pad != NULL);
        vpad_sce_shutdown();
        return 0;
    }

    users_init        = (fn_ptr)sym(users, "sceUserServiceInitialize", &missing);
    p_login_list      = (fn_users)sym(users, "sceUserServiceGetLoginUserIdList", &missing);
    p_initial_user    = (fn_users)sym(users, "sceUserServiceGetInitialUser", &missing);
    p_foreground_user = (fn_users)sym(users, "sceUserServiceGetForegroundUser", &missing);
    p_bind            = (fn_bind)sym(mbus, "sceMbusBindDeviceWithUserId", &missing);
    pad_init          = (fn_void)sym(pad, "scePadInit", &missing);
    privilege         = (fn_i32)sym(pad, "scePadSetProcessPrivilege", &missing);
    p_add             = (fn_add)sym(pad, "scePadVirtualDeviceAddDevice", &missing);
    p_insert          = (fn_insert)sym(pad, "scePadVirtualDeviceInsertData", &missing);
    p_delete          = (fn_i32)sym(pad, "scePadVirtualDeviceDeleteDevice", &missing);
    if (missing) {
        vpad_sce_shutdown();
        return 0;
    }

    users_init(NULL);   /* "already initialised" is fine */

    /* scePadInit before the privilege call, which otherwise answers "not
     * initialised" (PadBridge, firmware 10.01). */
    if ((r = pad_init()) != 0) {
        log_line("  vpad: scePadInit failed: %#x", (unsigned)r);
        vpad_sce_shutdown();
        return 0;
    }
    if ((r = privilege(1)) != 0)
        log_line("  vpad: scePadSetProcessPrivilege answered %#x, carrying on", (unsigned)r);

    g_ready = 1;
    log_line("  vpad: Sony's pad library ready -- the Xbox button acts as PS");
    return 1;
}

/* ---- users -------------------------------------------------------------- */

/* A signed-in user to give the pad to: the initial user if signed in, then
 * the foreground user, then the first signed-in one (PadBridge). Binding to
 * a user who is not signed in fails. */
static int32_t main_user(void)
{
    int32_t login[4] = { -1, -1, -1, -1 }, init = -1, fore = -1;
    int i;

    if (p_login_list(login) != 0) return -1;
    if (p_initial_user(&init) != 0) init = -1;
    if (p_foreground_user(&fore) != 0) fore = -1;

    for (i = 0; i < 4; i++) if (init != -1 && login[i] == init) return init;
    for (i = 0; i < 4; i++) if (fore != -1 && login[i] == fore) return fore;
    for (i = 0; i < 4; i++) if (login[i] != -1 && login[i] != 0) return login[i];
    return -1;
}

/* ---- identification through the kernel log ------------------------------ */

static uint64_t hex_after(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    uint64_t v = 0;
    int digits = 0;

    if (!p) return 0;
    p += strlen(key);
    while (*p == ':' || *p == '=' || *p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (isxdigit((unsigned char)*p) && digits++ < 16) {
        char c = *p++;
        v = v << 4 | (uint64_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

/* A virtual DualSense appearing:
 *   SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x...][type:1][subType:22]
 *   [capabilityBattery:0]...
 * A real pad reports a battery, so a line saying otherwise is not ours. */
static uint64_t virtual_device_in(const char *line)
{
    const char *bat;

    if (!strstr(line, "DEVICE_ADDED") || !strstr(line, "type:1") ||
        !strstr(line, "subType:22"))
        return 0;
    bat = strstr(line, "capabilityBattery:");
    if (bat && bat[18] != '0') return 0;
    return hex_after(line, "DeviceId");
}

/* Reads what the log has. Every line is copied into our own log: /dev/klog
 * has one reader, so whatever is read here is otherwise lost. Returns the
 * number of matching devices seen, and the first in `dev`. */
static int read_log(int fd, char *line, size_t *len, size_t cap, uint64_t *dev,
                    int copy)
{
    char buf[1024];
    int found = 0, rounds;

    for (rounds = 0; rounds < 8; rounds++) {
        ssize_t n = read(fd, buf, sizeof buf), i;
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            uint64_t d;
            if (buf[i] != '\n' && *len < cap - 1) {
                line[(*len)++] = buf[i];
                continue;
            }
            line[*len] = '\0';
            *len = 0;
            if (copy) log_line("    klog: %s", line);
            if ((d = virtual_device_in(line)) != 0) {
                if (!found) *dev = d;
                if (!found || d != *dev) found++;
            }
        }
    }
    return found;
}

int32_t vpad_sce_add(char *why, int whylen)
{
    char line[1024];
    size_t len = 0;
    uint64_t dev = 0, ignored;
    int32_t param[8], user, r, handle = -1;
    int fd, found = 0, i;
    long start;

    if (!g_ready) {
        snprintf(why, (size_t)whylen, "Sony's pad library not in use");
        return -1;
    }
    if (g_failed) {
        snprintf(why, (size_t)whylen, "off after an earlier failure -- stop the payload "
                 "and set ps_button=0");
        return -1;
    }
    user = main_user();
    if (user == -1) {
        snprintf(why, (size_t)whylen, "nobody signed in");
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(why, (size_t)whylen, "/dev/klog: %s", strerror(errno));
        goto out;
    }
    read_log(fd, line, &len, sizeof line, &ignored, 1);   /* not ours */
    len = 0;

    /* The parameter block as the PS5 accepts it: size 32, user 1, the rest
     * marked so a write into it would show. */
    for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
    param[0] = (int32_t)sizeof param;
    param[1] = 1;
    r = p_add(param, VIRTUAL_DEVICE_DUALSENSE);
    log_line("  vpad: AddDevice %#x, block %08x %08x %08x %08x %08x %08x", (unsigned)r,
             (unsigned)param[2], (unsigned)param[3], (unsigned)param[4],
             (unsigned)param[5], (unsigned)param[6], (unsigned)param[7]);
    if (r != 0 && r != (int32_t)ADD_PENDING) {
        snprintf(why, (size_t)whylen, "AddDevice %#x", (unsigned)r);
        close(fd);
        goto out;
    }

    /* Wait for the device to be named, then a moment longer to be sure no
     * second one appears: an ambiguous match is never bound. */
    start = now_ms();
    while (!found && now_ms() - start < IDENTIFY_MS) {
        uint64_t d = 0;
        int n = read_log(fd, line, &len, sizeof line, &d, 1);
        if (n > 0) {
            dev = d;
            found = n;
        } else {
            usleep(20000);
        }
    }
    if (found == 1) {
        uint64_t d = 0;
        usleep(300000);
        if (read_log(fd, line, &len, sizeof line, &d, 1) > 0 && d != dev) found++;
    }
    close(fd);

    if (found != 1) {
        snprintf(why, (size_t)whylen, "new device not identified (%d candidates)", found);
        goto out;
    }

    handle = (int32_t)(dev & 0xFFFFFFFFu);
    r = p_bind(dev, user);
    if (r != 0) {
        /* Refused from a payload on 13.60 (0x809b0005). The shell may bind:
         * YetAnotherControllerEnabler runs the same call inside SceShellUI,
         * or SceShellCore if that cannot be reached. */
        static const char *const shells[] = { "SceShellUI", "SceShellCore" };
        int64_t res = -1;
        int k, c = -1;

        for (k = 0; k < 2; k++) {
            c = shell_call(shells[k], "libSceMbus", "sceMbusBindDeviceWithUserId",
                           dev, (uint64_t)(uint32_t)user, &res);
            log_line("  vpad: bind from %s -> %s %#llx", shells[k],
                     c == 0 ? "returned" : "could not call, code", c == 0 ?
                     (unsigned long long)(uint32_t)res : (unsigned long long)(unsigned)c);
            if (c == 0) break;
        }
        if (c == 0 && res == 0) r = 0;
    }
    if (r != 0) {
        ScePadData ps;
        struct timespec ts;
        int press;

        /* The new pad is already visible to the account-assignment screen,
         * but that screen ignores InsertData from this payload while it is
         * busy.  Its own SceShellUI accepts the pad's PS report; this is the
         * final assignment nudge used by YetAnotherControllerEnabler.  Do not
         * synthesize Cross: the user must select the account deliberately. */
        memset(&ps, 0, sizeof ps);
        ps.buttons = SCE_PS;
        ps.orientation[3] = 1.0f;
        ps.acceleration[1] = -1.0f;
        ps.connected = 1;
        ps.connectedCount = 1;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ps.timestamp = (uint64_t)ts.tv_sec * 1000000u +
                       (uint64_t)ts.tv_nsec / 1000u;
        press = shellui_press_ps(handle, &ps, sizeof ps);
        if (press == 0)
            log_line("  vpad: PS press from SceShellUI -> delivered");
        else
            log_line("  vpad: PS press from SceShellUI -> could not deliver (code %d)",
                     press);

        /* Refused (0x809b0005 has been seen, perhaps because the user
         * already has a controller). The device is kept: the console's own
         * account list now accepts this new pad.  The user chooses the
         * account with it; no account-selection button is injected. */
        snprintf(why, (size_t)whylen, "device %#llx, binding to user %#x refused (%#x); "
                 "ShellUI PS %s -- choose the user on screen", (unsigned long long)dev,
                 (unsigned)user, (unsigned)r, press == 0 ? "delivered" : "failed");
        goto out;
    }
    snprintf(why, (size_t)whylen, "device %#llx bound to user %#x",
             (unsigned long long)dev, (unsigned)user);

out:
    if (handle < 0) {
        g_failed = 1;
        notify("FGG-XSense: PS button route failed - stop the payload, set ps_button=0");
    }
    pthread_mutex_unlock(&g_lock);
    return handle;
}

/* ---- input -------------------------------------------------------------- */

static uint8_t axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

int vpad_sce_ready(void) { return g_ready; }

/* ---- diagnostics: why InsertData refuses ------------------------------- */

#define HID_INSERT_DATA 0x8018482CUL
#define DIAG_DETAILED   5
#define DIAG_GIVE_UP    30

typedef struct {
    uint32_t cmd;
    uint32_t reserved;
    void    *data;
    void    *out;
} hid_req;

static int g_insert_fails;

/* Logs Sony's error code and whether the same device takes the same input
 * through the direct kernel call. After DIAG_GIVE_UP refusals in a row the
 * payload is stopped, so the controller is not left dead. */
static void diag_insert_failure(int32_t handle, const pad_state *s, int32_t r)
{
    g_insert_fails++;
    if (g_insert_fails <= DIAG_DETAILED) {
        unsigned char report[VPAD_REPORT_SIZE];
        hid_req req;
        int fd = open("/dev/hid", O_RDWR), direct = -1, e = 0;

        vpad_translate(s, report);
        if (fd >= 0) {
            memset(&req, 0, sizeof req);
            req.cmd = (uint32_t)handle;
            req.data = report;
            direct = ioctl(fd, HID_INSERT_DATA, &req);
            e = direct ? errno : 0;
            close(fd);
        }
        log_line("  vpad diag: InsertData(%#x) -> %#x; direct insert into the same "
                 "device -> %s", (unsigned)handle, (unsigned)r,
                 direct == 0 ? "accepted" : fd < 0 ? "no /dev/hid" : strerror(e));
    }
    if (g_insert_fails == DIAG_GIVE_UP) {
        FILE *f = fopen("/data/fgg-xsense/stop", "w");
        if (f) fclose(f);
        log_line("  vpad diag: %d refusals in a row -- stopping the payload", DIAG_GIVE_UP);
        notify("FGG-XSense: PS button diagnostics done - payload stopped");
    }
}

int vpad_sce_insert(int32_t handle, const pad_state *s)
{
    ScePadData d;
    struct timespec ts;
    uint32_t m = s->buttons, b = 0;
    int32_t r;

    memset(&d, 0, sizeof d);

    if (m & PAD_A)     b |= SCE_CROSS;
    if (m & PAD_B)     b |= SCE_CIRCLE;
    if (m & PAD_X)     b |= SCE_SQUARE;
    if (m & PAD_Y)     b |= SCE_TRIANGLE;
    if (m & PAD_UP)    b |= SCE_UP;
    if (m & PAD_DOWN)  b |= SCE_DOWN;
    if (m & PAD_LEFT)  b |= SCE_LEFT;
    if (m & PAD_RIGHT) b |= SCE_RIGHT;
    if (m & PAD_LB)    b |= SCE_L1;
    if (m & PAD_RB)    b |= SCE_R1;
    if (m & PAD_LS)    b |= SCE_L3;
    if (m & PAD_RS)    b |= SCE_R3;
    if (m & PAD_MENU)  b |= SCE_OPTIONS;
    if (m & PAD_VIEW)  b |= SCE_TOUCHPAD;
    if (m & PAD_GUIDE) b |= SCE_PS;
    if (s->lt > TRIGGER_DIGITAL) b |= SCE_L2;
    if (s->rt > TRIGGER_DIGITAL) b |= SCE_R2;

    d.buttons = b;
    d.lx = axis(s->lx);
    d.ly = axis_inv(s->ly);
    d.rx = axis(s->rx);
    d.ry = axis_inv(s->ry);
    d.l2 = (uint8_t)(s->lt >> 2);
    d.r2 = (uint8_t)(s->rt >> 2);
    d.orientation[3] = 1.0f;
    d.acceleration[1] = -1.0f;      /* at rest, held level */
    d.connected = 1;
    d.connectedCount = 1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d.timestamp = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;

    r = p_insert(handle, &d);
    if (r == 0) {
        g_insert_fails = 0;
        return 1;
    }
    diag_insert_failure(handle, s, r);
    return 0;
}

void vpad_sce_delete(int32_t handle)
{
    if (g_ready && handle >= 0) p_delete(handle);
}
