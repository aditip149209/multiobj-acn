#!/usr/bin/env bash
# Copies the MultiObj scheduler into contrib/nr, registers it in CMakeLists.txt, generates the TDMA twin,
# and installs the scenario into scratch/.   usage: ./apply_patch.sh /path/to/ns-3-dev
set -euo pipefail
NS3=${1:?usage: apply_patch.sh <ns3 dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
NR=$NS3/contrib/nr
[ -d "$NR/model" ] || { echo "contrib/nr not found in $NS3"; exit 1; }
cp "$HERE"/nr-patch/nr-mac-scheduler-ofdma-multiobj.{h,cc} "$NR/model/"
for ext in h cc; do   # TDMA twin: identical logic, TDMA base class
  sed 's/Ofdma/Tdma/g; s/OFDMA/TDMA/g; s/ofdma/tdma/g' "$HERE/nr-patch/nr-mac-scheduler-ofdma-multiobj.$ext" > "$NR/model/nr-mac-scheduler-tdma-multiobj.$ext"
done
CM=$NR/CMakeLists.txt
grep -q multiobj "$CM" || {
  sed -i '0,/model\/nr-mac-scheduler-ofdma-qos.cc/s##model/nr-mac-scheduler-ofdma-qos.cc\n        model/nr-mac-scheduler-ofdma-multiobj.cc\n        model/nr-mac-scheduler-tdma-multiobj.cc#' "$CM"
  sed -i '0,/model\/nr-mac-scheduler-ofdma-qos.h/s##model/nr-mac-scheduler-ofdma-qos.h\n        model/nr-mac-scheduler-ofdma-multiobj.h\n        model/nr-mac-scheduler-tdma-multiobj.h#' "$CM"
}
grep -n multiobj "$CM" || { echo "CMake edit failed: add the four multiobj files next to nr-mac-scheduler-ofdma-qos.{cc,h} by hand"; exit 1; }
cp "$HERE"/scratch/nr-multiobj-scenario.cc "$NS3/scratch/"
echo "patched. Now: cd $NS3 && ./ns3 build"
