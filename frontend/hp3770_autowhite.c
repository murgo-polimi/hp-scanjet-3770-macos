/* Automatic white point for document scans (HP ScanJet 3770).

   The backend maps the scanner's white calibration strip to a fixed level in
   every channel. Real paper is usually a little darker and bluer than that
   strip (optical brighteners), so white paper came out light grey-blue
   (about 205/201/234 of 255). This finds the paper's own white in the
   finished image and scales each channel so that it becomes a neutral
   white, leaving darker colours proportionally unchanged.

   The paper white is the per-channel median of the brightest 20 % of the
   pixels. It is only used when those pixels form a tight cluster (a paper
   background), the result is bright and only mildly tinted; photos without a
   white background are left alone.

   Included by hp3770_common.c. */

struct autowhite_info
{
  double white[3];		/* measured paper white, 0..255 scale */
  double gain[3];
  double spread;		/* (p98 - p80) / p90 of the luminance */
};

#define AW_BINS 1024

static double
aw_percentile (const unsigned long *hist, unsigned long total, double frac)
{
  unsigned long want = (unsigned long) (frac * (double) total), sum = 0;
  int i;

  for (i = 0; i < AW_BINS; i++)
    {
      sum += hist[i];
      if (sum > want)
	return (i + 0.5) / AW_BINS;
    }
  return 1.0;
}

/* returns 0 if the white point was applied */
static int
autowhite_image (unsigned char *img, int lines, int bytes_per_line, int width, int channels,
		 int sixteen, double target, struct autowhite_info *info)
{
  const double fullscale = sixteen ? 65535.0 : 255.0;
  const double weight[3] = { 0.299, 0.587, 0.114 };
  unsigned long *yhist = NULL, *chist = NULL, total = 0, ctotal = 0;
  long npix = (long) lines * width, step;
  double p80, p90, p98;
  int c, rst = -1;
  long i;

  memset (info, 0, sizeof (*info));
  if ((lines < 16) || (width < 16) || ((channels != 1) && (channels != 3)))
    return -1;

  yhist = calloc (AW_BINS, sizeof (unsigned long));
  chist = calloc ((size_t) AW_BINS * 3, sizeof (unsigned long));
  if (!yhist || !chist)
    goto done;

  /* at most ~4 million samples */
  step = npix / 4000000 + 1;

  /* 1. luminance histogram */
  for (i = 0; i < npix; i += step)
    {
      int y = (int) (i / width), x = (int) (i % width);
      double lum = 0.0;
      for (c = 0; c < channels; c++)
	lum += ((channels == 3) ? weight[c] : 1.0)
	  * deripple_sample (img, bytes_per_line, width, channels, sixteen, y, x, c);
      lum /= fullscale;
      yhist[(lum >= 1.0) ? AW_BINS - 1 : (int) (lum * AW_BINS)]++;
      total++;
    }
  p80 = aw_percentile (yhist, total, 0.80);
  p90 = aw_percentile (yhist, total, 0.90);
  p98 = aw_percentile (yhist, total, 0.98);
  info->spread = (p90 > 0.0) ? (p98 - p80) / p90 : 1.0;

  /* 2. per-channel histograms of the brightest 20 % */
  for (i = 0; i < npix; i += step)
    {
      int y = (int) (i / width), x = (int) (i % width);
      double v[3], lum = 0.0;
      for (c = 0; c < channels; c++)
	{
	  v[c] = deripple_sample (img, bytes_per_line, width, channels, sixteen, y, x, c) / fullscale;
	  lum += ((channels == 3) ? weight[c] : 1.0) * v[c];
	}
      if (lum < p80)
	continue;
      for (c = 0; c < channels; c++)
	chist[(size_t) c * AW_BINS + ((v[c] >= 1.0) ? AW_BINS - 1 : (int) (v[c] * AW_BINS))]++;
      ctotal++;
    }
  if (ctotal < 100)
    goto done;
  for (c = 0; c < channels; c++)
    info->white[c] = 255.0 * aw_percentile (chist + (size_t) c * AW_BINS, ctotal, 0.5);

  /* 3. plausibility: a tight, bright, mildly tinted background */
  {
    double wmin = 255.0, wmax = 0.0;
    for (c = 0; c < channels; c++)
      {
	if (info->white[c] < wmin) wmin = info->white[c];
	if (info->white[c] > wmax) wmax = info->white[c];
      }
    if ((info->spread > 0.10) || (wmin < 150.0) || (wmax / wmin > 1.35))
      goto done;
  }

  /* 4. gains, limited to a moderate correction */
  for (c = 0; c < channels; c++)
    {
      double g = target / info->white[c];
      if (g < 0.85) g = 0.85;
      if (g > 1.5) g = 1.5;
      info->gain[c] = g;
    }

  /* 5. apply (8-bit through a lookup table) */
  if (!sixteen)
    {
      unsigned char lut[3][256];
      int v, y, x;
      for (c = 0; c < channels; c++)
	for (v = 0; v < 256; v++)
	  {
	    double o = v * info->gain[c] + 0.5;
	    lut[c][v] = (unsigned char) ((o > 255.0) ? 255 : o);
	  }
      for (y = 0; y < lines; y++)
	{
	  unsigned char *p = img + (size_t) y * (size_t) bytes_per_line;
	  for (x = 0; x < width; x++)
	    for (c = 0; c < channels; c++, p++)
	      *p = lut[c][*p];
	}
    }
  else
    {
      int y, x;
      for (y = 0; y < lines; y++)
	for (x = 0; x < width; x++)
	  for (c = 0; c < channels; c++)
	    deripple_store (img, bytes_per_line, channels, 1, y, x, c,
			    deripple_sample (img, bytes_per_line, width, channels, 1, y, x, c)
			    * info->gain[c]);
    }
  rst = 0;

done:
  free (yhist);
  free (chist);
  return rst;
}
