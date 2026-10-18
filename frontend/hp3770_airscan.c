/* hp3770-airscan: makes the HP ScanJet 3770 usable from regular Mac apps.

   macOS no longer loads third-party scanner drivers, but Image Capture,
   Preview ("Import from Scanner"), Photos and every app using Apple's
   scanner framework talk to network scanners that speak eSCL ("AirScan")
   without any driver. This program is such a scanner: a small HTTP server
   implementing eSCL, announced with Bonjour, that drives the 3770 through
   the hp3770 backend and delivers JPEG or PDF with the lamp-ripple and
   paper-white corrections of hp3770-scan applied.

   By default it is visible to this Mac only: Bonjour registers the service
   on the local-only interface and the HTTP server answers only connections
   that come from one of this Mac's own addresses. --share makes it a
   network scanner for the whole local network.

   eSCL resources served (rs=eSCL):
     GET    /eSCL/ScannerCapabilities
     GET    /eSCL/ScannerStatus
     POST   /eSCL/ScanJobs                  -> 201, Location: .../ScanJobs/<uuid>
     GET    /eSCL/ScanJobs/<uuid>/NextDocument
     DELETE /eSCL/ScanJobs/<uuid>
     GET    /                               status page

   A scan of the 3770 takes up to a minute and a half (lamp warm-up,
   calibration, then the scan itself), longer than HTTP clients wait for an
   idle connection. NextDocument therefore answers at once with a chunked
   response that starts the file (the JPEG SOI and JFIF header, or the PDF
   header) and sends a JPEG comment segment or a PDF comment line every few
   seconds until the image is ready. Both are valid parts of the file.

   Copyright (C) 2026 hp3770-macos contributors. GPL-2.0-or-later. */

#include "sane/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && !defined(HAVE_DNSSD)
#define HAVE_DNSSD 1
#endif
#if HAVE_DNSSD
#include <dns_sd.h>
#endif

#include "sane/sane.h"
#include "sane/saneopts.h"

#define BACKEND_NAME hp3770
#include "sane/sanei_backend.h"	/* maps sane_xxx to sane_hp3770_xxx */

#define TOOL_VERSION "0.1.0"
#define HP3770_PROG "hp3770-airscan"

#include "hp3770_common.c"
#include "hp3770_jpeg.c"

/* ------------------------------------------------------------- settings */

#define DEFAULT_PORT 8770
#define DEFAULT_NAME "HP ScanJet 3770"
#define DEFAULT_UUID "4a0c3770-0000-1000-8000-03f025050001"
#define KEEPALIVE_SECONDS 2
#define IDLE_CONNECTION_SECONDS 120
#define MAX_JOBS 8
#define READ_CHUNK (256 * 1024)

/* scan area of the 3770 in 1/300 inch (216 x 300 mm) */
#define PLATEN_MAX_WIDTH 2551
#define PLATEN_MAX_HEIGHT 3543
#define PLATEN_MIN_SIZE 16

static const int resolutions[] = { 75, 100, 150, 200, 300, 600, 1200 };
#define N_RESOLUTIONS ((int) (sizeof (resolutions) / sizeof (resolutions[0])))

static struct
{
  int port;
  int share;			/* accept connections from other computers */
  int announce_network;		/* Bonjour on all interfaces */
  int no_bonjour;
  int verbose;
  int quality;
  const char *name;
  const char *uuid;
} opt = { DEFAULT_PORT, 0, 0, 0, 0, 90, DEFAULT_NAME, DEFAULT_UUID };

static volatile sig_atomic_t stop_requested = 0;

static void
on_signal (int sig)
{
  (void) sig;
  stop_requested = 1;
}

static void
logmsg (const char *fmt, ...)
{
  char stamp[32];
  time_t now = time (NULL);
  struct tm tmv;
  va_list ap;

  localtime_r (&now, &tmv);
  strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", &tmv);
  flockfile (stderr);
  fprintf (stderr, "%s ", stamp);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  funlockfile (stderr);
}

/* ----------------------------------------------------------------- jobs */

enum job_state
{ JOB_PENDING, JOB_PROCESSING, JOB_COMPLETED, JOB_CANCELED, JOB_ABORTED };

static const char *job_state_names[] =
  { "Pending", "Processing", "Completed", "Canceled", "Aborted" };

