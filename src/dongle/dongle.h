/* The Xbox Wireless Adapter.
 *
 * So far only recognised and its firmware checked: bringing the radio up is
 * the next milestone. Until then the adapter's node is owned here so no other
 * backend's scan touches it.
 */
#ifndef FGG_DONGLE_H
#define FGG_DONGLE_H

/* Accepts Microsoft's wireless adapters: 02E6 (the original), 02FE (the
 * slim one), 02F9 and 091E (built into some laptops). */
int dongle_match(unsigned vendor, unsigned product,
                 const unsigned char *cfg, int cfglen);

/* Reports the adapter on `node` and checks its firmware, logging the result. */
void dongle_report(const char *node);

#endif
