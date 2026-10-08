#ifndef INFINITY_BLADE3_NX_CONFIG_H
#define INFINITY_BLADE3_NX_CONFIG_H

#define LOG_NAME "infinityblade3_nx.log"
#define APPSTATE_NAME "SaveData/appstate.txt"

/* Developer diagnostics (block tracer, thread-dump watchdog). */
#ifndef IB3_DIAGNOSTICS
#define IB3_DIAGNOSTICS 0
#endif

/* Very chatty logging: guest debug/verbose log lines, every package-file
 * access, and no log rate limit. */
#ifndef IB3_VERBOSE_LOG
#define IB3_VERBOSE_LOG 0
#endif

extern int screen_width;
extern int screen_height;

#endif
