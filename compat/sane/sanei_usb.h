/* Subset of SANE's sanei_usb.h, implemented on libusb-1.0 by
   compat/sanei_usb_libusb.c (works on macOS without kernel extensions). */
#ifndef HP3770_COMPAT_SANEI_USB_H
#define HP3770_COMPAT_SANEI_USB_H

#include <stddef.h>
#include "sane/sane.h"

extern void sanei_usb_init (void);
extern void sanei_usb_exit (void);
extern SANE_Status sanei_usb_find_devices (SANE_Int vendor, SANE_Int product,
					   SANE_Status (*attach)
					   (SANE_String_Const devname));
extern void sanei_usb_attach_matching_devices (const char *name,
					       SANE_Status (*attach)
					       (SANE_String_Const devname));
extern SANE_Status sanei_usb_open (SANE_String_Const devname, SANE_Int * dn);
extern void sanei_usb_close (SANE_Int dn);
extern SANE_Status sanei_usb_get_vendor_product (SANE_Int dn,
						 SANE_Word * vendor,
						 SANE_Word * product);
extern void sanei_usb_set_timeout (SANE_Int timeout);
extern SANE_Status sanei_usb_read_bulk (SANE_Int dn, SANE_Byte * buffer,
					size_t * size);
extern SANE_Status sanei_usb_write_bulk (SANE_Int dn, const SANE_Byte * buffer,
					 size_t * size);
extern SANE_Status sanei_usb_control_msg (SANE_Int dn, SANE_Int rtype,
					  SANE_Int req, SANE_Int value,
					  SANE_Int index, SANE_Int len,
					  SANE_Byte * data);

/* hp3770 addition: when set (HP3770_USB_TRACE=file), every transfer is
   logged in a simple text format that can be attached to bug reports. */
extern void sanei_usb_trace_note (const char *fmt, ...);

#endif
