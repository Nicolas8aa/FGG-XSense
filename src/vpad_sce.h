/* Virtual pads through Sony's own libScePad, with the PS button.
 *
 * The direct route (vpad.c) writes the kernel's report itself, and no bit of
 * that report has been found to act as the PS button. libScePad builds the
 * report from a ScePadData instead -- and its PS bit does reach the system.
 * It is only usable from a payload with system-service credentials, and the
 * device it creates must be found in the kernel log and bound to a user over
 * MBus, which also spares the "who is using this controller" screen.
 *
 * Ported from ps5-wireless-pads / PadBridge-PS5 (src/ps5_vpad.c),
 * GPL-3.0-or-later. Every step is checked; anything that fails leaves the
 * caller to fall back to the direct route.
 */
#ifndef FGG_VPAD_SCE_H
#define FGG_VPAD_SCE_H

#include "pad.h"

#include <stdint.h>

/* Raises credentials and initialises libScePad. Returns 0 if this route is
 * unavailable, with the original credentials put back. */
int vpad_sce_init(void);

/* With `on`, the system log read while identifying a new pad is copied into
 * ours. */
void vpad_sce_set_trace(int on);

/* Puts the original credentials back. */
void vpad_sce_shutdown(void);

/* Whether this route is in use. While it is, no pad is made any other way:
 * a second pad made while this route's is pending wedges the console. */
int vpad_sce_ready(void);

/* Creates a virtual DualSense bound to the signed-in user. Returns its
 * handle, or -1 on failure with a reason in `why`. Blocks while the console
 * asks who is using the controller (up to a minute), then for up to about
 * three seconds while the device is identified. */
int32_t vpad_sce_add(char *why, int whylen);

/* Publishes one state; PAD_GUIDE becomes the PS button. Returns 0 if the
 * device refused it. */
int vpad_sce_insert(int32_t handle, const pad_state *state);

void vpad_sce_delete(int32_t handle);

#endif
