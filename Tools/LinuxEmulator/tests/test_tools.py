#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Isolated stdlib-only tests: no edk2 checkout, network, BMC, or compiler needed."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


TOOLS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location("linux_emulator_" + name, TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


prepare = load("prepare")
bootstrap = load("bootstrap")
runner = load("run")


def put(root, name, contents):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)
    return path


def snapshot(root):
    return {str(path.relative_to(root)): path.read_bytes()
            for path in root.rglob("*") if path.is_file() and not path.is_symlink()}


def source_fixture(root):
    """Small pinned-source anchor fixture, not a firmware implementation."""
    put(root, "EmulatorPkg/EmulatorPkg.dsc", "\n".join([
        "PLATFORM_NAME                  = EmulatorPkg",
        "FLASH_DEFINITION               = EmulatorPkg/EmulatorPkg.fdf",
        "OUTPUT_DIRECTORY = Build/Emulator$(ARCH)",
        "!include NetworkPkg/Network.dsc.inc",
        "!include RedfishPkg/Redfish.dsc.inc",
        "EmulatorPkg/Library/PlatformBmLib/PlatformBmLib.inf",
        "EmulatorPkg/Unix/Host/Host.inf",
        "EmulatorPkg/Sec/Sec.inf",
        'gEmulatorPkgTokenSpaceGuid.PcdEmuGop|L"GOP Window"',
        'gEmulatorPkgTokenSpaceGuid.PcdEmuNetworkInterface|L"en0"',
        'MAC(000000000000,0x1)',
        "GCC:*_*_*_DLINK2_FLAGS == -lpthread -ldl -lXext -lX11",
        "EmulatorPkg/Library/RedfishPlatformHostInterfaceLib/RedfishPlatformHostInterfaceLib.inf",
    ]) + "\n")
    put(root, "EmulatorPkg/EmulatorPkg.fdf", "!include RedfishPkg/Redfish.fdf.inc\nINF EmulatorPkg/Sec/Sec.inf\n")
    put(root, "EmulatorPkg/Sec/Sec.inf", "X64/SwitchRam.nasm\n")
    put(root, "EmulatorPkg/Sec/X64/SwitchRam.nasm",
        "  push    rbp\n  push    rdx\n  push    r8\n  push    r9\n"
        "  ; CopyMem (PermanentMemoryBase, TemporaryMemoryBase, CopySize);\n"
        "  sub     rsp, 0x28     ; Allocate register spill area & 16-byte align stack\n"
        "  call    CopyMem\n  add     rsp, 0x28\n"
        "  pop     r9\n  pop     r8\n  pop     rdx\n"
        "  ; ZeroMem (TemporaryMemoryBase /* rcx */, CopySize /* rdx */);\n"
        "  sub     rsp, 0x28     ; Allocate register spill area & 16-byte align stack\n"
        "  call    ZeroMem\n  add     rsp, 0x28\n  pop     rbp\n  ret\n")
    put(root, "EmulatorPkg/Unix/Host/Host.inf", "LinuxPacketFilter.c\n")
    put(root, "EmulatorPkg/Unix/Host/LinuxPacketFilter.c", "upstream packet backend\n")
    put(root, "EmulatorPkg/Library/PlatformBmLib/PlatformBmLib.inf", "PlatformBmData.c\n")
    put(root, "EmulatorPkg/Library/PlatformBmLib/PlatformBmData.c", "upstream platform data\n")
    put(root, "EmulatorPkg/Library/RedfishPlatformHostInterfaceLib/RedfishPlatformHostInterfaceLib.c",
        '  Status = gRT->GetVariable (\n                  L"HostIpAssignmentType",\n')
    put(root, "RedfishPkg/RedfishHttpDxe/RedfishHttpDxe.c",
        "Host = AllocateZeroPool (REDFISH_HOST_NAME_MAX);\n    if (AsciiLocation == NULL)\n"
        "ZeroMem (BasicAuthString, EncodedAuthStrSize);\n")
    put(root, "RedfishPkg/RedfishRestExDxe/RedfishRestExProtocol.c", "    HttpIoDestroyIo (&(Instance->HttpIo));\n")
    put(root, "RedfishPkg/Redfish.dsc.inc", "  !include RedfishPkg/RedfishComponents.dsc.inc\n")
    for name in ("RedfishComponents.dsc.inc", "Redfish.fdf.inc"):
        put(root, "RedfishPkg/" + name,
            "RedfishPkg/RedfishHttpDxe/RedfishHttpDxe.inf\n"
            "RedfishPkg/RedfishRestExDxe/RedfishRestExDxe.inf\n")


