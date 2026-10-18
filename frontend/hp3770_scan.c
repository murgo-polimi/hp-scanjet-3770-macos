/* hp3770-scan: command-line scanner for the HP ScanJet 3770 on macOS.

   A small SANE frontend linked directly against the hp3770 backend
   (sane_hp3770_* entry points), so no SANE installation is required.
   It writes TIFF (default) or PNM files.

   Copyright (C) 2026 hp3770-macos contributors. GPL-2.0-or-later. */

#include "sane/config.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sane/sane.h"
#include "sane/saneopts.h"

#define BACKEND_NAME hp3770
#include "sane/sanei_backend.h"	/* maps sane_xxx to sane_hp3770_xxx */
#include "sane/sanei_usb.h"

#define TOOL_VERSION "0.1.0"

#define HP3770_PROG "hp3770-scan"
#include "hp3770_common.c"

static volatile sig_atomic_t cancel_requested = 0;

static void
on_signal (int sig)
{
  (void) sig;
  cancel_requested = 1;
}

struct settings
{
  const char *output;
  const char *format;		/* "tiff" or "pnm" */
  const char *mode;		/* SANE mode string */
  int resolution;
  int depth;
  double tl_x, tl_y, br_x, br_y;	/* mm, < 0 = backend default */
  const char *device;
  int list;
  int info;
  int show_options;
  int verbose;
  int overwrite;
  int no_deripple;
  int no_auto_white;
  char *extra[32];		/* name=value pairs */
  int extra_count;
};

static void
usage (const char *argv0)
{
  printf ("hp3770-scan %s - HP ScanJet 3770 scanner for macOS\n\n", TOOL_VERSION);
  printf ("Usage: %s [options] -o FILE\n\n", argv0);
  puts ("  -o, --output FILE        output file (.tif/.tiff or .pnm/.pgm/.ppm/.pbm)");
  puts ("      --format tiff|pnm    output format (default: from file name, else tiff)");
  puts ("  -m, --mode MODE          color, gray or lineart (default: color)");
  puts ("  -r, --resolution DPI     50 75 100 150 200 300 600 1200 (default: 300)");
  puts ("  -d, --depth BITS         8 or 16 bits per channel (default: 8)");
  puts ("  -l, --left MM            scan area left edge in mm (default 0)");
  puts ("  -t, --top MM             scan area top edge in mm (default 0)");
  puts ("  -x, --width MM           scan area width in mm (default: full width)");
  puts ("  -y, --height MM          scan area height in mm (default: full height)");
  puts ("      --a4 | --letter      preset scan area (210x297 mm / 215.9x279.4 mm)");
  puts ("      --set NAME=VALUE     set any backend option (see --all-options)");
  puts ("  -L, --list               list connected scanners and exit");
  puts ("      --info               open the scanner, print chipset/sensor info, exit");
  puts ("  -A, --all-options        print every backend option and exit");
  puts ("      --device NAME        device name from --list (default: first 3770)");
  puts ("  -f, --force              overwrite an existing output file");
  puts ("      --no-deripple        keep the lamp ripple bands (no correction)");
  puts ("      --no-auto-white      keep the calibrated levels (no paper white point)");
  puts ("  -v, --verbose            progress and parameter details");
  puts ("  -h, --help               this help");
  puts ("");
  puts ("Environment: SANE_DEBUG_HP3770=1..4 backend debug output,");
  puts ("             HP3770_USB_TRACE=FILE log every USB transfer to FILE,");
  puts ("             HP3770_MAINS=50|60 mains frequency for the ripple correction");
  puts ("             (default: try both),");
  puts ("             HP3770_WHITE_TARGET=200..255 paper white for --auto-white (245).");
}

static const char *
mode_value (const char *mode)
{
  if (strcasecmp (mode, "color") == 0 || strcasecmp (mode, "colour") == 0)
    return SANE_VALUE_SCAN_MODE_COLOR;
  if (strcasecmp (mode, "gray") == 0 || strcasecmp (mode, "grey") == 0)
    return SANE_VALUE_SCAN_MODE_GRAY;
  if (strcasecmp (mode, "lineart") == 0 || strcasecmp (mode, "bw") == 0)
    return SANE_VALUE_SCAN_MODE_LINEART;
  return NULL;
}

