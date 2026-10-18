/* Minimal sanei_backend.h for the standalone hp3770 build.
   Maps the sane_* entry points to sane_hp3770_* like SANE's dll loader expects
   (see sane-backends include/sane/sanei_backend.h). */
#ifndef HP3770_COMPAT_SANEI_BACKEND_H
#define HP3770_COMPAT_SANEI_BACKEND_H

#include <limits.h>
#include <fcntl.h>
#include "sane/sanei.h"
#include "sane/sanei_debug.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef MM_PER_INCH
#define MM_PER_INCH 25.4
#endif

#define ENTRY(name)     PASTE(PASTE(PASTE(sane_,BACKEND_NAME),_),name)

#ifdef __cplusplus
extern "C" {
#endif
extern SANE_Status ENTRY(init) (SANE_Int *, SANE_Auth_Callback);
extern SANE_Status ENTRY(get_devices) (const SANE_Device ***, SANE_Bool);
extern SANE_Status ENTRY(open) (SANE_String_Const, SANE_Handle *);
extern const SANE_Option_Descriptor *ENTRY(get_option_descriptor) (SANE_Handle, SANE_Int);
extern SANE_Status ENTRY(control_option) (SANE_Handle, SANE_Int, SANE_Action, void *, SANE_Word *);
extern SANE_Status ENTRY(get_parameters) (SANE_Handle, SANE_Parameters *);
extern SANE_Status ENTRY(start) (SANE_Handle);
extern SANE_Status ENTRY(read) (SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *);
extern SANE_Status ENTRY(set_io_mode) (SANE_Handle, SANE_Bool);
extern SANE_Status ENTRY(get_select_fd) (SANE_Handle, SANE_Int *);
extern void ENTRY(cancel) (SANE_Handle);
extern void ENTRY(close) (SANE_Handle);
extern void ENTRY(exit) (void);
#ifdef __cplusplus
}
#endif

#define sane_init(a,b)                  ENTRY(init) (a,b)
#define sane_get_devices(a,b)           ENTRY(get_devices) (a,b)
#define sane_open(a,b)                  ENTRY(open) (a,b)
#define sane_get_option_descriptor(a,b) ENTRY(get_option_descriptor) (a,b)
#define sane_control_option(a,b,c,d,e)  ENTRY(control_option) (a,b,c,d,e)
#define sane_get_parameters(a,b)        ENTRY(get_parameters) (a,b)
#define sane_start(a)                   ENTRY(start) (a)
#define sane_read(a,b,c,d)              ENTRY(read) (a,b,c,d)
#define sane_set_io_mode(a,b)           ENTRY(set_io_mode) (a,b)
#define sane_get_select_fd(a,b)         ENTRY(get_select_fd) (a,b)
#define sane_cancel(a)                  ENTRY(cancel) (a)
#define sane_close(a)                   ENTRY(close) (a)
#define sane_exit(a)                    ENTRY(exit) (a)

#ifndef SANE_I18N
#define SANE_I18N(text) text
#endif

#ifndef SANE_OPTION
typedef union
{
  SANE_Bool b;
  SANE_Word w;
  SANE_Word *wa;
  SANE_String s;
}
Option_Value;
#define SANE_OPTION 1
#endif

#endif
