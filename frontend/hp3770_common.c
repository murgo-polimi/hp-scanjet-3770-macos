/* Code shared by hp3770-scan and hp3770-airscan: setting backend options by
   name, and the image post-processing (lamp ripple removal and paper white
   point) applied to a finished 8- or 16-bit image in memory.

   Included by hp3770_scan.c and hp3770_airscan.c after the SANE headers;
   HP3770_PROG names the program in error messages. */

#include "hp3770_deripple.c"
#include "hp3770_autowhite.c"

/* exported by the backend (hp3770_sane.c), not part of the SANE API */
extern SANE_Int hp3770_backend_ccd_status (void);
extern double hp3770_backend_line_time (SANE_Int *hw_resolution);

static int
find_option (SANE_Handle h, const char *name)
{
  SANE_Int count = 0;
  int i;

  if (sane_control_option (h, 0, SANE_ACTION_GET_VALUE, &count, NULL) != SANE_STATUS_GOOD)
    return -1;
  for (i = 1; i < count; i++)
    {
      const SANE_Option_Descriptor *d = sane_get_option_descriptor (h, i);
      if (d != NULL && d->name != NULL && strcmp (d->name, name) == 0)
	return i;
    }
  return -1;
}

/* Sets option NAME from a text value, converting to the option's type. */
static int
set_option_text (SANE_Handle h, const char *name, const char *text, int verbose)
{
  int idx = find_option (h, name);
  const SANE_Option_Descriptor *d;
  SANE_Status st;
  SANE_Int info = 0;

  if (idx < 0)
    {
      fprintf (stderr, HP3770_PROG ": unknown option '%s' (see --all-options)\n", name);
      return -1;
    }
  d = sane_get_option_descriptor (h, idx);
  if (!SANE_OPTION_IS_SETTABLE (d->cap) || !SANE_OPTION_IS_ACTIVE (d->cap))
    {
      fprintf (stderr, HP3770_PROG ": option '%s' cannot be set now\n", name);
      return -1;
    }

  switch (d->type)
    {
    case SANE_TYPE_BOOL:
    case SANE_TYPE_INT:
      {
	SANE_Word w;
	if (d->type == SANE_TYPE_BOOL)
	  w = (strcasecmp (text, "yes") == 0 || strcasecmp (text, "true") == 0
	       || strcmp (text, "1") == 0) ? SANE_TRUE : SANE_FALSE;
	else
	  w = (SANE_Word) strtol (text, NULL, 0);
	st = sane_control_option (h, idx, SANE_ACTION_SET_VALUE, &w, &info);
	break;
      }
    case SANE_TYPE_FIXED:
      {
	SANE_Word w = SANE_FIX (strtod (text, NULL));
	st = sane_control_option (h, idx, SANE_ACTION_SET_VALUE, &w, &info);
	break;
      }
    case SANE_TYPE_STRING:
      {
	char *buf = calloc (1, (size_t) d->size + 1);
	if (buf == NULL)
	  return -1;
	strncpy (buf, text, (size_t) d->size);
	st = sane_control_option (h, idx, SANE_ACTION_SET_VALUE, buf, &info);
	free (buf);
	break;
      }
    case SANE_TYPE_BUTTON:
      st = sane_control_option (h, idx, SANE_ACTION_SET_VALUE, NULL, &info);
      break;
    default:
      fprintf (stderr, HP3770_PROG ": option '%s' has an unsupported type\n", name);
      return -1;
    }

  if (st != SANE_STATUS_GOOD)
    {
      fprintf (stderr, HP3770_PROG ": setting %s=%s failed: %s\n", name, text,
	       sane_strstatus (st));
      return -1;
    }
  if (verbose)
    fprintf (stderr, "  %s = %s%s\n", name, text,
	     (info & SANE_INFO_INEXACT) ? " (rounded by the backend)" : "");
  return 0;
}

/* Geometry options are SANE_TYPE_INT (mm) in this backend, but accept
   SANE_TYPE_FIXED too so the frontend does not depend on that detail. */
