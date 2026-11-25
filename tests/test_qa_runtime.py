"""Managed bridge regressions found by QA; local stub upstreams only."""
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
import unittest

import test_runtime as runtime_base

SERVER = r'''
import json, os, pathlib, sys, time
marker = pathlib.Path(sys.argv[1])
with marker.open("a") as stream: stream.write(str(os.getpid()) + "\n")
env = os.environ.get
fail_first = env("KM_QA_FAIL_FIRST")
if env("KM_QA_ALWAYS_FAIL") or (fail_first and not os.path.exists(fail_first)):
    if fail_first: pathlib.Path(fail_first).touch()
    sys.exit(1)
time.sleep(float(env("KM_QA_DELAY", "0")))
def send(packet): print(json.dumps({"jsonrpc": "2.0", **packet}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method, params = request.get("method"), request.get("params") or {}
    if method == "initialize":
        if env("KM_QA_NOISE"):
            print("server listening on stdio", flush=True)
            print("", flush=True)
        send({"id": request["id"], "result": {"protocolVersion": params["protocolVersion"],
              "capabilities": json.loads(env("KM_QA_CAPS", '{"tools":{}}')),
              "serverInfo": {"name": "qa-fixture", "version": "1"}}})
    elif method == "tools/list":
        if env("KM_QA_CRASH_LIST"): sys.exit(3)
        send({"id": request["id"], "result": {"tools": [{"name": "echo", "inputSchema": {"type": "object"}}]}})
    elif method == "tools/call":
        name, arguments = params.get("name"), params.get("arguments") or {}
        if name == "raw":
            sys.stdout.write('{"jsonrpc":"2.0","id":%s,"result":{"content":[],"structuredContent":%s}}\n'
                             % (json.dumps(request["id"]), arguments["raw"]))
            sys.stdout.flush()
            continue
        if name == "deep":
            value = 1
            for _ in range(int(arguments["depth"])): value = {"k": value}
            send({"id": request["id"], "result": {"content": [], "structuredContent": value}})
            continue
        send({"id": request["id"], "result": {"content": [{"type": "text", "text": str(os.getpid())}]}})
eof = env("KM_QA_EOF_MARKER")
if eof: pathlib.Path(eof).write_text("eof")
'''


