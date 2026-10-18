/* Minimal subset of SANE's internal sanei.h for the standalone hp3770 build.
   Derived from sane-backends include/sane/sanei.h (GPL-2.0-or-later with the
   SANE exception). */
#ifndef HP3770_COMPAT_SANEI_H
#define HP3770_COMPAT_SANEI_H

#include "sane/sane.h"

#ifdef __GNUC__
#define __sane_unused__ __attribute__((unused))
#else
#define __sane_unused__
#endif

#define STRINGIFY1(x)   #x
#define STRINGIFY(x)    STRINGIFY1(x)
#define PASTE1(x, y)    x##y
#define PASTE(x, y)     PASTE1(x, y)

#define NELEMS(a)       ((int)(sizeof (a) / sizeof (a[0])))

extern SANE_Status sanei_check_value (const SANE_Option_Descriptor * opt,
				      void *value);
extern SANE_Status sanei_constrain_value (const SANE_Option_Descriptor * opt,
					  void *value, SANE_Word * info);

#endif
