# Native Linux Redfish firmware emulation

This runs real X64 UEFI firmware as a Linux process, with the EDK II Redfish
foundation and this repository's Redfish client drivers. It does not emulate a
BMC or a specific motherboard, and it does not need QEMU, KVM, root, TAP devices,
a bridge, or an X server.

## Why the Windows recipe does not work unchanged

`edk2/EmulatorPkg` supplies both Windows `WinHost.exe` and Linux `Host` launchers.
The Windows network thunk uses `SnpNt32Io.dll` and WinPcap. At the pinned upstream
revision, `EmulatorPkg/Unix/Host/LinuxPacketFilter.c` is a template: its SNP
start, initialize, transmit, and receive operations return `EFI_UNSUPPORTED`.
Changing a build toolchain or installing the Python simulator cannot fix that.

This directory supplies a Linux-only libslirp Ethernet backend and generates an
isolated platform overlay from upstream EmulatorPkg. The firmware's normal SNP,
IPv4, TCP, HTTP, REST EX and Redfish drivers still run. A serial stdin/stdout
console replaces the platform's graphics-only default. The generated platform
also integrates the RedfishClientPkg DSC/FDF includes and a firmware smoke app.
No Windows host implementation or upstream tracked file is replaced.

The separately bundled `Tools/Redfish-Profile-Simulator` supplies an in-memory
HTTP service for tests. Its Python tests alone are **not firmware emulation**.

## Supported baseline

- Linux x86-64, Python 3.10+, native GCC 13 or newer, EDK toolchain tag `GCC`
- edk2-stable202608, commit `2970e5699ba6267f3384ffab20f96647578aebc8`
- edk2-redfish-client baseline `92fabf8572c226cf180c62b1204380385a518db3`
- libslirp 4.7 or newer (4.7.0 and 4.8.0 tested)
- All edk2 submodules are checked out at the gitlinks of that exact commit.
  No edk2-platforms checkout is needed.

The lock is in `lock.json`. Preparation fails on a different edk2 revision or
tracked changes or dirty submodules anywhere in the upstream checkout. Use a separate workspace; it never resets an
existing checkout to make it fit. GCC/libslirp system packages receive normal
distribution security updates rather than being downloaded as unverified blobs.

## Build and test on Ubuntu 24.04

From the root of this repository:

```sh
sudo apt-get update
sudo apt-get install -y build-essential git uuid-dev nasm python3-venv \
  pkg-config libx11-dev libxext-dev libslirp-dev libglib2.0-dev
python3 -m venv .venv
.venv/bin/pip install -r Tools/Redfish-Profile-Simulator/requirements.txt
python3 Tools/LinuxEmulator/bootstrap.py .linux-emulator
Tools/LinuxEmulator/build.sh .linux-emulator/edk2 2
.venv/bin/python Tools/LinuxEmulator/run.py .linux-emulator/edk2 \
  --smoke --output .linux-emulator/smoke-1
```

X11 development libraries remain link-time requirements of upstream Host;
there is no display connection at runtime. The final argument to `build.sh` is
parallelism (default 2). Do not reuse the output directory of a prior run.

The smoke runner starts a new simulator on an ephemeral **127.0.0.1** TCP port,
boots the built UEFI firmware through a PTY, and runs `RedfishSmokeTest.efi` from
an isolated virtual filesystem. The app creates a REST EX instance, calls the
EDK II Redfish HTTP protocol, gets the service root and authenticated Systems
collection, PATCHes the first mock system's AssetTag, and GETs it back to verify.
It accepts only a port, with destination fixed to the libslirp host alias
`10.0.2.2`; no external BMC endpoint can be supplied to this test.

Success requires all of: firmware process exit 0, the app's complete
GET/PATCH/GET pass marker, the shell's successful application status, and a
successful ordered root GET, authenticated Systems GET, PATCH, and subsequent
readback observed by the local mock server. A boot banner or a Python
GET is not accepted as a pass. A timeout terminates and reaps the child, then
returns nonzero.

Each run keeps its own writable firmware/NVRAM copy and produces:

