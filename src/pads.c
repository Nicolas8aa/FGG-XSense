#include "pads.h"
#include "log.h"
#include "vpad.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Hold View+Menu together this long to stop the payload deliberately. Held
 * rather than tapped, so it cannot fire by accident mid-game. */
#define QUIT_HOLD_SECONDS 2

/* The user the virtual pad belongs to. The console ignores this and asks who
 * is playing instead, but the field must hold something. */
#define VPAD_USER 1

typedef struct {
    int      used;
    char     name[48];
    vpad    *pad, *mirror;
    uint32_t buttons;
    long     delivered, failed;
} slot;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static slot g_slots[PADS_MAX];
static int g_mirror;
static time_t g_quit_since;
static volatile int g_stop;

void pads_set_mirror(int on) { g_mirror = on; }

int pads_attach(const char *source, const char *id)
{
    char why[160];
    int i, n = -1;

    pthread_mutex_lock(&g_lock);

    for (i = 0; i < PADS_MAX && n < 0; i++)
        if (!g_slots[i].used) n = i;

    if (n < 0) {
        log_line("  %s %s: already %d controllers -- not published",
                 source, id, PADS_MAX);
        goto out;
    }

    memset(&g_slots[n], 0, sizeof g_slots[n]);
    snprintf(g_slots[n].name, sizeof g_slots[n].name, "%s %s", source, id);

    g_slots[n].pad = vpad_open(VPAD_USER, why, (int)sizeof why);
    if (!g_slots[n].pad) {
        log_line("  %s: could not register a virtual pad -- %s",
                 g_slots[n].name, why);
        notify("FGG-XSense: could not register a virtual pad");
        n = -1;
        goto out;
    }
    log_line("  %s: virtual pad registered, %s", g_slots[n].name, why);

    if (g_mirror) {
        g_slots[n].mirror = vpad_open(VPAD_USER, why, (int)sizeof why);
        log_line("  %s: mirror pad %s -- %s", g_slots[n].name,
                 g_slots[n].mirror ? "registered" : "refused", why);
    }

    g_slots[n].used = 1;
    notify("FGG-XSense: controller ready - hold View+Menu to stop");

out:
    pthread_mutex_unlock(&g_lock);
    return n;
}

int pads_update(int n, const pad_state *state)
{
    unsigned char report[VPAD_REPORT_SIZE];
    int ok;

    vpad_translate(state, report);

    pthread_mutex_lock(&g_lock);
    g_slots[n].buttons = state->buttons;
    ok = vpad_submit(g_slots[n].pad, report);
    if (g_slots[n].mirror) vpad_submit(g_slots[n].mirror, report);
    if (ok) g_slots[n].delivered++;
    else    g_slots[n].failed++;
    pthread_mutex_unlock(&g_lock);

    return ok;
}

void pads_detach(int n)
{
    pthread_mutex_lock(&g_lock);
    if (n >= 0 && n < PADS_MAX && g_slots[n].used) {
        log_line("  %s: removed after %ld delivered, %ld refused",
                 g_slots[n].name, g_slots[n].delivered, g_slots[n].failed);
        vpad_close(g_slots[n].mirror);
        vpad_close(g_slots[n].pad);
        memset(&g_slots[n], 0, sizeof g_slots[n]);
    }
    pthread_mutex_unlock(&g_lock);
}

int pads_tick(time_t now)
{
    int i, held = 0, quit = 0;

    pthread_mutex_lock(&g_lock);

    /* Judged from the last state each controller sent, not from reports as
     * they arrive: a controller reports changes, so a combination held
     * perfectly still sends nothing more. */
    for (i = 0; i < PADS_MAX; i++)
        if (g_slots[i].used &&
            (g_slots[i].buttons & (PAD_VIEW | PAD_MENU)) == (PAD_VIEW | PAD_MENU))
            held = 1;

    if (!held)                    g_quit_since = 0;
    else if (g_quit_since == 0)   g_quit_since = now;
    else if (now - g_quit_since >= QUIT_HOLD_SECONDS) quit = 1;

    pthread_mutex_unlock(&g_lock);
    return quit;
}

int pads_count(void)
{
    int i, n = 0;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < PADS_MAX; i++) n += g_slots[i].used;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void pads_request_stop(void) { g_stop = 1; }
int  pads_stopping(void)     { return g_stop; }
