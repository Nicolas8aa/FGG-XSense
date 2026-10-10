/* pad-info-probe -- read-only comparison of libScePad's public information
 * records for the currently signed-in users' physical and virtual pads.
 *
 * This does not create, bind, write to, or delete any controller.  It calls
 * GetHandle and the two information getters; if this process has no handle,
 * it opens a temporary local handle, reads the records, and closes it
 * immediately.  The goal is to establish whether the ShellUI battery source
 * is visible through public libScePad metadata before investigating the lower
 * MBus/HID capability path. */
#include "log.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH  STATE_DIR "/pad-info-probe.log"

#define PAD_TYPE_STANDARD 0
#define PAD_TYPE_DUALSENSE 3
#define PAD_INFO_BYTES 256

typedef int32_t (*fn_init)(void *);
typedef int32_t (*fn_pad_init)(void);
typedef int32_t (*fn_users)(int32_t *);
typedef int32_t (*fn_handle)(int32_t, int32_t, int32_t);
typedef int32_t (*fn_open)(int32_t, int32_t, int32_t, void *);
typedef int32_t (*fn_close)(int32_t);
typedef int32_t (*fn_info)(int32_t, void *);

static void dump(const char *what, const unsigned char *p, unsigned n)
{
    char line[3 * 16 + 1];
    unsigned i, o;

    for (o = 0; o < n; o += 16) {
        unsigned len = n - o < 16 ? n - o : 16;
        for (i = 0; i < len; i++) snprintf(line + 3 * i, 4, "%02X ", p[o + i]);
        line[3 * len] = '\0';
        log_line("    %s +%02X: %s", what, o, line);
    }
}

static void compare_handle(fn_info info, fn_info ext, int32_t user, int type, int index,
                           fn_handle get_handle, fn_open open_pad, fn_close close_pad)
{
    unsigned char basic[PAD_INFO_BYTES], extended[PAD_INFO_BYTES];
    int32_t handle, initial_handle, r1, r2;

    int from_open = 0;

    handle = get_handle(user, type, index);
    initial_handle = handle;
    if (handle < 0) {
        handle = open_pad(user, type, index, NULL);
        from_open = 1;
    }
    if (handle < 0) {
        log_line("user %#x type %d index %d -> GetHandle %#x; Open %#x", (unsigned)user,
                 type, index, (unsigned)initial_handle, (unsigned)handle);
        return;
    }
    memset(basic, 0xA5, sizeof basic);
    memset(extended, 0xA5, sizeof extended);
    r1 = info(handle, basic);
    r2 = ext(handle, extended);
    log_line("user %#x type %d index %d -> handle %#x%s; info %#x ext %#x", (unsigned)user,
             type, index, (unsigned)handle, from_open ? " (opened then closed)" : "",
             (unsigned)r1, (unsigned)r2);
    if (r1 == 0) dump("info", basic, 64);
    if (r2 == 0) dump("ext ", extended, 96);
    if (from_open) close_pad(handle);
}

int main(void)
{
    void *users, *mbus, *pad;
    fn_init users_init;
    fn_pad_init pad_init;
    fn_users login_list;
    fn_handle get_handle;
    fn_open open_pad;
    fn_close close_pad;
    fn_info info, ext;
    int32_t ids[4] = { -1, -1, -1, -1 };
    int i, type, index;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-XSense pad-info probe -- read-only");

    users = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
    mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!mbus) mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    pad = mbus ? dlopen("libScePad.sprx", RTLD_NOW | RTLD_GLOBAL) : NULL;
    users_init = users ? (fn_init)dlsym(users, "sceUserServiceInitialize") : NULL;
    login_list = users ? (fn_users)dlsym(users, "sceUserServiceGetLoginUserIdList") : NULL;
    get_handle = pad ? (fn_handle)dlsym(pad, "scePadGetHandle") : NULL;
    pad_init = pad ? (fn_pad_init)dlsym(pad, "scePadInit") : NULL;
    open_pad = pad ? (fn_open)dlsym(pad, "scePadOpen") : NULL;
    close_pad = pad ? (fn_close)dlsym(pad, "scePadClose") : NULL;
    info = pad ? (fn_info)dlsym(pad, "scePadGetControllerInformation") : NULL;
    ext = pad ? (fn_info)dlsym(pad, "scePadGetExtControllerInformation") : NULL;
    if (!users_init || !login_list || !pad_init || !get_handle || !open_pad || !close_pad ||
        !info || !ext) {
        log_line("missing library export(s): users %d mbus %d pad %d init %d list %d "
                 "pad-init %d handle %d open %d close %d info %d ext %d", users != NULL, mbus != NULL,
                 pad != NULL, users_init != NULL, login_list != NULL, pad_init != NULL,
                 get_handle != NULL, open_pad != NULL, close_pad != NULL, info != NULL,
                 ext != NULL);
        log_close();
        return 1;
    }
    users_init(NULL);  /* already initialized is harmless */
    if (pad_init() != 0) {
        log_line("could not initialize the local libScePad client");
        log_close();
        return 1;
    }
    if (login_list(ids) != 0) {
        log_line("could not list signed-in users");
        log_close();
        return 1;
    }

    for (i = 0; i < 4; i++) {
        if (ids[i] < 0) continue;
        log_line("user %#x", (unsigned)ids[i]);
        for (type = PAD_TYPE_STANDARD; type <= PAD_TYPE_DUALSENSE; type += PAD_TYPE_DUALSENSE)
            for (index = 0; index < 4; index++)
                compare_handle(info, ext, ids[i], type, index, get_handle,
                               open_pad, close_pad);
    }
    log_line("done: see public controller-information comparison above");
    notify("Pad-info probe done: see pad-info-probe.log");
    log_close();
    return 0;
}
