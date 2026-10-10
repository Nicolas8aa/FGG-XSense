/* Host-side stand-ins for what the tested files reach but the tests never
 * exercise: Sony's pad library route and the console log. */
#include "vpad_sce.h"

#include <stdio.h>
#include <string.h>

int32_t vpad_sce_add(char *why, int whylen)
{
    snprintf(why, (size_t)whylen, "Sony's pad library not in use");
    return -1;
}

int vpad_sce_insert(int32_t handle, const pad_state *state)
{
    (void)handle;
    (void)state;
    return 0;
}

void vpad_sce_delete(int32_t handle) { (void)handle; }

void log_line(const char *fmt, ...) { (void)fmt; }
