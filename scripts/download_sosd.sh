#!/usr/bin/env bash
# Download SOSD datasets (Marcus et al., VLDB '20) into data/.
#
# All four come from the SOSD Harvard Dataverse (doi:10.7910/DVN/JGVF9A) as
# 200M-key uint64 files. Each download is checked against the Dataverse's MD5
# of the compressed file before it is decompressed (zstd also verifies its own
# frame checksums). Needs curl and zstd. Uncompressed, each file is 1.6 GB.
#
#   scripts/download_sosd.sh                 # books fb osm wiki
#   scripts/download_sosd.sh fb wiki         # a subset
set -euo pipefail
cd "$(dirname "$0")/../data"

DV="https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A"
# name -> "file dataverse-id md5-of-.zst" (a case statement, since macOS
# ships bash 3.2 without associative arrays)
spec() {
  case $1 in
    books) echo "books_200M_uint64 A6HDNT cd1f8bcb0dfd36f9ab08d160b887bf8a" ;;
    fb) echo "fb_200M_uint64 EATHF7 fec241e8b021b198b0849fbd5564c05f" ;;
    osm) echo "osm_cellids_200M_uint64 8FX9BV 42575cb58f24bb7ea0a623d422d4c9a6" ;;
    wiki) echo "wiki_ts_200M_uint64 SVN8PI 6a2b17020959084ce2640177ee4afd5e" ;;
    *) return 1 ;;
  esac
}

md5_of() { if command -v md5sum >/dev/null; then md5sum "$1" | cut -d' ' -f1; else md5 -q "$1"; fi; }

[[ $# -gt 0 ]] || set -- books fb osm wiki
for name in "$@"; do
  info=$(spec "$name") || { echo "unknown dataset: $name (books fb osm wiki)" >&2; exit 1; }
  read -r file id md5 <<< "$info"
  if [[ -f $file && -f $file.ok ]]; then
    echo "$file: already present"
    continue
  fi
  echo "$file: downloading"
  curl -L --fail --retry 5 --retry-delay 10 -o "$file.zst" "$DV/$id"
  got=$(md5_of "$file.zst")
  if [[ $got != "$md5" ]]; then
    echo "$file: checksum mismatch (got $got)" >&2
    rm -f "$file.zst"
    exit 1
  fi
  zstd -d -f --rm "$file.zst" -o "$file"
  touch "$file.ok"
  echo "$file: ok"
done
