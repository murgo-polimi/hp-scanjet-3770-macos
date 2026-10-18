/* Debug helpers for the standalone hp3770 build (subset of SANE's
   sanei_debug.h). The level comes from SANE_DEBUG_HP3770 (0..4). */
#ifndef HP3770_COMPAT_SANEI_DEBUG_H
#define HP3770_COMPAT_SANEI_DEBUG_H

#include "sane/sanei.h"

#define DBG_LEVEL       PASTE(sanei_debug_,BACKEND_NAME)
#define DBG_LOCAL       PASTE(DBG_LEVEL,_call)

extern int DBG_LEVEL;
extern void DBG_LOCAL (int level, const char *msg, ...)
#ifdef __GNUC__
  __attribute__ ((format (printf, 2, 3)))
#endif
  ;

#define DBG             DBG_LOCAL
#define DBG_INIT()      sanei_init_debug (STRINGIFY(BACKEND_NAME), &DBG_LEVEL)
#define IF_DBG(x)       x

extern void sanei_init_debug (const char *backend, int *level);

#endif
