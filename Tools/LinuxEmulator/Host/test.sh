#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 EDK2_ROOT [DEPENDENCY_SYSROOT]" >&2
  exit 2
fi
edk2=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
includes=( -I"$edk2/MdePkg/Include" -I"$edk2/MdePkg/Include/X64"
           -I"$edk2/EmulatorPkg/Include" -I"$edk2/EmulatorPkg/Unix/Host" )
if [[ $# == 2 ]]; then
  deps=$(cd "$2" && pwd)
  includes+=( -I"$deps/usr/include/slirp" )
  libs=( -L"$deps/usr/lib/x86_64-linux-gnu" -lslirp )
  export LD_LIBRARY_PATH="$deps/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  read -r -a slirp_cflags <<< "$(pkg-config --cflags slirp)"
  read -r -a libs <<< "$(pkg-config --libs slirp)"
  includes+=( "${slirp_cflags[@]}" )
fi
flags=( -std=gnu11 -g -O1 -Wall -Wextra -Werror -fshort-wchar -DEFIAPI= -include Base.h )
if [[ ${SANITIZE:-0} == 1 ]]; then
  flags+=( -fsanitize=address,undefined -fno-omit-frame-pointer )
fi
"${CC:-cc}" "${flags[@]}" "${includes[@]}" "$here/test_linux_snp.c" "${libs[@]}" -o "$build/test-linux-snp"
"$build/test-linux-snp"