static void
print_options (SANE_Handle h)
{
  SANE_Int count = 0;
  int i;

  sane_control_option (h, 0, SANE_ACTION_GET_VALUE, &count, NULL);
  for (i = 1; i < count; i++)
    {
      const SANE_Option_Descriptor *d = sane_get_option_descriptor (h, i);
      if (d == NULL)
	continue;
      if (d->type == SANE_TYPE_GROUP)
	{
	  printf ("\n%s:\n", d->title ? d->title : "");
	  continue;
	}
      printf ("  %-18s ", d->name ? d->name : "");
      if (SANE_OPTION_IS_ACTIVE (d->cap) && d->type != SANE_TYPE_BUTTON)
	{
	  void *val = calloc (1, (size_t) d->size + 1);
	  if (val != NULL
	      && sane_control_option (h, i, SANE_ACTION_GET_VALUE, val, NULL) == SANE_STATUS_GOOD)
	    {
	      switch (d->type)
		{
		case SANE_TYPE_BOOL: printf ("[%s]", *(SANE_Word *) val ? "yes" : "no"); break;
		case SANE_TYPE_INT: printf ("[%d]", *(SANE_Word *) val); break;
		case SANE_TYPE_FIXED: printf ("[%.2f]", SANE_UNFIX (*(SANE_Word *) val)); break;
		case SANE_TYPE_STRING: printf ("[%s]", (char *) val); break;
		default: break;
		}
	    }
	  free (val);
	}
      else if (!SANE_OPTION_IS_ACTIVE (d->cap))
	printf ("[inactive]");

      if (d->constraint_type == SANE_CONSTRAINT_STRING_LIST)
	{
	  const SANE_String_Const *s;
	  printf (" {");
	  for (s = d->constraint.string_list; *s; s++)
	    printf ("%s%s", *s, s[1] ? "|" : "");
	  printf ("}");
	}
      else if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST)
	{
	  int k;
	  printf (" {");
	  for (k = 1; k <= d->constraint.word_list[0]; k++)
	    {
	      if (d->type == SANE_TYPE_FIXED)
		printf ("%.2f", SANE_UNFIX (d->constraint.word_list[k]));
	      else
		printf ("%d", d->constraint.word_list[k]);
	      printf ("%s", k < d->constraint.word_list[0] ? "|" : "");
	    }
	  printf ("}");
	}
      else if (d->constraint_type == SANE_CONSTRAINT_RANGE)
	{
	  if (d->type == SANE_TYPE_FIXED)
	    printf (" {%.2f..%.2f}", SANE_UNFIX (d->constraint.range->min),
		    SANE_UNFIX (d->constraint.range->max));
	  else
	    printf (" {%d..%d}", d->constraint.range->min, d->constraint.range->max);
	}
      printf ("  %s\n", d->title ? d->title : "");
    }
}

/* ------------------------------------------------------------ writers */

static void
put16 (FILE *f, unsigned v)
{
  fputc (v & 0xff, f);
  fputc ((v >> 8) & 0xff, f);
}

static void
put32 (FILE *f, uint32_t v)
{
  put16 (f, v & 0xffff);
  put16 (f, (v >> 16) & 0xffff);
}

static void
tiff_entry (FILE *f, unsigned tag, unsigned type, uint32_t count, uint32_t value)
{
  put16 (f, tag);
  put16 (f, type);
  put32 (f, count);
  if (type == 3 && count == 1)
    {
      put16 (f, value);
      put16 (f, 0);
    }
  else
    put32 (f, value);
}

/* Baseline little-endian TIFF: one strip, image data first, IFD at the end.
   For 16-bit data the samples are converted to little-endian. */
static int
write_tiff_header (FILE *f)
{
  fputs ("II", f);
  put16 (f, 42);
  put32 (f, 0);			/* IFD offset, patched by write_tiff_trailer */
  return ferror (f) ? -1 : 0;
}

