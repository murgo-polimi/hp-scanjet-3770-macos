/* Removal of lamp ripple bands (HP ScanJet 3770).

   The 3770's lamp brightness carries a ripple at twice the mains frequency
   (100 Hz in Europe), whatever the lamp PWM settings and also in a dark
   room. Every scan line integrates 3-6 ms of it, so white areas show thin
   horizontal bands of a few percent. The CCD's red, green and blue rows see
   each paper line at different times, so the bands are coloured.

   The ripple is a sinusoid in time, hence also in the line number, with a
   frequency known from the line period. For every line and channel this code
   measures the brightness relative to the neighbouring lines (median over
   the line, so image content cancels out), fits the ripple frequency and its
   second harmonic in overlapping windows, and divides the fit out.

   Included by hp3770_common.c. */

#include <math.h>

struct deripple_info
{
  double freq;			/* fitted, cycles per line (0 = not applied) */
  double amplitude[3];		/* largest correction per channel, percent */
};

/* median by quickselect (the order of v is destroyed) */
static float
deripple_median (float *v, int n)
{
  int lo = 0, hi = n - 1, k = n / 2;

  if (n <= 0)
    return 0.0f;
  while (lo < hi)
    {
      float pivot = v[(lo + hi) / 2];
      int i = lo, j = hi;
      while (i <= j)
	{
	  while (v[i] < pivot) i++;
	  while (v[j] > pivot) j--;
	  if (i <= j)
	    {
	      float t = v[i]; v[i] = v[j]; v[j] = t;
	      i++; j--;
	    }
	}
      if (k <= j) hi = j;
      else if (k >= i) lo = i;
      else break;
    }
  return v[k];
}

static double
deripple_sample (const unsigned char *img, int bytes_per_line, int width, int channels,
		 int sixteen, int row, int x, int c)
{
  const unsigned char *p = img + (size_t) row * (size_t) bytes_per_line;
  size_t idx = (size_t) x * (size_t) channels + (size_t) c;

  (void) width;
  if (sixteen)
    {
      uint16_t v;
      memcpy (&v, p + idx * 2, 2);	/* host order, as delivered by the backend */
      return v;
    }
  return p[idx];
}

static void
deripple_store (unsigned char *img, int bytes_per_line, int channels, int sixteen,
		int row, int x, int c, double value)
{
  unsigned char *p = img + (size_t) row * (size_t) bytes_per_line;
  size_t idx = (size_t) x * (size_t) channels + (size_t) c;
  double max = sixteen ? 65535.0 : 255.0;

  if (value < 0.0)
    value = 0.0;
  if (value > max)
    value = max;
  if (sixteen)
    {
      uint16_t v = (uint16_t) (value + 0.5);
      memcpy (p + idx * 2, &v, 2);
    }
  else
    p[idx] = (unsigned char) (value + 0.5);
}

/* response of x[n] - mean(x[n-2..n+2]) at angular frequency w */
static double
deripple_filter_gain (double w)
{
  return 1.0 - (1.0 + 2.0 * cos (w) + 2.0 * cos (2.0 * w)) / 5.0;
}

/* least squares for up to 4 regressors (normal equations, Gauss-Jordan) */
static int
deripple_solve (double a[4][5], int n)
{
  int i, j, k;

  for (i = 0; i < n; i++)
    {
      int piv = i;
      for (j = i + 1; j < n; j++)
	if (fabs (a[j][i]) > fabs (a[piv][i]))
	  piv = j;
      if (fabs (a[piv][i]) < 1e-12)
	return -1;
      if (piv != i)
	for (k = 0; k <= n; k++)
	  {
	    double t = a[i][k];
	    a[i][k] = a[piv][k];
	    a[piv][k] = t;
	  }
      for (j = 0; j < n; j++)
	if (j != i)
	  {
	    double f = a[j][i] / a[i][i];
	    for (k = i; k <= n; k++)
	      a[j][k] -= f * a[i][k];
	  }
    }
  for (i = 0; i < n; i++)
    a[i][n] /= a[i][i];
  return 0;
}

/* Fits sum of regressors to v[y0..y1) and returns the fitted signal (already
   divided by the filter gains) in out[y0..y1). */
