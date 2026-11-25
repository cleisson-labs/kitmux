"""Antigravity CLI (agy) client support: MCP, skills, subagents, and compatibility."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))

MCP = ".gemini/config/mcp_config.json"
SKILLS = ".gemini/config/skills"
MANIFEST = ".gemini/config/skills.json"
AGENTS = ".gemini/config/agents"

NATIVE = {
    "mcpServers": {
        "fs": {"command": "/usr/bin/true", "args": ["--flag"], "env": {"TOKEN": "fixture-secret"},
               "disabled": False, "enabledTools": ["read"], "timeoutSeconds": 30},
        "api": {"serverUrl": "https://api.invalid/mcp", "headers": {"Authorization": "Bearer fixture"}},
        "legacy": {"url": "https://legacy.invalid/mcp"},
        "off": {"command": "/usr/bin/env", "disabled": True},
    },
    "owner": {"keep": True},
}


class AntigravityCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name).resolve()
        self.catalog_path = self.home / ".config/kitmux/catalog.json"
        self.journal = self.home / ".local/state/kitmux/adapters.json"

    def cli(self, *args, ok=True):
        result = subprocess.run([BINARY, "--home", str(self.home), *args],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def write(self, relative, text):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def catalog(self):
        return json.loads(self.catalog_path.read_text())

    def servers(self):
        return json.loads((self.home / MCP).read_text()).get("mcpServers", {})

    def skill_package(self, name="review"):
        source = self.home / "sources" / name
        source.mkdir(parents=True)
        (source / "SKILL.md").write_text(f"---\nname: {name}\ndescription: Review code\n---\nReview carefully.\n")
        return source


class McpTests(AntigravityCase):
    def test_import_reads_native_shapes_and_leaves_bytes_unchanged(self):
        path = self.write(MCP, json.dumps(NATIVE, indent=2))
        original = path.read_bytes()
        self.cli("import", "--from", "antigravity", "--apply")
        mcps = self.catalog()["mcps"]
        self.assertEqual(mcps["fs"]["command"], ["/usr/bin/true", "--flag"])
        self.assertEqual(mcps["api"]["url"], "https://api.invalid/mcp")
        self.assertEqual(mcps["legacy"]["url"], "https://legacy.invalid/mcp")
        self.assertEqual({name: mcps[name]["agents"]["antigravity"] for name in mcps},
                         {"fs": "on", "api": "on", "legacy": "on", "off": "off"})
        self.cli("sync", "--check")
        self.cli("mcp", "disable", "fs", "--agent", "antigravity")
        self.assertTrue(self.servers()["fs"]["disabled"])
        self.assertEqual(self.servers()["fs"]["enabledTools"], ["read"])
        self.cli("restore", "--agent", "antigravity", "--apply")
        self.assertEqual(json.loads(path.read_text()), json.loads(original))

    def test_blank_config_created_by_agy_is_an_empty_configuration(self):
        path = self.write(MCP, "")
        self.cli("import", "--from", "antigravity", "--apply")
        self.cli("sync", "--check")
        self.assertEqual(path.read_bytes(), b"")
        self.cli("mcp", "add", "local", "--command", "/usr/bin/true")
        self.cli("mcp", "enable", "local", "--agent", "antigravity")
        self.assertEqual(self.servers(), {"local": {"command": "/usr/bin/true"}})

    def test_sync_projects_assigned_mcps_in_agy_shape_only(self):
        self.cli("mcp", "add", "local", "--command", "/usr/bin/true", "--", "--stdio")
        self.cli("mcp", "add", "remote", "--url", "https://remote.invalid/mcp")
        self.cli("mcp", "add", "unassigned", "--command", "/usr/bin/false")
        for name in ("local", "remote"):
            self.cli("mcp", "enable", name, "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        servers = self.servers()
        self.assertEqual(servers["local"], {"command": "/usr/bin/true", "args": ["--stdio"]})
        self.assertEqual(servers["remote"], {"serverUrl": "https://remote.invalid/mcp"})
        self.assertNotIn("unassigned", servers)
        self.assertNotIn("\\/", (self.home / MCP).read_text())
        self.cli("mcp", "disable", "local", "--agent", "antigravity")
        self.assertNotIn("local", self.servers())

    def test_agy_rewrites_and_dropped_defaults_are_not_conflicts(self):
        claude = {"mcpServers": {"tool": {"command": "/usr/bin/true", "args": [], "env": {}}}}
        self.write(".claude.json", json.dumps(claude))
        self.cli("import", "--from", "claude", "--apply")
        self.cli("mcp", "enable", "tool", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertEqual(self.servers()["tool"], {"command": "/usr/bin/true"})
        rewritten = {"mcpServers": {"tool": {"command": "/usr/bin/true", "disabled": False}}}
        self.write(MCP, json.dumps(rewritten, indent=2, sort_keys=True))
        self.cli("sync", "--check")
        self.cli("mcp", "disable", "tool", "--agent", "claude")

    def test_claude_sse_endpoint_is_refused_for_antigravity(self):
        self.write(".claude.json", json.dumps({"mcpServers": {"events": {"type": "sse", "url": "https://sse.invalid/sse"}}}))
        self.cli("import", "--from", "claude,antigravity", "--apply")
        result = self.cli("mcp", "enable", "events", "--agent", "antigravity", ok=False)
        self.assertIn("SSE", result.stderr)
        self.assertIn("antigravity", result.stderr)
        self.assertFalse((self.home / MCP).exists())

    def test_reconnect_advice_names_the_agy_mcp_panel(self):
        self.cli("mcp", "add", "local", "--command", "/usr/bin/true")
        result = self.cli("mcp", "reconnect", "local", "--agent", "antigravity", ok=False)
        self.assertIn("/mcp", result.stdout + result.stderr)

    def test_managed_bridge_registers_for_antigravity(self):
        self.cli("mcp", "add", "bridged", "--command", "/usr/bin/true", "--mode", "managed")
        self.cli("mcp", "enable", "bridged", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        entry = self.servers()["bridged"]
        self.assertEqual(entry["args"][-4:], ["bridge", "bridged", "--agent", "antigravity"])
        self.cli("mcp", "disable", "bridged", "--agent", "antigravity")
        self.assertNotIn("disabled", self.servers()["bridged"])

    def test_native_source_format_accepts_server_url(self):
        source = self.write("api.json", json.dumps({"serverUrl": "https://api.invalid/mcp", "timeoutSeconds": 9}))
        self.cli("mcp", "add", "api", "--from", str(source), "--format", "antigravity")
        self.cli("mcp", "enable", "api", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertEqual(self.servers()["api"], {"serverUrl": "https://api.invalid/mcp", "timeoutSeconds": 9})


class SkillTests(AntigravityCase):
    def test_skill_projects_into_gemini_config_skills(self):
        self.cli("skill", "add", str(self.skill_package()))
        self.cli("skill", "enable", "review", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        link = self.home / SKILLS / "review"
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), self.catalog()["skills"]["review"]["path"])
        self.assertFalse((self.home / MANIFEST).exists())
        self.cli("skill", "disable", "review", "--agent", "antigravity")
        self.assertFalse(link.exists() or link.is_symlink())

    def test_shared_root_skill_off_for_antigravity_is_excluded_by_manifest(self):
        manifest = self.write(MANIFEST, json.dumps({"exclude": ["owner-hidden"]}, indent=2))
        self.cli("skill", "add", str(self.skill_package()))
        self.cli("skill", "enable", "review", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertTrue((self.home / ".agents/skills/review").is_symlink())
        self.assertEqual(json.loads(manifest.read_text())["exclude"], ["owner-hidden", "review"])
        self.cli("skill", "enable", "review", "--agent", "antigravity")
        self.assertEqual(json.loads(manifest.read_text())["exclude"], ["owner-hidden"])
        self.cli("skill", "disable", "review", "--agent", "antigravity")
        self.assertIn("review", json.loads(manifest.read_text())["exclude"])
        self.cli("restore", "--agent", "antigravity", "--apply")
        self.assertEqual(json.loads(manifest.read_text()), {"exclude": ["owner-hidden"]})

    def test_created_manifest_is_removed_when_no_longer_needed(self):
        self.cli("skill", "add", str(self.skill_package()))
        self.cli("skill", "enable", "review", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "codex")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertEqual(json.loads((self.home / MANIFEST).read_text()), {"exclude": ["review"]})
        self.cli("skill", "disable", "review", "--agent", "codex")
        self.assertFalse((self.home / MANIFEST).exists())

    def test_import_adopts_skill_links_and_native_exclusions(self):
        first, second = self.skill_package("review"), self.skill_package("lint")
        root = self.home / SKILLS
        root.mkdir(parents=True)
        (root / "review").symlink_to(first)
        (root / "lint").symlink_to(second)
        manifest = self.write(MANIFEST, json.dumps({"exclude": ["lint"]}))
        self.cli("import", "--from", "antigravity", "--apply")
        skills = self.catalog()["skills"]
        self.assertEqual(skills["review"]["agents"]["antigravity"], "on")
        self.assertEqual(skills["lint"]["agents"]["antigravity"], "off")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.cli("sync", "--check")
        self.assertEqual(os.readlink(root / "review"), skills["review"]["path"])
        self.assertEqual(json.loads(manifest.read_text()), {"exclude": ["lint"]})


class SubagentTests(AntigravityCase):
    def test_portable_profile_projects_and_is_removed_when_off(self):
        source = self.write("review.md", "Review changes carefully.\n")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--description", "Review: code")
        self.cli("subagent", "enable", "reviewer", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        path = self.home / AGENTS / "reviewer.md"
        text = path.read_text()
        self.assertTrue(text.startswith("---\nname: \"reviewer\"\ndescription: \"Review: code\"\n---\n"))
        self.cli("subagent", "disable", "reviewer", "--agent", "antigravity")
        self.assertFalse(path.exists())

    def test_flat_and_folder_definitions_import_and_edit_in_place(self):
        flat = self.write(f"{AGENTS}/reviewer.md",
                          "---\nname: reviewer\ndescription: Reviews code\nmodel: gemini-pro\n"
                          "skills: [review]\n---\nReview carefully.\n")
        folder = self.write(f"{AGENTS}/planner/agent.md",
                            "---\nname: planner\ndescription: Plans work\nsubagent: true\n---\nPlan first.\n")
        self.write(f"{AGENTS}/notes/README.md", "Not an agent.\n")
        originals = (flat.read_bytes(), folder.read_bytes())
        self.cli("import", "--from", "antigravity", "--apply")
        subagents = self.catalog()["subagents"]
        self.assertEqual(subagents["reviewer"]["native"]["antigravity"]["requires"], ["skill:review"])
        self.assertEqual(subagents["planner"]["origins"]["antigravity"]["path"], str(folder))
        self.cli("sync", "--check")
        source = self.write("plan.md", "Plan before editing.\n")
        self.cli("subagent", "edit", "planner", "--from", str(source))
        self.assertIn("Plan before editing.", folder.read_text())
        self.assertIn("subagent: true", folder.read_text())
        self.assertFalse((self.home / AGENTS / "planner.md").exists())
        self.cli("restore", "--agent", "antigravity", "--apply")
        self.assertEqual((flat.read_bytes(), folder.read_bytes()), originals)

    def test_folder_definition_blocks_a_shadowing_flat_projection(self):
        folder = self.write(f"{AGENTS}/reviewer/agent.md",
                            "---\nname: reviewer\ndescription: Owner reviewer\n---\nOwner text.\n")
        source = self.write("review.md", "Review.\n")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--description", "Review code")
        self.cli("subagent", "enable", "reviewer", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity", ok=False)
        self.assertFalse((self.home / AGENTS / "reviewer.md").exists())
        self.assertIn("Owner text.", folder.read_text())

    def test_native_antigravity_format_round_trips(self):
        text = "---\nname: helper\ndescription: Helps\ntools: [read_file]\nmcpServers: [fs]\n---\nHelp out.\n"
        source = self.write("helper.md", text)
        self.cli("subagent", "add", "helper", "--from", str(source), "--format", "antigravity")
        self.cli("subagent", "enable", "helper", "--agent", "antigravity")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertEqual((self.home / AGENTS / "helper.md").read_text(), text)
        self.assertEqual(self.catalog()["subagents"]["helper"]["native"]["antigravity"]["requires"], ["mcp:fs"])


class CompatibilityTests(AntigravityCase):
    def test_v06_state_loads_with_antigravity_unassigned_and_unadopted(self):
        self.write(".config/kitmux/catalog.json", json.dumps({
            "version": 3, "revision": 4, "skills": {}, "subagents": {}, "groups": {},
            "mcps": {"tool": {"transport": "stdio", "command": ["/usr/bin/true"], "mode": "direct",
                              "enabled": True, "agents": {"codex": "on"}}}}))
        self.write(".local/state/kitmux/adapters.json", json.dumps(
            {"version": 1, "files": {}, "managed_agents": {"codex": True, "claude": True, "opencode": True}}))
        status = json.loads(self.cli("status", "--json").stdout)["mcps"]["tool"]["agents"]
        self.assertFalse(status["antigravity"]["enabled"])
        self.cli("mcp", "disable", "tool", "--agent", "claude")
        self.assertFalse((self.home / ".gemini").exists())
        self.assertIn("antigravity", self.cli("doctor").stdout)

    def test_groups_and_selectors_accept_antigravity(self):
        self.cli("mcp", "add", "tool", "--command", "/usr/bin/true")
        self.cli("mcp", "group", "add", "work")
        self.cli("mcp", "group", "include", "work", "tool")
        self.cli("mcp", "group", "assign", "work", "--agents", "codex,antigravity")
        self.cli("mcp", "group", "enable", "work")
        self.cli("sync", "--apply", "--agent", "antigravity")
        self.assertIn("tool", self.servers())
        self.assertIn("antigravity", self.cli("--help").stdout)


if __name__ == "__main__":
    unittest.main()
