/* Build configuration for the standalone hp3770 backend (replaces the
   autoconf-generated SANE config.h). */
#ifndef HP3770_COMPAT_CONFIG_H
#define HP3770_COMPAT_CONFIG_H

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define PACKAGE_STRING "hp3770-macos"

#endif
