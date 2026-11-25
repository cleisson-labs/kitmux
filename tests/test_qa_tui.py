"""Regression tests for interactive defects found during the v0.5 QA review."""
import json
import time
import unittest

from test_tui import TerminalTestCase

LEFT = b"\x1bOD"


class QaTerminalTests(TerminalTestCase):
    def catalog(self):
        return json.loads((self.home / ".config/kitmux/catalog.json").read_text())

    def add_mcp(self, name):
        self.run_cli("mcp", "add", name, "--command", "/usr/bin/true")

    def conflicting_claude_setup(self):
        claude = self.home / ".claude.json"
        claude.write_text(json.dumps({"mcpServers": {
            "alpha": {"command": "/usr/bin/true", "args": ["--stdio"]},
            "gamma": {"command": "/bin/cat"}}}))
        self.run_cli("import", "--from", "claude", "--apply")
        data = json.loads(claude.read_text())
        data["mcpServers"]["alpha"]["args"] = ["--changed-outside-kitmux"]
        claude.write_text(json.dumps(data))

    def quit(self, terminal):
        terminal.send(b"q")
        self.assertEqual(terminal.wait_exit(), 0)

    def test_external_group_removal_does_not_crash_after_a_command(self):
        self.add_mcp("alpha")
        self.run_cli("mcp", "group", "add", "grp")
        self.run_cli("mcp", "group", "include", "grp", "alpha")
        terminal = self.start()
        terminal.wait_text("grp")
        self.run_cli("mcp", "group", "remove", "grp")
        terminal.send(b"c")
        terminal.read_for(1.0)
        self.assertIsNone(terminal.process.poll(), terminal.output[-2000:])
        terminal.send(b"\x1b")
        self.quit(terminal)

    def test_external_catalog_change_refreshes_the_list(self):
        self.add_mcp("alpha")
        terminal = self.start()
        terminal.wait_text("alpha")
        self.run_cli("mcp", "add", "zeta", "--command", "/usr/bin/true")
        terminal.wait_text("zeta", timeout=3)
        self.quit(terminal)

    def test_assign_dialog_on_globally_disabled_item_keeps_assignments(self):
        self.add_mcp("gamma")
        self.run_cli("mcp", "enable", "gamma", "--agent", "claude")
        self.run_cli("mcp", "disable", "gamma")
        terminal = self.start()
        terminal.wait_text("gamma")
        terminal.send(b"ja")
        terminal.wait_text("Globally OFF")
        terminal.send(b" \n")
        terminal.wait_text("Assignments")
        self.quit(terminal)
        gamma = self.catalog()["mcps"]["gamma"]
        self.assertEqual(gamma["agents"], {"claude": "on", "codex": "on"})
        self.assertFalse(gamma["enabled"])

    def test_assign_dialog_without_changes_keeps_group_inheritance(self):
        self.add_mcp("beta")
        self.run_cli("mcp", "enable", "beta", "--agent", "codex")
        self.run_cli("mcp", "group", "add", "team")
        self.run_cli("mcp", "group", "include", "team", "beta")
        self.run_cli("mcp", "group", "assign", "team", "--agents", "claude")
        terminal = self.start()
        terminal.wait_text("beta")
        terminal.send(b"ja")
        terminal.wait_text("Assign agents")
        terminal.send(b"\n")
        terminal.wait_text("Assignments")
        self.quit(terminal)
        self.assertEqual(self.catalog()["mcps"]["beta"]["agents"], {"codex": "on"})

    def test_assign_dialog_saves_every_client_when_native_sync_is_pending(self):
        self.conflicting_claude_setup()
        terminal = self.start()
        terminal.wait_text("gamma")
        terminal.send(b"jja")
        terminal.wait_text("Assign agents")
        terminal.send(b" jj \n")
        terminal.wait_text("Native sync pending")
        self.quit(terminal)
        agents = self.catalog()["mcps"]["gamma"]["agents"]
        self.assertEqual((agents.get("codex"), agents.get("opencode")), ("on", "on"))

    def test_pending_native_sync_is_visible_on_start_and_in_clients_tab(self):
        self.conflicting_claude_setup()
        terminal = self.start()
        terminal.wait_text("Native sync pending")
        terminal.send(b"4")
        terminal.wait_text("Native sync pending: configuration conflict")
        self.assertIn(b"claude: adopted", terminal.output)
        self.quit(terminal)

    def test_bulk_toggle_includes_marked_rows_hidden_in_collapsed_group(self):
        self.add_mcp("alpha")
        self.add_mcp("beta")
        self.run_cli("mcp", "group", "add", "grp")
        self.run_cli("mcp", "group", "include", "grp", "alpha")
        terminal = self.start()
        terminal.wait_text("beta")
        terminal.send(b"j ")
        terminal.send(b"k" + LEFT)
        terminal.wait_text("grp collapsed")
        terminal.send(b"e")
        terminal.wait_text("disabled", timeout=3)
        self.quit(terminal)
        self.assertFalse(self.catalog()["mcps"]["alpha"]["enabled"])
        self.assertTrue(self.catalog()["mcps"]["beta"].get("enabled", True))

    def test_escape_responds_without_terminal_escape_delay(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.read_for(0.3)
        started = time.monotonic()
        terminal.send(b"\x1b")
        terminal.wait_text("Filter cleared.", timeout=3)
        self.assertLess(time.monotonic() - started, 0.6)
        self.quit(terminal)

    def test_cancelled_search_does_not_apply_typed_filter(self):
        self.add_mcp("alpha")
        terminal = self.start()
        terminal.wait_text("alpha")
        terminal.send(b"/")
        terminal.wait_text("Search")
        terminal.send(b"zzz")
        terminal.read_for(0.3)
        terminal.send(b"\x1b")
        after = terminal.read_for(1.0)
        self.assertNotIn(b"Filter: zzz", after)
        self.assertIn(b"alpha", after)
        terminal.send(b"/zzz\n")
        terminal.wait_text("No matches")
        self.quit(terminal)

    def test_long_ids_remain_distinguishable(self):
        self.add_mcp("a-very-long-mcp-server-alpha")
        self.add_mcp("a-very-long-mcp-server-omega")
        terminal = self.start()
        terminal.wait_text("a-very-long-mcp-server-alpha")
        terminal.read_for(0.3)
        self.assertIn(b"a-very-long-mcp-server-omega", terminal.output)
        self.quit(terminal)

    def test_doctor_shows_complete_output(self):
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"c")
        terminal.wait_text("opencode:")
        terminal.send(b"\x1b")
        self.quit(terminal)

    def test_edit_key_on_skill_explains_update_and_rollback(self):
        package = self.home / "notes"
        package.mkdir()
        (package / "SKILL.md").write_text("---\nname: notes\ndescription: Notes\n---\nTake notes.\n")
        self.run_cli("skill", "add", str(package))
        terminal = self.start()
        terminal.wait_text("1 MCPs")
        terminal.send(b"2jx")
        terminal.wait_text("Skills change with u")
        self.quit(terminal)


if __name__ == "__main__":
    unittest.main()