static void
deripple_fit_window (const double *v, int y0, int y1, double w, int harmonics, double *out)
{
  double h1 = deripple_filter_gain (w), h2 = deripple_filter_gain (2.0 * w);
  double a[4][5];
  int n = 2, i, j, y;

  if ((harmonics > 1) && (fabs (h2) > 0.2))
    n = 4;
  memset (a, 0, sizeof (a));
  for (y = y0; y < y1; y++)
    {
      double r[4] = { h1 * cos (w * y), h1 * sin (w * y), h2 * cos (2 * w * y), h2 * sin (2 * w * y) };
      for (i = 0; i < n; i++)
	{
	  for (j = 0; j < n; j++)
	    a[i][j] += r[i] * r[j];
	  a[i][n] += r[i] * v[y];
	}
    }
  for (y = y0; y < y1; y++)
    out[y] = 0.0;
  if (deripple_solve (a, n) != 0)
    return;
  for (y = y0; y < y1; y++)
    {
      out[y] = a[0][n] * cos (w * y) + a[1][n] * sin (w * y);
      if (n == 4)
	out[y] += a[2][n] * cos (2 * w * y) + a[3][n] * sin (2 * w * y);
    }
}

/* power of the fundamental fitted over all lines, for the frequency search;
   phase (radians) returned if requested */
static double
deripple_fit_global (const double *v, int lines, double w, double *phase)
{
  double cc = 0, ss = 0, cs = 0, cv = 0, sv = 0, det, a, b;
  int y;

  for (y = 0; y < lines; y++)
    {
      double c = cos (w * y), s = sin (w * y);
      cc += c * c; ss += s * s; cs += c * s; cv += c * v[y]; sv += s * v[y];
    }
  det = cc * ss - cs * cs;
  if (fabs (det) < 1e-12)
    return 0.0;
  a = (cv * ss - sv * cs) / det;
  b = (sv * cc - cv * cs) / det;
  if (phase != NULL)
    *phase = atan2 (-b, a);
  return a * a + b * b;
}

static double
deripple_power (const double *v, int lines, double w)
{
  return deripple_fit_global (v, lines, w, NULL);
}

/* wraps an angle into -pi..pi */
static double
deripple_wrap (double a)
{
  while (a > M_PI) a -= 2.0 * M_PI;
  while (a < -M_PI) a += 2.0 * M_PI;
  return a;
}

/* img: lines x bytes_per_line, interleaved samples (8-bit or 16-bit host
   order), 1 or 3 channels. nominal: expected ripple frequencies in cycles per
   line (folded to 0..0.5), count candidates. Returns 0 if corrected. */
static int
deripple_image (unsigned char *img, int lines, int bytes_per_line, int width, int channels,
		int sixteen, const double *nominal, int count, double line_distance,
		struct deripple_info *info)
{
  const double fullscale = sixteen ? 65535.0 : 255.0;
  const int win = 200;
  double *v = NULL, *g = NULL, *seg = NULL, *norm = NULL;
  float *tmp = NULL;
  double best_power = 0.0, best_w = 0.0, noise = 0.0;
  int c, y, x, k, rst = -1;

  memset (info, 0, sizeof (*info));
  if ((lines < 60) || (width < 16) || (channels < 1) || (channels > 3) || (count < 1))
    return -1;

  v = calloc ((size_t) lines * (size_t) channels, sizeof (double));
  g = calloc ((size_t) lines * (size_t) channels, sizeof (double));
  seg = calloc ((size_t) lines, sizeof (double));
  norm = calloc ((size_t) lines, sizeof (double));
  tmp = malloc ((size_t) width * sizeof (float));
  if (!v || !g || !seg || !norm || !tmp)
    goto done;

  /* 1. per line and channel: median log ratio to the 5-line neighbourhood
        (at most ~2000 samples per line) */
  for (c = 0; c < channels; c++)
    for (y = 0; y < lines; y++)
      {
	int n = 0, stride = (width + 1999) / 2000, used = 0;
	for (x = 0; x < width; x += stride)
	  {
	    used++;
	    double s = 0.0, iv;
	    for (k = -2; k <= 2; k++)
	      {
		int yy = y + k;
		if (yy < 0) yy = 0;
		if (yy >= lines) yy = lines - 1;
		s += deripple_sample (img, bytes_per_line, width, channels, sixteen, yy, x, c);
	      }
	    s /= 5.0;
	    if (s < fullscale * 0.15)
	      continue;		/* dark pixels are mostly noise */
	    iv = deripple_sample (img, bytes_per_line, width, channels, sixteen, y, x, c);
	    if (iv < 1.0)
	      iv = 1.0;
	    tmp[n++] = (float) log (iv / s);
	  }
	v[(size_t) c * lines + y] = (n >= used / 8 && n > 0) ? deripple_median (tmp, n) : 0.0;
      }

