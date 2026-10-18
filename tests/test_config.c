/* Consistency checks for the HP3770 configuration.

   HP's 3770 driver always selects its "t6" profile (FitCalibrate [PLATFORM]
   CCDType=2): a 1200 dpi CCD, 1200 dpi motor, t6_Timing1..12, 12 motor
   curves and its own scan modes. The HP3970 profile of hp3900 (2400 dpi)
   does not apply. These checks pin the HP3770 configuration to the
   generated t6 tables, so an accidental fall-through to HP3970 values is
   noticed.

   Build: make check (links the simulated USB layer, no hardware needed). */

#include "../backend/hp3770.c"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf ("FAIL: " __VA_ARGS__); printf ("\n"); } } while (0)

int
main (void)
{
  int tm, n;

  RTS_Debug = malloc (sizeof (struct st_debug_opts));
  memset (RTS_Debug, 0, sizeof (struct st_debug_opts));
  RTS_Debug->usbtype = USB20;
  RTS_Debug->dev_model = HP3770;

  /* device identification */
  CHECK (cfg_device_get (0x2505, 0x03f0) == HP3770, "03f0:2505 is not HP3770");
  CHECK (cfg_chipset_model_get (HP3770) == RTS8822L_01H, "HP3770 chipset");
  CHECK (strcmp (HP3770_CCD_PROFILE, "t6") == 0, "tables not generated from the t6 profile");

  /* timings: t6_Timing1..12 for any sensor type */
  {
    static const int res[12] = { 1200, 600, 300, 200, 200, 1200, 600, 300, 200, 200, 100, 100 };
    struct st_timing t, t2;
    for (tm = 0; tm < 12; tm++)
      {
	CHECK (cfg_timing_get (CCD_SENSOR, tm, &t) == OK, "timing %d", tm);
	CHECK (memcmp (&t, &hp3770_timing[tm], sizeof (t)) == 0, "timing %d is not t6_Timing%d", tm, tm + 1);
	CHECK (t.sensorresolution == res[tm], "timing %d resolution %d", tm, t.sensorresolution);
	CHECK (cfg_timing_get (CIS_SENSOR, tm, &t2) == OK && memcmp (&t, &t2, sizeof (t)) == 0,
	       "timing %d depends on the sensor type", tm);
      }
    CHECK (cfg_timing_get (CCD_SENSOR, 12, &t) != OK, "timing 12 should not exist");
    cfg_timing_get (CCD_SENSOR, 2, &t);
    CHECK (t.clampe == -1, "36-bit CLAMPE stored as -1 (got %d)", t.clampe);
  }

  /* motor curves: the 12 curves of the 3770 driver, not HP3970's */
  {
    SANE_Int *c3770, *c3970;
    c3770 = cfg_motorcurve_get ();
    CHECK (c3770 != NULL && memcmp (c3770, hp3770_motorcurves, sizeof (hp3770_motorcurves)) == 0,
	   "motor curves are not the generated table");
    RTS_Debug->dev_model = HP3970;
    c3970 = cfg_motorcurve_get ();
    RTS_Debug->dev_model = HP3770;
    CHECK (c3970 != NULL && memcmp (c3770, c3970, sizeof (hp3770_motorcurves)) != 0,
	   "motor curves unexpectedly equal HP3970");
    free (c3770);
    free (c3970);
  }

  /* scan modes: 5 resolutions x color/gray/lineart, all <= 1200 dpi */
  {
    struct st_scanmode m;
    int sm, found100c = 0, found100g = 0;
    for (sm = 0; cfg_scanmode_get (CCD_SENSOR, sm, &m) == OK; sm++)
      {
	CHECK (m.scantype == ST_NORMAL, "scan mode %d is not reflective", sm);
	CHECK (m.resolution >= 100 && m.resolution <= 1200, "scan mode %d at %d dpi", sm, m.resolution);
	CHECK (m.timing >= 0 && m.timing < 12, "scan mode %d timing %d", sm, m.timing);
	CHECK (m.motorcurve >= -1 && m.motorcurve < 12, "scan mode %d curve %d", sm, m.motorcurve);
	if (m.resolution == 100 && m.colormode == CM_COLOR)
	  {
	    found100c = 1;
	    CHECK (m.timing == 2 && m.motorcurve == 11 && m.ctpc == 2999 && m.motorbackstep == 1024
		   && m.dummyline == 1 && m.systemclock == 4, "color 100 dpi mode (ScanColorMode5)");
	  }
	if (m.resolution == 100 && m.colormode == CM_GRAY)
	  {
	    found100g = 1;
	    CHECK (m.timing == 7 && m.motorcurve == 11 && m.ctpc == 2999, "gray 100 dpi mode (ScanGrayMode5)");
	  }
      }
    CHECK (sm == 15, "expected 15 scan modes, got %d", sm);
    CHECK (found100c && found100g, "100 dpi modes missing");
    RTS_Debug->usbtype = USB11;
    CHECK (cfg_scanmode_get (CCD_SENSOR, 0, &m) == OK && m.resolution == 1200, "USB 1.1 uses the same modes");
    RTS_Debug->usbtype = USB20;
  }

  /* motor moves, sensor and motor geometry */
  {
    struct st_motormove mv;
    struct st_sensorcfg sc;
    struct st_motorcfg mc;
    /* head moves: the HP3970 values that parked this scanner correctly
       (the t6 [MotorMove1] values stalled the head under hp3900) */
    CHECK (cfg_motormove_get (CCD_SENSOR, 0, &mv) == OK && mv.systemclock == 2 && mv.ctpc == 6431
	   && mv.scanmotorsteptype == STT_HALF && mv.motorcurve == 1, "park/relocate move");
    CHECK (cfg_motormove_get (CCD_SENSOR, 1, &mv) == OK && mv.systemclock == 2 && mv.ctpc == 2000
	   && mv.motorcurve == -1, "base-speed move");
    CHECK (cfg_sensor_get (&sc) == OK && sc.type == CCD_SENSOR && sc.resolution == 1200
	   && sc.line_distance == 24 && sc.evenodd_distance == 4, "sensor config");
    CHECK (cfg_motor_get (&mc) == OK && mc.resolution == 1200 && mc.basespeedpps == 800
	   && mc.basespeedmotormove == 1, "motor config");
  }

  /* AFE reference voltages, SER/LER, lamp PWM, lineart thresholds */
  {
    SANE_Byte vrts, vrms, vrbs;
    SANE_Int ser, ler;
    cfg_refvoltages_get (CCD_SENSOR, &vrts, &vrms, &vrbs);
    CHECK (vrts == 2 && vrms == 3 && vrbs == 2, "VRTS/VRMS/VRBS %d %d %d", vrts, vrms, vrbs);
    cfg_vrefs_get (CCD_SENSOR, 100, &ser, &ler);
    CHECK (ser == 51 && ler == 35, "SER_100/LER_100 %d %d", ser, ler);
    cfg_vrefs_get (CCD_SENSOR, 1200, &ser, &ler);
    CHECK (ser == 202 && ler == 363, "SER_1200/LER_1200 %d %d", ser, ler);
    CHECK (cfg_fixedpwm_get (CCD_SENSOR, ST_NORMAL) == 0, "fixed PWM");
    CHECK (HP3770_PWM_FREQUENCY == 9, "PWM frequency");
    CHECK (get_value (PLATFORM, BINARYTHRESHOLDH, 0, T_RTINIFILE) == 100
	   && get_value (PLATFORM, BINARYTHRESHOLDL, 0, T_RTINIFILE) == 99, "binary thresholds");
  }

  /* calibration starting values come from the 3770 FitCalibrate */
  {
    static const int options[] = {
      OFFSETEVEN1R, OFFSETODD1R, OFFSETEVEN1G, ADCOFFPREDICTR, PEAKR, MINR,
      GAIN1R, GAINTARGETFACTOR, WSTRIPXPOS, BREFR, OFFSETNSIGMA
    };
    int diffs;
    for (diffs = 0, n = 0; n < (int) (sizeof (options) / sizeof (options[0])); n++)
      if (hp3770_calibreflective (options[n], -12345) != hp3970_calibreflective (options[n], -12345))
	diffs++;
    CHECK (diffs > 0, "calibration values equal HP3970");
    CHECK (hp3770_calibreflective (GAINTARGETFACTOR, 0) == 80, "GainTargetFactor uses first token");
  }

  /* reflective shading cut */
  {
    SANE_Int r, g, b;
    cfg_shading_cut_get (CCD_SENSOR, 8, 300, ST_NORMAL, &r, &g, &b);
    CHECK (r == -6 && g == -6 && b == -6, "reflective shading cut %d %d %d", r, g, b);
  }

  /* lamp stability and auto reference */
  {
    struct st_checkstable cs;
    struct st_autoref ar;
    CHECK (cfg_checkstable_get (FLB_LAMP, &cs) == OK && cs.tottime == 10000, "checkstable");
    cfg_autoref_get (&ar);
    CHECK (ar.type == REF_TAKEFROMSCANNER && ar.offset_x == 88 && ar.offset_y == 650, "autoref");
  }

  if (failures == 0)
    printf ("test_config: all HP3770 configuration checks passed\n");
  return failures == 0 ? 0 : 1;
}
