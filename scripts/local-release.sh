#!/usr/bin/env bash

# Build the OTA image and full USB-install image locally, then publish the same
# assets produced by the CI release workflow. A marker in the annotated tag tells that workflow
# to skip its own build and avoids two publishers racing for one release.
set -euo pipefail

version="${1:-}"
if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf '%s\n' 'Usage: make local-release VERSION=MAJOR.MINOR.PATCH' >&2
    exit 2
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    printf '%s\n' 'Working tree is not clean; commit or stash changes first.' >&2
    exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
    printf '%s\n' 'GitHub CLI is required. Install gh and run: gh auth login' >&2
    exit 1
fi
gh auth status >/dev/null

tag="v${version}"
if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null || \
   git ls-remote --exit-code --tags origin "refs/tags/${tag}" >/dev/null 2>&1; then
    printf 'Tag %s already exists.\n' "$tag" >&2
    exit 1
fi

release_dir="$(mktemp -d)"
version_backup="$(mktemp)"
restore_version() {
    if [[ -n "$version_backup" && -f "$version_backup" ]]; then
        cp "$version_backup" firmware/version.txt
        rm -f "$version_backup"
    fi
    rm -rf "$release_dir"
}
trap restore_version EXIT

cp firmware/version.txt "$version_backup"
printf '%s\n' "$version" > firmware/version.txt

# Keep the source commit available on main before the release tag is pushed,
# matching the standard make release workflow.
git push origin HEAD:main

idf_python="${PWD}/.tools/espressif/python_env/idf5.4_py3.12_env/bin/python"
if [ ! -x "$idf_python" ]; then
    printf '%s\n' 'Installing the missing ESP-IDF Python 3.12 environment...'
    PATH="/opt/homebrew/opt/python@3.12/libexec/bin:${PATH}" \
        IDF_TOOLS_PATH="${PWD}/.tools/espressif" \
        .tools/esp-idf/install.sh esp32s3
fi

# shellcheck disable=SC1091
source scripts/idf-env.sh
idf.py -C firmware build

binary="${release_dir}/klipper-pendant-onx3248g035.bin"
installer_binary="${release_dir}/klipper-pendant-onx3248g035-install.bin"
cp firmware/build/klipper-pendant.bin "$binary"
esptool.py --chip esp32s3 merge_bin \
    --output "$installer_binary" \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    0x0 firmware/build/bootloader/bootloader.bin \
    0x8000 firmware/build/partition_table/partition-table.bin \
    0xf000 firmware/build/ota_data_initial.bin \
    0x20000 firmware/build/klipper-pendant.bin
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$binary" | awk '{print $1}' > "${release_dir}/firmware.sha256"
    sha256sum "$installer_binary" | awk '{print $1}' > "${release_dir}/installer.sha256"
else
    shasum -a 256 "$binary" | awk '{print $1}' > "${release_dir}/firmware.sha256"
    shasum -a 256 "$installer_binary" | awk '{print $1}' > "${release_dir}/installer.sha256"
fi

repository="$(gh repo view --json nameWithOwner --jq '.nameWithOwner')"
sha256="$(<"${release_dir}/firmware.sha256")"
printf '%s\n' \
    "{\"format\":1,\"board\":\"onx3248g035\",\"version\":\"${version}\",\"url\":\"https://github.com/${repository}/releases/download/${tag}/klipper-pendant-onx3248g035.bin\",\"sha256\":\"${sha256}\"}" \
    > "${release_dir}/manifest.json"

# Restore version.txt before creating the tag, so the working tree is clean
# again.  Release versioning is intentionally injected only into the build.
cp "$version_backup" firmware/version.txt
rm -f "$version_backup"
version_backup=""

git tag -a "$tag" -m "$tag" -m '[local-release]'
git push origin "$tag"
gh release create "$tag" --draft --title "$tag" --generate-notes \
    "$binary" "$installer_binary" "${release_dir}/firmware.sha256" \
    "${release_dir}/installer.sha256" "${release_dir}/manifest.json"
gh release edit "$tag" --draft=false
printf 'Published local release %s.\n' "$tag"
