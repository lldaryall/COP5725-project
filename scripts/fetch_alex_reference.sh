#!/usr/bin/env bash
# Clone the authors' reference ALEX (MIT license) into third_party/ALEX for
# validation runs (`make reference`). Pinned to the commit we validated against.
set -euo pipefail
cd "$(dirname "$0")/../third_party"
COMMIT=4370da6aa8b509fdc9b0d2c49faa0624b0078589
if [[ ! -d ALEX ]]; then
  git clone https://github.com/microsoft/ALEX.git
fi
git -C ALEX checkout -q "$COMMIT"
echo "third_party/ALEX at $COMMIT"
