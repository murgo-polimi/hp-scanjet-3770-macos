/* sanei_usb subset on top of libusb-1.0, for running the hp3770 backend as
   a native macOS user-space driver (no kernel extension needed: the scanner
   exposes a vendor-specific interface that IOKit leaves unclaimed).

   Semantics follow sane-backends sanei/sanei_usb.c (libusb-1.0 branch):
   30 s default timeout, bulk errors clear the endpoint halt and return
   SANE_STATUS_IO_ERROR, a zero-length bulk read returns SANE_STATUS_EOF.

   Copyright (C) 2026 hp3770-macos contributors. GPL-2.0-or-later. */

#include "sane/config.h"

#include <libusb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sane/sane.h"
#include "sane/sanei_config.h"
#include "sane/sanei_usb.h"

#define MAX_DEVICES 16

struct usb_dev
{
  char devname[32];
  libusb_device *device;
  libusb_device_handle *handle;
  int vendor, product;
  int interface_nr;
  unsigned char bulk_in_ep, bulk_out_ep, int_in_ep;
  int open;
};

static libusb_context *ctx = NULL;
static struct usb_dev devices[MAX_DEVICES];
static int device_number = 0;
static int usb_timeout = 30 * 1000;
static FILE *trace = NULL;

static void
usb_dbg (const char *fmt, ...)
{
  static int level = -1;
  va_list ap;

  if (level < 0)
    {
      const char *env = getenv ("SANE_DEBUG_SANEI_USB");
      level = (env != NULL) ? atoi (env) : 0;
    }
  if (level <= 0)
    return;

  fputs ("[sanei_usb] ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
}

static void
trace_open (void)
{
  static int tried = 0;
  const char *path;

  if (tried)
    return;
  tried = 1;
  path = getenv ("HP3770_USB_TRACE");
  if ((path != NULL) && (path[0] != '\0'))
    {
      trace = fopen (path, "a");
      if (trace != NULL)
	{
	  time_t now = time (NULL);
	  fprintf (trace, "# hp3770 USB trace started %s", ctime (&now));
	}
      else
	fprintf (stderr, "hp3770: cannot open USB trace file %s\n", path);
    }
}

void
sanei_usb_trace_note (const char *fmt, ...)
{
  va_list ap;

  trace_open ();
  if (trace == NULL)
    return;
  fputs ("# ", trace);
  va_start (ap, fmt);
  vfprintf (trace, fmt, ap);
  va_end (ap);
  fputc ('\n', trace);
  fflush (trace);
}

static void
trace_bytes (const unsigned char *data, int len, int limit)
{
  int i;

  for (i = 0; (i < len) && (i < limit); i++)
    fprintf (trace, " %02x", data[i]);
  if (len > limit)
    fprintf (trace, " ...(+%d)", len - limit);
}

void
sanei_usb_init (void)
{
  int rc;

  trace_open ();
  if (ctx != NULL)
    return;

  rc = libusb_init (&ctx);
  if (rc != LIBUSB_SUCCESS)
    {
      fprintf (stderr, "hp3770: libusb_init failed: %s\n", libusb_error_name (rc));
      ctx = NULL;
      return;
    }

  {
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list (ctx, &list);
    ssize_t i;

    device_number = 0;
    for (i = 0; (i < count) && (device_number < MAX_DEVICES); i++)
      {
	struct libusb_device_descriptor desc;
	struct usb_dev *d;

	if (libusb_get_device_descriptor (list[i], &desc) != LIBUSB_SUCCESS)
	  continue;

	d = &devices[device_number++];
	memset (d, 0, sizeof (*d));
	d->device = libusb_ref_device (list[i]);
	d->vendor = desc.idVendor;
	d->product = desc.idProduct;
	snprintf (d->devname, sizeof (d->devname), "libusb:%03u:%03u",
		  libusb_get_bus_number (list[i]),
		  libusb_get_device_address (list[i]));
      }
    if (count > 0)
      libusb_free_device_list (list, 1);
  }
  usb_dbg ("found %d USB devices\n", device_number);
}

void
sanei_usb_exit (void)
{
  int i;

  for (i = 0; i < device_number; i++)
    {
      if (devices[i].open)
	sanei_usb_close (i);
      if (devices[i].device != NULL)
	libusb_unref_device (devices[i].device);
      devices[i].device = NULL;
    }
  device_number = 0;
  if (ctx != NULL)
    libusb_exit (ctx);
  ctx = NULL;
  if (trace != NULL)
    fclose (trace);
  trace = NULL;
}

SANE_Status
sanei_usb_find_devices (SANE_Int vendor, SANE_Int product,
			SANE_Status (*attach) (SANE_String_Const devname))
{
  int i;

  for (i = 0; i < device_number; i++)
    if ((devices[i].vendor == vendor) && (devices[i].product == product)
	&& (attach != NULL))
      attach (devices[i].devname);

  return SANE_STATUS_GOOD;
}

void
sanei_usb_attach_matching_devices (const char *name,
				   SANE_Status (*attach) (SANE_String_Const devname))
{
  if (strncmp (name, "usb", 3) == 0)
    {
      char *vendor = NULL, *product = NULL;
      SANE_Int vendor_id = 0, product_id = 0;

      name = sanei_config_skip_whitespace (name + 3);
      if (*name)
	{
	  name = sanei_config_get_string (name, &vendor);
	  if (vendor != NULL)
	    {
	      vendor_id = (SANE_Int) strtol (vendor, NULL, 0);
	      free (vendor);
	    }
	  name = sanei_config_skip_whitespace (name);
	}
      if (*name)
	{
	  name = sanei_config_get_string (name, &product);
	  if (product != NULL)
	    {
	      product_id = (SANE_Int) strtol (product, NULL, 0);
	      free (product);
	    }
	}
      sanei_usb_find_devices (vendor_id, product_id, attach);
    }
  else if (attach != NULL)
    attach (name);
}

static void
find_endpoints (struct usb_dev *d)
{
  struct libusb_config_descriptor *config = NULL;
  int i;

  d->bulk_in_ep = 0x81;		/* defaults confirmed on the HP 3770 */
  d->bulk_out_ep = 0x02;
  d->int_in_ep = 0x83;
  if (libusb_get_active_config_descriptor (d->device, &config) != LIBUSB_SUCCESS)
    return;

  if (config->bNumInterfaces > 0 && config->interface[0].num_altsetting > 0)
    {
      const struct libusb_interface_descriptor *iface =
	&config->interface[0].altsetting[0];

      d->interface_nr = iface->bInterfaceNumber;
      for (i = 0; i < iface->bNumEndpoints; i++)
	{
	  const struct libusb_endpoint_descriptor *ep = &iface->endpoint[i];
	  int type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
	  int in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;

	  if ((type == LIBUSB_TRANSFER_TYPE_BULK) && in)
	    d->bulk_in_ep = ep->bEndpointAddress;
	  else if ((type == LIBUSB_TRANSFER_TYPE_BULK) && !in)
	    d->bulk_out_ep = ep->bEndpointAddress;
	  else if ((type == LIBUSB_TRANSFER_TYPE_INTERRUPT) && in)
	    d->int_in_ep = ep->bEndpointAddress;
	}
    }
  libusb_free_config_descriptor (config);
}

SANE_Status
sanei_usb_open (SANE_String_Const devname, SANE_Int * dn)
{
  int i, rc;

  for (i = 0; i < device_number; i++)
    if (strcmp (devices[i].devname, devname) == 0)
      break;
  if (i == device_number)
    {
      fprintf (stderr, "hp3770: USB device %s not found\n", devname);
      return SANE_STATUS_INVAL;
    }
  if (devices[i].open)
    return SANE_STATUS_DEVICE_BUSY;

  rc = libusb_open (devices[i].device, &devices[i].handle);
  if (rc != LIBUSB_SUCCESS)
    {
      fprintf (stderr, "hp3770: cannot open %s: %s\n", devname, libusb_error_name (rc));
      return (rc == LIBUSB_ERROR_ACCESS) ? SANE_STATUS_ACCESS_DENIED : SANE_STATUS_IO_ERROR;
    }

  find_endpoints (&devices[i]);

  rc = libusb_claim_interface (devices[i].handle, devices[i].interface_nr);
  if (rc != LIBUSB_SUCCESS)
    {
      fprintf (stderr, "hp3770: cannot claim interface %d of %s: %s\n",
	       devices[i].interface_nr, devname, libusb_error_name (rc));
      libusb_close (devices[i].handle);
      devices[i].handle = NULL;
      return (rc == LIBUSB_ERROR_BUSY) ? SANE_STATUS_DEVICE_BUSY : SANE_STATUS_IO_ERROR;
    }

  devices[i].open = 1;
  *dn = i;
  usb_dbg ("opened %s (bulk in 0x%02x, bulk out 0x%02x)\n", devname,
	   devices[i].bulk_in_ep, devices[i].bulk_out_ep);
  if (trace != NULL)
    {
      fprintf (trace, "# open %s %04x:%04x\n", devname, devices[i].vendor,
	       devices[i].product);
      fflush (trace);
    }
  return SANE_STATUS_GOOD;
}

void
sanei_usb_close (SANE_Int dn)
{
  if ((dn < 0) || (dn >= device_number) || !devices[dn].open)
    return;

  libusb_release_interface (devices[dn].handle, devices[dn].interface_nr);
  libusb_close (devices[dn].handle);
  devices[dn].handle = NULL;
  devices[dn].open = 0;
  if (trace != NULL)
    {
      fprintf (trace, "# close\n");
      fflush (trace);
    }
}

SANE_Status
sanei_usb_get_vendor_product (SANE_Int dn, SANE_Word * vendor, SANE_Word * product)
{
  if ((dn < 0) || (dn >= device_number))
    return SANE_STATUS_INVAL;
  if (vendor != NULL)
    *vendor = devices[dn].vendor;
  if (product != NULL)
    *product = devices[dn].product;
  return SANE_STATUS_GOOD;
}

void
sanei_usb_set_timeout (SANE_Int timeout)
{
  usb_timeout = timeout;
}

SANE_Status
sanei_usb_read_bulk (SANE_Int dn, SANE_Byte * buffer, size_t * size)
{
  int rc, transferred = 0;

  if ((dn < 0) || (dn >= device_number) || !devices[dn].open || (size == NULL))
    return SANE_STATUS_INVAL;

  rc = libusb_bulk_transfer (devices[dn].handle, devices[dn].bulk_in_ep,
			     buffer, (int) *size, &transferred, usb_timeout);
  if (trace != NULL)
    {
      fprintf (trace, "BI %02x len=%lu got=%d rc=%d", devices[dn].bulk_in_ep,
	       (unsigned long) *size, transferred, rc);
      if (transferred > 0)
	trace_bytes (buffer, transferred, 16);
      fputc ('\n', trace);
    }
  if (rc != LIBUSB_SUCCESS)
    {
      fprintf (stderr, "hp3770: bulk read of %lu bytes failed: %s\n",
	       (unsigned long) *size, libusb_error_name (rc));
      libusb_clear_halt (devices[dn].handle, devices[dn].bulk_in_ep);
      *size = 0;
      return SANE_STATUS_IO_ERROR;
    }
  if (transferred == 0)
    {
      *size = 0;
      return SANE_STATUS_EOF;
    }
  *size = (size_t) transferred;
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_usb_write_bulk (SANE_Int dn, const SANE_Byte * buffer, size_t * size)
{
  int rc, transferred = 0;

  if ((dn < 0) || (dn >= device_number) || !devices[dn].open || (size == NULL))
    return SANE_STATUS_INVAL;

  rc = libusb_bulk_transfer (devices[dn].handle, devices[dn].bulk_out_ep,
			     (unsigned char *) buffer, (int) *size,
			     &transferred, usb_timeout);
  if (trace != NULL)
    {
      fprintf (trace, "BO %02x len=%lu sent=%d rc=%d", devices[dn].bulk_out_ep,
	       (unsigned long) *size, transferred, rc);
      trace_bytes (buffer, (int) *size, 16);
      fputc ('\n', trace);
    }
  if (rc != LIBUSB_SUCCESS)
    {
      fprintf (stderr, "hp3770: bulk write of %lu bytes failed: %s\n",
	       (unsigned long) *size, libusb_error_name (rc));
      libusb_clear_halt (devices[dn].handle, devices[dn].bulk_out_ep);
      *size = (size_t) transferred;
      return SANE_STATUS_IO_ERROR;
    }
  *size = (size_t) transferred;
  return SANE_STATUS_GOOD;
}

SANE_Status
sanei_usb_control_msg (SANE_Int dn, SANE_Int rtype, SANE_Int req,
		       SANE_Int value, SANE_Int index, SANE_Int len,
		       SANE_Byte * data)
{
  int rc;
  unsigned char dummy[1] = { 0 };

  if ((dn < 0) || (dn >= device_number) || !devices[dn].open)
    return SANE_STATUS_INVAL;

  rc = libusb_control_transfer (devices[dn].handle, (uint8_t) rtype,
				(uint8_t) req, (uint16_t) value,
				(uint16_t) index,
				(data != NULL) ? data : dummy, (uint16_t) len,
				(unsigned int) usb_timeout);
  if (trace != NULL)
    {
      fprintf (trace, "%s %02x %02x %04x %04x %04x rc=%d",
	       (rtype & 0x80) ? "CI" : "CO", rtype & 0xff, req & 0xff,
	       value & 0xffff, index & 0xffff, len, rc);
      if ((data != NULL) && (len > 0) && (((rtype & 0x80) == 0) || (rc > 0)))
	trace_bytes (data, ((rtype & 0x80) != 0) ? rc : len, 64);
      fputc ('\n', trace);
      fflush (trace);
    }
  if (rc < 0)
    {
      fprintf (stderr, "hp3770: control transfer %02x/%02x value=%04x index=%04x len=%d failed: %s\n",
	       rtype & 0xff, req & 0xff, value & 0xffff, index & 0xffff, len,
	       libusb_error_name (rc));
      return SANE_STATUS_IO_ERROR;
    }
  if (((rtype & 0x80) != 0) && (rc != len))
    {
      fprintf (stderr, "hp3770: short control read (%d of %d bytes)\n", rc, len);
      return SANE_STATUS_IO_ERROR;
    }
  return SANE_STATUS_GOOD;
}