static int
set_number (SANE_Handle h, const char *name, double value, int verbose)
{
  char text[64];
  int idx = find_option (h, name);
  const SANE_Option_Descriptor *d = (idx >= 0) ? sane_get_option_descriptor (h, idx) : NULL;

  if (d != NULL && d->type == SANE_TYPE_INT)
    snprintf (text, sizeof (text), "%ld", lround (value));
  else
    snprintf (text, sizeof (text), "%.3f", value);
  return set_option_text (h, name, text, verbose);
}

static double
get_number (SANE_Handle h, const char *name, int want_max)
{
  int idx = find_option (h, name);
  const SANE_Option_Descriptor *d;
  SANE_Word w = 0;

  if (idx < 0)
    return 0.0;
  d = sane_get_option_descriptor (h, idx);
  if (want_max && d->constraint_type == SANE_CONSTRAINT_RANGE)
    w = d->constraint.range->max;
  else
    sane_control_option (h, idx, SANE_ACTION_GET_VALUE, &w, NULL);
  return (d->type == SANE_TYPE_FIXED) ? SANE_UNFIX (w) : (double) w;
}

/* Lamp ripple removal (colour only), then the paper white point, on the
   image in host byte order. dpi is the output resolution. Returns 1 if the
   image was changed. */
static int
postprocess_buffer (unsigned char *img, const SANE_Parameters *p, int lines, int dpi,
		    int no_deripple, int no_auto_white, int verbose)
{
  int channels = (p->format == SANE_FRAME_RGB) ? 3 : 1, changed = 0;

  if (p->depth < 8)
    return 0;
  if (!no_deripple)
    {
      SANE_Int hw_res = 0;
      double line_time = hp3770_backend_line_time (&hw_res), t_out, nominal[2];
      const char *env = getenv ("HP3770_MAINS");
      int count = 0;
      struct deripple_info info;

      if ((line_time > 0.0) && (hw_res > 0) && (dpi > 0))
	{
	  /* time per output line (the backend resizes when dpi differs from
	     the resolution the scanner used) */
	  t_out = line_time * (double) hw_res / (double) dpi;
	  if ((env == NULL) || (atoi (env) != 60))
	    nominal[count++] = deripple_fold (100.0 * t_out);
	  if ((env == NULL) || (atoi (env) != 50))
	    nominal[count++] = deripple_fold (120.0 * t_out);

	  /* CCD colour rows are 24 lines apart at 1200 dpi
	     (HP3770_LINE_DISTANCE), i.e. dpi/50 output lines after
	     line-distance correction */
	  if (deripple_image (img, lines, p->bytes_per_line, p->pixels_per_line, channels,
			      p->depth == 16, nominal, count, 24.0 * dpi / 1200.0, &info) == 0)
	    {
	      changed = 1;
	      if (verbose)
		fprintf (stderr, "  lamp ripple removed: %.3f lines/cycle, up to %.1f/%.1f/%.1f %%\n",
			 1.0 / info.freq, info.amplitude[0], info.amplitude[1], info.amplitude[2]);
	    }
	  else if (verbose)
	    fprintf (stderr, "  no lamp ripple found\n");
	}
    }

  if (!no_auto_white)
    {
      struct autowhite_info aw;
      const char *env = getenv ("HP3770_WHITE_TARGET");
      double target = 245.0;

      if (env != NULL)
	{
	  double v = atof (env);
	  if ((v >= 200.0) && (v <= 255.0))
	    target = v;
	}
      if (autowhite_image (img, lines, p->bytes_per_line, p->pixels_per_line, channels,
			   p->depth == 16, target, &aw) == 0)
	{
	  changed = 1;
	  if (verbose)
	    {
	      if (channels == 3)
		fprintf (stderr, "  paper white %.0f/%.0f/%.0f -> %.0f (gains %.2f/%.2f/%.2f)\n",
			 aw.white[0], aw.white[1], aw.white[2], target, aw.gain[0], aw.gain[1], aw.gain[2]);
	      else
		fprintf (stderr, "  paper white %.0f -> %.0f (gain %.2f)\n", aw.white[0], target, aw.gain[0]);
	    }
	}
      else if (verbose)
	fprintf (stderr, "  no paper background found, levels kept (spread %.3f)\n", aw.spread);
    }

  return changed;
}
