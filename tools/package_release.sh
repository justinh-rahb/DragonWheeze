#!/usr/bin/env bash
# package_release.sh — assemble reproducible DragonWheeze release artifacts
set -euo pipefail

VERSION="${VERSION:?set VERSION (e.g. v1.0.0)}"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-dist}"
SOURCE_SHA="${SOURCE_SHA:-$(git rev-parse HEAD)}"
TARGET="${TARGET:-esp32c3}"
BOARD="${BOARD:-Sovol SH01 Filament Dryer (ESP32-C3 Super Mini)}"
IDF_VERSION="${IDF_VERSION:-v5.3}"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

esptool() { if command -v esptool.py >/dev/null; then esptool.py "$@"; else python3 -m esptool "$@"; fi; }

if [ -f main/dev_config.h ]; then
    echo "REFUSING to package: main/dev_config.h present — credentials would be baked into release." >&2
    exit 1
fi

for f in dragonwheeze.bin bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin flash_args; do
    [ -f "$BUILD_DIR/$f" ] || { echo "missing $BUILD_DIR/$f — run idf.py build first" >&2; exit 1; }
done

rm -rf "$OUT" bundle-stage
mkdir -p "$OUT" bundle-stage

APP="dragonwheeze-${VERSION}.bin"
FACTORY="dragonwheeze-${VERSION}-factory.bin"
BUNDLE="dragonwheeze-${VERSION}-install-bundle.zip"

# 1. OTA application image
cp "$BUILD_DIR/dragonwheeze.bin" "$OUT/$APP"

# 2. Merged factory image, flash @ 0x0
( cd "$BUILD_DIR" && esptool --chip "$TARGET" merge_bin -o "$OLDPWD/$OUT/$FACTORY" @flash_args >/dev/null )

# 3. Stage bundle
mkdir -p bundle-stage/bootloader bundle-stage/partition_table
cp "$BUILD_DIR/bootloader/bootloader.bin"           bundle-stage/bootloader/
cp "$BUILD_DIR/partition_table/partition-table.bin" bundle-stage/partition_table/
cp "$BUILD_DIR/ota_data_initial.bin" "$BUILD_DIR/dragonwheeze.bin" "$OUT/$FACTORY" bundle-stage/

cat > bundle-stage/FLASHING.txt <<EOF
DragonWheeze ${VERSION} — Flashing (${TARGET})

1. OTA Update: Upload ${APP} via device Web UI (/fw) or dc_portal.
2. Factory USB Flash:
   esptool.py --chip ${TARGET} write_flash 0x0 ${FACTORY}

Verify SHA256 checksums before flashing.
EOF

# 4. Manifest
export VERSION SOURCE_SHA IDF_VERSION TARGET BOARD BUILT_AT
python3 - "$OUT/$APP" "$OUT/$FACTORY" \
    "$BUILD_DIR/bootloader/bootloader.bin" \
    "$BUILD_DIR/partition_table/partition-table.bin" \
    "$BUILD_DIR/ota_data_initial.bin" <<PY > "$OUT/manifest.json"
import hashlib, json, os, sys
def h(p):
    with open(p,'rb') as f: return hashlib.sha256(f.read()).hexdigest()
arts=[{"name":os.path.basename(p),"size":os.path.getsize(p),"sha256":h(p)} for p in sys.argv[1:]]
print(json.dumps({
    "product":"dragonwheeze","version":os.environ["VERSION"],
    "source_sha":os.environ["SOURCE_SHA"],
    "idf_version":os.environ["IDF_VERSION"],
    "target":os.environ["TARGET"],"board":os.environ["BOARD"],
    "shared_core":{"source":"github.com/justinh-rahb/dragon-core","components":["dc_evlog","dc_source","dc_wifi","dc_ui","dc_mqtt","dc_portal"]},
    "built_at_utc":os.environ["BUILT_AT"],
    "artifacts":arts,
}, indent=2))
PY
cp "$OUT/manifest.json" bundle-stage/

# 5. Zip bundle
( cd bundle-stage && python3 -m zipfile -c "$OLDPWD/$OUT/$BUNDLE" * )
rm -rf bundle-stage

# 6. Checksums
( cd "$OUT" && sha256sum "$APP" "$FACTORY" "$BUNDLE" manifest.json > SHA256SUMS.txt )

echo ">> Release artifacts packaged in $OUT/:"
( cd "$OUT" && ls -la && echo && cat SHA256SUMS.txt )
