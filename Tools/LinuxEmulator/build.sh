#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 <isolated pinned edk2 checkout> [jobs, default 2]" >&2
  exit 2
fi
edk2=$(cd -- "$1" && pwd)
jobs=${2:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be positive' >&2; exit 2; }
[[ $(uname -sm) == 'Linux x86_64' ]] || { echo 'Requires Linux x86_64' >&2; exit 2; }
for tool in gcc g++ make nasm pkg-config python3; do
  command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 2; }
done
pkg-config --atleast-version=4.7 slirp || { echo 'Install libslirp-dev >= 4.7' >&2; exit 2; }
export SLIRP_CFLAGS="$(pkg-config --cflags slirp)"
export SLIRP_LIBS="$(pkg-config --libs slirp)"
python3 "$here/prepare.py" "$edk2"
export PACKAGES_PATH="$edk2:$repo"
export PYTHON_COMMAND=python3
cd "$edk2"
source ./edksetup.sh BaseTools
make -C BaseTools -j"$jobs"
build -p RedfishEmulatorPkg/RedfishEmulator.dsc -a X64 -t GCC -b DEBUG -n "$jobs" \
  -D REDFISH_ENABLE=TRUE -D REDFISH_CLIENT=TRUE -D NETWORK_ENABLE=TRUE \
  -D NETWORK_HTTP_ENABLE=TRUE -D NETWORK_ALLOW_HTTP_CONNECTIONS=TRUE \
  -D NETWORK_PXE_BOOT_ENABLE=FALSE -D NO_PLATFORM_BOOT_DELAYS=TRUE
