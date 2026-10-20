/*
 * xemu-hdd-cache.h -- clear the Xbox HDD's game cache partitions at each
 * machine power-on and reset (see xemu-hdd-cache.c).
 */
#ifndef XEMU_HDD_CACHE_H
#define XEMU_HDD_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the reset handler; it checks sys.clear_cache_on_boot each time. */
void xemu_hdd_cache_init(void);

#ifdef __cplusplus
}
#endif

#endif