static int
write_tiff_trailer (FILE *f, const SANE_Parameters *p, int lines, int dpi)
{
  long data_end = ftell (f);
  uint32_t data_size = (uint32_t) (data_end - 8);
  int spp = (p->format == SANE_FRAME_RGB) ? 3 : 1;
  int entries = 14;
  uint32_t ifd, extra, bps_off, xres_off, yres_off, sw_off;
  const char software[] = "hp3770-scan " TOOL_VERSION;

  if (data_end & 1)
    {
      fputc (0, f);
      data_end++;
    }
  ifd = (uint32_t) data_end;
  extra = ifd + 2 + (uint32_t) entries * 12 + 4;
  bps_off = extra;
  xres_off = bps_off + 6;
  yres_off = xres_off + 8;
  sw_off = yres_off + 8;

  put16 (f, entries);
  tiff_entry (f, 254, 4, 1, 0);			/* NewSubfileType */
  tiff_entry (f, 256, 4, 1, (uint32_t) p->pixels_per_line);	/* ImageWidth */
  tiff_entry (f, 257, 4, 1, (uint32_t) lines);	/* ImageLength */
  if (spp == 3)
    tiff_entry (f, 258, 3, 3, bps_off);		/* BitsPerSample */
  else
    tiff_entry (f, 258, 3, 1, (uint32_t) p->depth);
  tiff_entry (f, 259, 3, 1, 1);			/* Compression: none */
  tiff_entry (f, 262, 3, 1, spp == 3 ? 2 : (p->depth == 1 ? 0 : 1));	/* Photometric */
  tiff_entry (f, 273, 4, 1, 8);			/* StripOffsets */
  tiff_entry (f, 277, 3, 1, (uint32_t) spp);	/* SamplesPerPixel */
  tiff_entry (f, 278, 4, 1, (uint32_t) lines);	/* RowsPerStrip */
  tiff_entry (f, 279, 4, 1, data_size);		/* StripByteCounts */
  tiff_entry (f, 282, 5, 1, xres_off);		/* XResolution */
  tiff_entry (f, 283, 5, 1, yres_off);		/* YResolution */
  tiff_entry (f, 296, 3, 1, 2);			/* ResolutionUnit: inch */
  tiff_entry (f, 305, 2, (uint32_t) sizeof (software), sw_off);	/* Software */
  put32 (f, 0);					/* no next IFD */

  put16 (f, (unsigned) p->depth);
  put16 (f, (unsigned) p->depth);
  put16 (f, (unsigned) p->depth);
  put32 (f, (uint32_t) dpi);
  put32 (f, 1);
  put32 (f, (uint32_t) dpi);
  put32 (f, 1);
  fwrite (software, 1, sizeof (software), f);

  if (fseek (f, 4, SEEK_SET) != 0)
    return -1;
  put32 (f, ifd);
  return ferror (f) ? -1 : 0;
}

static int
write_pnm_header (FILE *f, const SANE_Parameters *p, int lines, int dpi)
{
  /* the line count is space-padded to a fixed width so the header can be
     rewritten in place once the real number of lines is known */
  if (p->depth == 1)
    fprintf (f, "P4\n# hp3770-scan %d dpi\n%d %10d\n", dpi, p->pixels_per_line, lines);
  else
    fprintf (f, "P%c\n# hp3770-scan %d dpi\n%d %10d\n%d\n",
	     p->format == SANE_FRAME_RGB ? '6' : '5', dpi, p->pixels_per_line,
	     lines, p->depth == 16 ? 65535 : 255);
  return ferror (f) ? -1 : 0;
}

/* Byte order of the backend's 16-bit samples is host order (SANE). */
static void
swap16_if_needed (SANE_Byte *buf, int len, int want_big_endian)
{
  uint16_t probe = 1;
  int host_little = *(uint8_t *) &probe == 1;
  int i;

  if (host_little == !want_big_endian)
    return;
  for (i = 0; i + 1 < len; i += 2)
    {
      SANE_Byte t = buf[i];
      buf[i] = buf[i + 1];
      buf[i + 1] = t;
    }
}

/* Post-processing of the finished image data in the output file: lamp
   ripple removal, then the paper white point. */
