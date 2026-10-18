/* Subset of SANE's sanei_config.h used by the hp3770 backend. */
#ifndef HP3770_COMPAT_SANEI_CONFIG_H
#define HP3770_COMPAT_SANEI_CONFIG_H

#include <stdio.h>
#include "sane/sane.h"

/* Opens FILENAME from $SANE_CONFIG_DIR, the current directory or the
   compiled-in HP3770_CONFIG_DIR. Returns NULL when it is not found. */
extern FILE *sanei_config_open (const char *filename);
extern char *sanei_config_read (char *str, int n, FILE * stream);
extern const char *sanei_config_skip_whitespace (const char *str);
extern const char *sanei_config_get_string (const char *str,
					    char **string_const);

#endif
