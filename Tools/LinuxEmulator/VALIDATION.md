# Linux emulator validation

Validated on 2026-10-04 using an isolated Linux x86-64 workspace. No physical
BMC, DUT, TAP/bridge interface, host network setting, or Windows installation
was changed.

## Versions

- Client base: `92fabf8572c226cf180c62b1204380385a518db3`
- EDK II: `2970e5699ba6267f3384ffab20f96647578aebc8` (`edk2-stable202608`)
- GCC: Debian 14.2.0-19
- NASM: 2.16.03
- libslirp: 4.8.0
- Python: 3.12.14
- Flask: 3.0.0; Werkzeug: 3.1.9
- C formatting: Tianocore uncrustify 73.0.11, configuration from the pinned edk2

Only the generated untracked `RedfishEmulatorPkg/` exists in the edk2 checkout;
all tracked edk2 files and initialized submodules remain unchanged.

## Completed checks

1. BaseTools compiled and its 301 tests passed.
2. Simulator: 30 tests passed, including real HTTP/HTTPS on ephemeral loopback
   ports, CLI startup from another directory, authentication, mutation/readback,
   ETag header changes, and credential-log regression checks.
3. Tooling: 27 tests passed, including pin/dirty-tree refusal, deterministic
   overlay generation, symlink refusal, SEC backport boundaries, unchanged
   upstream inputs and pre-existing shell startup scripts, private NVRAM, timeout/reaping, and success/failure oracles.
4. Native SNP backend: strict GCC warnings and real libslirp ARP plus
   bidirectional loopback UDP passed. State transitions, receive filters,
   bounded queues, FIFO transmit recycling, interrupt clearing, short-buffer
   receive behavior, statistics, reset, and timer delivery were exercised.
5. The same native tests passed AddressSanitizer and UndefinedBehaviorSanitizer.
   LeakSanitizer was unavailable under the executor's ptrace restriction;
   leak detection was explicitly disabled for this sanitizer run.
6. Complete DEBUG X64 UEFI image built successfully, including the Linux Host,
   Redfish foundation, client drivers, and smoke application. Firmware volume:
   3,680,648 bytes used of 13,107,200 (28%).
7. Three independent firmware smoke runs passed with fresh firmware/NVRAM copies:
   SEC, PEI, DXE, UEFI Shell, and the smoke application actually executed.
   Each exited 0 and recorded eight successful local HTTP requests. A separate
   actual-firmware run deliberately returned HTTP 401 for Systems: the oracle
   correctly failed it, even though the emulator process exited 0.
8. Added C files passed the pinned Tianocore uncrustify check. Shell syntax,
   Python compilation, and CRLF-aware Git whitespace checks passed.

Representative actual firmware console:

```text
Linux SNP: libslirp 4.8.0, MAC 52:54:00:12:34:56, host 10.0.2.2
REDFISH_SMOKE: client feature core present
REDFISH_SMOKE: service root version 1.0.2
REDFISH_SMOKE: authenticated GET/PATCH/GET PASS
REDFISH_SMOKE_SCRIPT_PASS
```

The first four HTTP records in each passing run were:

```text
GET   /redfish/v1                          200
GET   /redfish/v1/Systems                  200
PATCH /redfish/v1/Systems/2M220100SL        200
GET   /redfish/v1/Systems/2M220100SL        200
```

The firmware checked that AssetTag changed to `LinuxEmulatorSmoke`. Four later
GETs read the mock AccountService during credential-service cleanup. Requests
and their statuses are recorded without Authorization headers or passwords.

## Reproducing

Use the build and test commands in [README.md](README.md). The standard commands
require normally installed distribution development packages. This executor
used checksum-verified Debian packages extracted in a workspace-local dependency
root instead of changing its system installation; that changes only compiler
include/library discovery, not the platform source or test.

For sanitizer checks, use:

```sh
ASAN_OPTIONS=detect_leaks=0 SANITIZE=1 \
  bash Tools/LinuxEmulator/Host/test.sh .linux-emulator/edk2
```

Do not disable leak detection on an executor where LeakSanitizer is supported.

## Findings and boundaries

- The first firmware boot reproduced a real upstream SEC stack-alignment
  segfault at the `movaps` spill in `InternalMemZeroMem`. Correcting only the
  second reservation in `SwitchRam.nasm` matched upstream commit
  [f014b047](https://github.com/tianocore/edk2/commit/f014b04755065a5a47765fcc5bf31fca9ed008f3)
  and allowed the full smoke runs to pass. No diagnostic preload is needed or
  used for passing runs.
- The smoke test directly configures REST EX. Fresh NVRAM has no Redfish service
  discovery settings, so earlier client feature callbacks report
  `no Redfish service configured`; this is expected and is not evidence of
  complete automatic provisioning. The feature core is present, but SMBIOS
  discovery and all BIOS/Boot/Memory/Task feature synchronization remain outside
  this test's coverage.
- Firmware TLS, physical hardware, IA32/ARM/macOS, and Windows execution were not
  validated. The new Windows/Linux simulator CI and Ubuntu firmware CI are
  configured but were not run on GitHub during local validation.
- Simulator ETag headers do not imply enforced resource If-Match/If-None-Match.
  Session authentication remains a fixed demonstration token. See the
  simulator README for its existing protocol limitations.
