#!/usr/bin/env bash

set -euo pipefail

PENDANT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_TOOLS_PATH="${PENDANT_ROOT}/.tools/espressif"
export IDF_COMPONENT_CACHE_PATH="${PENDANT_ROOT}/.tools/component-cache"
if [ -x /opt/homebrew/opt/python@3.12/libexec/bin/python3 ]; then
    export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:${PATH}"
fi

# ESP-IDF 5.4's detector accepts Python 3.14, but the checked-in tools were
# installed with Python 3.12. Prefer that managed interpreter when it is
# available so export.sh selects its complete, matching virtual environment.
for PENDANT_PYTHON_BIN in "${PENDANT_ROOT}"/.tools/python/cpython-3.12*/bin; do
    if [ -x "${PENDANT_PYTHON_BIN}/python3" ]; then
        export PATH="${PENDANT_PYTHON_BIN}:${PATH}"
        break
    fi
done
unset PENDANT_PYTHON_BIN

# shellcheck disable=SC1091
source "${PENDANT_ROOT}/.tools/esp-idf/export.sh"