- `firmware.log`: UEFI and host console output
- `requests.json`: mock HTTP method/path/status records, without credentials
- `result.json`: pass/fail summary

Build outputs are under `edk2/Build/RedfishEmulatorX64/DEBUG_GCC`. The generated
platform is under `edk2/RedfishEmulatorPkg`; these are disposable local outputs,
not files to submit upstream.

## Interactive use

```sh
python3 Tools/LinuxEmulator/run.py .linux-emulator/edk2 \
  --output .linux-emulator/interactive-1
```

Existing build-directory startup.nsh files are excluded from every staged run.
The UEFI shell appears in the terminal. `reset -s` exits the process. The fixed
virtual NIC MAC is `52:54:00:12:34:56`; its network is `10.0.2.0/24`, and
`10.0.2.2` reaches the Linux host's loopback. libslirp supports DHCP; the smoke
app deliberately uses a deterministic `10.0.2.15/24` local address. No host
interface is changed. The backend does not create inbound port forwards.

For manual client provisioning experiments, first run the simulator explicitly
on loopback. Inside the shell, `RedfishPlatformConfig.efi -a 10.0.2.2
255.255.255.0 5000` stores discovery settings. Restarting is required by the
upstream discovery flow; keep the same isolated runtime/NVRAM if doing this
manually by running `cd <runtime>/X64 && ./Host`. The generated host-interface
library initializes two previously uninitialized GetVariable size inputs. The
private RedfishHttpDxe copy corrects a freed authentication-buffer wipe and a
wrong allocation-result check; RedfishRestExDxe resets its HTTP configuration
without prematurely destroying the child/event resources. These narrow fixes
are confined to generated overlays at the pinned revision. A further X64 SEC
stack-migration correction reserves 32 bytes, rather than 40, before ZeroMem:
the upstream mismatch misaligns the stack and faults on GCC-generated SIMD
spills before DXE can start. This backports upstream
[commit f014b047](https://github.com/tianocore/edk2/commit/f014b04755065a5a47765fcc5bf31fca9ed008f3).
The earlier CopyMem reservation remains unchanged. The automated smoke test directly
configures REST EX and does not claim to validate SMBIOS discovery or every
client feature's provisioning sequence.

## Separate test layers

```sh
# Python service and CLI: no firmware involved.
.venv/bin/python -W error::ResourceWarning -m unittest discover \
  -s Tools/Redfish-Profile-Simulator/tests -v
# Tooling safety and process cleanup: no firmware build required.
python3 -m unittest discover -s Tools/LinuxEmulator/tests -v
# Native SNP state/filter/packet tests against the actual libslirp library.
bash Tools/LinuxEmulator/Host/test.sh .linux-emulator/edk2
# Actual built firmware-to-local-service exchange.
.venv/bin/python Tools/LinuxEmulator/run.py .linux-emulator/edk2 \
  --smoke --output .linux-emulator/smoke-2
```

The new CI workflow runs the simulator tests on Windows and Linux, then builds
and runs the native firmware on Ubuntu. Local Windows execution is not implied
by a Linux test result.

## Limits and safety

- This development platform uses HTTP and public mock credentials. Do not use
  these settings, firmware images, or credentials on production systems.
- The local mock is not a complete Redfish implementation. Its README documents
  conditional-request and fixed-session-token limitations.
- The smoke test covers real UEFI networking and Redfish request/response paths;
  it does not validate real hardware, BMC interoperability, TLS certificate
  provisioning, reset/boot effects on a physical system, or all feature drivers.
- Host networking is user-mode NAT, not an Ethernet bridge. It cannot reproduce
  physical link behavior, VLAN topology, or broadcast discovery on a real LAN.
- Linux X64 is the initial target. IA32, ARM, macOS, and libslirp versions beyond 4.7/4.8
  have not been validated by this workflow.
- Kernel policies may prohibit process execution, executable memory, PTYs or
  loopback sockets. Report the precise failure; do not disable security controls
  or treat a simulator-only result as a firmware pass.