  /* 2. frequency search around each nominal value (mains tolerance ~1 %) */
  for (k = 0; k < count; k++)
    {
      int i;
      if ((nominal[k] <= 0.02) || (nominal[k] >= 0.48))
	continue;
      for (i = -20; i <= 20; i++)
	{
	  double w = 2.0 * M_PI * nominal[k] * (1.0 + i * 0.0005), p = 0.0;
	  for (c = 0; c < channels; c++)
	    p += deripple_power (v + (size_t) c * lines, lines, w);
	  if (p > best_power)
	    {
	      best_power = p;
	      best_w = w;
	    }
	}
    }
  if (best_w <= 0.0)
    goto done;

  /* 3. significance: fitted amplitude against the spread of v */
  for (c = 0; c < channels; c++)
    for (y = 0; y < lines; y++)
      noise += v[(size_t) c * lines + y] * v[(size_t) c * lines + y];
  noise = sqrt (noise / (lines * channels));
  if (sqrt (best_power / channels) * fabs (deripple_filter_gain (best_w)) < 0.15 * noise
      || sqrt (best_power / channels) < 0.006)
    goto done;			/* no clear ripple: leave the image alone */

  /* 3b. the lamp ripple reaches the R, G and B rows of the CCD at different
         times (they look at paper lines line_distance apart), so its phase
         steps by 2*pi*f*line_distance from channel to channel. Periodic image
         content (halftone moire) is in phase in all channels. Require the
         expected step (either sign) within 45 degrees for both channel
         pairs. A single-channel image cannot be checked and is left alone:
         hp3770's gray is computed from a colour scan, where the three phases
         largely cancel (residual ~0.5 %). */
  if (channels == 3)
    {
      double ph[3], expect = deripple_wrap (best_w * line_distance), d1, d2;
      int ok = 0, sgn;

      for (c = 0; c < 3; c++)
	deripple_fit_global (v + (size_t) c * lines, lines, best_w, &ph[c]);
      d1 = deripple_wrap (ph[1] - ph[0]);
      d2 = deripple_wrap (ph[2] - ph[1]);
      for (sgn = -1; sgn <= 1; sgn += 2)
	if ((fabs (deripple_wrap (d1 - sgn * expect)) < M_PI / 4)
	    && (fabs (deripple_wrap (d2 - sgn * expect)) < M_PI / 4))
	  ok = 1;
      if (!ok)
	goto done;
    }
  else
    goto done;			/* see below */

  /* 4. windowed fit with triangular blending */
  for (c = 0; c < channels; c++)
    {
      double *gc = g + (size_t) c * lines, peak = 0.0;
      memset (norm, 0, (size_t) lines * sizeof (double));
      for (y = 0; y < lines; y += win / 2)
	{
	  int y1 = (y + win < lines) ? y + win : lines, yy;
	  int y0 = (y1 - y < win / 2 && y1 - win > 0) ? y1 - win : y;
	  deripple_fit_window (v + (size_t) c * lines, y0, y1, best_w, 2, seg);
	  for (yy = y0; yy < y1; yy++)
	    {
	      double t = (double) ((yy - y0 + 1) < (y1 - yy) ? (yy - y0 + 1) : (y1 - yy));
	      gc[yy] += seg[yy] * t;
	      norm[yy] += t;
	    }
	  if (y1 == lines)
	    break;
	}
      for (y = 0; y < lines; y++)
	{
	  gc[y] = (norm[y] > 0.0) ? gc[y] / norm[y] : 0.0;
	  if (gc[y] > 0.1) gc[y] = 0.1;	/* never more than ~10 % */
	  if (gc[y] < -0.1) gc[y] = -0.1;
	  if (fabs (gc[y]) > peak)
	    peak = fabs (gc[y]);
	}
      if (c < 3)
	info->amplitude[c] = 100.0 * (exp (peak) - 1.0);
    }

  /* 5. apply */
  for (y = 0; y < lines; y++)
    for (c = 0; c < channels; c++)
      {
	double f = exp (-g[(size_t) c * lines + y]);
	for (x = 0; x < width; x++)
	  deripple_store (img, bytes_per_line, channels, sixteen, y, x, c,
			  deripple_sample (img, bytes_per_line, width, channels, sixteen, y, x, c) * f);
      }

  info->freq = best_w / (2.0 * M_PI);
  rst = 0;

done:
  free (v);
  free (g);
  free (seg);
  free (norm);
  free (tmp);
  return rst;
}

/* folds a frequency in cycles per line into 0..0.5 */
static double
deripple_fold (double f)
{
  f -= floor (f);
  return (f > 0.5) ? 1.0 - f : f;
}
