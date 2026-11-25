"""Opt-in qualification against the installed Antigravity CLI (agy).

Runs agy only for offline configuration commands (mcp add/list/enable/
disable, agents) under macOS sandbox-exec with a temporary HOME, an otherwise
empty environment, no outbound network, and no reads of the real home or
keychains. It never runs a prompt, signs in, or contacts a model.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("KITMUX_BINARY", ROOT / "build/kitmux")).resolve()
AGY = shutil.which("agy")
SANDBOX = Path("/usr/bin/sandbox-exec")
ENABLED = (os.environ.get("KITMUX_NATIVE_TESTS") == "1" and AGY is not None
           and sys.platform == "darwin" and SANDBOX.is_file())
MCP = ".gemini/config/mcp_config.json"


@unittest.skipUnless(ENABLED, "opt-in macOS native Antigravity qualification (needs agy)")
class AntigravityNativeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="km-agy-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name).resolve()
        (self.home / "tmp").mkdir()
        self.executable = os.path.realpath(AGY)
        self.env = {"PATH": "/usr/bin:/bin", "HOME": str(self.home), "TMPDIR": str(self.home / "tmp")}

    def profile(self):
        real_home = json.dumps(str(Path.home()))
        return "\n".join([
            "(version 1)", "(allow default)", "(deny mach-lookup)",
            f"(deny file-read* (subpath {real_home}))", f"(deny file-write* (subpath {real_home}))",
            f"(allow file-read* (literal {json.dumps(self.executable)}))",
            '(deny file-read* (subpath "/Library/Keychains"))',
            '(deny process-exec (literal "/usr/bin/security"))',
            "(deny network-outbound)",
        ])

    def agy(self, *args):
        result = subprocess.run([str(SANDBOX), "-p", self.profile(), self.executable, *args],
                                cwd=self.home, env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def kitmux(self, *args, ok=True):
        result = subprocess.run([str(BINARY), "--home", str(self.home), *args], cwd=self.home,
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def listed(self):
        rows = [line.split() for line in self.agy("mcp", "list").splitlines()[1:] if line.strip()]
        return {row[0]: (row[1], row[2]) for row in rows}

    def test_agy_reads_kitmux_projections_and_its_rewrites_stay_current(self):
        self.agy("mcp", "add", "--env", "TOKEN=fixture-secret", "fs", "/usr/bin/true", "--flag")
        self.agy("mcp", "add", "--header", "Authorization: Bearer fixture", "api", "http://127.0.0.1:9/mcp")
        original = json.loads((self.home / MCP).read_text())
        self.kitmux("import", "--from", "antigravity", "--apply")
        self.kitmux("sync", "--check")
        self.kitmux("mcp", "add", "local", "--command", "/usr/bin/true", "--", "--stdio")
        self.kitmux("mcp", "enable", "local", "--agent", "antigravity")
        self.kitmux("mcp", "disable", "fs", "--agent", "antigravity")
        self.assertEqual(self.listed(), {"api": ("http", "enabled"), "fs": ("stdio", "disabled"),
                                         "local": ("stdio", "enabled")})
        self.agy("mcp", "enable", "local")
        self.kitmux("sync", "--check")
        self.kitmux("restore", "--agent", "antigravity", "--apply")
        self.assertEqual(json.loads((self.home / MCP).read_text()), original)

    def test_agy_disable_is_adopted_by_refresh(self):
        self.agy("mcp", "add", "fs", "/usr/bin/true")
        self.kitmux("import", "--from", "antigravity", "--apply")
        self.agy("mcp", "disable", "fs")
        self.kitmux("mcp", "add", "other", "--command", "/usr/bin/true", ok=False)
        self.kitmux("mcp", "enable", "other", "--agent", "antigravity", ok=False)
        self.kitmux("import", "--refresh", "--from", "antigravity", "--apply")
        catalog = json.loads((self.home / ".config/kitmux/catalog.json").read_text())
        self.assertEqual(catalog["mcps"]["fs"]["agents"]["antigravity"], "off")
        self.kitmux("sync", "--apply", "--agent", "antigravity")
        self.assertEqual(self.listed(), {"fs": ("stdio", "disabled"), "other": ("stdio", "enabled")})

    def test_agy_lists_a_kitmux_projected_subagent(self):
        self.kitmux("import", "--from", "antigravity", "--apply")
        source = self.home / "review.md"
        source.write_text("Review changes carefully.\n")
        self.kitmux("subagent", "add", "reviewer", "--from", str(source), "--description", "Review: code and # notes")
        self.kitmux("subagent", "enable", "reviewer", "--agent", "antigravity")
        self.assertIn("reviewer", self.agy("agents").split())
        self.kitmux("subagent", "disable", "reviewer", "--agent", "antigravity")
        self.assertNotIn("reviewer", self.agy("agents").split())


if __name__ == "__main__":
    unittest.main()
