#!/usr/bin/env python3
"""
Generate backend/hp3770_legacy_tables.c from the original HP ScanJet 3770
Linux backend (libsane-hp3770.so.1.0.13).

The HP/Realtek driver embeds its configuration as INI text blobs in .data
(t_RtIniFile, t_Timing1..12, MotorCurve1..12, FitCalibrate, ...). This tool
reads those blobs from the ELF file and emits the subset of values that the
hp3770 engine (derived from SANE's hp3900 backend) uses where the 3770 driver
differs from the HP ScanJet 3970 configuration:

  * CCD timing tables for both CCD variants (Toshiba TCD2952 = t_, Sony = s_)
  * motor acceleration/deceleration curves 1..8
  * reflective-calibration starting values ([CalibReflective])
  * lamp stability check ([CheckStable]) and auto-reference ([AutoRefPos])

Only the first number of an INI value is used, exactly like the legacy
GetInitializeData() parser (it stops at the first non-digit).

Usage:
  tools/gen_legacy_tables.py LEGACY_SO HP3770_CONFIG_C > backend/hp3770_legacy_tables.c
  tools/gen_legacy_tables.py --dump-ini LEGACY_SO OUTDIR   (write the raw INI blobs)
"""
import os
import re
import struct
import sys


# ---------------------------------------------------------------- ELF access

