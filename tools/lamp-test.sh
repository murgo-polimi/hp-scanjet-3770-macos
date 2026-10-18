#!/bin/sh
# Lamp ripple test: five small 300 dpi colour scans of plain white paper
# with different lamp PWM settings. Put a blank white sheet on the glass
# (it must cover the top-left corner), close the lid, then run from the
# project folder:
#
#   sh tools/lamp-test.sh
#
# Takes about 4 minutes. If you can, run it a second time with the room
# lights switched off:  sh tools/lamp-test.sh dark

set -u
cd "$(dirname "$0")/.." || exit 1
[ -x ./hp3770-scan ] || { echo "build first: make"; exit 1; }

out="lamp-$(date +%Y%m%d-%H%M%S)${1:+-$1}"
mkdir -p "$out"
echo "Writing results to $out/"

run() {
    name="$1"; shift
    echo "== $name: $*"
    start=$(date +%s)
    SANE_DEBUG_HP3770=2 env "$@" ./hp3770-scan -f -m color -r 300 -l 20 -t 20 -x 30 -y 30 \
        -o "$out/$name.tif" > "$out/$name.out" 2> "$out/$name.log"
    echo "   exit $? after $(( $(date +%s) - start )) s" | tee -a "$out/$name.out"
}

run 1-default     HP3770_DUMMY=1
run 2-pwm-off     HP3770_LAMP_PWM=off
run 3-pwm-63      HP3770_LAMP_PWM=63
run 4-freq0       HP3770_PWM_FREQ=0
run 5-freq15      HP3770_PWM_FREQ=15

gzip -9 "$out"/*.log 2>/dev/null
echo "Done. The folder is $out"
