/* Small pieces of SANE's sanei library needed by the standalone hp3770
   backend: configuration-file helpers and debug output.
   Behaviour follows sane-backends sanei/sanei_config.c and
   sanei/sanei_init_debug.c. GPL-2.0-or-later. */

#include "sane/config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sane/sane.h"
#include "sane/sanei_config.h"

#ifndef HP3770_CONFIG_DIR
#define HP3770_CONFIG_DIR "/usr/local/etc/sane.d"
#endif

const char *
sanei_config_skip_whitespace (const char *str)
{
  while ((str != NULL) && *str && isspace ((unsigned char) *str))
    ++str;
  return str;
}

const char *
sanei_config_get_string (const char *str, char **string_const)
{
  const char *start;
  size_t len;

  str = sanei_config_skip_whitespace (str);
  if (*str == '"')
    {
      start = ++str;
      while (*str && *str != '"')
	++str;
      len = (size_t) (str - start);
      if (*str == '"')
	++str;
      else
	start = NULL;		/* final double quote is missing */
    }
  else
    {
      start = str;
      while (*str && !isspace ((unsigned char) *str))
	++str;
      len = (size_t) (str - start);
    }

  if (start != NULL)
    {
      *string_const = malloc (len + 1);
      if (*string_const != NULL)
	{
	  memcpy (*string_const, start, len);
	  (*string_const)[len] = '\0';
	}
    }
  else
    *string_const = NULL;

  return str;
}

char *
sanei_config_read (char *str, int n, FILE * stream)
{
  char *rc = fgets (str, n, stream);
  size_t len;
  char *start;

  if (rc == NULL)
    return NULL;

  len = strlen (str);
  while ((len > 0) && isspace ((unsigned char) str[len - 1]))
    str[--len] = '\0';

  start = str;
  while (isspace ((unsigned char) *start))
    start++;
  if (start != str)
    memmove (str, start, strlen (start) + 1);

  return rc;
}

FILE *
sanei_config_open (const char *filename)
{
  char path[1024];
  const char *dirs[3];
  int i;
  FILE *fp;

  dirs[0] = getenv ("SANE_CONFIG_DIR");
  dirs[1] = ".";
  dirs[2] = HP3770_CONFIG_DIR;

  for (i = 0; i < 3; i++)
    {
      if ((dirs[i] == NULL) || (dirs[i][0] == '\0'))
	continue;
      snprintf (path, sizeof (path), "%s/%s", dirs[i], filename);
      fp = fopen (path, "r");
      if (fp != NULL)
	return fp;
    }

  return NULL;
}

/* ---------------------------------------------------------------- debug */

int sanei_debug_hp3770 = 0;

void
sanei_init_debug (const char *backend, int *level)
{
  char name[64];
  const char *value;
  size_t i;

  snprintf (name, sizeof (name), "SANE_DEBUG_%s", backend);
  for (i = 0; name[i] != '\0'; i++)
    name[i] = (char) toupper ((unsigned char) name[i]);

  value = getenv (name);
  *level = (value != NULL) ? atoi (value) : 0;
  if (*level > 0)
    fprintf (stderr, "[%s] debug level %d\n", backend, *level);
}

void
sanei_debug_hp3770_call (int level, const char *msg, ...)
{
  va_list ap;

  if (level > sanei_debug_hp3770)
    return;

  va_start (ap, msg);
  fputs ("[hp3770] ", stderr);
  vfprintf (stderr, msg, ap);
  va_end (ap);
}
