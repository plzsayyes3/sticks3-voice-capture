#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <serial-port> [output-dir]" >&2
  echo "Example: $0 /dev/cu.usbmodem1101 ./recording-dump" >&2
  exit 2
fi

PORT="$1"
OUT_DIR="${2:-./recording-dump}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PARTITIONS="$REPO_ROOT/firmware/partitions_ota.csv"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set. Run this after exporting the ESP-IDF v5.5.1 environment." >&2
  exit 1
fi

PARSER="$IDF_PATH/components/fatfs/fatfsparse.py"
if [[ ! -f "$PARSER" ]]; then
  echo "Cannot find ESP-IDF FatFS parser: $PARSER" >&2
  exit 1
fi

read -r OFFSET SIZE < <(
  python3 - "$PARTITIONS" <<'PY'
import csv
import sys

path = sys.argv[1]
with open(path, newline="", encoding="utf-8") as f:
    for row in csv.reader(line for line in f if not line.lstrip().startswith("#")):
        if not row:
            continue
        fields = [field.strip() for field in row]
        if fields[0] == "storage":
            print(fields[3], fields[4])
            break
    else:
        raise SystemExit("storage partition not found")
PY
)

mkdir -p "$OUT_DIR"
OUT_DIR_ABS="$(cd "$OUT_DIR" && pwd)"
RAW_IMAGE="$OUT_DIR_ABS/storage.bin"
EXTRACT_DIR="$OUT_DIR_ABS/extracted"

if command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=(esptool.py)
else
  ESPTOOL=(python3 -m esptool)
fi

echo "Reading storage partition from $PORT"
echo "  offset: $OFFSET"
echo "  size:   $SIZE"
"${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" read_flash "$OFFSET" "$SIZE" "$RAW_IMAGE"

rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
(
  cd "$EXTRACT_DIR"
  python3 "$PARSER" --wl-layer enabled "$RAW_IMAGE"
)

echo
echo "Extracted recording files:"
find "$EXTRACT_DIR" -type f \( -name '*.ogg' -o -name '*.part' \) -print | sort

echo
echo "Raw image: $RAW_IMAGE"
echo "Parsed files: $EXTRACT_DIR"
