/* Minimal baseline JPEG encoder and single-page PDF wrapper.

   Dependency-free (no libjpeg, no macOS frameworks), so hp3770-airscan
   builds with the plain command-line tools. Baseline sequential DCT,
   4:4:4 sampling, the standard Huffman tables of ITU T.81 Annex K and the
   Annex K quantisation tables scaled by quality (as libjpeg does).

   Included by hp3770_airscan.c. */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct membuf
{
  unsigned char *data;
  size_t len, cap;
  int failed;
};

static void
mb_put (struct membuf *b, const void *p, size_t n)
{
  if (b->failed)
    return;
  if (b->len + n > b->cap)
    {
      size_t cap = b->cap ? b->cap : 65536;
      unsigned char *d;
      while (cap < b->len + n)
	cap *= 2;
      d = realloc (b->data, cap);
      if (d == NULL)
	{
	  b->failed = 1;
	  return;
	}
      b->data = d;
      b->cap = cap;
    }
  memcpy (b->data + b->len, p, n);
  b->len += n;
}

static void
mb_byte (struct membuf *b, int v)
{
  unsigned char c = (unsigned char) v;
  mb_put (b, &c, 1);
}

static void
mb_word (struct membuf *b, int v)
{
  mb_byte (b, (v >> 8) & 0xff);
  mb_byte (b, v & 0xff);
}

static void
mb_printf (struct membuf *b, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  int n;

  va_start (ap, fmt);
  n = vsnprintf (tmp, sizeof (tmp), fmt, ap);
  va_end (ap);
  if (n > 0)
    mb_put (b, tmp, (size_t) ((n < (int) sizeof (tmp)) ? n : (int) sizeof (tmp) - 1));
}

