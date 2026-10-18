/* Simulated RTS8822 scanner behind the sanei_usb API, for exercising the
   hp3770 backend and hp3770-scan end to end without hardware.

   It models just enough of the controller: a 64 KiB register space seeded
   from a real HP 3770 idle snapshot (HP3770_SIM_REGS=tests/idle-registers.bin), an
   NVRAM page, the "execute" bit 0xe800.7 that clears itself, DMA RAM for
   gamma/shading uploads with read-back, and a synthetic image stream on DMA
   channel 0x0008. It is a test harness, not an emulator: calibration and
   reference detection see artificial data.

   GPL-2.0-or-later. */

#include "sane/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sane/sane.h"
#include "sane/sanei_config.h"
#include "sane/sanei_usb.h"

#define RAM_SIZE (8 * 1024 * 1024)

static unsigned char regs[0x10000];
static unsigned char nvram[0x100];
static unsigned char *ram = NULL;
static int is_open = 0;
static int exec_countdown = 0;
/* home sensor model: a job that moves the head forward (0xe8d9 bit 7 set)
   takes it away from home, a backward move (park) brings it back */
static int head_away = 0, park_pending = 0;
static unsigned long mock_jobs_moving = 0;
static int dma_dmacs = -1, dma_write = 0;
static long dma_offset = 0, dma_left = 0;
/* image data the current job can still deliver (a real RTS8822 reports its
   buffer fill level during a scan, whatever DMA transfer is programmed) */
static long scan_avail = 0;
static unsigned long image_bytes = 0;
static unsigned long stats_ctl = 0, stats_bulk_in = 0, stats_bulk_out = 0;
static FILE *trace = NULL;

void
sanei_usb_trace_note (const char *fmt, ...)
{
  va_list ap;
  if (trace == NULL)
    return;
  va_start (ap, fmt);
  vfprintf (trace, fmt, ap);
  va_end (ap);
  fputc ('\n', trace);
}

void
sanei_usb_init (void)
{
  const char *path = getenv ("HP3770_SIM_REGS");
  const char *tr = getenv ("HP3770_USB_TRACE");

  memset (regs, 0, sizeof (regs));
  memset (nvram, 0xff, sizeof (nvram));
  if (path != NULL)
    {
      FILE *f = fopen (path, "rb");
      if (f != NULL)
	{
	  size_t n = fread (regs + 0xe800, 1, 0x71a, f);
	  fclose (f);
	  if (n != 0x71a)
	    fprintf (stderr, "[sim] short register snapshot (%lu bytes)\n", (unsigned long) n);
	}
      else
	fprintf (stderr, "[sim] cannot open %s\n", path);
    }
  regs[0xf910] = getenv ("HP3770_SIM_F910") ? (unsigned char) strtol (getenv ("HP3770_SIM_F910"), NULL, 0) : 0x7a;	/* 0x7a: Toshiba CCD, init done */
  if (getenv ("HP3770_SIM_BAD_REFS") != NULL)
    {
      /* checksum-valid but implausible reference position in NVRAM */
      unsigned x = 5000, y = 100, c = (0x5a - (x + y)) & 0xff;
      nvram[0x6a] = x & 0xff; nvram[0x6b] = x >> 8;
      nvram[0x6c] = y & 0xff; nvram[0x6d] = y >> 8;
      nvram[0x6e] = (unsigned char) c; nvram[0x6f] = 0;
      nvram[0x78] = 5;		/* reference counter != 0: use stored value */
      regs[0xe829] = 1;		/* not the first connection after power-up */
    }
  regs[0xfe11] = 0x01;		/* USB 2.0 */
  regs[0xfe3e] = 0x10;		/* revision reported by the real 3770 */
  regs[0xe96f] |= 0x40;		/* head at home */
  regs[0xef09] = 0x01;		/* DMA ready */
  if (ram == NULL)
    ram = calloc (1, RAM_SIZE);
  if (tr != NULL && trace == NULL)
    trace = fopen (tr, "w");
}

void
sanei_usb_exit (void)
{
  fprintf (stderr, "[sim] %lu control, %lu bulk-in, %lu bulk-out transfers; "
	   "%lu image bytes streamed\n", stats_ctl, stats_bulk_in, stats_bulk_out,
	   image_bytes);
  if (trace != NULL)
    fclose (trace);
  trace = NULL;
}

