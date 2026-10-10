/* The controllers currently connected, whatever backend brought them in.
 *
 * Each backend attaches a controller when it comes up, feeds it states while
 * it is connected, and detaches it when it goes. Every attached controller is
 * published as its own virtual pad. The table is shared between backend
 * threads and the main loop, and locks itself.
 */
#ifndef FGG_PADS_H
#define FGG_PADS_H

#include "pad.h"

#include <time.h>

#define PADS_MAX 4

/* Publishes every controller twice when on: a test of how the console treats
 * more than one virtual pad, before a second real controller exists. */
void pads_set_mirror(int on);

/* Registers a controller from `source` (e.g. "wired"), known as `id`.
 * Returns its slot, or -1 if no virtual pad could be registered. */
int pads_attach(const char *source, const char *id);

/* Publishes the controller's latest state. Returns 1 if it was delivered. */
int pads_update(int slot, const pad_state *state);

/* Removes the controller and its virtual pad. */
void pads_detach(int slot);

/* Called regularly by the main loop. Returns 1 once View+Menu has been held
 * on any controller long enough to stop the payload. */
int pads_tick(time_t now);

int pads_count(void);

/* Set once the payload is stopping; backends poll it and wind down. */
void pads_request_stop(void);
int  pads_stopping(void);

#endif