static const unsigned char jpg_dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const unsigned char jpg_dc_lum_vals[12] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
static const unsigned char jpg_ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
static const unsigned char jpg_ac_lum_vals[162] = {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const unsigned char jpg_dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const unsigned char jpg_dc_chr_vals[12] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
static const unsigned char jpg_ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
static const unsigned char jpg_ac_chr_vals[162] = {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

static const unsigned char jpg_zigzag[64] = {
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const unsigned char jpg_q_lum[64] = {
  16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
  14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
  18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};

static const unsigned char jpg_q_chr[64] = {
  17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
  24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

struct jpg_huff
{
  unsigned short code[256];
  unsigned char size[256];
};

struct jpg_writer
{
  struct membuf *out;
  uint32_t bitbuf;
  int bitcnt;
};

static void
jpg_build_huff (struct jpg_huff *h, const unsigned char *bits, const unsigned char *vals)
{
  int len, i, k = 0;
  unsigned code = 0;

  memset (h, 0, sizeof (*h));
  for (len = 1; len <= 16; len++)
    {
      for (i = 0; i < bits[len - 1]; i++)
	{
	  h->code[vals[k]] = (unsigned short) code;
	  h->size[vals[k]] = (unsigned char) len;
	  k++;
	  code++;
	}
      code <<= 1;
    }
}

static void
jpg_bits (struct jpg_writer *w, unsigned code, int size)
{
  w->bitbuf = (w->bitbuf << size) | (code & ((1u << size) - 1));
  w->bitcnt += size;
  while (w->bitcnt >= 8)
    {
      int c = (int) ((w->bitbuf >> (w->bitcnt - 8)) & 0xff);
      mb_byte (w->out, c);
      if (c == 0xff)
	mb_byte (w->out, 0);	/* byte stuffing */
      w->bitcnt -= 8;
    }
}

static void
jpg_flush (struct jpg_writer *w)
{
  if (w->bitcnt > 0)
    jpg_bits (w, 0x7f, 8 - w->bitcnt);	/* pad with ones */
}

/* magnitude category and bit pattern of a coefficient */
static void
jpg_coef (int v, int *cat, unsigned *bits)
{
  int a = (v < 0) ? -v : v, n = 0;
  while (a > 0)
    {
      n++;
      a >>= 1;
    }
  *cat = n;
  *bits = (v < 0) ? (unsigned) (v - 1) : (unsigned) v;
}

static float jpg_cos[8][8];

static void
jpg_init_cos (void)
{
  int x, u;
  for (u = 0; u < 8; u++)
    for (x = 0; x < 8; x++)
      jpg_cos[u][x] = (float) (cos ((2 * x + 1) * u * M_PI / 16.0) * ((u == 0) ? sqrt (0.125) : 0.5));
}

/* forward DCT, quantisation and entropy coding of one 8x8 block */
static void
jpg_block (struct jpg_writer *w, const float *in, const float *qdiv, int *dc_pred,
	   const struct jpg_huff *dch, const struct jpg_huff *ach)
{
  float tmp[64], co[64];
  int q[64], u, v, x, k, run, cat;
  unsigned bits;

  for (v = 0; v < 8; v++)	/* rows */
    for (u = 0; u < 8; u++)
      {
	float s = 0;
	for (x = 0; x < 8; x++)
	  s += in[v * 8 + x] * jpg_cos[u][x];
	tmp[v * 8 + u] = s;
      }
  for (u = 0; u < 8; u++)	/* columns */
    for (v = 0; v < 8; v++)
      {
	float s = 0;
	for (x = 0; x < 8; x++)
	  s += tmp[x * 8 + u] * jpg_cos[v][x];
	co[v * 8 + u] = s;
      }
  for (k = 0; k < 64; k++)
    {
      float f = co[jpg_zigzag[k]] * qdiv[k];
      q[k] = (int) ((f < 0) ? f - 0.5f : f + 0.5f);
    }

  /* DC */
  jpg_coef (q[0] - *dc_pred, &cat, &bits);
  *dc_pred = q[0];
  jpg_bits (w, dch->code[cat], dch->size[cat]);
  if (cat)
    jpg_bits (w, bits, cat);

  /* AC */
  run = 0;
  for (k = 1; k < 64; k++)
    {
      if (q[k] == 0)
	{
	  run++;
	  continue;
	}
      while (run > 15)
	{
	  jpg_bits (w, ach->code[0xf0], ach->size[0xf0]);
	  run -= 16;
	}
      jpg_coef (q[k], &cat, &bits);
      jpg_bits (w, ach->code[(run << 4) | cat], ach->size[(run << 4) | cat]);
      jpg_bits (w, bits, cat);
      run = 0;
    }
  if (run > 0)
    jpg_bits (w, ach->code[0x00], ach->size[0x00]);	/* EOB */
}

static void
jpg_dht (struct membuf *b, int tc, const unsigned char *bits, const unsigned char *vals)
{
  int n = 0, i;
  for (i = 0; i < 16; i++)
    n += bits[i];
  mb_word (b, 0xffc4);
  mb_word (b, 2 + 1 + 16 + n);
  mb_byte (b, tc);
  mb_put (b, bits, 16);
  mb_put (b, vals, (size_t) n);
}

/* SOI and the JFIF APP0 segment: the first JPEG_JFIF_HEADER_LEN bytes of
   every file jpeg_encode writes. hp3770-airscan sends them before the scan
   has finished, followed by comment segments as keep-alives. */
#define JPEG_JFIF_HEADER_LEN 20

static void
jpeg_jfif_header (struct membuf *out, int dpi)
{
  mb_word (out, 0xffd8);
  mb_word (out, 0xffe0);
  mb_word (out, 16);
  mb_put (out, "JFIF\0", 5);
  mb_word (out, 0x0101);
  mb_byte (out, 1);		/* density in dots per inch */
  mb_word (out, dpi);
  mb_word (out, dpi);
  mb_byte (out, 0);
  mb_byte (out, 0);
}

/* A comment segment, valid anywhere between the header segments. */
static void
jpeg_comment (struct membuf *out, const char *text)
{
  size_t n = strlen (text);
  mb_word (out, 0xfffe);
  mb_word (out, (int) n + 2);
  mb_put (out, text, n);
}

/* Encodes 8-bit interleaved RGB (channels 3) or gray (channels 1) rows into
   out. dpi is stored in the JFIF header. Returns 0 on success. */
static int
jpeg_encode (struct membuf *out, const unsigned char *img, int width, int height,
	     int bytes_per_line, int channels, int quality, int dpi)
{
  unsigned char qt[2][64];
  float qdiv[2][64];
  struct jpg_huff dcl, acl, dcc, acc;
  struct jpg_writer w;
  int dc[3] = { 0, 0, 0 };
  int i, c, bx, by, scale;
  float *blk;

  if ((width <= 0) || (height <= 0) || ((channels != 1) && (channels != 3)) || (width > 65535)
      || (height > 65535))
    return -1;
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;
  scale = (quality < 50) ? 5000 / quality : 200 - 2 * quality;
  jpg_init_cos ();
  for (i = 0; i < 64; i++)
    {
      int a = (jpg_q_lum[jpg_zigzag[i]] * scale + 50) / 100;
      int b = (jpg_q_chr[jpg_zigzag[i]] * scale + 50) / 100;
      qt[0][i] = (unsigned char) ((a < 1) ? 1 : (a > 255) ? 255 : a);
      qt[1][i] = (unsigned char) ((b < 1) ? 1 : (b > 255) ? 255 : b);
      qdiv[0][i] = 1.0f / qt[0][i];
      qdiv[1][i] = 1.0f / qt[1][i];
    }
  jpg_build_huff (&dcl, jpg_dc_lum_bits, jpg_dc_lum_vals);
  jpg_build_huff (&acl, jpg_ac_lum_bits, jpg_ac_lum_vals);
  jpg_build_huff (&dcc, jpg_dc_chr_bits, jpg_dc_chr_vals);
  jpg_build_huff (&acc, jpg_ac_chr_bits, jpg_ac_chr_vals);

  /* headers */
  jpeg_jfif_header (out, dpi);
  for (i = 0; i < ((channels == 3) ? 2 : 1); i++)
    {
      mb_word (out, 0xffdb);
      mb_word (out, 67);
      mb_byte (out, i);
      mb_put (out, qt[i], 64);	/* already in zigzag order */
    }
  mb_word (out, 0xffc0);
  mb_word (out, 8 + 3 * channels);
  mb_byte (out, 8);
  mb_word (out, height);
  mb_word (out, width);
  mb_byte (out, channels);
  for (c = 0; c < channels; c++)
    {
      mb_byte (out, c + 1);
      mb_byte (out, 0x11);
      mb_byte (out, c ? 1 : 0);
    }
  jpg_dht (out, 0x00, jpg_dc_lum_bits, jpg_dc_lum_vals);
  jpg_dht (out, 0x10, jpg_ac_lum_bits, jpg_ac_lum_vals);
  if (channels == 3)
    {
      jpg_dht (out, 0x01, jpg_dc_chr_bits, jpg_dc_chr_vals);
      jpg_dht (out, 0x11, jpg_ac_chr_bits, jpg_ac_chr_vals);
    }
  mb_word (out, 0xffda);
  mb_word (out, 6 + 2 * channels);
  mb_byte (out, channels);
  for (c = 0; c < channels; c++)
    {
      mb_byte (out, c + 1);
      mb_byte (out, c ? 0x11 : 0x00);
    }
  mb_byte (out, 0);
  mb_byte (out, 63);
  mb_byte (out, 0);

  /* entropy-coded data, one MCU (one block per component) at a time */
  memset (&w, 0, sizeof (w));
  w.out = out;
  blk = malloc (sizeof (float) * 64 * 3);
  if (blk == NULL)
    return -1;
  for (by = 0; by < height; by += 8)
    for (bx = 0; bx < width; bx += 8)
      {
	int x, y;
	for (y = 0; y < 8; y++)
	  {
	    int sy = (by + y < height) ? by + y : height - 1;
	    const unsigned char *row = img + (size_t) sy * (size_t) bytes_per_line;
	    for (x = 0; x < 8; x++)
	      {
		int sx = (bx + x < width) ? bx + x : width - 1;
		if (channels == 3)
		  {
		    float r = row[sx * 3], g = row[sx * 3 + 1], b = row[sx * 3 + 2];
		    blk[y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
		    blk[64 + y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
		    blk[128 + y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
		  }
		else
		  blk[y * 8 + x] = row[sx] - 128.0f;
	      }
	  }
	for (c = 0; c < channels; c++)
	  jpg_block (&w, blk + 64 * c, qdiv[c ? 1 : 0], &dc[c], c ? &dcc : &dcl, c ? &acc : &acl);
      }
  free (blk);
  jpg_flush (&w);
  mb_word (out, 0xffd9);
  return out->failed ? -1 : 0;
}

/* The PDF header line and binary-marker comment. */
static void
pdf_header (struct membuf *out)
{
  mb_printf (out, "%%PDF-1.4\n%%\xe2\xe3\xcf\xd3\n");
}

/* Wraps a JPEG into a one-page PDF of the scanned size. sent is the number
   of bytes of this PDF already delivered (the header from pdf_header and
   any comment lines); 0 writes the whole file. */
static int
pdf_wrap_jpeg (struct membuf *out, const struct membuf *jpg, int width, int height,
	       int channels, int dpi, size_t sent)
{
  long off[6];
  double pw = width * 72.0 / dpi, ph = height * 72.0 / dpi;
  char content[128];
  int clen;
  size_t xref;
  long base;
  int i;

  clen = snprintf (content, sizeof (content), "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pw, ph);
  /* file offset of what is appended to out = base + out->len */
  base = (long) sent - (long) out->len;
  if (sent == 0)
    pdf_header (out);
  off[1] = base + (long) out->len;
  mb_printf (out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
  off[2] = base + (long) out->len;
  mb_printf (out, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
  off[3] = base + (long) out->len;
  mb_printf (out, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
	     "/Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n", pw, ph);
  off[4] = base + (long) out->len;
  mb_printf (out, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
	     "/ColorSpace /%s /BitsPerComponent 8 /Filter /DCTDecode /Length %lu >>\nstream\n",
	     width, height, (channels == 3) ? "DeviceRGB" : "DeviceGray", (unsigned long) jpg->len);
  mb_put (out, jpg->data, jpg->len);
  mb_printf (out, "\nendstream\nendobj\n");
  off[5] = base + (long) out->len;
  mb_printf (out, "5 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", clen, content);
  xref = (size_t) (base + (long) out->len);
  mb_printf (out, "xref\n0 6\n0000000000 65535 f \n");
  for (i = 1; i <= 5; i++)
    mb_printf (out, "%010ld 00000 n \n", off[i]);
  mb_printf (out, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%lu\n%%%%EOF\n", (unsigned long) xref);
  return out->failed ? -1 : 0;
}
