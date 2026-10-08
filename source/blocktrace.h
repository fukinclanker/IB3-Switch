#ifndef INFINITY_BLADE_NX_BLOCKTRACE_H
#define INFINITY_BLADE_NX_BLOCKTRACE_H

#include "config.h"

/* Live "who is blocked where" tracker (developer diagnostics).
 *
 * Every shimmed blocking primitive (futex, condvar, mutex, semaphore, sleep)
 * brackets its wait with bt_enter()/bt_leave(). A watchdog thread prints, every
 * 2 s, each thread that has been blocked for a while (kind, address, caller as
 * an offset into libib3.so) plus how many blocking calls each thread made in the
 * interval, so both a deadlock and a polling loop are visible in the log.
 *
 * Off by default (IB3_DIAGNOSTICS=0): the watchdog ran at a priority above the
 * game, wrote to the SD card every 2 s, and at 40/80/180 s froze every game
 * thread to walk its stack (audio drop-outs, and a crash if a frame pointer
 * led into unmapped memory). Build with `make DIAGNOSTICS=1` to get it back. */

#if IB3_DIAGNOSTICS
void bt_enter(const char *kind, const void *addr, const void *caller,
              long long timeout_ns);
void bt_leave(void);
void bt_start_watchdog(void);
#else
static inline void bt_enter(const char *kind, const void *addr,
                            const void *caller, long long timeout_ns) {
  (void)kind;
  (void)addr;
  (void)caller;
  (void)timeout_ns;
}
static inline void bt_leave(void) {}
static inline void bt_start_watchdog(void) {}
#endif

#endif