def elf_objects(blob):
    """Return {name: bytes} for every defined OBJECT symbol in .dynsym."""
    if blob[:4] != b"\x7fELF" or blob[4] != 1:
        raise SystemExit("expected a 32-bit ELF file")
    e_shoff, = struct.unpack_from("<I", blob, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", blob, 0x2E)
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from("<IIIIIIIIII", blob, e_shoff + i * e_shentsize)
        sections.append(sh)  # name,type,flags,addr,offset,size,link,info,align,entsize
    dynsym = [s for s in sections if s[1] == 11]  # SHT_DYNSYM
    if not dynsym:
        raise SystemExit("no .dynsym section")
    sym = dynsym[0]
    strtab = sections[sym[6]]
    objects = {}
    for off in range(sym[4], sym[4] + sym[5], 16):
        st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack_from("<IIIBBH", blob, off)
        if (st_info & 0xF) != 1 or st_shndx == 0 or st_shndx >= len(sections):
            continue
        name_off = strtab[4] + st_name
        name = blob[name_off:blob.index(b"\0", name_off)].decode()
        sec = sections[st_shndx]
        file_off = st_value - sec[3] + sec[4]
        objects[name] = blob[file_off:file_off + st_size]
    return objects


def ini_text(raw):
    return raw.split(b"\0")[0].decode("latin-1").replace("\r\n", "\n").replace("\r", "\n")


class IniSection(dict):
    """Keys are matched case-insensitively, like the legacy StrNiCmp()."""

    def __init__(self):
        super().__init__()
        self.lower = {}

    def __setitem__(self, key, value):
        super().__setitem__(key, value)
        self.lower[key.lower()] = value

    def __contains__(self, key):
        return key.lower() in self.lower

    def __getitem__(self, key):
        return self.lower[key.lower()]

    def get(self, key, default=None):
        return self.lower.get(key.lower(), default)


def parse_ini(text):
    """Ordered list of (section, {key: value}); duplicate sections are kept."""
    out, cur = [], None
    for line in text.split("\n"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = (line[1:-1], IniSection())
            out.append(cur)
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[1][k.strip()] = v.strip()
    return out


def section(ini, name):
    for sec, kv in ini:
        if sec.lower() == name.lower():
            return kv
    raise KeyError(name)


def first_int(value):
    """Mimic the legacy GetInitializeData(): first hex or decimal number."""
    m = re.match(r"\s*(-?0[xX][0-9A-Fa-f]+|-?\d+)", value)
    if not m:
        raise ValueError("no number in %r" % value)
    return int(m.group(1), 0)


# ------------------------------------------------------------- generators

TIMING_KEYS = (["SensorResolution", "CNPP", "CVTRP1", "CVTRP2", "CVTRP3",
                "CVTRW", "CVTRFPW", "CVTRBPW"])


def c_int(v):
    """An INI number for a SANE_Int field. Wider all-ones values (e.g. a
    36-bit CLAMPE of 0xFFFFFFFFF) become -1 like in hp3900's own tables;
    the engine writes only 24 bits of these fields."""
    if v > 0x7FFFFFFF:
        v &= 0xFFFFFFFF
        v -= 1 << 32
    return "%d" % v if v < 0 else "0x%X" % v


def timing_row(kv):
    g = lambda k: first_int(kv.get(k, "0"))
    cph = []
    for p in range(6):
        cph.append("{%d., %d., 0x%02X, 0x%02X, 0x%02X}" % (
            g("CPH%dP1" % p), g("CPH%dP2" % p), g("CPH%dPS" % p),
            g("CPH%dGE" % p), g("CPH%dGO" % p)))
    return ("{0x%04X, 0x%02X, {0x%02X, 0x%02X, 0x%02X}, 0x%02X, 0x%02X, 0x%02X, {%s}, "
            "%s, %s, %s, %s, {0x%02X, 0x%02X}, {0x%02X, 0x%02X}, {0x%02X, 0x%02X}, "
            "{%d., %d.}, 0x%X}" % (
                g("SensorResolution"), g("CNPP"), g("CVTRP1"), g("CVTRP2"), g("CVTRP3"),
                g("CVTRW"), g("CVTRFPW"), g("CVTRBPW"), ", ".join(cph),
                c_int(g("CPHBP2S")), c_int(g("CPHBP2E")), c_int(g("CLAMPS")), c_int(g("CLAMPE")),
                g("CDSS1"), g("CDSS2"), g("CDSC1"), g("CDSC2"),
                g("CDSCS1"), g("CDSCS2"), g("ADCCLKP1"), g("ADCCLKP2"), g("ADCCLKP2E")))


def gen_timings(objs, prefix, cname):
    rows = []
    for i in range(1, 13):
        kv = section(parse_ini(ini_text(objs["%s_Timing%d" % (prefix, i)])), "Timing")
        rows.append("\t/* %s_Timing%-2d (%4d dpi) */ %s" % (
            prefix, i, first_int(kv["SensorResolution"]), timing_row(kv)))
    return "static const struct st_timing %s[12] =\n{\n%s\n};\n" % (cname, ",\n".join(rows))


CURVE_TYPES = [("ACC", "ACC_CURVE", [("NormalScan", "CRV_NORMALSCAN"), ("ParkHome", "CRV_PARKHOME"),
                                     ("Smearing", "CRV_SMEARING")]),
               ("DEC", "DEC_CURVE", [("NormalScan", "CRV_NORMALSCAN"), ("ParkHome", "CRV_PARKHOME"),
                                     ("Smearing", "CRV_SMEARING"), ("ScanBufferFull", "CRV_BUFFERFULL")])]


def gen_motorcurves(objs, count):
    out = ["static const SANE_Int hp3770_motorcurves[] =", "{"]
    for n in range(1, count + 1):
        ini = parse_ini(ini_text(objs["MotorCurve%d" % n]))
        ms = section(ini, "MotorSetting")
        out.append("\t/* MotorCurve%d */" % n)
        out.append("\t%d, %d, %d, 0, /* mri, msi, skiplinecount, motorbackstep */" % (
            first_int(ms.get("MRI", "1")), first_int(ms.get("MSI", "1")),
            first_int(ms.get("SkipLineCount", "1"))))
        for sec, kind, kinds in CURVE_TYPES:
            mapping = section(ini, sec)
            for key, crv in kinds:
                if key not in mapping:
                    continue
                cname = ("AccCurve" if sec == "ACC" else "DecCurve") + str(first_int(mapping[key]))
                steps_kv = section(ini, cname)
                count_steps = first_int(steps_kv["Count"])
                steps = [first_int(steps_kv["Step%d" % k]) for k in range(1, count_steps + 1)]
                if any(s <= 0 for s in steps):
                    raise SystemExit("non-positive step in MotorCurve%d %s" % (n, cname))
                out.append("\t%s, %s, %s, 0," % (kind, crv, ", ".join(map(str, steps))))
        out.append("\t%s," % ("-2" if n < count else "-1"))
    out.append("};")
    return "\n".join(out) + "\n"


def norm(key):
    return re.sub(r"[^A-Z0-9]", "", key.upper())


def gen_calibreflective(objs, config_c):
    src = open(config_c).read()
    m = re.search(r"static int hp3970_calibreflective\(int option, int defvalue\)\n\{(.*?)\n\}\n",
                  src, re.S)
    if not m:
        raise SystemExit("hp3970_calibreflective() not found in %s" % config_c)
    names = re.findall(r"case (\w+):", m.group(1))
    calib = {norm(k): v for k, v in section(parse_ini(ini_text(objs["FitCalibrate"])),
                                            "CalibReflective").items()}
    lines = ["/* [CalibReflective] values of the 3770 driver for every option the",
             "   engine knows; options absent from the 3770 INI keep the HP3970 value. */",
             "static int hp3770_calibreflective(int option, int defvalue)", "{",
             "\tint rst = hp3970_calibreflective(option, defvalue);", "", "\tswitch(option)", "\t{"]
    used = 0
    for name in names:
        if norm(name) in calib:
            lines.append("\t\tcase %s: rst = %d; break;" % (name, first_int(calib[norm(name)])))
            used += 1
    lines += ["\t}", "", "\treturn rst;", "}"]
    return "\n".join(lines) + "\n", used, len(names)


def gen_shading_cut(objs, ccd="T6"):
    """Reflective shading cut ([CalibReflective] U2_<ccd>_<depth>B_<res/100>D_Cut_<c>).
    The T6 profile has no 2400 dpi entry; 2400 reuses the 1200 dpi values."""
    calib = section(parse_ini(ini_text(objs["FitCalibrate"])), "CalibReflective")
    rows = []
    for depth in (8, 16):
        for res in (2400, 1200, 600, 300, 200, 100):
            key = lambda c, r=res: "U2_%s_%dB_%dD_Cut_%s" % (ccd, depth, r // 100, c)
            if key("R") not in calib:
                key = lambda c: "U2_%s_%dB_%dD_Cut_%s" % (ccd, depth, 12, c)
            vals = [first_int(calib[key(c)]) for c in "RGB"]
            rows.append("\t{%2d, %4d, {%d, %d, %d}}" % (depth, res, vals[0], vals[1], vals[2]))
    return ("/* [CalibReflective] U2_%s_*_Cut: reflective white-shading cut, USB 2 */\n"
            "static const struct { SANE_Int depth, res, cut[3]; } hp3770_reflective_cut[] =\n{\n%s\n};\n\n"
            % (ccd, ",\n".join(rows)))


def gen_gain_offset(rt):
    """Fixed AFE settings for scans without hardware calibration.
    Offsets: [SCAN_CALI] DCOffsetEven1/Odd1 (coarse) and DCOffsetEven2/Odd2
    (fine), the profile's typical calibrated offsets. PAG: [SCAN_CALI] PGA.
    VGA: [SCAN_PARAM] VGAG1n/VGAG2n, the gains HP's driver programs before
    it calibrates (e814-e819 = VRxS | 4 in its traces)."""
    sc = section(rt, "SCAN_CALI")
    sp = section(rt, "SCAN_PARAM")
    c = lambda fmt: ", ".join(str(first_int(sc[fmt % ch])) for ch in (1, 2, 3))
    p = lambda fmt: ", ".join(str(first_int(sp[fmt % ch])) for ch in (1, 2, 3))
    return ("/* AFE without hardware calibration: {edcg1}, {edcg2}, {odcg1}, {odcg2},\n"
            "   {pag}, {vgag1}, {vgag2} from [SCAN_CALI] DCOffsetN and PGA, [SCAN_PARAM] VGAGnn.\n"
            "   Used for the white-shading pass and the scan alike. */\n"
            "static const struct st_gain_offset hp3770_gain_offset =\n"
            "\t{{%s}, {%s}, {%s}, {%s}, {%s}, {%s}, {%s}};\n\n" % (
                c("DCOffsetEven1[%d]"), c("DCOffsetEven2[%d]"), c("DCOffsetOdd1[%d]"),
                c("DCOffsetOdd2[%d]"), c("PGA[%d]"), p("VGAG1%d"), p("VGAG2%d")))


def gen_scanmodes(rt):
    """Reflective scan modes ([ScanColorModeN], [ScanGrayModeN]) in hp3900's
    struct st_scanmode order. Timing and MotorCurve are 1-based in the INI
    (MotorCurve 0 = none); hp3900 indexes from 0 (-1 = none). Lineart uses
    the gray modes, as hp3900 does for the HP3970."""
    plat = section(rt, "PlatForm")
    rows = []
    for cm, inikey, rate in (("CM_COLOR", "Color", "PIXEL_RATE"), ("CM_GRAY", "Gray", "LINE_RATE"),
                             ("CM_LINEART", "Gray", "LINE_RATE")):
        count = first_int(plat["Scan%sCount" % inikey])
        for n in range(1, count + 1):
            res = first_int(plat["Scan%sMode%d" % (inikey, n)])
            m = section(rt, "Scan%sMode%d" % (inikey, n))
            g = lambda k, d="0": first_int(m.get(k, d))
            if g("SampleRate") != (0 if cm == "CM_COLOR" else 1):
                raise SystemExit("unexpected SampleRate in Scan%sMode%d" % (inikey, n))
            rows.append("\t{ST_NORMAL, %-10s, %4d, 0x%02x, %3d, %s, 0x%02x, %5d, %5d, %d, 0x%02x, "
                        "{0, 0, 0}, {0, 0, 0}, 0, 1, 1, 0x01, 0x01, 0x10, 0x02, 0x02, 0x00}" % (
                            cm, res, g("Timing") - 1, g("MotorCurve") - 1, rate, g("SystemClock"),
                            g("CTPC"), g("MotorBackStep"), g("ScanMotorStepType"), g("DummyLine")))
    return ("/* USB 2 reflective scan modes: {scantype, colormode, res, timing, curve, samplerate,\n"
            "   clock, ctpc, backstep, steptype, dummyline, expt, mexpt, motorplus, mexpt16,\n"
            "   mexptfull, mexposure, mri, msi, mmtir, mmtirh, skips} */\n"
            "static const struct st_scanmode hp3770_scanmodes[] =\n{\n%s\n};\n\n" % ",\n".join(rows))


def gen_motormoves(rt):
    count = first_int(section(rt, "PlatForm")["MotorMoveCount"])
    rows = []
    for n in range(1, count + 1):
        m = section(rt, "MotorMove%d" % n)
        rows.append("\t{0x%02x, %d, %d, %d}" % (first_int(m["SystemClock"]), first_int(m["CTPC"]),
                                                first_int(m["ScanMotorStepType"]), first_int(m["MotorCurve"]) - 1))
    return ("/* [MotorMoveN]: {systemclock, ctpc, steptype, motorcurve} */\n"
            "static const struct st_motormove hp3770_motormoves[] =\n{\n%s\n};\n\n" % ",\n".join(rows))


def gen_profile(objs, rt, ccd_prefix):
    """Scalar parameters of the selected CCD profile."""
    sp, plat = section(rt, "SCAN_PARAM"), section(rt, "PlatForm")
    mm = section(rt, "MoveMotorSetting")
    fitp = section(parse_ini(ini_text(objs["FitCalibrate"])), "PLATFORM")
    v = lambda d, k, dflt="0": first_int(d.get(k, dflt))
    out = []
    out.append("#define HP3770_SENSOR_RESOLUTION %d" % v(plat, "RealSensorResolution"))
    out.append("#define HP3770_LINE_DISTANCE %d" % v(plat, "SensorLineDistance"))
    out.append("#define HP3770_EVENODD_DISTANCE %d" % v(plat, "SensorEvenOddDistance"))
    out.append("#define HP3770_MOTOR_RESOLUTION %d" % v(plat, "MotorResolution"))
    out.append("#define HP3770_BASESPEED_PPS %d" % v(mm, "BaseSpeedPPS"))
    out.append("#define HP3770_BASESPEED_MOVE %d" % (v(mm, "BaseSpeedMotorMove") - 1))
    out.append("#define HP3770_HIGHSPEED_MOVE %d" % (v(mm, "HighSpeedMotorMove") - 1))
    out.append("#define HP3770_PARKHOME_MOVE %d" % (v(mm, "ParkHomeMotorMove") - 1))
    out.append("#define HP3770_CHANGE_MOTOR_CURRENT %d" % v(mm, "ChangeMotorCurrent"))
    out.append("#define HP3770_VRTS %d\n#define HP3770_VRMS %d\n#define HP3770_VRBS %d" % (
        v(sp, "VRTS"), v(sp, "VRMS"), v(sp, "VRBS")))
    out.append("#define HP3770_FIXED_PWM %d" % v(fitp, "%s_USB2FixedPwm" % ccd_prefix))
    out.append("#define HP3770_PWM_FREQUENCY %d" % v(sp, "PwmLampFrequencyFLB1200"))
    out.append("#define HP3770_BINARY_THRESHOLD_H %d" % v(plat, "BinaryThresholdH"))
    out.append("#define HP3770_BINARY_THRESHOLD_L %d" % v(plat, "BinaryThresholdL"))
    rows = []
    for res in (100, 200, 300, 600, 1200, 2400):
        if "SER_%d" % res in sp:
            rows.append("\t{%4d, %4d, %4d}" % (res, v(sp, "SER_%d" % res), v(sp, "LER_%d" % res)))
    out.append("\n/* [SCAN_PARAM] SER_/LER_: {resolution, ser, ler} */\n"
               "static const SANE_Int hp3770_vrefs[][3] =\n{\n%s\n};" % ",\n".join(rows))
    return "\n".join(out) + "\n\n"


def gen_platform(objs):
    fit = parse_ini(ini_text(objs["FitCalibrate"]))
    cs = section(fit, "CheckStable")
    ar = section(fit, "AutoRefPos")
    v = lambda d, k: first_int(d[k])
    return ("/* [CheckStable]: lamp stability check {diff, interval, total time} */\n"
            "static const struct st_checkstable hp3770_checkstable_preview = {%d., %d, %d};\n"
            "static const struct st_checkstable hp3770_checkstable_flb = {%d., %d, %d};\n"
            "static const struct st_checkstable hp3770_checkstable_tma = {%d., %d, %d};\n\n"
            "/* [AutoRefPos]: SEROffset, LEROffset, ExternBoundary (2400 dpi units, as used by Refs_Set) */\n"
            "#define HP3770_AUTOREF_SER %d\n#define HP3770_AUTOREF_LER %d\n"
            "#define HP3770_AUTOREF_BOUNDARY %d\n" % (
                v(cs, "Preview_Diff"), v(cs, "Preview_Interval"), v(cs, "Preview_TotTime"),
                v(cs, "Flb_Diff"), v(cs, "Flb_Interval"), v(cs, "Flb_TotTime"),
                v(cs, "Tma_Diff"), v(cs, "Tma_Interval"), v(cs, "Tma_TotTime"),
                v(ar, "SEROffset"), v(ar, "LEROffset"), v(ar, "ExternBoundary")))


def main(argv):
    if len(argv) == 4 and argv[1] == "--dump-ini":
        objs = elf_objects(open(argv[2], "rb").read())
        os.makedirs(argv[3], exist_ok=True)
        for name, raw in sorted(objs.items()):
            if re.match(r"^((s|t|t6)_(RtIniFile|USB1IniFile|Timing\d+)|MotorCurve\d+|FitCalibrate)$", name):
                with open(os.path.join(argv[3], name + ".ini"), "w") as f:
                    f.write(ini_text(raw))
        return 0
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 64
    objs = elf_objects(open(argv[1], "rb").read())
    pid = section(parse_ini(ini_text(objs["FitCalibrate"])), "PLATFORM").get("PID&VID", "")
    if first_int(pid) != 0x03F02505:
        raise SystemExit("this is not the HP ScanJet 3770 driver (PID&VID=%s)" % pid)
    fit_platform = section(parse_ini(ini_text(objs["FitCalibrate"])), "PLATFORM")
    ccd_type = first_int(fit_platform["CCDType"])
    # CFitApi::OpenDeviceDriver: CCDType from [PLATFORM] (>= 0) overrides GPIO
    # detection; Ke_InitProfileName maps 0/1/2 to the s_/t_/t6_ files.
    prefix = {0: "s", 1: "t", 2: "t6"}.get(ccd_type)
    if prefix is None:
        raise SystemExit("unsupported CCDType %d" % ccd_type)
    rt = parse_ini(ini_text(objs["%s_RtIniFile" % prefix]))
    motor_count = first_int(section(rt, "PlatForm")["MotorCount"])
    calib, used, total = gen_calibreflective(objs, argv[2])
    sys.stdout.write(
        "/* GENERATED FILE - do not edit.\n"
        "   Source: HP ScanJet 3770 Linux backend libsane-hp3770.so.1.0.13 (embedded INI data)\n"
        "   Generator: tools/gen_legacy_tables.py\n"
        "   Hardware parameters extracted for interoperability (see docs/porting-notes.md).\n"
        "   CCD profile: FitCalibrate [PLATFORM] CCDType=%d -> %s_RtIniFile / %s_Timing1..12.\n"
        "   CalibReflective: %d of %d engine options taken from the 3770 INI. */\n\n"
        % (ccd_type, prefix, prefix, used, total))
    sys.stdout.write("#define HP3770_CCD_PROFILE \"%s\"\n\n" % prefix)
    sys.stdout.write(gen_profile(objs, rt, prefix.upper()))
    sys.stdout.write("/* %s_Timing1..12: CCD timing (index = TimingN - 1) */\n" % prefix)
    sys.stdout.write(gen_timings(objs, prefix, "hp3770_timing") + "\n")
    sys.stdout.write(gen_scanmodes(rt))
    sys.stdout.write(gen_motormoves(rt))
    sys.stdout.write(gen_motorcurves(objs, motor_count) + "\n")
    sys.stdout.write(calib + "\n")
    sys.stdout.write(gen_shading_cut(objs, prefix.upper()))
    sys.stdout.write(gen_gain_offset(rt))
    sys.stdout.write(gen_platform(objs))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