def build_fixture(root):
    build = root / "Build/RedfishEmulatorX64/DEBUG_GCC"
    put(build, "X64/Host", "host fixture\n")
    put(build, "X64/RedfishSmokeTest.efi", "firmware fixture\n")
    put(build, "FV/FV_RECOVERY.fd", "nvram fixture\n")
    return build


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        source_fixture(self.root)
        self.git = mock.patch.object(prepare.subprocess, "check_output", side_effect=self.git_output).start()
        self.addCleanup(mock.patch.stopall)

    def git_output(self, command, **kwargs):
        return prepare.LOCK["edk2_commit"] + "\n" if "rev-parse" in command else ""

    def test_replace_once_rejects_missing_and_duplicate_anchors(self):
        self.assertEqual(prepare.replace_once("old", "old", "new"), "new")
        for contents in ("missing", "old old"):
            with self.assertRaises(ValueError):
                prepare.replace_once(contents, "old", "new")

    def test_generated_overlay_preserves_sources_and_is_idempotent(self):
        upstream = snapshot(self.root)
        output = prepare.prepare(self.root)
        generated = snapshot(output)
        self.assertEqual(output, prepare.prepare(self.root))
        self.assertEqual(generated, snapshot(output))
        for name, value in upstream.items():
            self.assertEqual((self.root / name).read_bytes(), value, name)
        self.assertIn("RedfishEmulatorPkg/Redfish.dsc.inc", (output / "RedfishEmulator.dsc").read_text())
        self.assertIn("RedfishEmulatorPkg/Redfish.fdf.inc", (output / "RedfishEmulator.fdf").read_text())
        self.assertIn("ZeroMem (EncodedAuthString, EncodedAuthStrSize)",
                      (output / "RedfishHttpDxe/RedfishHttpDxe.c").read_text())
        self.assertNotIn("HttpIoDestroyIo (&(Instance->HttpIo))",
                         (output / "RedfishRestExDxe/RedfishRestExProtocol.c").read_text())
        self.assertIn("HostIpAssignmentTypeSize = sizeof", (output /
                      "RedfishPlatformHostInterfaceLib/RedfishPlatformHostInterfaceLib.c").read_text())
        status_calls = [call.args[0] for call in self.git.call_args_list if "status" in call.args[0]]
        self.assertTrue(status_calls)
        for command in status_calls:
            self.assertIn("--untracked-files=no", command)
            self.assertIn("--ignore-submodules=none", command)
            self.assertNotIn("--", command)  # Do not restrict to only overlay input packages.

    def test_wrong_revision_and_dirty_inputs_are_rejected_before_writes(self):
        for outputs in (["wrong\n"], [prepare.LOCK["edk2_commit"], " M RedfishPkg/file.c\n"],
                        [prepare.LOCK["edk2_commit"], " m CryptoPkg/Library/OpensslLib/openssl\n"]):
            with mock.patch.object(prepare.subprocess, "check_output", side_effect=outputs):
                with self.assertRaises(ValueError):
                    prepare.prepare(self.root)
            self.assertFalse((self.root / prepare.PACKAGE).exists())

    def test_sec_overlay_changes_only_zeromem_stack_reservation(self):
        original = self.root / "EmulatorPkg/Sec/X64/SwitchRam.nasm"
        upstream = original.read_text()
        marker = "  ; ZeroMem (TemporaryMemoryBase /* rcx */, CopySize /* rdx */);"
        before, after = upstream.split(marker)
        output = prepare.prepare(self.root)
        generated = (output / "Sec/X64/SwitchRam.nasm").read_text()
        expected = (before + marker + after.replace("sub     rsp, 0x28", "sub     rsp, 0x20")
                    .replace("add     rsp, 0x28", "add     rsp, 0x20"))
        self.assertEqual(generated, expected)
        self.assertEqual(original.read_text(), upstream)
        self.assertEqual(generated.count("sub     rsp, 0x28"), 1)
        self.assertEqual(generated.count("add     rsp, 0x28"), 1)
        self.assertEqual(generated.count("sub     rsp, 0x20"), 1)
        self.assertEqual(generated.count("add     rsp, 0x20"), 1)
        self.assertEqual((output / "Sec/Sec.inf").read_bytes(),
                         (self.root / "EmulatorPkg/Sec/Sec.inf").read_bytes())
        for name in ("RedfishEmulator.dsc", "RedfishEmulator.fdf"):
            self.assertIn("RedfishEmulatorPkg/Sec/Sec.inf", (output / name).read_text())
            self.assertNotIn("EmulatorPkg/Sec/Sec.inf", (output / name).read_text()
                             .replace("RedfishEmulatorPkg/Sec/Sec.inf", ""))

    def test_bad_sec_anchor_preserves_existing_overlay(self):
        original = self.root / "EmulatorPkg/Sec/X64/SwitchRam.nasm"
        upstream = original.read_text()
        marker = "  ; ZeroMem (TemporaryMemoryBase /* rcx */, CopySize /* rdx */);"
        output = prepare.prepare(self.root)
        before = snapshot(output)
        broken_variants = [
            upstream.replace(marker, "; missing"),
            upstream + marker,
            upstream.replace("  call    ZeroMem", "  sub     rsp, 0x28\n  call    ZeroMem"),
            upstream.rsplit("add     rsp, 0x28", 1)[0] + "add     rsp, 0x30\n",
        ]
        for broken in broken_variants:
            with self.subTest(assembly=broken):
                original.write_text(broken)
                with self.assertRaises(ValueError):
                    prepare.prepare(self.root)
                self.assertEqual(snapshot(output), before)
                self.assertEqual(original.read_text(), broken)

    def test_unowned_output_is_never_overwritten(self):
        path = put(self.root, prepare.PACKAGE + "/keep", "user data")
        with self.assertRaises(ValueError):
            prepare.prepare(self.root)
        self.assertEqual(path.read_text(), "user data")

    def test_symlinked_root_and_output_file_are_rejected(self):
        output = prepare.prepare(self.root)
        original = self.root / "EmulatorPkg/EmulatorPkg.dsc"
        contents = original.read_bytes()
        (output / "RedfishEmulator.dsc").unlink()
        (output / "RedfishEmulator.dsc").symlink_to(original)
        with self.assertRaises(ValueError):
            prepare.prepare(self.root)
        self.assertEqual(original.read_bytes(), contents)
        (output / "RedfishEmulator.dsc").unlink()
        alternate = self.root / "other"
        output.rename(alternate)
        output.symlink_to(alternate, target_is_directory=True)
        with self.assertRaises(ValueError):
            prepare.prepare(self.root)

    def test_fresh_generation_removes_obsolete_files_and_unused_x11_link(self):
        output = prepare.prepare(self.root)
        put(output, "SmokeTest/obsolete.c", "old")
        (output / "Host/X11IncludeHack").symlink_to("/nonexistent-test-target")
        prepare.prepare(self.root)
        self.assertFalse((output / "SmokeTest/obsolete.c").exists())
        self.assertFalse((output / "Host/X11IncludeHack").is_symlink())

    def test_bad_anchor_keeps_prior_overlay_intact(self):
        output = prepare.prepare(self.root)
        before = snapshot(output)
        put(self.root, "EmulatorPkg/EmulatorPkg.fdf", "anchor missing\n")
        with self.assertRaises(ValueError):
            prepare.prepare(self.root)
        self.assertEqual(snapshot(output), before)
        self.assertEqual(list(self.root.glob(".RedfishEmulatorPkg-*")), [])

    def test_backend_false_uses_fresh_upstream_backend(self):
        output = prepare.prepare(self.root)
        prepare.prepare(self.root, backend=False)
        self.assertEqual((output / "Host/LinuxPacketFilter.c").read_text(), "upstream packet backend\n")


