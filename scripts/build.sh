#!/usr/bin/env bash

set -euo pipefail

PENDANT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# shellcheck disable=SC1091
source "${PENDANT_ROOT}/scripts/idf-env.sh"
idf.py -C "${PENDANT_ROOT}/firmware" build