static void
postprocess_image (FILE *out, long data_start, const SANE_Parameters *p, int lines,
		   const struct settings *cfg, int tiff)
{
  size_t size = (size_t) lines * (size_t) p->bytes_per_line;
  int changed;
  int dpi = cfg->resolution;
  unsigned char *img;

  img = malloc (size);
  if (img == NULL)
    return;
  if (fseek (out, data_start, SEEK_SET) != 0 || fread (img, 1, size, out) != size)
    {
      free (img);
      return;
    }
  if (p->depth == 16)
    swap16_if_needed (img, (int) size, !tiff);	/* file order -> host order */

  changed = postprocess_buffer (img, p, lines, dpi, cfg->no_deripple, cfg->no_auto_white,
			       cfg->verbose);

  if (changed)
    {
      if (p->depth == 16)
	swap16_if_needed (img, (int) size, !tiff);
      if (fseek (out, data_start, SEEK_SET) == 0)
	fwrite (img, 1, size, out);
    }
  free (img);
}

/* SANE lineart is 1 = black; TIFF Photometric 0 (WhiteIsZero) matches it,
   and so does PBM. */

/* --------------------------------------------------------------- scan */

static int
do_scan (SANE_Handle h, const struct settings *cfg)
{
  SANE_Parameters p;
  SANE_Status st;
  FILE *out;
  SANE_Byte *buf;
  const int bufsize = 256 * 1024;
  long total = 0, expected;
  int lines, tiff, odd = 0;
  long data_start = 0;
  SANE_Byte carry = 0;
  time_t started = time (NULL);
  char tmpname[4096];

  tiff = strcmp (cfg->format, "tiff") == 0;

  if (!cfg->overwrite && access (cfg->output, F_OK) == 0)
    {
      fprintf (stderr, "hp3770-scan: %s exists (use --force to overwrite)\n", cfg->output);
      return 1;
    }

  fprintf (stderr, "Scanning... (lamp warm-up and calibration can take up to a minute)\n");
  st = sane_start (h);
  if (st != SANE_STATUS_GOOD)
    {
      fprintf (stderr, "hp3770-scan: sane_start failed: %s\n", sane_strstatus (st));
      return 1;
    }
  st = sane_get_parameters (h, &p);
  if (st != SANE_STATUS_GOOD || p.bytes_per_line <= 0)
    {
      fprintf (stderr, "hp3770-scan: cannot get scan parameters: %s\n", sane_strstatus (st));
      sane_cancel (h);
      return 1;
    }
  if (cfg->verbose)
    fprintf (stderr, "  frame: %s, %d x %d px, depth %d, %d bytes/line\n",
	     p.format == SANE_FRAME_RGB ? "RGB" : "gray", p.pixels_per_line,
	     p.lines, p.depth, p.bytes_per_line);

  snprintf (tmpname, sizeof (tmpname), "%s.part", cfg->output);
  out = fopen (tmpname, "w+b");
  if (out == NULL)
    {
      fprintf (stderr, "hp3770-scan: cannot create %s: %s\n", tmpname, strerror (errno));
      sane_cancel (h);
      return 1;
    }
  buf = malloc ((size_t) bufsize);
  if (buf == NULL)
    {
      fclose (out);
      remove (tmpname);
      sane_cancel (h);
      return 1;
    }

  if (tiff)
    write_tiff_header (out);
  else
    write_pnm_header (out, &p, p.lines > 0 ? p.lines : 0, cfg->resolution);
  data_start = ftell (out);

  expected = (p.lines > 0) ? (long) p.lines * p.bytes_per_line : -1;
  for (;;)
    {
      SANE_Int len = 0;

      if (cancel_requested)
	{
	  fprintf (stderr, "\nCancelling...\n");
	  sane_cancel (h);
	  fclose (out);
	  remove (tmpname);
	  free (buf);
	  return 130;
	}
      st = sane_read (h, buf, bufsize, &len);
      if (st == SANE_STATUS_EOF)
	break;
      if (st != SANE_STATUS_GOOD)
	{
	  fprintf (stderr, "\nhp3770-scan: read error: %s\n", sane_strstatus (st));
	  sane_cancel (h);
	  fclose (out);
	  fprintf (stderr, "hp3770-scan: partial data kept in %s\n", tmpname);
	  free (buf);
	  return 1;
	}
      if (len <= 0)
	continue;

      if (p.depth == 16)
	{
	  /* keep 16-bit samples aligned across reads before swapping */
	  SANE_Byte *data = buf;
	  int n = len;
	  if (odd)
	    {
	      SANE_Byte pair[2] = { carry, buf[0] };
	      swap16_if_needed (pair, 2, !tiff);
	      fwrite (pair, 1, 2, out);
	      data++;
	      n--;
	      odd = 0;
	    }
	  if (n & 1)
	    {
	      carry = data[n - 1];
	      odd = 1;
	      n--;
	    }
	  swap16_if_needed (data, n, !tiff);
	  fwrite (data, 1, (size_t) n, out);
	}
      else
	fwrite (buf, 1, (size_t) len, out);

      total += len;
      if (cfg->verbose || isatty (STDERR_FILENO))
	{
	  if (expected > 0)
	    fprintf (stderr, "\r  %3ld%%  %ld KiB", total * 100 / expected, total / 1024);
	  else
	    fprintf (stderr, "\r  %ld KiB", total / 1024);
	}
    }
  fprintf (stderr, "\n");
  sane_cancel (h);
  free (buf);

  lines = (int) (total / p.bytes_per_line);
  if (lines <= 0)
    {
      fprintf (stderr, "hp3770-scan: no image data received\n");
      fclose (out);
      remove (tmpname);
      return 1;
    }

  if ((!cfg->no_deripple || !cfg->no_auto_white) && p.depth >= 8)
    postprocess_image (out, data_start, &p, lines, cfg, tiff);

  if (tiff)
    {
      fseek (out, 0, SEEK_END);
      write_tiff_trailer (out, &p, lines, cfg->resolution);
    }
  else if (p.lines <= 0 || lines != p.lines)
    {
      /* rewrite the PNM header with the real line count */
      fseek (out, 0, SEEK_SET);
      write_pnm_header (out, &p, lines, cfg->resolution);
    }

  if (fclose (out) != 0 || rename (tmpname, cfg->output) != 0)
    {
      fprintf (stderr, "hp3770-scan: cannot write %s: %s\n", cfg->output, strerror (errno));
      return 1;
    }
  fprintf (stderr, "Saved %s: %d x %d px, %d dpi, %s %d-bit (%ld s)\n", cfg->output,
	   p.pixels_per_line, lines, cfg->resolution,
	   p.format == SANE_FRAME_RGB ? "colour" : (p.depth == 1 ? "lineart" : "gray"),
	   p.depth, (long) (time (NULL) - started));
  return 0;
}

