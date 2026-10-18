#!/bin/sh
# Runs a short series of diagnostic scans and collects images and logs in
# one folder. Put a printed page face down in the top-left corner of the
# glass, close the lid, then run from the project folder:
#
#   sh tools/diag-scans.sh
#
# Takes about 10-15 minutes (every run warms up the lamp and calibrates).

set -u
cd "$(dirname "$0")/.." || exit 1
[ -x ./hp3770-scan ] || { echo "build first: make"; exit 1; }

out="diag-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "Writing results to $out/"

run() {
    name="$1"; shift
    echo "== $name: $*"
    start=$(date +%s)
    SANE_DEBUG_HP3770=2 HP3770_USB_TRACE="$out/$name.trace" \
        env "$@" > "$out/$name.out" 2> "$out/$name.log"
    echo "   exit $? after $(( $(date +%s) - start )) s" | tee -a "$out/$name.out"
}

area="-x 60 -y 60"

run 1-color100        ./hp3770-scan -f -m color -r 100 $area -o "$out/1-color100.tif"
run 2-gray100         ./hp3770-scan -f -m gray  -r 100 $area -o "$out/2-gray100.tif"
run 3-color300        ./hp3770-scan -f -m color -r 300 $area -o "$out/3-color300.tif"
run 4-raw100          ./hp3770-scan -f -m color -r 100 $area --set opt_nowshading=yes -o "$out/4-raw100.tif"
run 5-chipgray100     ./hp3770-scan -f -m gray  -r 100 $area --set opt_emulategray=no -o "$out/5-chipgray100.tif"
run 6-afe-hp3970      HP3770_AFE=hp3970 ./hp3770-scan -f -m color -r 100 $area -o "$out/6-afe-hp3970.tif"
run 7-color600        ./hp3770-scan -f -m color -r 600 -x 25 -y 25 -o "$out/7-color600.tif"

# keep the upload small: compress the traces
gzip -9 "$out"/*.trace 2>/dev/null
echo "Done. Please send the folder $out (zip it with: zip -r $out.zip $out)"
