#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /dev/ttyUSB0" >&2
    exit 2
fi

PENDANT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PENDANT_PORT="$1"

# shellcheck disable=SC1091
source "${PENDANT_ROOT}/scripts/idf-env.sh"
idf.py -C "${PENDANT_ROOT}/firmware" -p "${PENDANT_PORT}" flash