SANE_Status
sanei_usb_find_devices (SANE_Int vendor, SANE_Int product,
			SANE_Status (*attach) (SANE_String_Const devname))
{
  if (vendor == 0x03f0 && product == 0x2505 && attach != NULL)
    attach ("sim:hp3770");
  return SANE_STATUS_GOOD;
}

void
sanei_usb_attach_matching_devices (const char *name,
				   SANE_Status (*attach) (SANE_String_Const devname))
{
  if (strstr (name, "0x2505") != NULL)
    sanei_usb_find_devices (0x03f0, 0x2505, attach);
}

SANE_Status
sanei_usb_open (SANE_String_Const devname, SANE_Int * dn)
{
  if (strcmp (devname, "sim:hp3770") != 0)
    return SANE_STATUS_INVAL;
  is_open = 1;
  *dn = 0;
  return SANE_STATUS_GOOD;
}

void
sanei_usb_close (SANE_Int dn)
{
  (void) dn;
  is_open = 0;
}

SANE_Status
sanei_usb_get_vendor_product (SANE_Int dn, SANE_Word * vendor, SANE_Word * product)
{
  (void) dn;
  *vendor = 0x03f0;
  *product = 0x2505;
  return SANE_STATUS_GOOD;
}

void
sanei_usb_set_timeout (SANE_Int timeout)
{
  (void) timeout;
}

static unsigned char
image_byte (unsigned long n)
{
  /* bright paper with a dark band every 64 KiB and a little texture */
  unsigned long block = (n / 4096) % 16;
  unsigned char v = (block == 7) ? 40 : 210;
  return (unsigned char) (v + (n * 7 % 13));
}