class BootstrapTests(unittest.TestCase):
    def test_lock_uses_full_commit_pins(self):
        for key in ("edk2_commit", "client_baseline"):
            self.assertRegex(bootstrap.LOCK[key], r"^[0-9a-f]{40}$")
        self.assertTrue(bootstrap.LOCK["edk2_repository"].startswith("https://"))

    def test_new_checkout_is_verified_before_submodules(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(sys, "argv", ["bootstrap.py", directory]), \
                mock.patch.object(bootstrap.subprocess, "run") as run, \
                mock.patch.object(bootstrap.subprocess, "check_output", side_effect=[bootstrap.LOCK["edk2_commit"] + "\n", ""]), \
                contextlib.redirect_stdout(io.StringIO()):
            bootstrap.main()
        self.assertEqual(run.call_count, 2)
        self.assertIn("clone", run.call_args_list[0].args[0])
        self.assertIn(bootstrap.LOCK["edk2_tag"], run.call_args_list[0].args[0])
        update = run.call_args_list[1].args[0]
        self.assertEqual(update[-len(bootstrap.LOCK["submodules"]):], bootstrap.LOCK["submodules"])
        self.assertTrue(all(call.kwargs["check"] for call in run.call_args_list))

    def test_existing_wrong_checkout_is_not_reset_or_updated(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "edk2").mkdir()
            with mock.patch.object(sys, "argv", ["bootstrap.py", directory]), \
                    mock.patch.object(bootstrap.subprocess, "run") as run, \
                    mock.patch.object(bootstrap.subprocess, "check_output", return_value="wrong\n"), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    bootstrap.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()

    def test_dirty_checkout_or_submodule_is_rejected_before_update(self):
        for dirty in (" M MdePkg/file.c\n", " m CryptoPkg/Library/OpensslLib/openssl\n"):
            with self.subTest(dirty=dirty), tempfile.TemporaryDirectory() as directory:
                (Path(directory) / "edk2").mkdir()
                with mock.patch.object(sys, "argv", ["bootstrap.py", directory]), \
                        mock.patch.object(bootstrap.subprocess, "run") as run, \
                        mock.patch.object(bootstrap.subprocess, "check_output", side_effect=[bootstrap.LOCK["edk2_commit"], dirty]) as git, \
                        contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        bootstrap.main()
                    self.assertEqual(error.exception.code, 2)
                    run.assert_not_called()
                    status = git.call_args.args[0]
                    self.assertIn("--untracked-files=no", status)
                    self.assertIn("--ignore-submodules=none", status)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = build_fixture(self.root)
        self.output = self.root / "runtime"

    def test_runtime_has_private_firmware_volume_and_preserves_build(self):
        before = snapshot(self.build)
        binary = runner.stage_runtime(self.root, self.output)
        self.assertEqual(binary, self.output / "X64")
        self.assertTrue((binary / "Host").is_file())
        self.assertFalse((self.output / "FV/FV_RECOVERY.fd").is_symlink())
        (self.output / "FV/FV_RECOVERY.fd").write_text("modified nvram")
        self.assertEqual(snapshot(self.build), before)

    def test_smoke_preserves_existing_build_startup_scripts(self):
        for name in ("startup.nsh", "STARTUP.NSH"):
            put(self.build, "X64/" + name, "echo user script: " + name)
        before = snapshot(self.build)
        console = runner.PASS_MARKER + b"\nREDFISH_SMOKE_SCRIPT_PASS\n"
        code, result = self.smoke(self.request_sequence(), (0, console))
        self.assertEqual(code, 0)
        self.assertTrue(result["passed"])
        self.assertEqual(snapshot(self.build), before)
        self.assertFalse((self.output / "X64/startup.nsh").is_symlink())
        self.assertFalse((self.output / "X64/STARTUP.NSH").exists())
        self.assertIn("RedfishSmokeTest.efi", (self.output / "X64/startup.nsh").read_text())

    def test_missing_build_and_existing_output_fail_without_overwrite(self):
        (self.build / "X64/Host").unlink()
        with self.assertRaises(ValueError):
            runner.stage_runtime(self.root, self.output)
        self.assertFalse(self.output.exists())
        put(self.build, "X64/Host", "host")
        put(self.output, "keep", "user data")
        with self.assertRaises(FileExistsError):
            runner.stage_runtime(self.root, self.output)
        self.assertEqual((self.output / "keep").read_text(), "user data")

    def test_invalid_timeouts_never_start_firmware(self):
        for value in ("nan", "inf", "-inf", "0", "-1"):
            with self.subTest(value=value), \
                    mock.patch.object(sys, "argv", ["run.py", str(self.root), "--smoke", "--timeout=" + value]), \
                    mock.patch.object(runner, "smoke") as smoke, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    runner.main()
                self.assertEqual(error.exception.code, 2)
                smoke.assert_not_called()

    def request_sequence(self):
        return [
            {"method": "GET", "path": "/redfish/v1", "status": 200},
            {"method": "GET", "path": "/redfish/v1/Systems", "status": 200},
            {"method": "PATCH", "path": "/redfish/v1/Systems/1", "status": 200},
            {"method": "GET", "path": "/redfish/v1/Systems/1", "status": 200},
        ]

    def smoke(self, requests, host_result=None, error=None):
        cleaned = []

        @contextlib.contextmanager
        def simulator():
            try:
                yield 54321, requests
            finally:
                cleaned.append(True)

        with mock.patch.object(runner, "local_simulator", simulator), \
                mock.patch.object(runner, "run_host", return_value=host_result, side_effect=error), \
                contextlib.redirect_stdout(io.StringIO()):
            code = runner.smoke(self.root, self.output, 2)
        self.assertEqual(cleaned, [True])
        self.assertEqual(json.loads((self.output / "requests.json").read_text()), requests)
        return code, json.loads((self.output / "result.json").read_text())

    def test_success_requires_firmware_and_shell_markers_and_http_readback(self):
        console = runner.PASS_MARKER + b"\nREDFISH_SMOKE_SCRIPT_PASS\n"
        code, result = self.smoke(self.request_sequence(), (0, console))
        self.assertEqual(code, 0)
        self.assertTrue(result["passed"])
        script = (self.output / "X64/startup.nsh").read_text()
        self.assertIn("RedfishSmokeTest.efi 54321", script)
        self.assertIn("reset -s", script)

    def test_console_markers_alone_cannot_pass(self):
        code, result = self.smoke([], (0, runner.PASS_MARKER + b"\nREDFISH_SMOKE_SCRIPT_PASS"))
        self.assertEqual(code, 1)
        self.assertFalse(result["passed"])

    def test_http_patch_without_readback_cannot_pass(self):
        code, result = self.smoke(self.request_sequence()[:-1],
                                  (0, runner.PASS_MARKER + b"\nREDFISH_SMOKE_SCRIPT_PASS"))
        self.assertEqual(code, 1)
        self.assertFalse(result["passed"])

    def test_nonzero_exit_cannot_pass(self):
        code, result = self.smoke(self.request_sequence(),
                                  (1, runner.PASS_MARKER + b"\nREDFISH_SMOKE_SCRIPT_PASS"))
        self.assertEqual(code, 1)
        self.assertFalse(result["passed"])

    def test_timeout_preserves_request_log_and_failure_manifest(self):
        code, result = self.smoke(self.request_sequence()[:2], error=TimeoutError("fixture timeout"))
        self.assertEqual(code, 2)
        self.assertFalse(result["passed"])
        self.assertIsNone(result["firmware_exit_code"])
        self.assertIn("fixture timeout", result["error"])

    def test_simulator_import_failure_preserves_failure_manifest(self):
        with mock.patch.object(runner, "local_simulator", side_effect=ImportError("missing fixture dependency")), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runner.smoke(self.root, self.output, 2), 2)
        self.assertEqual(json.loads((self.output / "requests.json").read_text()), [])
        self.assertFalse(json.loads((self.output / "result.json").read_text())["passed"])


