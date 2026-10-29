#!/usr/bin/env bash
# Fresh Ubuntu 22.04 / 24.04 (or WSL2) -> working ns-3 + 5G-LENA (nr) + MultiObj scheduler.
#   ./setup.sh                      # latest nr release, matching ns-3 release
#   NR_TAG=v3.2 ./setup.sh          # pin a specific nr tag
#   NS3_DIR=$HOME/ns-3-dev ./setup.sh
# Needs access to gitlab.com (nr and ns-3-dev are hosted there) and the apt/pip registries.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NS3_DIR=${NS3_DIR:-$HOME/ns-3-dev}
NR_URL=https://gitlab.com/cttc-lena/nr.git
NS3_URL=https://gitlab.com/nsnam/ns-3-dev.git

echo "== 1/6 apt dependencies"
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build ccache git pkg-config python3 python3-pip python3-venv \
     libsqlite3-dev libeigen3-dev libxml2-dev libc6-dev libgsl-dev libboost-all-dev

echo "== 2/6 clone nr and pick the ns-3 release it supports"
TMP=$(mktemp -d)
git clone "$NR_URL" "$TMP/nr"
if [ -z "${NR_TAG:-}" ]; then
  NR_TAG=$(git -C "$TMP/nr" tag --list | grep -E '^(v|nr-)?[0-9]+\.[0-9]+(\.[0-9]+)?$' | sort -V | tail -1)
fi
git -C "$TMP/nr" checkout "$NR_TAG"
# nr's README / CHANGES state the supported ns-3 release (e.g. "ns-3.44"). Override with NS3_TAG if the grep is wrong.
NS3_TAG=${NS3_TAG:-$(grep -ohE 'ns-3\.[0-9]+' "$TMP/nr/README.md" "$TMP/nr/CHANGES.md" 2>/dev/null | sort -V | tail -1)}
[ -n "$NS3_TAG" ] || { echo "could not infer ns-3 release from nr docs: set NS3_TAG=ns-3.XX"; exit 1; }
echo "   nr tag: $NR_TAG   ns-3 tag: $NS3_TAG"

echo "== 3/6 clone ns-3 $NS3_TAG and install nr into contrib/"
[ -d "$NS3_DIR" ] || git clone --branch "$NS3_TAG" --depth 1 "$NS3_URL" "$NS3_DIR"
rm -rf "$NS3_DIR/contrib/nr"; mv "$TMP/nr" "$NS3_DIR/contrib/nr"

echo "== 4/6 verify nr API symbols used by the scheduler / scenario"
"$HERE/verify_api.sh" "$NS3_DIR" | tee "$HERE/verify_api.log"
if grep -q MISSING "$HERE/verify_api.log"; then
  echo "!! Some symbols are missing in this nr release. Edit nr-patch/* and scratch/* accordingly, then re-run:"
  echo "   $HERE/apply_patch.sh $NS3_DIR && cd $NS3_DIR && ./ns3 build"; exit 2
fi

echo "== 5/6 apply patch, configure (optimized), build"
"$HERE/apply_patch.sh" "$NS3_DIR"
cd "$NS3_DIR"
./ns3 configure -d optimized --enable-examples --enable-tests -G Ninja
./ns3 build

echo "== 6/6 sanity check with a stock nr example, then Python deps"
./ns3 run --no-build "cttc-nr-demo --simTime=0.5" 2>&1 | tail -15
python3 -m pip install --user --break-system-packages pandas numpy matplotlib seaborn pyyaml
echo "ns-3 ready in $NS3_DIR. Smoke test:"
echo "  cd $HERE/.. && python3 run_experiments.py --backend ns3 --ns3dir $NS3_DIR --mode smoke"