SANE_Status
sanei_usb_read_bulk (SANE_Int dn, SANE_Byte * buffer, size_t * size)
{
  size_t i, n = *size;
  (void) dn;

  if (!is_open)
    return SANE_STATUS_INVAL;
  stats_bulk_in++;
  if (dma_dmacs == 0x0008)
    {
      for (i = 0; i < n; i++)
	buffer[i] = image_byte (image_bytes++);
    }
  else if (dma_dmacs >= 0 && dma_offset + (long) n <= RAM_SIZE)
    {
      memcpy (buffer, ram + dma_offset, n);
      dma_offset += (long) n;
    }
  else
    memset (buffer, 0, n);
  if (trace)
    fprintf (trace, "BI dmacs=%04x len=%lu\n", dma_dmacs & 0xffff, (unsigned long) n);
  if (dma_left > 0)
    dma_left -= (long) n;
  if ((dma_dmacs == 0x0008) && (scan_avail > 0))
    scan_avail -= (long) n;
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_usb_write_bulk (SANE_Int dn, const SANE_Byte * buffer, size_t * size)
{
  (void) dn;
  if (!is_open)
    return SANE_STATUS_INVAL;
  stats_bulk_out++;
  if (dma_write && dma_offset + (long) *size <= RAM_SIZE)
    {
      memcpy (ram + dma_offset, buffer, *size);
      dma_offset += (long) *size;
    }
  if (trace)
    fprintf (trace, "BO dmacs=%04x len=%lu\n", dma_dmacs & 0xffff, (unsigned long) *size);
  return SANE_STATUS_GOOD;
}

static void
put_le (unsigned char *p, unsigned long v, int n)
{
  int i;
  for (i = 0; i < n; i++)
    p[i] = (unsigned char) (v >> (8 * i));
}

SANE_Status
sanei_usb_control_msg (SANE_Int dn, SANE_Int rtype, SANE_Int req,
		       SANE_Int value, SANE_Int index, SANE_Int len,
		       SANE_Byte * data)
{
  int space = index & 0xff00;
  int i;
  (void) dn;

  if (!is_open)
    return SANE_STATUS_INVAL;
  stats_ctl++;
  /* same line format as the legacy-driver harness: writes carry their
     data, so register images can be rebuilt from either trace */
  if (trace)
    {
      fprintf (trace, "%s %02x %04x %04x %04x:", (rtype & 0x80) ? "CI" : "CO",
	       req & 0xff, value & 0xffff, index & 0xffff, len);
      if (!(rtype & 0x80) && data != NULL)
	for (i = 0; i < len; i++)
	  fprintf (trace, " %02x", data[i]);
      fputc ('\n', trace);
    }
  if (req != 0x00 && getenv ("HP3770_SIM_ANY_REQUEST") == NULL)
    {
      fprintf (stderr, "[sim] unexpected bRequest 0x%02x\n", req);
      return SANE_STATUS_IO_ERROR;
    }

  if (rtype & 0x80)
    {
      if (space == 0x0100)
	{
	  if (exec_countdown > 0 && --exec_countdown == 0)
	    {
	      regs[0xe800] &= 0x7f;	/* job finished */
	      if (park_pending)
		head_away = park_pending = 0;
	    }
	  if (head_away)
	    regs[0xe96f] &= ~0x40;
	  else
	    regs[0xe96f] |= 0x40;
	  if (value + len > 0x10000)
	    return SANE_STATUS_INVAL;
	  /* pending image amount in the units Reading_BufferSize_Get expects */
	  {
	    int cpd = regs[0xe812] >> 6;
	    unsigned long amount;
	    if (cpd == 0)
	      cpd = 1;
	    {
	      long avail = scan_avail;
	      if (avail > 0x100000)
		avail = 0x100000;
	      amount = (avail > 0) ? (unsigned long) avail / (unsigned long) (cpd * 32) : 0;
	    }
	    put_le (regs + 0xef16, amount, 3);
	  }
	  memcpy (data, regs + value, (size_t) len);
	}
      else if (space == 0x0200)
	{
	  for (i = 0; i < len; i++)
	    data[i] = nvram[(value + i) & 0xff];
	}
      else
	memset (data, 0, (size_t) len);
      return SANE_STATUS_GOOD;
    }

  switch (space)
    {
    case 0x0000:
      if (len > 0x40)
	{
	  fprintf (stderr, "[sim] register write of %d bytes not split\n", len);
	  return SANE_STATUS_IO_ERROR;
	}
      if (value + len > 0x10000)
	return SANE_STATUS_INVAL;
      for (i = 0; i < len; i++)
	{
	  int a = value + i;
	  if (a == 0xe800 && (data[i] & 0x80) && !(regs[0xe800] & 0x80))
	    {
	      exec_countdown = 3;	/* start a job: busy for three status reads */
	      scan_avail = 1024L * 1024 * 1024;
	      if ((regs[0xe8d8] & 0x80) || (regs[0xe801] & 0x10))
		{
		  if (regs[0xe8d9] & 0x80)
		    head_away = 1;
		  else
		    park_pending = 1;
		  mock_jobs_moving++;
		  if (getenv ("HP3770_SIM_MOVES") != NULL)
		    fprintf (stderr, "[sim] motor job %s: clock %d, ctpc %lu, step %d, curve %d, cnpp %d\n",
			     (regs[0xe8d9] & 0x80) ? "forward" : "backward", regs[0xe800] & 0x0f,
			     (unsigned long) (regs[0xe830] | (regs[0xe831] << 8) | (regs[0xe832] << 16)),
			     regs[0xe8e0] + 1, (regs[0xe8df] & 0x10) ? 1 : 0, (regs[0xe896] & 0x3f) + 1);
		}
	    }
	  regs[a] = data[i];
	}
      if (head_away)
	regs[0xe96f] &= ~0x40;
      else
	regs[0xe96f] |= 0x40;
      regs[0xef09] = 0x01;
      break;
    case 0x0200:
      for (i = 0; i < len; i++)
	nvram[(value + i) & 0xff] = data[i];
      break;
    case 0x0400:
      if (len == 6)
	{
	  unsigned long options = ((unsigned long) data[0] << 16) | ((unsigned long) data[1] << 8) | data[2];
	  unsigned long words = (unsigned long) data[3] | ((unsigned long) data[4] << 8) | ((unsigned long) data[5] << 16);
	  dma_dmacs = value;
	  dma_write = (index & 1) != 0;
	  dma_offset = (long) ((options * 2) % (RAM_SIZE / 2));
	  dma_left = (long) (words * 2);
	}
      break;
    default:			/* 0x0600 DMA cancel, 0x0800 resets */
      dma_dmacs = -1;
      dma_left = 0;
      break;
    }
  return SANE_STATUS_GOOD;
}