static void
print_info (SANE_Handle h)
{
  static const char *names[] = { "opt_chipname", "opt_chipid", "opt_scancount", NULL };
  int i, ccd = hp3770_backend_ccd_status ();

  /* the information options are refreshed by pressing this button */
  {
    int idx = find_option (h, "opt_infoupdate");
    if (idx >= 0)
      sane_control_option (h, idx, SANE_ACTION_SET_VALUE, NULL, NULL);
  }
  if (ccd >= 0)
    printf ("  %-22s 0x%02x (CCD profile t6, as HP's driver)\n", "Status byte 0xf910", ccd);

  for (i = 0; names[i] != NULL; i++)
    {
      int idx = find_option (h, names[i]);
      const SANE_Option_Descriptor *d;
      char val[256] = { 0 };

      if (idx < 0)
	continue;
      d = sane_get_option_descriptor (h, idx);
      if (d->size > (SANE_Int) sizeof (val) - 1)
	continue;
      if (sane_control_option (h, idx, SANE_ACTION_GET_VALUE, val, NULL) != SANE_STATUS_GOOD)
	continue;
      if (d->type == SANE_TYPE_STRING)
	printf ("  %-22s %s\n", d->title, val);
      else
	printf ("  %-22s %d\n", d->title, *(SANE_Word *) val);
    }
  printf ("  %-22s %.1f x %.1f mm\n", "Maximum scan area",
	  get_number (h, SANE_NAME_SCAN_BR_X, 1), get_number (h, SANE_NAME_SCAN_BR_Y, 1));
}

static const char *
format_from_name (const char *name)
{
  const char *dot = strrchr (name, '.');
  if (dot != NULL)
    {
      if (strcasecmp (dot, ".pnm") == 0 || strcasecmp (dot, ".ppm") == 0
	  || strcasecmp (dot, ".pgm") == 0 || strcasecmp (dot, ".pbm") == 0)
	return "pnm";
    }
  return "tiff";
}