class QaRuntimeTests(unittest.TestCase):
    save = runtime_base.RuntimeTests.save
    command = runtime_base.RuntimeTests.command
    close_bridge = runtime_base.RuntimeTests.close_bridge
    send = runtime_base.RuntimeTests.send
    packet = runtime_base.RuntimeTests.packet
    reply = runtime_base.RuntimeTests.reply
    call = runtime_base.RuntimeTests.call

    def setUp(self):
        runtime_base.RuntimeTests.setUp(self)
        script = self.home / "server.py"
        script.write_text(SERVER)
        self.config["mcps"]["stub"]["command"] = [sys.executable, str(script), str(self.marker)]
        self.save()
        self.sockets = self.home / ".local/state/kitmux/r"

    def environment(self, **values):
        self.config["mcps"]["stub"]["env"] = {key: str(value) for key, value in values.items()}
        self.save()

    def start(self):
        self.process = subprocess.Popen([runtime_base.BINARY, "--home", str(self.home), "bridge", "stub",
                                         "--agent", "codex"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.addCleanup(self.close_bridge)

    def bridge(self):
        self.start()
        self.send({"id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "qa-test", "version": "1"}}})
        self.assertIn("result", self.reply(1))
        self.send({"method": "notifications/initialized"})

    def tools(self, request_id=2, timeout=6):
        self.send({"id": request_id, "method": "tools/list"})
        for _ in range(20):
            response = self.packet(timeout)
            if response.get("id") == request_id:
                return response
        self.fail("tools/list response missing")

    def starts(self):
        return len(self.marker.read_text().splitlines()) if self.marker.exists() else 0

    def status(self):
        result = self.command("mcp", "status", "stub", "--agent", "codex", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def reconnect(self):
        return subprocess.run([runtime_base.BINARY, "--home", str(self.home), "mcp", "reconnect", "stub",
                               "--agent", "codex", "--json"], text=True, capture_output=True, timeout=60)

    def readable(self, seconds):
        return bool(select.select([self.process.stdout], [], [], max(0, seconds))[0])

    def dead_pid(self):
        process = subprocess.Popen(["true"])
        process.wait()
        return process.pid

    def stale_socket(self, path):
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        endpoint.bind(str(path))
        endpoint.close()

    # 1: extra upstream capabilities
    def test_extra_upstream_capabilities_are_ignored(self):
        self.environment(KM_QA_CAPS=json.dumps({"experimental": {}, "logging": {}, "completions": {},
            "prompts": {"listChanged": False}, "resources": {"subscribe": False, "listChanged": False},
            "tools": {"listChanged": False}}))
        self.bridge()
        self.assertEqual(self.tools()["result"]["tools"][0]["name"], "echo")
        self.assertIn("result", self.call())

    def test_upstream_without_tools_capability_is_rejected(self):
        self.environment(KM_QA_CAPS='{"prompts":{}}')
        self.bridge()
        response = self.tools()
        self.assertIn("tools capability", response["error"]["message"])

    # 2: startup budget, retry, and recovery notification
    def test_slow_upstream_start_completes(self):
        self.environment(KM_QA_DELAY=6)
        self.bridge()
        self.assertEqual(self.tools(timeout=20)["result"]["tools"][0]["name"], "echo")

    def test_failed_start_retries_and_announces_recovery(self):
        self.environment(KM_QA_FAIL_FIRST=self.home / "failed-once")
        self.bridge()
        self.assertIn("error", self.tools())
        self.send({"id": 3, "method": "tools/call", "params": {"name": "echo", "arguments": {}}})
        packets = [self.packet(), self.packet()]
        self.assertIn({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, packets)
        self.assertIn("result", next(p for p in packets if p.get("id") == 3))
        self.assertEqual(self.starts(), 2)

    def test_reconnect_waits_for_slow_upstream_discovery(self):
        self.environment(KM_QA_DELAY=6)
        self.bridge()
        result = self.reconnect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "reconnected")
        self.assertEqual(self.packet()["method"], "notifications/tools/list_changed")

    # 3: crash loop and backoff
    def test_crashing_upstream_does_not_cause_relist_loop(self):
        self.environment(KM_QA_CRASH_LIST=1)
        self.bridge()
        request_id, deadline = 2, time.monotonic() + 3
        self.send({"id": request_id, "method": "tools/list"})
        while time.monotonic() < deadline:
            if b"\n" not in self.buffer and not self.readable(deadline - time.monotonic()):
                break
            if self.packet().get("method") == "notifications/tools/list_changed":
                request_id += 1
                self.send({"id": request_id, "method": "tools/list"})
        self.assertLessEqual(self.starts(), 2)

    def test_repeated_start_failures_back_off(self):
        self.environment(KM_QA_ALWAYS_FAIL=1)
        self.bridge()
        for request_id in (2, 3, 4):
            self.assertIn("error", self.tools(request_id))
        self.assertEqual(self.starts(), 2)

    # 4: stale control sockets
    def test_sighup_removes_control_socket(self):
        self.bridge()
        self.assertEqual(len(list(self.sockets.iterdir())), 1)
        self.process.send_signal(signal.SIGHUP)
        self.process.wait(timeout=5)
        self.assertEqual(list(self.sockets.iterdir()), [])

    def test_scans_prune_sockets_of_exited_bridges(self):
        self.bridge()
        live = next(self.sockets.iterdir())
        stale = live.with_name(live.name.split(".")[0] + "." + str(self.dead_pid()))
        self.stale_socket(stale)
        self.assertEqual(self.status()["state"], "idle")
        self.assertFalse(stale.exists())
        self.stale_socket(stale)
        result = self.reconnect()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(stale.exists())

    def test_bridge_replaces_stale_socket_carrying_its_pid(self):
        self.bridge()
        prefix = next(self.sockets.iterdir()).name.split(".")[0]
        self.close_bridge()
        code = ("import os, socket, sys\n"
                "s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1] + '.' + str(os.getpid())); s.close()\n"
                "os.execv(sys.argv[2], sys.argv[2:])\n")
        self.process = subprocess.Popen([sys.executable, "-c", code, str(self.sockets / prefix), runtime_base.BINARY,
                                         "--home", str(self.home), "bridge", "stub", "--agent", "codex"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.send({"id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}}})
        self.assertIn("result", self.reply(1))

    # 5: parser limits
    def test_deeply_nested_result_is_forwarded(self):
        self.bridge()
        self.send({"id": 3, "method": "tools/call", "params": {"name": "deep", "arguments": {"depth": 100}}})
        value = self.reply(3)["result"]["structuredContent"]
        for _ in range(100):
            value = value["k"]
        self.assertEqual(value, 1)

    def test_large_integer_is_forwarded_exactly(self):
        self.bridge()
        literal = "123456789012345678901234567890"
        self.send({"id": 3, "method": "tools/call", "params": {"name": "raw", "arguments": {"raw": f"[{literal}]"}}})
        deadline = time.monotonic() + 6
        while b"\n" not in self.buffer and time.monotonic() < deadline:
            if self.readable(.1):
                self.buffer += os.read(self.process.stdout.fileno(), 65536)
        line = self.buffer.split(b"\n", 1)[0]
        self.assertIn(literal.encode(), line)
        self.assertEqual(json.loads(line)["id"], 3)

    def test_too_deep_upstream_result_fails_only_that_call(self):
        self.bridge()
        upstream = self.call()["result"]["content"][0]["text"]
        self.send({"id": 4, "method": "tools/call", "params": {"name": "deep", "arguments": {"depth": 600}}})
        self.assertIn("nesting", self.reply(4)["error"]["message"])
        self.assertEqual(self.call(5)["result"]["content"][0]["text"], upstream)

    def test_too_deep_client_request_gets_correlated_error(self):
        self.bridge()
        nested = 1
        for _ in range(600):
            nested = {"k": nested}
        self.send({"id": "deep", "method": "tools/call", "params": {"name": "echo", "arguments": nested}})
        response = self.packet()
        self.assertEqual(response["id"], "deep")
        self.assertIn("error", response)

    # 6: graceful upstream stop
    def test_upstream_receives_eof_before_termination(self):
        marker = self.home / "eof"
        self.environment(KM_QA_EOF_MARKER=marker)
        self.bridge()
        self.assertIn("result", self.call())
        self.close_bridge()
        self.assertTrue(marker.exists())

    # 7: policy observation
    def test_in_place_catalog_edit_is_observed(self):
        self.bridge()
        self.assertIn("result", self.call())
        self.config["mcps"]["stub"]["agents"]["codex"] = "off"
        with self.catalog.open("r+") as stream:
            stream.truncate(0)
            stream.write(json.dumps(self.config))
        self.assertEqual(self.packet(3)["method"], "notifications/tools/list_changed")

    # 8: status while busy
    def test_status_reports_starting_while_upstream_initializes(self):
        self.environment(KM_QA_DELAY=3)
        self.bridge()
        self.send({"id": 2, "method": "tools/list"})
        time.sleep(.3)
        observed = self.status()
        self.assertEqual(observed["state"], "starting")
        self.assertGreater(observed["connections"][0]["upstream_pid"], 0)
        self.assertIn("result", self.reply(2))

    # 9: reconnect failure reasons
    def test_reconnect_reports_bridge_failure_reason(self):
        self.environment(KM_QA_CAPS='{"prompts":{}}')
        self.bridge()
        result = self.reconnect()
        self.assertNotEqual(result.returncode, 0)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "failed")
        self.assertIn("tools capability", report["failures"][0]["message"])
        self.assertIn("tools capability", result.stderr)
        text = self.command("mcp", "reconnect", "stub", "--agent", "codex")
        self.assertNotEqual(text.returncode, 0)
        self.assertIn("failed: Managed bridge requires an MCP endpoint with the tools capability", text.stdout)

    def test_direct_reconnect_text_output_gives_client_advice(self):
        self.config["mcps"]["stub"]["mode"] = "direct"
        self.save()
        result = self.command("mcp", "reconnect", "stub", "--agent", "opencode")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("/mcps", result.stdout)

    # 10: tolerant framing and early ping
    def test_upstream_noise_lines_are_skipped(self):
        self.environment(KM_QA_NOISE=1)
        self.bridge()
        self.assertEqual(self.tools()["result"]["tools"][0]["name"], "echo")
        self.assertIn("result", self.call())

    def test_ping_is_answered_before_initialize(self):
        self.start()
        self.send({"id": "early", "method": "ping"})
        self.assertEqual(self.reply("early")["result"], {})


if __name__ == "__main__":
    unittest.main()
