#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the Linux firmware headlessly, or test it against an isolated local mock."""
import argparse
import contextlib
import io
import json
import math
import os
import pathlib
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
SIMULATOR = HERE.parent / "Redfish-Profile-Simulator"
PASS_MARKER = b"REDFISH_SMOKE: authenticated GET/PATCH/GET PASS"


def stage_runtime(edk2, output):
    """Keep firmware/NVRAM writes separate from the build and other test runs."""
    build = edk2 / "Build/RedfishEmulatorX64/DEBUG_GCC"
    for required in (build / "X64/Host", build / "X64/RedfishSmokeTest.efi", build / "FV/FV_RECOVERY.fd"):
        if not required.is_file():
            raise ValueError(f"Missing build output: {required}; run build.sh first")
    output.mkdir(parents=True, exist_ok=False)
    binary = output / "X64"
    binary.mkdir()
    (output / "FV").mkdir()
    shutil.copy2(build / "FV/FV_RECOVERY.fd", output / "FV/FV_RECOVERY.fd")
    for source in (build / "X64").iterdir():
        # Never inherit an automatic shell script or write through a build-tree
        # symlink when smoke() creates its own script. Match EFI case variants.
        if source.is_file() and source.name.casefold() != "startup.nsh":
            (binary / source.name).symlink_to(source.resolve())
    return binary


def run_host(binary, timeout, log):
    """Use an actual TTY for EmulatorPkg's stdin thunk; always reap our child."""
    master, slave = pty.openpty()
    process = None
    data = bytearray()
    try:
        process = subprocess.Popen([str(binary / "Host")], cwd=binary,
                                   stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        slave = None
        deadline = time.monotonic() + timeout
        with log.open("wb") as stream:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"Firmware timed out after {timeout}s; see {log}")
                ready, _, _ = select.select([master], [], [], min(0.1, remaining))
                if ready:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError as error:
                        # Linux reports EIO when the final slave closes.
                        if error.errno != 5:
                            raise
                        break
                    if not chunk:
                        break
                    stream.write(chunk)
                    stream.flush()
                    data.extend(chunk)
                elif process.poll() is not None:
                    break
        code = process.wait(timeout=5)
        return code, bytes(data)
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        os.close(master)
        if slave is not None:
            os.close(slave)


@contextlib.contextmanager
def local_simulator():
    # No BMC URL accepted. The in-memory fixture is new for every run.
    sys.path.insert(0, str(SIMULATOR))
    from v1sim.serviceRoot import RfServiceRoot
    from v1sim.serviceVersions import RfServiceVersions
    from v1sim.redfishURIs import create_app
    from werkzeug.serving import WSGIRequestHandler, make_server
    from flask import request

    profile = SIMULATOR / "MockupData/SimpleOcpServerV1"
    with contextlib.redirect_stdout(io.StringIO()):
        root = RfServiceRoot(str(profile), os.path.join("redfish", "v1"))
        versions = RfServiceVersions(str(profile), "redfish")
        app = create_app(root, versions)
    requests = []

    @app.after_request
    def record(response):
        # Record only methods, paths and statuses, never authentication headers.
        requests.append({"method": request.method, "path": request.path, "status": response.status_code})
        return response

    class QuietHandler(WSGIRequestHandler):
        protocol_version = "HTTP/1.1"

        def log(self, *args, **kwargs):
            pass

    server = make_server("127.0.0.1", 0, app, request_handler=QuietHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port, requests
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()


def smoke(edk2, output, timeout):
    binary = stage_runtime(edk2, output)
    requests = []
    result = {"passed": False, "firmware_exit_code": None,
              "firmware_log": str(output / "firmware.log"), "http_log": str(output / "requests.json")}
    try:
        with local_simulator() as (port, requests):
            script = ("@echo -off\nfs0:\n"
                      f"RedfishSmokeTest.efi {port}\n"
                      "if %lasterror% == 0 then\n"
                      "  echo REDFISH_SMOKE_SCRIPT_PASS\n"
                      "else\n  echo REDFISH_SMOKE_SCRIPT_FAIL\nendif\nreset -s\n")
            (binary / "startup.nsh").write_text(script, encoding="ascii")
            code, console = run_host(binary, timeout, output / "firmware.log")
        result["firmware_exit_code"] = code
        # Firmware validates the JSON values; independently require the actual
        # on-wire request sequence, including the GET that follows the PATCH.
        root_seen = False
        systems_seen = False
        patched = None
        read_back = False
        for item in requests:
            if item["method"] == "GET" and item["status"] == 200:
                if item["path"].rstrip("/") == "/redfish/v1":
                    root_seen = True
                elif root_seen and item["path"].rstrip("/") == "/redfish/v1/Systems":
                    systems_seen = True
                elif patched is not None and item["path"] == patched:
                    read_back = True
            elif (systems_seen and item["method"] == "PATCH" and
                  item["status"] in (200, 204) and
                  item["path"].startswith("/redfish/v1/Systems/")):
                patched = item["path"]
        result["passed"] = (code == 0 and PASS_MARKER in console and
                            b"REDFISH_SMOKE_SCRIPT_PASS" in console and read_back)
    except (OSError, ValueError, TimeoutError, ImportError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
        return 2
    finally:
        result["requests"] = len(requests)
        (output / "requests.json").write_text(json.dumps(requests, indent=2) + "\n")
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("edk2", type=pathlib.Path)
    parser.add_argument("--smoke", action="store_true", help="run actual firmware GET/PATCH/GET against a fresh local fixture")
    parser.add_argument("--output", type=pathlib.Path, help="new runtime/log directory (must not exist)")
    parser.add_argument("--timeout", type=float, default=120, help="smoke timeout seconds, default 120")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Requires Linux")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be finite and positive")
    # Keep successful and failed logs available. No existing directory is removed.
    output = (args.output or pathlib.Path(tempfile.gettempdir()) / f"redfish-emulator-{time.time_ns()}").resolve()
    try:
        if args.smoke:
            return smoke(args.edk2.resolve(), output, args.timeout)
        binary = stage_runtime(args.edk2.resolve(), output)
        print(f"Runtime/NVRAM: {output}", flush=True)
        return subprocess.call([str(binary / "Host")], cwd=binary)
    except (OSError, ValueError, TimeoutError, ImportError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