int
main (int argc, char **argv)
{
  struct settings cfg;
  const SANE_Device **list = NULL;
  SANE_Handle h = NULL;
  SANE_Status st;
  int rc = 0, i, c;
  enum { OPT_FORMAT = 256, OPT_SET, OPT_INFO, OPT_DEVICE, OPT_A4, OPT_LETTER, OPT_NODERIPPLE, OPT_NOAUTOWHITE };
  static const struct option longopts[] = {
    {"output", required_argument, NULL, 'o'},
    {"format", required_argument, NULL, OPT_FORMAT},
    {"mode", required_argument, NULL, 'm'},
    {"resolution", required_argument, NULL, 'r'},
    {"depth", required_argument, NULL, 'd'},
    {"left", required_argument, NULL, 'l'},
    {"top", required_argument, NULL, 't'},
    {"width", required_argument, NULL, 'x'},
    {"height", required_argument, NULL, 'y'},
    {"a4", no_argument, NULL, OPT_A4},
    {"letter", no_argument, NULL, OPT_LETTER},
    {"set", required_argument, NULL, OPT_SET},
    {"list", no_argument, NULL, 'L'},
    {"info", no_argument, NULL, OPT_INFO},
    {"all-options", no_argument, NULL, 'A'},
    {"device", required_argument, NULL, OPT_DEVICE},
    {"force", no_argument, NULL, 'f'},
    {"no-deripple", no_argument, NULL, OPT_NODERIPPLE},
    {"no-auto-white", no_argument, NULL, OPT_NOAUTOWHITE},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };
  double width = -1.0, height = -1.0;

  memset (&cfg, 0, sizeof (cfg));
  cfg.mode = "color";
  cfg.resolution = 300;
  cfg.depth = 8;
  cfg.tl_x = cfg.tl_y = 0.0;
  cfg.br_x = cfg.br_y = -1.0;

  while ((c = getopt_long (argc, argv, "o:m:r:d:l:t:x:y:LAfvh", longopts, NULL)) != -1)
    {
      switch (c)
	{
	case 'o': cfg.output = optarg; break;
	case OPT_FORMAT: cfg.format = optarg; break;
	case 'm': cfg.mode = optarg; break;
	case 'r': cfg.resolution = atoi (optarg); break;
	case 'd': cfg.depth = atoi (optarg); break;
	case 'l': cfg.tl_x = atof (optarg); break;
	case 't': cfg.tl_y = atof (optarg); break;
	case 'x': width = atof (optarg); break;
	case 'y': height = atof (optarg); break;
	case OPT_A4: width = 210.0; height = 297.0; break;
	case OPT_LETTER: width = 215.9; height = 279.4; break;
	case OPT_SET:
	  if (cfg.extra_count < 32 && strchr (optarg, '=') != NULL)
	    cfg.extra[cfg.extra_count++] = optarg;
	  else
	    {
	      fprintf (stderr, "hp3770-scan: --set expects NAME=VALUE\n");
	      return 64;
	    }
	  break;
	case 'L': cfg.list = 1; break;
	case OPT_INFO: cfg.info = 1; break;
	case 'A': cfg.show_options = 1; break;
	case OPT_DEVICE: cfg.device = optarg; break;
	case 'f': cfg.overwrite = 1; break;
	case OPT_NODERIPPLE: cfg.no_deripple = 1; break;
	case OPT_NOAUTOWHITE: cfg.no_auto_white = 1; break;
	case 'v': cfg.verbose = 1; break;
	case 'h': usage (argv[0]); return 0;
	default: usage (argv[0]); return 64;
	}
    }
  if (optind < argc)
    {
      fprintf (stderr, "hp3770-scan: unexpected argument '%s'\n", argv[optind]);
      return 64;
    }
  if (!cfg.list && !cfg.info && !cfg.show_options && cfg.output == NULL)
    {
      usage (argv[0]);
      return 64;
    }
  if (mode_value (cfg.mode) == NULL)
    {
      fprintf (stderr, "hp3770-scan: unknown mode '%s' (color, gray, lineart)\n", cfg.mode);
      return 64;
    }
  if (cfg.format == NULL && cfg.output != NULL)
    cfg.format = format_from_name (cfg.output);
  if (cfg.format == NULL)
    cfg.format = "tiff";
  if (strcmp (cfg.format, "tiff") != 0 && strcmp (cfg.format, "pnm") != 0)
    {
      fprintf (stderr, "hp3770-scan: format must be tiff or pnm\n");
      return 64;
    }
  if (cfg.depth != 8 && cfg.depth != 16)
    {
      fprintf (stderr, "hp3770-scan: depth must be 8 or 16\n");
      return 64;
    }

  signal (SIGINT, on_signal);
  signal (SIGTERM, on_signal);

  st = sane_init (NULL, NULL);
  if (st != SANE_STATUS_GOOD)
    {
      fprintf (stderr, "hp3770-scan: backend initialisation failed: %s\n", sane_strstatus (st));
      return 1;
    }
  st = sane_get_devices (&list, SANE_TRUE);
  if (st != SANE_STATUS_GOOD || list == NULL || list[0] == NULL)
    {
      fprintf (stderr, "hp3770-scan: no HP ScanJet 3770 (03f0:2505) found. "
	       "Connect it directly by USB and power it on.\n");
      sane_exit ();
      return 2;
    }
  if (cfg.list)
    {
      for (i = 0; list[i] != NULL; i++)
	printf ("%s: %s %s (%s)\n", list[i]->name, list[i]->vendor, list[i]->model,
		list[i]->type);
      sane_exit ();
      return 0;
    }

  fprintf (stderr, "Opening %s %s (the scan head returns home)...\n",
	   list[0]->vendor, list[0]->model);
  st = sane_open (cfg.device ? cfg.device : list[0]->name, &h);
  if (st != SANE_STATUS_GOOD)
    {
      fprintf (stderr, "hp3770-scan: cannot open the scanner: %s\n", sane_strstatus (st));
      sane_exit ();
      return 1;
    }

  if (cfg.info)
    {
      printf ("%s %s at %s\n", list[0]->vendor, list[0]->model, list[0]->name);
      print_info (h);
      goto done;
    }

  /* --set first: changing opt_model reloads the configuration and the
     option lists, so mode, resolution and geometry are applied after it */
  for (i = 0; i < cfg.extra_count; i++)
    {
      char *eq = strchr (cfg.extra[i], '=');
      *eq = '\0';
      if (set_option_text (h, cfg.extra[i], eq + 1, cfg.verbose) != 0)
	{ rc = 64; goto done; }
      *eq = '=';
    }

  if (set_option_text (h, SANE_NAME_SCAN_MODE, mode_value (cfg.mode), cfg.verbose) != 0)
    { rc = 64; goto done; }
  {
    char text[32];
    snprintf (text, sizeof (text), "%d", cfg.resolution);
    if (set_option_text (h, SANE_NAME_SCAN_RESOLUTION, text, cfg.verbose) != 0)
      { rc = 64; goto done; }
    if (strcasecmp (cfg.mode, "lineart") != 0)
      {
	snprintf (text, sizeof (text), "%d", cfg.depth);
	if (set_option_text (h, SANE_NAME_BIT_DEPTH, text, cfg.verbose) != 0)
	  { rc = 64; goto done; }
      }
  }

  /* geometry: br = tl + size, clipped to the scanner's maximum */
  {
    double max_x = get_number (h, SANE_NAME_SCAN_BR_X, 1);
    double max_y = get_number (h, SANE_NAME_SCAN_BR_Y, 1);
    double bx = (width > 0) ? cfg.tl_x + width : max_x;
    double by = (height > 0) ? cfg.tl_y + height : max_y;

    if (bx > max_x) bx = max_x;
    if (by > max_y) by = max_y;
    if (set_number (h, SANE_NAME_SCAN_TL_X, cfg.tl_x, cfg.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_TL_Y, cfg.tl_y, cfg.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_BR_X, bx, cfg.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_BR_Y, by, cfg.verbose) != 0)
      { rc = 64; goto done; }
  }

  if (cfg.show_options)
    {
      print_options (h);
      goto done;
    }

  rc = do_scan (h, &cfg);

done:
  sane_close (h);
  sane_exit ();
  return rc;
}