@unittest.skipUnless(sys.platform.startswith("linux"), "PTY lifecycle is Linux-specific")
class HostLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.binary = Path(self.temporary.name)
        self.log = self.binary / "firmware.log"

    def host(self, code):
        executable = put(self.binary, "Host", "#!" + sys.executable + "\n" + code)
        executable.chmod(0o700)

    def test_pty_captures_output_exit_status_and_has_tty_stdin(self):
        self.host("import os, sys\nprint('TTY=' + str(os.isatty(0)), flush=True)\nsys.exit(7)\n")
        code, console = runner.run_host(self.binary, 5, self.log)
        self.assertEqual(code, 7)
        self.assertIn(b"TTY=True", console)
        self.assertEqual(self.log.read_bytes(), console)

    def test_timeout_reaps_child_and_preserves_partial_log(self):
        self.host("import time\nprint('started', flush=True)\ntime.sleep(20)\n")
        children = []
        real_popen = subprocess.Popen

        def capture(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            children.append(process)
            return process

        started = time.monotonic()
        with mock.patch.object(runner.subprocess, "Popen", side_effect=capture):
            with self.assertRaises(TimeoutError):
                runner.run_host(self.binary, 0.2, self.log)
        self.assertLess(time.monotonic() - started, 5)
        self.assertIsNotNone(children[0].poll())
        self.assertIn(b"started", self.log.read_bytes())

    def test_log_open_failure_still_reaps_started_child(self):
        self.host("import time\ntime.sleep(20)\n")
        children = []
        real_popen = subprocess.Popen

        def capture(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            children.append(process)
            return process

        with mock.patch.object(runner.subprocess, "Popen", side_effect=capture):
            with self.assertRaises(FileNotFoundError):
                runner.run_host(self.binary, 5, self.binary / "missing/log")
        self.assertIsNotNone(children[0].poll())


if __name__ == "__main__":
    unittest.main()