struct job
{
  char uuid[40];
  time_t created;
  enum job_state state;
  int cancel;			/* DELETE received */
  int pdf;			/* application/pdf, else image/jpeg */
  int color;			/* RGB24, else Grayscale8 */
  int resolution;
  double tl_x, tl_y, br_x, br_y;	/* mm */
  int ready;			/* scan finished (successfully or not) */
  int streaming;		/* a NextDocument response is in progress */
  int delivered;		/* the document was sent completely */
  struct membuf jpeg;		/* encoded result */
  int width, height, channels;
  char error[160];
  int in_use;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_changed = PTHREAD_COND_INITIALIZER;
static struct job jobs[MAX_JOBS];
static int scanner_busy = 0;
static pthread_t worker_thread;
static int worker_running = 0;
static unsigned job_counter = 0;

/* lock held */
static struct job *
job_find (const char *uuid, size_t len)
{
  int i;
  for (i = 0; i < MAX_JOBS; i++)
    if (jobs[i].in_use && strlen (jobs[i].uuid) == len && strncmp (jobs[i].uuid, uuid, len) == 0)
      return &jobs[i];
  return NULL;
}

/* lock held: a free slot, recycling the oldest finished job */
static struct job *
job_alloc (void)
{
  struct job *best = NULL;
  int i;

  for (i = 0; i < MAX_JOBS; i++)
    if (!jobs[i].in_use)
      {
	best = &jobs[i];
	break;
      }
  if (best == NULL)
    for (i = 0; i < MAX_JOBS; i++)
      if (jobs[i].ready && !jobs[i].streaming && (best == NULL || jobs[i].created < best->created))
	best = &jobs[i];
  if (best == NULL)
    return NULL;
  free (best->jpeg.data);
  memset (best, 0, sizeof (*best));
  best->in_use = 1;
  return best;
}

static void
make_uuid (char *out, size_t size)
{
  struct timeval tv;
  unsigned long long a, b;

  gettimeofday (&tv, NULL);
  a = ((unsigned long long) tv.tv_sec << 20) ^ (unsigned long long) tv.tv_usec;
  b = ((unsigned long long) getpid () << 32) ^ (unsigned long long) (++job_counter) * 2654435761u
    ^ (unsigned long long) rand ();
  snprintf (out, size, "%08x-%04x-4%03x-%04x-%012llx", (unsigned) (a >> 16) & 0xffffffffu,
	    (unsigned) (a & 0xffff), (unsigned) (b >> 48) & 0xfff,
	    (unsigned) (0x8000 | ((b >> 32) & 0x3fff)), b & 0xffffffffffffULL);
}

/* ------------------------------------------------------------- scanning */

static int
nearest_resolution (int want)
{
  int i, best = resolutions[0];
  for (i = 0; i < N_RESOLUTIONS; i++)
    if (abs (resolutions[i] - want) < abs (best - want))
      best = resolutions[i];
  return best;
}

static int
job_cancelled (struct job *j)
{
  int c;
  pthread_mutex_lock (&lock);
  c = j->cancel || stop_requested;
  pthread_mutex_unlock (&lock);
  return c;
}

/* Runs one scan through the SANE API and encodes the result into j->jpeg.
   Returns 0, 1 on error (j->error set) or 2 if cancelled. */
static int
scan_job (struct job *j)
{
  const SANE_Device **list = NULL;
  SANE_Handle h = NULL;
  SANE_Parameters p;
  SANE_Status st;
  unsigned char *img = NULL;
  size_t cap = 0, total = 0;
  int rc = 1, lines;
  time_t started = time (NULL);

  st = sane_init (NULL, NULL);
  if (st != SANE_STATUS_GOOD)
    {
      snprintf (j->error, sizeof (j->error), "backend initialisation failed: %s", sane_strstatus (st));
      return 1;
    }
  if (sane_get_devices (&list, SANE_TRUE) != SANE_STATUS_GOOD || list == NULL || list[0] == NULL)
    {
      snprintf (j->error, sizeof (j->error), "no HP ScanJet 3770 found (connected and switched on?)");
      goto out_exit;
    }
  st = sane_open (list[0]->name, &h);
  if (st != SANE_STATUS_GOOD)
    {
      snprintf (j->error, sizeof (j->error), "cannot open the scanner: %s", sane_strstatus (st));
      goto out_exit;
    }

  {
    char text[32];
    double max_x = get_number (h, SANE_NAME_SCAN_BR_X, 1);
    double max_y = get_number (h, SANE_NAME_SCAN_BR_Y, 1);

    if (j->br_x > max_x) j->br_x = max_x;
    if (j->br_y > max_y) j->br_y = max_y;
    if (j->tl_x > j->br_x - 1.0) j->tl_x = (j->br_x > 1.0) ? j->br_x - 1.0 : 0.0;
    if (j->tl_y > j->br_y - 1.0) j->tl_y = (j->br_y > 1.0) ? j->br_y - 1.0 : 0.0;

    snprintf (text, sizeof (text), "%d", j->resolution);
    if (set_option_text (h, SANE_NAME_SCAN_MODE,
			 j->color ? SANE_VALUE_SCAN_MODE_COLOR : SANE_VALUE_SCAN_MODE_GRAY,
			 opt.verbose) != 0
	|| set_option_text (h, SANE_NAME_SCAN_RESOLUTION, text, opt.verbose) != 0
	|| set_option_text (h, SANE_NAME_BIT_DEPTH, "8", opt.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_TL_X, j->tl_x, opt.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_TL_Y, j->tl_y, opt.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_BR_X, j->br_x, opt.verbose) != 0
	|| set_number (h, SANE_NAME_SCAN_BR_Y, j->br_y, opt.verbose) != 0)
      {
	snprintf (j->error, sizeof (j->error), "the scanner rejected the scan settings");
	goto out_close;
      }
  }

  if (job_cancelled (j))
    {
      rc = 2;
      goto out_close;
    }
  st = sane_start (h);
  if (st != SANE_STATUS_GOOD)
    {
      snprintf (j->error, sizeof (j->error), "sane_start failed: %s", sane_strstatus (st));
      goto out_close;
    }
  st = sane_get_parameters (h, &p);
  if (st != SANE_STATUS_GOOD || p.bytes_per_line <= 0 || p.depth != 8)
    {
      snprintf (j->error, sizeof (j->error), "unexpected scan parameters");
      sane_cancel (h);
      goto out_close;
    }
  if (opt.verbose)
    logmsg ("  frame: %s, %d x %d px, %d bytes/line", p.format == SANE_FRAME_RGB ? "RGB" : "gray",
	    p.pixels_per_line, p.lines, p.bytes_per_line);

  /* the backend fills at most a third of the request in emulated gray
     mode, so every read asks for a whole READ_CHUNK and the buffer keeps
     that much room free */
  cap = ((p.lines > 0) ? (size_t) p.lines * (size_t) p.bytes_per_line : (size_t) 16 << 20)
    + READ_CHUNK;
  img = malloc (cap);
  if (img == NULL)
    {
      snprintf (j->error, sizeof (j->error), "out of memory");
      sane_cancel (h);
      goto out_close;
    }

  for (;;)
    {
      SANE_Int len = 0, want;

      if (job_cancelled (j))
	{
	  sane_cancel (h);
	  rc = 2;
	  goto out_close;
	}
      if (cap - total < READ_CHUNK)
	{
	  unsigned char *n = realloc (img, cap * 2);
	  if (n == NULL)
	    {
	      snprintf (j->error, sizeof (j->error), "out of memory");
	      sane_cancel (h);
	      goto out_close;
	    }
	  img = n;
	  cap *= 2;
	}
      want = READ_CHUNK;
      st = sane_read (h, img + total, want, &len);
      if (st == SANE_STATUS_EOF)
	break;
      if (st != SANE_STATUS_GOOD)
	{
	  snprintf (j->error, sizeof (j->error), "read error: %s", sane_strstatus (st));
	  sane_cancel (h);
	  goto out_close;
	}
      if (len > 0)
	total += (size_t) len;
    }
  sane_cancel (h);
  sane_close (h);
  h = NULL;
  sane_exit ();

  lines = (int) (total / (size_t) p.bytes_per_line);
  if (lines <= 0)
    {
      snprintf (j->error, sizeof (j->error), "no image data received");
      free (img);
      return 1;
    }

  postprocess_buffer (img, &p, lines, j->resolution, 0, 0, opt.verbose);

  j->channels = (p.format == SANE_FRAME_RGB) ? 3 : 1;
  j->width = p.pixels_per_line;
  j->height = lines;
  if (jpeg_encode (&j->jpeg, img, j->width, j->height, p.bytes_per_line, j->channels,
		   opt.quality, j->resolution) != 0)
    {
      snprintf (j->error, sizeof (j->error), "JPEG encoding failed");
      free (img);
      return 1;
    }
  free (img);
  logmsg ("job %s: scanned %d x %d px at %d dpi in %ld s (%lu KiB JPEG)", j->uuid, j->width,
	  j->height, j->resolution, (long) (time (NULL) - started),
	  (unsigned long) (j->jpeg.len / 1024));
  return 0;

out_close:
  free (img);
  if (h != NULL)
    sane_close (h);
out_exit:
  sane_exit ();
  return rc;
}

static void *
worker_main (void *arg)
{
  struct job *j = arg;
  int rc;

  pthread_mutex_lock (&lock);
  j->state = JOB_PROCESSING;
  pthread_mutex_unlock (&lock);

  logmsg ("job %s: %s %d dpi, %.1f,%.1f - %.1f,%.1f mm, %s", j->uuid,
	  j->color ? "colour" : "gray", j->resolution, j->tl_x, j->tl_y, j->br_x, j->br_y,
	  j->pdf ? "PDF" : "JPEG");
  rc = scan_job (j);

  pthread_mutex_lock (&lock);
  if (rc == 0 && !j->cancel)
    j->state = JOB_COMPLETED;
  else if (rc == 2 || j->cancel)
    {
      j->state = JOB_CANCELED;
      logmsg ("job %s: cancelled", j->uuid);
    }
  else
    {
      j->state = JOB_ABORTED;
      logmsg ("job %s: failed: %s", j->uuid, j->error);
    }
  j->ready = 1;
  scanner_busy = 0;
  pthread_cond_broadcast (&job_changed);
  pthread_mutex_unlock (&lock);
  return NULL;
}

/* ---------------------------------------------------------------- HTTP */

struct conn
{
  int fd;
  char host[256];		/* Host header of the current request */
  char buf[16384];		/* received, not yet consumed */
  size_t have;
};

struct request
{
  char method[16];
  char path[1024];
  char *body;
  size_t body_len;
  int keep_alive;
};

static int
send_all (int fd, const void *data, size_t len)
{
  const char *p = data;
  while (len > 0)
    {
#ifdef MSG_NOSIGNAL
      ssize_t n = send (fd, p, len, MSG_NOSIGNAL);
#else
      ssize_t n = send (fd, p, len, 0);
#endif
      if (n < 0 && errno == EINTR)
	continue;
      if (n <= 0)
	return -1;
      p += n;
      len -= (size_t) n;
    }
  return 0;
}

static int
send_response (struct conn *c, const struct request *r, int code, const char *reason,
	       const char *ctype, const void *body, size_t len, const char *extra)
{
  char head[1024];
  int n = snprintf (head, sizeof (head),
		    "HTTP/1.1 %d %s\r\nServer: hp3770-airscan/" TOOL_VERSION "\r\n"
		    "Cache-Control: no-cache\r\nContent-Length: %lu\r\n%s%s%s%s"
		    "Connection: %s\r\n\r\n",
		    code, reason, (unsigned long) len, ctype ? "Content-Type: " : "",
		    ctype ? ctype : "", ctype ? "\r\n" : "", extra ? extra : "",
		    r->keep_alive ? "keep-alive" : "close");
  if (send_all (c->fd, head, (size_t) n) != 0)
    return -1;
  if (len > 0 && strcmp (r->method, "HEAD") != 0 && send_all (c->fd, body, len) != 0)
    return -1;
  return 0;
}

static int
send_text (struct conn *c, const struct request *r, int code, const char *reason,
	   const char *ctype, const struct membuf *b, const char *extra)
{
  if (b->failed)
    return send_response (c, r, 500, "Internal Server Error", NULL, NULL, 0, NULL);
  return send_response (c, r, code, reason, ctype, b->data, b->len, extra);
}

static int
send_chunk (int fd, const void *data, size_t len)
{
  char head[32];
  int n = snprintf (head, sizeof (head), "%lx\r\n", (unsigned long) len);
  if (len == 0)
    return 0;
  if (send_all (fd, head, (size_t) n) != 0 || send_all (fd, data, len) != 0
      || send_all (fd, "\r\n", 2) != 0)
    return -1;
  return 0;
}

/* Reads more data into c->buf. Returns bytes read, 0 on EOF/timeout/error. */
static ssize_t
conn_fill (struct conn *c)
{
  ssize_t n;
  /* one byte stays free for the terminating NUL of read_request */
  if (c->have >= sizeof (c->buf) - 1)
    return 0;
  do
    n = recv (c->fd, c->buf + c->have, sizeof (c->buf) - 1 - c->have, 0);
  while (n < 0 && errno == EINTR && !stop_requested);
  if (n <= 0)
    return 0;
  c->have += (size_t) n;
  return n;
}

static void
conn_consume (struct conn *c, size_t n)
{
  memmove (c->buf, c->buf + n, c->have - n);
  c->have -= n;
}

/* case-insensitive header lookup in the header block */
static int
header_value (const char *headers, const char *name, char *out, size_t size)
{
  size_t nl = strlen (name);
  const char *p = headers;

  while ((p = strchr (p, '\n')) != NULL)
    {
      p++;
      if (strncasecmp (p, name, nl) == 0 && p[nl] == ':')
	{
	  const char *v = p + nl + 1, *e;
	  size_t len;
	  while (*v == ' ' || *v == '\t')
	    v++;
	  e = v;
	  while (*e && *e != '\r' && *e != '\n')
	    e++;
	  len = (size_t) (e - v);
	  if (len >= size)
	    len = size - 1;
	  memcpy (out, v, len);
	  out[len] = '\0';
	  return 1;
	}
    }
  return 0;
}

/* Reads one request. Returns 0, -1 to close the connection. The body is
   malloc'ed. */
static int
read_request (struct conn *c, struct request *r)
{
  char *end, *headers, value[256], version[16];
  size_t head_len;
  int chunked = 0;

  memset (r, 0, sizeof (*r));
  for (;;)
    {
      c->buf[c->have] = '\0';
      end = (c->have > 0) ? strstr (c->buf, "\r\n\r\n") : NULL;
      if (end != NULL)
	break;
      if (c->have >= sizeof (c->buf) - 1 || conn_fill (c) == 0)
	return -1;
    }
  head_len = (size_t) (end - c->buf) + 4;
  headers = malloc (head_len + 1);
  if (headers == NULL)
    return -1;
  memcpy (headers, c->buf, head_len);
  headers[head_len] = '\0';
  conn_consume (c, head_len);

  if (sscanf (headers, "%15s %1023s %15s", r->method, r->path, version) != 3)
    {
      free (headers);
      return -1;
    }
  r->keep_alive = strcmp (version, "HTTP/1.0") != 0;
  if (header_value (headers, "Connection", value, sizeof (value)))
    {
      if (strcasecmp (value, "close") == 0)
	r->keep_alive = 0;
      else if (strcasecmp (value, "keep-alive") == 0)
	r->keep_alive = 1;
    }
  if (!header_value (headers, "Host", c->host, sizeof (c->host)))
    c->host[0] = '\0';
  if (header_value (headers, "Transfer-Encoding", value, sizeof (value))
      && strncasecmp (value, "chunked", 7) == 0)
    chunked = 1;
  if (header_value (headers, "Expect", value, sizeof (value))
      && strcasecmp (value, "100-continue") == 0)
    send_all (c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

  if (header_value (headers, "Content-Length", value, sizeof (value)) && !chunked)
    {
      long len = strtol (value, NULL, 10);
      if (len < 0 || len > (1 << 20))
	{
	  free (headers);
	  return -1;
	}
      r->body = malloc ((size_t) len + 1);
      if (r->body == NULL)
	{
	  free (headers);
	  return -1;
	}
      while (c->have < (size_t) len)
	{
	  size_t take = c->have;
	  memcpy (r->body + r->body_len, c->buf, take);
	  r->body_len += take;
	  len -= (long) take;
	  conn_consume (c, take);
	  if (conn_fill (c) == 0)
	    {
	      free (headers);
	      free (r->body);
	      r->body = NULL;
	      return -1;
	    }
	}
      memcpy (r->body + r->body_len, c->buf, (size_t) len);
      r->body_len += (size_t) len;
      conn_consume (c, (size_t) len);
      r->body[r->body_len] = '\0';
    }
  else if (chunked)
    {
      size_t cap = 4096;
      r->body = malloc (cap);
      for (;;)
	{
	  char *eol;
	  unsigned long size;
	  if (r->body == NULL)
	    break;
	  c->buf[c->have] = '\0';
	  while ((eol = strstr (c->buf, "\r\n")) == NULL)
	    {
	      if (conn_fill (c) == 0)
		goto chunk_fail;
	      c->buf[c->have] = '\0';
	    }
	  size = strtoul (c->buf, NULL, 16);
	  conn_consume (c, (size_t) (eol - c->buf) + 2);
	  if (size > (1 << 20) || r->body_len + size > (1 << 20))
	    goto chunk_fail;
	  while (c->have < size + 2)
	    if (conn_fill (c) == 0)
	      goto chunk_fail;
	  if (size == 0)
	    {
	      /* no trailers expected: consume the final CRLF */
	      conn_consume (c, 2);
	      break;
	    }
	  if (r->body_len + size + 1 > cap)
	    {
	      char *n;
	      while (r->body_len + size + 1 > cap)
		cap *= 2;
	      n = realloc (r->body, cap);
	      if (n == NULL)
		goto chunk_fail;
	      r->body = n;
	    }
	  memcpy (r->body + r->body_len, c->buf, size);
	  r->body_len += size;
	  conn_consume (c, size + 2);
	}
      if (r->body != NULL)
	r->body[r->body_len] = '\0';
      if (0)
	{
	chunk_fail:
	  free (r->body);
	  free (headers);
	  r->body = NULL;
	  return -1;
	}
    }
  free (headers);
  return 0;
}

/* ------------------------------------------------------------- XML */

/* Text of the first element with local name NAME (namespace prefix ignored),
   copied into out. Returns 1 if found. */
static int
xml_find (const char *xml, const char *name, char *out, size_t size)
{
  const char *p = xml;
  size_t nl = strlen (name);

  if (xml == NULL)
    return 0;
  while ((p = strchr (p, '<')) != NULL)
    {
      const char *n = ++p, *colon, *e;
      if (*n == '/' || *n == '?' || *n == '!')
	continue;
      e = n;
      while (*e && *e != '>' && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n' && *e != '/')
	e++;
      colon = memchr (n, ':', (size_t) (e - n));
      if (colon != NULL)
	n = colon + 1;
      if ((size_t) (e - n) == nl && strncmp (n, name, nl) == 0)
	{
	  const char *v = strchr (e, '>'), *ve;
	  size_t len;
	  if (v == NULL || v[-1] == '/')
	    return 0;
	  v++;
	  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
	    v++;
	  ve = strchr (v, '<');
	  if (ve == NULL)
	    return 0;
	  while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n'))
	    ve--;
	  len = (size_t) (ve - v);
	  if (len >= size)
	    len = size - 1;
	  memcpy (out, v, len);
	  out[len] = '\0';
	  return 1;
	}
    }
  return 0;
}

static const char *xml_head =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
#define ESCL_NS "xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" " \
  "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\""

static void
capabilities_xml (struct membuf *b, const char *host)
{
  int i, k;
  static const char *color_modes[] = { "RGB24", "Grayscale8" };
  static const char *formats[] = { "image/jpeg", "application/pdf" };

  mb_printf (b, "%s<scan:ScannerCapabilities " ESCL_NS ">\n", xml_head);
  mb_printf (b, "  <pwg:Version>2.63</pwg:Version>\n");
  mb_printf (b, "  <pwg:MakeAndModel>%s</pwg:MakeAndModel>\n", opt.name);
  mb_printf (b, "  <pwg:SerialNumber>3770-0001</pwg:SerialNumber>\n");
  mb_printf (b, "  <scan:Manufacturer>HP</scan:Manufacturer>\n");
  mb_printf (b, "  <scan:UUID>%s</scan:UUID>\n", opt.uuid);
  if (host != NULL && host[0] != '\0')
    mb_printf (b, "  <scan:AdminURI>http://%s/</scan:AdminURI>\n", host);
  mb_printf (b, "  <scan:Platen>\n    <scan:PlatenInputCaps>\n");
  mb_printf (b, "      <scan:MinWidth>%d</scan:MinWidth>\n", PLATEN_MIN_SIZE);
  mb_printf (b, "      <scan:MaxWidth>%d</scan:MaxWidth>\n", PLATEN_MAX_WIDTH);
  mb_printf (b, "      <scan:MinHeight>%d</scan:MinHeight>\n", PLATEN_MIN_SIZE);
  mb_printf (b, "      <scan:MaxHeight>%d</scan:MaxHeight>\n", PLATEN_MAX_HEIGHT);
  mb_printf (b, "      <scan:MaxScanRegions>1</scan:MaxScanRegions>\n");
  mb_printf (b, "      <scan:SettingProfiles>\n        <scan:SettingProfile>\n");
  mb_printf (b, "          <scan:ColorModes>\n");
  for (i = 0; i < 2; i++)
    mb_printf (b, "            <scan:ColorMode>%s</scan:ColorMode>\n", color_modes[i]);
  mb_printf (b, "          </scan:ColorModes>\n          <scan:DocumentFormats>\n");
  for (i = 0; i < 2; i++)
    mb_printf (b, "            <pwg:DocumentFormat>%s</pwg:DocumentFormat>\n"
	       "            <scan:DocumentFormatExt>%s</scan:DocumentFormatExt>\n",
	       formats[i], formats[i]);
  mb_printf (b, "          </scan:DocumentFormats>\n");
  mb_printf (b, "          <scan:SupportedResolutions>\n            <scan:DiscreteResolutions>\n");
  for (k = 0; k < N_RESOLUTIONS; k++)
    mb_printf (b, "              <scan:DiscreteResolution><scan:XResolution>%d</scan:XResolution>"
	       "<scan:YResolution>%d</scan:YResolution></scan:DiscreteResolution>\n",
	       resolutions[k], resolutions[k]);
  mb_printf (b, "            </scan:DiscreteResolutions>\n          </scan:SupportedResolutions>\n");
  mb_printf (b, "          <scan:ColorSpaces><scan:ColorSpace>sRGB</scan:ColorSpace></scan:ColorSpaces>\n");
  mb_printf (b, "        </scan:SettingProfile>\n      </scan:SettingProfiles>\n");
  mb_printf (b, "      <scan:SupportedIntents>\n");
  {
    static const char *intents[] = { "Document", "TextAndGraphic", "Photo", "Preview" };
    for (i = 0; i < 4; i++)
      mb_printf (b, "        <scan:Intent>%s</scan:Intent>\n", intents[i]);
  }
  mb_printf (b, "      </scan:SupportedIntents>\n");
  mb_printf (b, "      <scan:MaxOpticalXResolution>1200</scan:MaxOpticalXResolution>\n");
  mb_printf (b, "      <scan:MaxOpticalYResolution>1200</scan:MaxOpticalYResolution>\n");
  mb_printf (b, "      <scan:RiskyLeftMargin>0</scan:RiskyLeftMargin>\n");
  mb_printf (b, "      <scan:RiskyRightMargin>0</scan:RiskyRightMargin>\n");
  mb_printf (b, "      <scan:RiskyTopMargin>0</scan:RiskyTopMargin>\n");
  mb_printf (b, "      <scan:RiskyBottomMargin>0</scan:RiskyBottomMargin>\n");
  mb_printf (b, "    </scan:PlatenInputCaps>\n  </scan:Platen>\n");
  mb_printf (b, "</scan:ScannerCapabilities>\n");
}

/* lock held */
static void
status_xml (struct membuf *b)
{
  time_t now = time (NULL);
  int i;

  mb_printf (b, "%s<scan:ScannerStatus " ESCL_NS ">\n", xml_head);
  mb_printf (b, "  <pwg:Version>2.63</pwg:Version>\n");
  mb_printf (b, "  <pwg:State>%s</pwg:State>\n", scanner_busy ? "Processing" : "Idle");
  mb_printf (b, "  <scan:Jobs>\n");
  for (i = 0; i < MAX_JOBS; i++)
    {
      struct job *j = &jobs[i];
      int done;
      if (!j->in_use)
	continue;
      done = j->delivered ? 1 : 0;
      mb_printf (b, "    <scan:JobInfo>\n");
      mb_printf (b, "      <pwg:JobUri>/eSCL/ScanJobs/%s</pwg:JobUri>\n", j->uuid);
      mb_printf (b, "      <pwg:JobUuid>%s</pwg:JobUuid>\n", j->uuid);
      mb_printf (b, "      <scan:Age>%ld</scan:Age>\n", (long) (now - j->created));
      mb_printf (b, "      <pwg:ImagesCompleted>%d</pwg:ImagesCompleted>\n", done);
      mb_printf (b, "      <pwg:ImagesToTransfer>%d</pwg:ImagesToTransfer>\n",
		 (j->state == JOB_PENDING || j->state == JOB_PROCESSING
		  || (j->state == JOB_COMPLETED && !j->delivered)) ? 1 : 0);
      mb_printf (b, "      <pwg:JobState>%s</pwg:JobState>\n",
		 (j->state == JOB_COMPLETED && !j->delivered) ? "Processing"
		 : job_state_names[j->state]);
      mb_printf (b, "      <pwg:JobStateReasons><pwg:JobStateReason>%s</pwg:JobStateReason>"
		 "</pwg:JobStateReasons>\n",
		 (j->state == JOB_PROCESSING || j->state == JOB_PENDING) ? "JobScanning"
		 : (j->state == JOB_COMPLETED && !j->delivered) ? "JobScanningAndTransferring"
		 : (j->state == JOB_COMPLETED) ? "JobCompletedSuccessfully"
		 : (j->state == JOB_CANCELED) ? "JobCanceledByUser" : "AbortedBySystem");
      mb_printf (b, "    </scan:JobInfo>\n");
    }
  mb_printf (b, "  </scan:Jobs>\n</scan:ScannerStatus>\n");
}

/* ---------------------------------------------------------- handlers */

static int
handle_scan_jobs_post (struct conn *c, struct request *r)
{
  char v[128], location[512], extra[600];
  struct job *j;
  double x = 0, y = 0, w = PLATEN_MAX_WIDTH, h = PLATEN_MAX_HEIGHT;
  int res = 300, color = 1, pdf = 0;
  struct membuf empty = { 0 };

  if (xml_find (r->body, "XResolution", v, sizeof (v)))
    res = atoi (v);
  if (xml_find (r->body, "ColorMode", v, sizeof (v)))
    {
      if (strncmp (v, "Grayscale", 9) == 0 || strncmp (v, "BlackAndWhite", 13) == 0)
	color = 0;
    }
  if (xml_find (r->body, "DocumentFormatExt", v, sizeof (v))
      || xml_find (r->body, "DocumentFormat", v, sizeof (v)))
    {
      if (strcmp (v, "application/pdf") == 0)
	pdf = 1;
      else if (strcmp (v, "image/jpeg") != 0 && strcmp (v, "application/octet-stream") != 0)
	{
	  logmsg ("scan request for unsupported format %s", v);
	  return send_text (c, r, 409, "Conflict", NULL, &empty, NULL);
	}
    }
  if (xml_find (r->body, "InputSource", v, sizeof (v)) && strcmp (v, "Platen") != 0)
    {
      logmsg ("scan request for unsupported input source %s", v);
      return send_text (c, r, 409, "Conflict", NULL, &empty, NULL);
    }
  if (xml_find (r->body, "XOffset", v, sizeof (v)))
    x = atof (v);
  if (xml_find (r->body, "YOffset", v, sizeof (v)))
    y = atof (v);
  if (xml_find (r->body, "Width", v, sizeof (v)))
    w = atof (v);
  if (xml_find (r->body, "Height", v, sizeof (v)))
    h = atof (v);
  if (xml_find (r->body, "ContentRegionUnits", v, sizeof (v))
      && strstr (v, "ThreeHundredthsOfInches") == NULL)
    logmsg ("warning: region units %s, assuming 1/300 inch", v);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (w < PLATEN_MIN_SIZE) w = PLATEN_MIN_SIZE;
  if (h < PLATEN_MIN_SIZE) h = PLATEN_MIN_SIZE;

  pthread_mutex_lock (&lock);
  if (scanner_busy || stop_requested)
    {
      pthread_mutex_unlock (&lock);
      return send_text (c, r, 503, "Service Unavailable", NULL, &empty, "Retry-After: 10\r\n");
    }
  j = job_alloc ();
  if (j == NULL)
    {
      pthread_mutex_unlock (&lock);
      return send_text (c, r, 503, "Service Unavailable", NULL, &empty, "Retry-After: 10\r\n");
    }
  make_uuid (j->uuid, sizeof (j->uuid));
  j->created = time (NULL);
  j->state = JOB_PENDING;
  j->pdf = pdf;
  j->color = color;
  j->resolution = nearest_resolution (res);
  j->tl_x = x * 25.4 / 300.0;
  j->tl_y = y * 25.4 / 300.0;
  j->br_x = (x + w) * 25.4 / 300.0;
  j->br_y = (y + h) * 25.4 / 300.0;
  if (worker_running)
    pthread_join (worker_thread, NULL);
  worker_running = 0;
  if (pthread_create (&worker_thread, NULL, worker_main, j) != 0)
    {
      j->in_use = 0;
      pthread_mutex_unlock (&lock);
      return send_text (c, r, 500, "Internal Server Error", NULL, &empty, NULL);
    }
  worker_running = 1;
  scanner_busy = 1;
  snprintf (location, sizeof (location), "%s%s/eSCL/ScanJobs/%s", c->host[0] ? "http://" : "",
	    c->host, j->uuid);
  pthread_mutex_unlock (&lock);

  snprintf (extra, sizeof (extra), "Location: %s\r\n", location);
  return send_text (c, r, 201, "Created", NULL, &empty, extra);
}

/* NextDocument: the single page of the job, streamed while it is scanned */
static int
handle_next_document (struct conn *c, struct request *r, struct job *j)
{
  struct membuf head = { 0 }, tail = { 0 }, empty = { 0 };
  size_t sent = 0;
  int pdf, rc = -1, streaming = 0, failed;

  /* lock held on entry */
  if (j->delivered || j->state == JOB_CANCELED || j->streaming)
    {
      int busy = j->streaming;
      pthread_mutex_unlock (&lock);
      if (busy)
	return send_text (c, r, 503, "Service Unavailable", NULL, &empty, "Retry-After: 5\r\n");
      return send_text (c, r, 404, "Not Found", NULL, &empty, NULL);
    }
  if (j->state == JOB_ABORTED)
    {
      pthread_mutex_unlock (&lock);
      return send_text (c, r, 500, "Internal Server Error", NULL, &empty, NULL);
    }
  j->streaming = 1;
  pdf = j->pdf;

  if (!j->ready)
    {
      /* start the file now, then keep the connection alive */
      int dpi = j->resolution;
      const char *ctype = pdf ? "application/pdf" : "image/jpeg";
      char hdr[512];
      int n;

      pthread_mutex_unlock (&lock);
      n = snprintf (hdr, sizeof (hdr),
		    "HTTP/1.1 200 OK\r\nServer: hp3770-airscan/" TOOL_VERSION "\r\n"
		    "Cache-Control: no-cache\r\nContent-Type: %s\r\n"
		    "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
		    ctype, r->keep_alive ? "keep-alive" : "close");
      if (pdf)
	pdf_header (&head);
      else
	jpeg_jfif_header (&head, dpi);
      streaming = 1;
      if (send_all (c->fd, hdr, (size_t) n) != 0 || send_chunk (c->fd, head.data, head.len) != 0)
	goto out_lock;
      sent = head.len;

      pthread_mutex_lock (&lock);
      while (!j->ready)
	{
	  struct timespec ts;
	  struct membuf ka = { 0 };
	  int wait_rc;

	  clock_gettime (CLOCK_REALTIME, &ts);
	  ts.tv_sec += KEEPALIVE_SECONDS;
	  wait_rc = pthread_cond_timedwait (&job_changed, &lock, &ts);
	  if (j->ready)
	    break;
	  if (wait_rc != ETIMEDOUT)
	    continue;
	  pthread_mutex_unlock (&lock);
	  if (pdf)
	    mb_printf (&ka, "%% scanning\n");
	  else
	    jpeg_comment (&ka, "hp3770: scanning");
	  if (ka.failed || send_chunk (c->fd, ka.data, ka.len) != 0)
	    {
	      free (ka.data);
	      logmsg ("job %s: client disconnected while waiting", j->uuid);
	      goto out_lock;
	    }
	  sent += ka.len;
	  free (ka.data);
	  pthread_mutex_lock (&lock);
	}
    }
  else
    {
      /* finished already: a plain response with Content-Length */
      if (pdf)
	pdf_header (&head);
      else
	jpeg_jfif_header (&head, j->resolution);
    }

  /* lock held, job ready */
  failed = (j->state != JOB_COMPLETED);
  pthread_mutex_unlock (&lock);

  if (failed)
    {
      if (streaming)
	{
	  /* the response already started: an incomplete chunked body tells
	     the client that the transfer failed */
	  rc = -1;
	  goto out_lock;
	}
      rc = send_text (c, r, 500, "Internal Server Error", NULL, &empty, NULL);
      pthread_mutex_lock (&lock);
      goto out;
    }

  /* the rest of the file; j->jpeg is not changed after ready is set */
  if (j->jpeg.len < JPEG_JFIF_HEADER_LEN)
    goto out_lock;
  if (pdf)
    {
      pdf_wrap_jpeg (&tail, &j->jpeg, j->width, j->height, j->channels, j->resolution,
		     streaming ? sent : 0);
    }
  else
    {
      if (!streaming)
	mb_put (&tail, head.data, head.len);
      mb_put (&tail, j->jpeg.data + JPEG_JFIF_HEADER_LEN, j->jpeg.len - JPEG_JFIF_HEADER_LEN);
    }
  if (tail.failed)
    goto out_lock;

  if (streaming)
    rc = (strcmp (r->method, "HEAD") == 0) ? 0
      : (send_chunk (c->fd, tail.data, tail.len) == 0 && send_all (c->fd, "0\r\n\r\n", 5) == 0)
      ? 0 : -1;
  else
    rc = send_response (c, r, 200, "OK", pdf ? "application/pdf" : "image/jpeg", tail.data,
			tail.len, NULL);

  pthread_mutex_lock (&lock);
  if (rc == 0)
    {
      j->delivered = 1;
      logmsg ("job %s: delivered %lu KiB %s", j->uuid, (unsigned long) ((sent + tail.len) / 1024),
	      pdf ? "PDF" : "JPEG");
      /* the image data is no longer needed */
      free (j->jpeg.data);
      memset (&j->jpeg, 0, sizeof (j->jpeg));
    }
  goto out;

out_lock:
  pthread_mutex_lock (&lock);
out:
  j->streaming = 0;
  pthread_cond_broadcast (&job_changed);
  pthread_mutex_unlock (&lock);
  free (head.data);
  free (tail.data);
  return rc;
}

static int
handle_status_page (struct conn *c, struct request *r)
{
  struct membuf b = { 0 };
  int i, rc;

  mb_printf (&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title>"
	     "<meta http-equiv=\"refresh\" content=\"5\"><style>body{font:15px -apple-system,"
	     "sans-serif;margin:2em;max-width:42em}td{padding:2px 10px}</style></head><body>",
	     opt.name);
  mb_printf (&b, "<h2>%s</h2><p>hp3770-airscan %s &mdash; eSCL (AirScan) server. "
	     "Use the scanner from Image Capture, Preview (File &rsaquo; Import from Scanner) "
	     "or any other Mac app that scans.</p>", opt.name, TOOL_VERSION);
  pthread_mutex_lock (&lock);
  mb_printf (&b, "<p>Status: <b>%s</b></p><table><tr><th>Job</th><th>State</th><th>Settings</th>"
	     "<th>Age</th></tr>", scanner_busy ? "scanning" : "idle");
  for (i = 0; i < MAX_JOBS; i++)
    if (jobs[i].in_use)
      mb_printf (&b, "<tr><td>%.8s</td><td>%s%s</td><td>%s, %d dpi, %s</td><td>%ld s</td></tr>",
		 jobs[i].uuid, job_state_names[jobs[i].state],
		 jobs[i].delivered ? " (delivered)" : "", jobs[i].color ? "colour" : "gray",
		 jobs[i].resolution, jobs[i].pdf ? "PDF" : "JPEG",
		 (long) (time (NULL) - jobs[i].created));
  pthread_mutex_unlock (&lock);
  mb_printf (&b, "</table></body></html>\n");
  rc = send_text (c, r, 200, "OK", "text/html; charset=utf-8", &b, NULL);
  free (b.data);
  return rc;
}

static int
handle_request (struct conn *c, struct request *r)
{
  struct membuf b = { 0 }, empty = { 0 };
  const char *path = r->path;
  int rc, get = strcmp (r->method, "GET") == 0 || strcmp (r->method, "HEAD") == 0;
  char *q = strchr (r->path, '?');

  if (q != NULL)
    *q = '\0';
  if (opt.verbose)
    logmsg ("%s %s", r->method, r->path);

  if (strncmp (path, "/eSCL", 5) == 0)
    path += 5;

  if (get && strcmp (path, "/ScannerCapabilities") == 0)
    {
      capabilities_xml (&b, c->host);
      rc = send_text (c, r, 200, "OK", "text/xml", &b, NULL);
    }
  else if (get && strcmp (path, "/ScannerStatus") == 0)
    {
      pthread_mutex_lock (&lock);
      status_xml (&b);
      pthread_mutex_unlock (&lock);
      rc = send_text (c, r, 200, "OK", "text/xml", &b, NULL);
    }
  else if (strcmp (r->method, "POST") == 0 && strcmp (path, "/ScanJobs") == 0)
    rc = handle_scan_jobs_post (c, r);
  else if (strncmp (path, "/ScanJobs/", 10) == 0)
    {
      const char *id = path + 10, *slash = strchr (id, '/');
      size_t idlen = slash ? (size_t) (slash - id) : strlen (id);
      const char *sub = slash ? slash : "";
      struct job *j;

      pthread_mutex_lock (&lock);
      j = job_find (id, idlen);
      if (j == NULL)
	{
	  pthread_mutex_unlock (&lock);
	  rc = send_text (c, r, 404, "Not Found", NULL, &empty, NULL);
	}
      else if (get && strcmp (sub, "/NextDocument") == 0)
	rc = handle_next_document (c, r, j);	/* unlocks */
      else if (strcmp (r->method, "DELETE") == 0 && sub[0] == '\0')
	{
	  if (!j->ready)
	    {
	      j->cancel = 1;
	      logmsg ("job %s: cancel requested", j->uuid);
	    }
	  else if (!j->delivered && j->state == JOB_COMPLETED)
	    {
	      /* finished but not fetched: drop it */
	      free (j->jpeg.data);
	      memset (&j->jpeg, 0, sizeof (j->jpeg));
	      if (!j->streaming)
		j->state = JOB_CANCELED;
	    }
	  pthread_cond_broadcast (&job_changed);
	  pthread_mutex_unlock (&lock);
	  rc = send_text (c, r, 200, "OK", NULL, &empty, NULL);
	}
      else
	{
	  pthread_mutex_unlock (&lock);
	  rc = send_text (c, r, 404, "Not Found", NULL, &empty, NULL);
	}
    }
  else if (get && (strcmp (r->path, "/") == 0 || strcmp (r->path, "/index.html") == 0))
    rc = handle_status_page (c, r);
  else
    rc = send_text (c, r, 404, "Not Found", NULL, &empty, NULL);

  free (b.data);
  return rc;
}

/* --------------------------------------------------- access control */

static int
is_loopback (const struct sockaddr_storage *a)
{
  if (a->ss_family == AF_INET)
    return (ntohl (((const struct sockaddr_in *) a)->sin_addr.s_addr) >> 24) == 127;
  if (a->ss_family == AF_INET6)
    {
      const struct in6_addr *v6 = &((const struct sockaddr_in6 *) a)->sin6_addr;
      if (IN6_IS_ADDR_LOOPBACK (v6))
	return 1;
      if (IN6_IS_ADDR_V4MAPPED (v6))
	return v6->s6_addr[12] == 127;
    }
  return 0;
}

static int
same_address (const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
  if (a->ss_family != b->ss_family)
    return 0;
  if (a->ss_family == AF_INET)
    return ((const struct sockaddr_in *) a)->sin_addr.s_addr
      == ((const struct sockaddr_in *) b)->sin_addr.s_addr;
  if (a->ss_family == AF_INET6)
    return memcmp (&((const struct sockaddr_in6 *) a)->sin6_addr,
		   &((const struct sockaddr_in6 *) b)->sin6_addr, sizeof (struct in6_addr)) == 0;
  return 0;
}

static void
address_text (const struct sockaddr_storage *a, char *out, size_t size)
{
  if (a->ss_family == AF_INET)
    inet_ntop (AF_INET, &((const struct sockaddr_in *) a)->sin_addr, out, (socklen_t) size);
  else if (a->ss_family == AF_INET6)
    inet_ntop (AF_INET6, &((const struct sockaddr_in6 *) a)->sin6_addr, out, (socklen_t) size);
  else
    snprintf (out, size, "?");
}

/* A connection from this computer arrives from loopback or from the very
   address it was made to (the Mac's own LAN address). */
static int
peer_allowed (int fd, char *peer_text, size_t size)
{
  struct sockaddr_storage peer, self;
  socklen_t pl = sizeof (peer), sl = sizeof (self);

  peer_text[0] = '\0';
  if (getpeername (fd, (struct sockaddr *) &peer, &pl) != 0
      || getsockname (fd, (struct sockaddr *) &self, &sl) != 0)
    return 0;
  address_text (&peer, peer_text, size);
  if (opt.share)
    return 1;
  return is_loopback (&peer) || same_address (&peer, &self);
}

static void *
connection_main (void *arg)
{
  struct conn *c = arg;
  struct request r;
  char peer[64];
  struct timeval tv = { IDLE_CONNECTION_SECONDS, 0 };
  int one = 1;

  setsockopt (c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
  setsockopt (c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one));
#ifdef SO_NOSIGPIPE
  setsockopt (c->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
#endif

  if (!peer_allowed (c->fd, peer, sizeof (peer)))
    {
      static const char deny[] =
	"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      logmsg ("refused connection from %s (start with --share to allow other computers)", peer);
      send_all (c->fd, deny, sizeof (deny) - 1);
      close (c->fd);
      free (c);
      return NULL;
    }

  while (!stop_requested && read_request (c, &r) == 0)
    {
      int rc = handle_request (c, &r);
      free (r.body);
      if (rc != 0 || !r.keep_alive)
	break;
    }
  close (c->fd);
  free (c);
  return NULL;
}

/* --------------------------------------------------------- Bonjour */

#if HAVE_DNSSD
static void DNSSD_API
on_registered (DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err,
	       const char *name, const char *type, const char *domain, void *ctx)
{
  (void) ref; (void) flags; (void) ctx;
  if (err == kDNSServiceErr_NoError)
    logmsg ("Bonjour: announced \"%s\" as %s%s", name, type, domain);
  else
    logmsg ("Bonjour: registration failed (error %d)", (int) err);
}

static void
txt_add (unsigned char *txt, size_t *len, size_t cap, const char *key, const char *value)
{
  size_t kl = strlen (key), vl = strlen (value), n = kl + 1 + vl;
  if (n > 255 || *len + 1 + n > cap)
    return;
  txt[(*len)++] = (unsigned char) n;
  memcpy (txt + *len, key, kl);
  *len += kl;
  txt[(*len)++] = '=';
  memcpy (txt + *len, value, vl);
  *len += vl;
}

static DNSServiceRef
bonjour_register (void)
{
  DNSServiceRef ref = NULL;
  unsigned char txt[1024];
  size_t len = 0;
  char admin[64];
  DNSServiceErrorType err;
  uint32_t iface = opt.announce_network ? kDNSServiceInterfaceIndexAny
    : kDNSServiceInterfaceIndexLocalOnly;

  snprintf (admin, sizeof (admin), "http://localhost:%d/", opt.port);
  txt_add (txt, &len, sizeof (txt), "txtvers", "1");
  txt_add (txt, &len, sizeof (txt), "ty", opt.name);
  txt_add (txt, &len, sizeof (txt), "note", "USB");
  txt_add (txt, &len, sizeof (txt), "adminurl", admin);
  txt_add (txt, &len, sizeof (txt), "pdl", "image/jpeg,application/pdf");
  txt_add (txt, &len, sizeof (txt), "UUID", opt.uuid);
  txt_add (txt, &len, sizeof (txt), "cs", "color,grayscale");
  txt_add (txt, &len, sizeof (txt), "is", "platen");
  txt_add (txt, &len, sizeof (txt), "duplex", "F");
  txt_add (txt, &len, sizeof (txt), "rs", "eSCL");
  txt_add (txt, &len, sizeof (txt), "vers", "2.63");

  err = DNSServiceRegister (&ref, 0, iface, opt.name, "_uscan._tcp", NULL, NULL,
			    htons ((uint16_t) opt.port), (uint16_t) len, txt, on_registered, NULL);
  if (err != kDNSServiceErr_NoError)
    {
      logmsg ("Bonjour: DNSServiceRegister failed (error %d); apps will not find the scanner",
	      (int) err);
      return NULL;
    }
  return ref;
}
#endif

/* ------------------------------------------------------------ main */

static int
open_listener (int port)
{
  int fd, one = 1, zero = 0;
  struct sockaddr_in6 a6;
  struct sockaddr_in a4;

  fd = socket (AF_INET6, SOCK_STREAM, 0);
  if (fd >= 0)
    {
      setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
      setsockopt (fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof (zero));
      memset (&a6, 0, sizeof (a6));
      a6.sin6_family = AF_INET6;
      a6.sin6_addr = in6addr_any;
      a6.sin6_port = htons ((uint16_t) port);
      if (bind (fd, (struct sockaddr *) &a6, sizeof (a6)) == 0 && listen (fd, 16) == 0)
	return fd;
      close (fd);
    }
  fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
  memset (&a4, 0, sizeof (a4));
  a4.sin_family = AF_INET;
  a4.sin_addr.s_addr = htonl (INADDR_ANY);
  a4.sin_port = htons ((uint16_t) port);
  if (bind (fd, (struct sockaddr *) &a4, sizeof (a4)) != 0 || listen (fd, 16) != 0)
    {
      close (fd);
      return -1;
    }
  return fd;
}

static void
usage (const char *argv0)
{
  printf ("hp3770-airscan %s - HP ScanJet 3770 as an AirScan (eSCL) scanner\n\n", TOOL_VERSION);
  printf ("Usage: %s [options]\n\n", argv0);
  puts ("Makes the scanner available to Image Capture, Preview, Photos and other");
  puts ("Mac apps. Runs until stopped (Ctrl-C); see 'make install-service' to start");
  puts ("it automatically at login.\n");
  printf ("  -p, --port N           HTTP port (default %d)\n", DEFAULT_PORT);
  printf ("  -n, --name NAME        name shown in apps (default \"%s\")\n", DEFAULT_NAME);
  puts ("      --share            also offer the scanner to other computers on the network");
  puts ("      --announce-network announce on the network but still accept this Mac only");
  puts ("      --no-bonjour       do not announce the scanner (HTTP only)");
  puts ("  -q, --quality N        JPEG quality 1..100 (default 90)");
  puts ("      --uuid UUID        scanner UUID (default fixed)");
  puts ("  -v, --verbose          log requests and scan details");
  puts ("  -h, --help             this help");
  puts ("");
  puts ("Environment: HP3770_MAINS=50|60, HP3770_WHITE_TARGET=200..255 as for hp3770-scan.");
}

int
main (int argc, char **argv)
{
  static const struct option longopts[] = {
    { "port", required_argument, NULL, 'p' },
    { "name", required_argument, NULL, 'n' },
    { "share", no_argument, NULL, 1 },
    { "announce-network", no_argument, NULL, 2 },
    { "no-bonjour", no_argument, NULL, 3 },
    { "uuid", required_argument, NULL, 4 },
    { "quality", required_argument, NULL, 'q' },
    { "verbose", no_argument, NULL, 'v' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };
  struct sigaction sa;
  int ch, lfd;
#if HAVE_DNSSD
  DNSServiceRef sd = NULL;
#endif

  while ((ch = getopt_long (argc, argv, "p:n:q:vh", longopts, NULL)) != -1)
    switch (ch)
      {
      case 'p': opt.port = atoi (optarg); break;
      case 'n': opt.name = optarg; break;
      case 'q': opt.quality = atoi (optarg); break;
      case 'v': opt.verbose = 1; break;
      case 1: opt.share = 1; opt.announce_network = 1; break;
      case 2: opt.announce_network = 1; break;
      case 3: opt.no_bonjour = 1; break;
      case 4: opt.uuid = optarg; break;
      case 'h': usage (argv[0]); return 0;
      default: usage (argv[0]); return 64;
      }
  if (opt.port <= 0 || opt.port > 65535 || opt.quality < 1 || opt.quality > 100
      || strlen (opt.name) > 60 || strpbrk (opt.name, "<>&\"") != NULL)
    {
      fprintf (stderr, "hp3770-airscan: invalid option value\n");
      return 64;
    }

  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = on_signal;	/* no SA_RESTART: poll() returns at once */
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  signal (SIGPIPE, SIG_IGN);
  srand ((unsigned) time (NULL) ^ (unsigned) getpid ());

  lfd = open_listener (opt.port);
  if (lfd < 0)
    {
      fprintf (stderr, "hp3770-airscan: cannot listen on port %d: %s\n", opt.port, strerror (errno));
      return 1;
    }
  logmsg ("hp3770-airscan %s listening on port %d (%s)", TOOL_VERSION, opt.port,
	  opt.share ? "shared with the network" : "this Mac only");

#if HAVE_DNSSD
  if (!opt.no_bonjour)
    sd = bonjour_register ();
#else
  if (!opt.no_bonjour)
    logmsg ("built without Bonjour (dns_sd.h); apps will not find the scanner by themselves");
#endif

  while (!stop_requested)
    {
      struct pollfd pfd[2];
      int nfds = 1, n;

      pfd[0].fd = lfd;
      pfd[0].events = POLLIN;
#if HAVE_DNSSD
      if (sd != NULL)
	{
	  pfd[1].fd = DNSServiceRefSockFD (sd);
	  pfd[1].events = POLLIN;
	  nfds = 2;
	}
#endif
      n = poll (pfd, (nfds_t) nfds, 1000);
      if (n <= 0)
	continue;
#if HAVE_DNSSD
      if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))
	{
	  if (DNSServiceProcessResult (sd) != kDNSServiceErr_NoError)
	    {
	      logmsg ("Bonjour: connection to mDNSResponder lost, re-registering");
	      DNSServiceRefDeallocate (sd);
	      sd = bonjour_register ();
	    }
	}
#endif
      if (pfd[0].revents & POLLIN)
	{
	  int fd = accept (lfd, NULL, NULL);
	  struct conn *c;
	  pthread_t t;

	  if (fd < 0)
	    continue;
	  c = calloc (1, sizeof (*c));
	  if (c == NULL)
	    {
	      close (fd);
	      continue;
	    }
	  c->fd = fd;
	  if (pthread_create (&t, NULL, connection_main, c) != 0)
	    {
	      close (fd);
	      free (c);
	      continue;
	    }
	  pthread_detach (t);
	}
    }

  logmsg ("stopping");
#if HAVE_DNSSD
  if (sd != NULL)
    DNSServiceRefDeallocate (sd);
#endif
  close (lfd);
  pthread_mutex_lock (&lock);
  pthread_cond_broadcast (&job_changed);
  if (worker_running)
    {
      pthread_mutex_unlock (&lock);
      logmsg ("waiting for the scan to stop and the head to park...");
      pthread_join (worker_thread, NULL);
      pthread_mutex_lock (&lock);
      worker_running = 0;
    }
  pthread_mutex_unlock (&lock);
  return 0;
}
