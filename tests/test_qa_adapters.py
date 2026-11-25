"""Regression tests for native MCP adapter sync, isolation, and preservation."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))

CODEX = """# top comment
model = "x"

[mcp_servers.alpha]
command = "/usr/bin/true" # cmd comment
args = ["--a"]
env = { TOKEN = "secret-a" }

[mcp_servers.beta]
command = "/usr/bin/env"
enabled = false
"""

CLAUDE = {
    "numStartups": 3,
    "mcpServers": {
        "alpha": {"type": "stdio", "command": "/usr/bin/true", "args": ["--a"], "env": {"TOKEN": "secret-a"}},
        "gamma": {"type": "http", "url": "https://example.invalid/mcp",
                  "headers": {"Authorization": "Bearer fixture"}},
    },
}

OPENCODE = """{
  // opencode comment
  "theme": "dark",
  "mcp": {
    "servers": {
      "alpha": {
        "type": "local",
        "command": ["/usr/bin/true", "--a"],
        "environment": {"TOKEN": "secret-a"}
      },
      "beta": {"type": "local", "command": ["/usr/bin/env"], "disabled": true}
    }
  }
}
"""


def jsonc(text):
    text = re.sub(r'("(?:\\.|[^"\\])*")|//[^\n]*', lambda match: match.group(1) or "", text)
    return json.loads(re.sub(r",(\s*[}\]])", r"\1", text))


class NativeSyncCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.codex = self.home / ".codex/config.toml"
        self.claude = self.home / ".claude.json"
        self.opencode = self.home / ".config/opencode/opencode.jsonc"
        self.catalog = self.home / ".config/kitmux/catalog.json"
        self.journal = self.home / ".local/state/kitmux/adapters.json"

    def cli(self, *args, ok=True, binary=BINARY):
        result = subprocess.run([binary, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=20)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def write(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def native_clients(self, codex=CODEX, claude=CLAUDE, opencode=OPENCODE):
        if codex is not None:
            self.write(self.codex, codex)
        if claude is not None:
            self.write(self.claude, json.dumps(claude, indent=2))
        if opencode is not None:
            self.write(self.opencode, opencode)

    def import_all(self, clients="codex,claude,opencode"):
        self.cli("import", "--from", clients, "--apply")

    def codex_servers(self):
        return tomllib.loads(self.codex.read_text()).get("mcp_servers", {})

    def claude_servers(self):
        return json.loads(self.claude.read_text()).get("mcpServers", {})

    def opencode_servers(self):
        return jsonc(self.opencode.read_text())["mcp"]["servers"]

    def assignments(self, mcp):
        return json.loads(self.catalog.read_text())["mcps"][mcp].get("agents", {})

    def codex_record(self, journal, mcp):
        record = next(r for r in journal["files"].values() if r["agent"] == "codex")
        return record["entries"][mcp]

    def edit_claude(self, change):
        doc = json.loads(self.claude.read_text())
        change(doc["mcpServers"])
        self.claude.write_text(json.dumps(doc, indent=2))


class ConflictIsolationTests(NativeSyncCase):
    def test_one_client_conflict_does_not_block_other_clients(self):
        self.native_clients()
        self.import_all()
        self.edit_claude(lambda servers: servers["alpha"]["env"].update({"EXTRA": "1"}))
        claude_before = self.claude.read_bytes()
        result = self.cli("mcp", "disable", "alpha", ok=False)
        self.assertFalse(self.codex_servers()["alpha"]["enabled"])
        self.assertTrue(self.opencode_servers()["alpha"]["disabled"])
        self.assertEqual(self.claude.read_bytes(), claude_before)
        self.assertIn("claude", result.stderr)
        self.assertIn(str(self.claude), result.stderr)
        self.assertIn("mcpServers/alpha", result.stderr)
        self.assertIn("import --refresh --from claude", result.stderr)
        self.assertNotIn("claude MCP alpha", result.stdout)

    def test_restore_isolates_client_conflicts(self):
        self.native_clients()
        self.import_all()
        self.cli("mcp", "disable", "alpha")
        self.edit_claude(lambda servers: servers["gamma"].update({"url": "https://changed.invalid/mcp"}))
        self.cli("restore", "--apply", ok=False)
        self.assertEqual(tomllib.loads(self.codex.read_text()), tomllib.loads(CODEX))

    def test_deleted_owned_entry_matching_desired_state_is_not_a_conflict(self):
        self.native_clients()
        self.import_all()
        self.edit_claude(lambda servers: servers.pop("alpha"))
        self.cli("mcp", "disable", "alpha", "--agent", "claude")
        self.cli("sync", "--check")

    def test_refresh_adopts_native_deletion_as_client_off(self):
        self.native_clients()
        self.import_all()
        self.edit_claude(lambda servers: servers.pop("alpha"))
        result = self.cli("mcp", "disable", "beta", "--agent", "opencode", ok=False)
        self.assertIn("import --refresh --from claude", result.stderr)
        before = self.assignments("alpha")
        self.cli("import", "--refresh", "--from", "claude", "--apply")
        after = self.assignments("alpha")
        self.assertEqual(after["claude"], "off")
        self.assertEqual({k: v for k, v in after.items() if k != "claude"},
                         {k: v for k, v in before.items() if k != "claude"})
        self.cli("sync", "--check")
        self.assertNotIn("alpha", self.claude_servers())


class AdoptionScopeTests(NativeSyncCase):
    def test_subagent_import_does_not_adopt_mcp_management(self):
        self.native_clients(opencode=None)
        self.import_all("codex")
        definition = self.home / ".claude/agents/rev.md"
        self.write(definition, "---\nname: rev\ndescription: Reviews code\n---\nReview carefully.\n")
        claude_before = self.claude.read_bytes()
        self.cli("subagent", "import", "--from", "claude", "--apply")
        self.cli("mcp", "disable", "alpha", "--agent", "codex")
        self.assertFalse(self.codex_servers()["alpha"]["enabled"])
        self.assertEqual(self.claude.read_bytes(), claude_before)
        self.cli("subagent", "disable", "rev", "--agent", "claude")
        self.assertFalse(definition.exists())
        self.assertEqual(self.claude.read_bytes(), claude_before)


class ProjectionTests(NativeSyncCase):
    SECRET = {"mcpServers": {"secretive": {"command": "/usr/bin/true", "env": {"API_TOKEN": "sk-FAKE"}}}}

    def test_unassigned_mcp_credentials_are_not_projected(self):
        self.native_clients(claude=self.SECRET)
        self.import_all()
        self.cli("mcp", "disable", "alpha", "--agent", "opencode")
        self.assertNotIn("secretive", self.codex_servers())
        self.assertNotIn("secretive", self.opencode_servers())
        self.assertNotIn("sk-FAKE", self.codex.read_text() + self.opencode.read_text())

    def test_kitmux_created_entry_is_removed_when_turned_off(self):
        self.native_clients(claude=self.SECRET)
        self.import_all()
        self.cli("mcp", "enable", "secretive", "--agent", "codex")
        self.assertNotIn("enabled", self.codex_servers()["secretive"])
        self.cli("mcp", "disable", "secretive", "--agent", "codex")
        self.assertNotIn("secretive", self.codex_servers())
        self.assertFalse(self.codex_servers()["beta"]["enabled"])

    def test_sync_check_is_clean_after_import(self):
        self.native_clients()
        self.import_all()
        self.cli("sync", "--check")

    def test_claude_sse_endpoint_is_not_projected_to_streamable_http_clients(self):
        claude = {"mcpServers": {"events": {"type": "sse", "url": "https://sse.invalid/sse"}}}
        self.native_clients(claude=claude)
        self.import_all()
        for client in ("codex", "opencode"):
            result = self.cli("mcp", "enable", "events", "--agent", client, ok=False)
            self.assertIn("SSE", result.stderr)
            self.assertIn(client, result.stderr)
        self.assertNotIn("events", self.codex_servers())
        self.assertNotIn("events", self.opencode_servers())
        self.assertEqual(self.claude_servers()["events"]["type"], "sse")


class RefreshPolicyTests(NativeSyncCase):
    def group_controls(self, mcp):
        self.native_clients(claude=None, opencode=None)
        self.import_all("codex")
        self.cli("mcp", "group", "add", "work")
        self.cli("mcp", "group", "include", "work", mcp)
        self.cli("mcp", "group", "assign", "work", "--agents", "codex")
        self.cli("mcp", "group", "enable", "work")

    def test_refresh_does_not_pin_inherited_availability(self):
        self.cli("mcp", "add", "delta", "--command", "/usr/bin/true")
        self.group_controls("delta")
        self.write(self.codex, self.codex.read_text().replace(
            '[mcp_servers.delta]\n', '[mcp_servers.delta]\nstartup_timeout_sec = 20\n'))
        self.cli("import", "--refresh", "--from", "codex", "--apply")
        self.assertNotIn("codex", self.assignments("delta"))
        self.cli("mcp", "group", "disable", "work")
        self.assertFalse(self.codex_servers().get("delta", {}).get("enabled", False))

    def test_refresh_records_native_state_that_contradicts_policy(self):
        self.group_controls("alpha")
        self.cli("mcp", "inherit", "alpha", "--agent", "codex")
        self.write(self.codex, self.codex.read_text().replace('args = ["--a"]', 'args = ["--a"]\nenabled = false'))
        self.cli("import", "--refresh", "--from", "codex", "--apply")
        self.assertEqual(self.assignments("alpha")["codex"], "off")


class DefaultNormalizationTests(NativeSyncCase):
    def test_stripped_client_defaults_are_not_conflicts(self):
        codex = CODEX.replace('args = ["--a"]', 'args = ["--a"]\nenabled = true')
        opencode = OPENCODE.replace('"environment": {"TOKEN": "secret-a"}',
                                    '"environment": {"TOKEN": "secret-a"},\n        "disabled": false')
        self.native_clients(codex=codex, opencode=opencode)
        self.import_all()
        self.write(self.codex, CODEX)
        self.write(self.opencode, OPENCODE)
        before = (self.codex.read_bytes(), self.opencode.read_bytes())
        self.cli("mcp", "disable", "gamma", "--agent", "claude")
        self.assertEqual((self.codex.read_bytes(), self.opencode.read_bytes()), before)
        self.cli("sync", "--check")

    def test_legacy_journal_with_explicit_enabled_default_keeps_working(self):
        self.native_clients(claude=None, opencode=None)
        self.import_all("codex")
        journal = json.loads(self.journal.read_text())
        record = self.codex_record(journal, "alpha")
        record["after"] = record["after"].replace('args = ["--a"]', 'args = ["--a"]\n"enabled" = true')
        self.journal.write_text(json.dumps(journal))
        self.cli("mcp", "disable", "alpha", "--agent", "codex")
        self.assertFalse(self.codex_servers()["alpha"]["enabled"])


class PreservationTests(NativeSyncCase):
    def test_comments_before_following_codex_tables_survive_rerender(self):
        self.native_clients()
        self.import_all()
        self.cli("mcp", "enable", "gamma", "--agent", "codex")
        self.write(self.codex, self.codex.read_text() +
                   "\n# ---- My profiles (hand-written, keep!) ----\n[profiles.fast]\nmodel = \"y\"\n")
        self.cli("mcp", "disable", "gamma", "--agent", "codex")
        self.assertIn("# ---- My profiles (hand-written, keep!) ----\n[profiles.fast]", self.codex.read_text())

    def test_removing_codex_table_keeps_comment_of_next_table(self):
        original = ('[mcp_servers.alpha]\ncommand = "/usr/bin/true"\n\n'
                    '# Beta: production docs server, do not touch\n[mcp_servers.beta]\ncommand = "/usr/bin/env"\n')
        self.native_clients(codex=original, claude=None, opencode=None)
        self.import_all("codex")
        self.cli("mcp", "remove", "alpha")
        self.assertIn("# Beta: production docs server, do not touch\n[mcp_servers.beta]", self.codex.read_text())

    def test_legacy_restore_baseline_does_not_duplicate_following_comment(self):
        original = ('[mcp_servers.alpha]\ncommand = "/usr/bin/true"\n\n'
                    '# Beta comment\n[mcp_servers.beta]\ncommand = "/usr/bin/env"\n')
        self.native_clients(codex=original, claude=None, opencode=None)
        self.import_all("codex")
        journal = json.loads(self.journal.read_text())
        record = self.codex_record(journal, "alpha")
        legacy = '[mcp_servers.alpha]\ncommand = "/usr/bin/true"\n\n# Beta comment\n'
        record["before"] = record["after"] = legacy
        self.journal.write_text(json.dumps(journal))
        self.cli("mcp", "disable", "alpha", "--agent", "codex")
        self.cli("restore", "--apply", "--agent", "codex")
        self.assertEqual(self.codex.read_text(), original)

    def test_lowercase_unicode_escape_in_codex_toml_is_accepted(self):
        codex = '[mcp_servers.alpha]\ncommand = "/usr/bin/caf\\u00e9"\n'
        self.native_clients(codex=codex, claude=None, opencode=None)
        self.import_all("codex")
        self.cli("mcp", "disable", "alpha", "--agent", "codex")
        self.assertEqual(self.codex_servers()["alpha"]["command"], "/usr/bin/café")

    def test_opencode_insert_uses_file_indentation_without_escaped_slashes(self):
        self.native_clients()
        self.import_all()
        self.cli("mcp", "enable", "gamma", "--agent", "opencode")
        text = self.opencode.read_text()
        self.assertNotIn("\\/", text)
        lines = text.splitlines()
        alpha = next(line for line in lines if line.strip().startswith('"alpha"'))
        gamma_index = next(i for i, line in enumerate(lines) if line.strip().startswith('"gamma"'))
        indent = len(alpha) - len(alpha.lstrip())
        self.assertEqual(len(lines[gamma_index]) - len(lines[gamma_index].lstrip()), indent)
        self.assertGreater(len(lines[gamma_index + 1]) - len(lines[gamma_index + 1].lstrip()), indent)
        self.assertEqual(self.opencode_servers()["gamma"]["url"], "https://example.invalid/mcp")
        self.cli("mcp", "disable", "gamma", "--agent", "opencode")
        self.assertFalse([line for line in self.opencode.read_text().splitlines() if line.strip() in ("", ",")])


class MessageTests(NativeSyncCase):
    def test_invalid_subagent_journal_keeps_its_specific_error(self):
        self.write(self.journal, json.dumps({"version": 2, "files": {}, "subagent_files": {"/outside": {}}}))
        result = self.cli("sync", "--check", ok=False)
        self.assertIn("subagent", result.stderr.lower())
        self.assertNotIn("invalid or unsafe adapter ownership record", result.stderr)

    def test_non_object_native_json_names_its_file(self):
        self.native_clients(codex=None, claude=None, opencode=None)
        self.write(self.claude, "[]\n")
        result = self.cli("import", "--from", "claude", "--apply", ok=False)
        self.assertIn(str(self.claude), result.stderr)


class BridgeExecutableTests(NativeSyncCase):
    def copy_binary(self, name):
        path = self.home / "bin" / name
        path.parent.mkdir(exist_ok=True)
        shutil.copy2(BINARY, path)
        return str(path.resolve())

    def bridge_program(self):
        return self.codex_servers()["alpha"]["command"]

    def test_managed_bridge_keeps_its_existing_executable(self):
        self.native_clients(claude=None, opencode=None)
        first, second = self.copy_binary("first"), self.copy_binary("second")
        self.import_all("codex")
        self.cli("mcp", "edit", "alpha", "--mode", "managed", binary=first)
        self.assertEqual(self.bridge_program(), first)
        self.cli("mcp", "disable", "beta", "--agent", "codex", binary=second)
        self.assertEqual(self.bridge_program(), first)
        self.cli("sync", "--check", binary=second)
        os.unlink(first)
        self.cli("mcp", "enable", "beta", "--agent", "codex", binary=second)
        self.assertEqual(self.bridge_program(), second)


if __name__ == "__main__":
    unittest.main()
