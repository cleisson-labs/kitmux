import fnmatch
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))
ROOTS = {"codex": ".agents/skills", "claude": ".claude/skills", "opencode": ".config/opencode/skills"}
OPENCODE = ".config/opencode/opencode.json"


def opencode_allows(rules, skill):
    """OpenCode evaluates permission rules with last-match-wins globbing."""
    effect = "allow"
    for rule in rules:
        if fnmatch.fnmatchcase("skill", rule["action"]) and fnmatch.fnmatchcase(skill, rule["resource"]):
            effect = rule["effect"]
    return effect != "deny"


class QaSkillTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="km-qa-skills-", dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()

    def cli(self, *args, ok=True, env=None, limits=None):
        result = subprocess.run([BINARY, "--home", str(self.home), *map(str, args)], text=True,
                                capture_output=True, timeout=60, env=env, preexec_fn=limits)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def write(self, relative, text):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def package(self, name, description="QA fixture", parent="src"):
        root = self.home / parent / name
        root.mkdir(parents=True, exist_ok=True)
        (root / "SKILL.md").write_text(f"---\nname: {name}\ndescription: {description}\n---\nBody of {name}\n")
        return root

    def catalog(self):
        return json.loads((self.home / ".config/kitmux/catalog.json").read_text())

    def skill(self, name):
        return self.catalog()["skills"][name]

    def rules(self):
        path = self.home / OPENCODE
        return json.loads(path.read_text()).get("permissions", []) if path.exists() else []

    def link(self, agent, name):
        return self.home / ROOTS[agent] / name

    def git(self, *args, cwd=None):
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        environment.update({"GIT_AUTHOR_NAME": "QA", "GIT_AUTHOR_EMAIL": "qa@example.invalid",
                            "GIT_COMMITTER_NAME": "QA", "GIT_COMMITTER_EMAIL": "qa@example.invalid"})
        return subprocess.run(["git", *map(str, args)], cwd=cwd, check=True, text=True,
                              capture_output=True, env=environment).stdout.strip()

    def repository(self, files):
        work = self.home / "work"
        work.mkdir()
        self.git("init", "-q", cwd=work)
        for relative, text in files.items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.git("add", "-A", cwd=work)
        self.git("commit", "-qm", "fixture", cwd=work)
        bare = self.home / "remote.git"
        self.git("clone", "-q", "--bare", work, bare)
        return bare, self.git("rev-parse", "HEAD", cwd=work)

    def test_reimport_keeps_group_controlled_availability(self):
        self.cli("sync", "--apply")
        self.cli("skill", "add", self.package("alpha"))
        for args in (("add", "g"), ("include", "g", "alpha"), ("assign", "g", "--agents", "codex"), ("enable", "g")):
            self.cli("skill", "group", *args)
        self.assertTrue(self.link("codex", "alpha").is_symlink())
        self.cli("import", "--from", "codex,claude,opencode", "--apply")
        self.assertEqual(self.skill("alpha")["agents"], {})
        self.cli("skill", "group", "disable", "g")
        self.assertFalse(self.link("codex", "alpha").exists())

    def test_import_records_override_only_when_native_state_contradicts(self):
        self.cli("sync", "--apply", "--agent", "claude")
        self.cli("skill", "add", self.package("alpha"))
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        self.cli("import", "--from", "opencode", "--apply")
        self.assertEqual(self.skill("alpha")["agents"], {"claude": "on", "opencode": "on"})

    def test_native_opencode_permission_edits_do_not_block_sync(self):
        self.write(OPENCODE, json.dumps({"permissions": [{"action": "bash", "resource": "*", "effect": "ask"}]}))
        self.cli("sync", "--apply")
        self.cli("skill", "add", self.package("alpha"))
        self.cli("skill", "add", self.package("beta"))
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        self.assertFalse(opencode_allows(self.rules(), "alpha"))
        document = json.loads((self.home / OPENCODE).read_text())
        user_rule = {"action": "webfetch", "resource": "*", "effect": "allow"}
        document["permissions"].insert(0, user_rule)
        (self.home / OPENCODE).write_text(json.dumps(document, indent=2))
        self.cli("skill", "enable", "beta", "--agent", "claude")
        self.assertTrue(self.link("claude", "beta").is_symlink())
        self.cli("skill", "enable", "alpha", "--agent", "opencode")
        rules = self.rules()
        self.assertIn(user_rule, rules)
        self.assertIn({"action": "bash", "resource": "*", "effect": "ask"}, rules)
        self.assertTrue(opencode_allows(rules, "alpha"))
        self.assertFalse(opencode_allows(rules, "beta"))
        self.cli("sync", "--check")
        self.cli("restore", "--apply")
        self.assertEqual(self.rules(), [user_rule, {"action": "bash", "resource": "*", "effect": "ask"}])

    def test_skill_check_failure_names_client_path_and_next_command(self):
        source = self.package("alpha")
        (self.home / ".codex/skills").mkdir(parents=True)
        (self.home / ".codex/skills/alpha").symlink_to(source)
        config = self.write(".codex/config.toml", '[[skills.config]]\nname = "alpha"\nenabled = false\n')
        self.cli("sync", "--apply")
        self.cli("import", "--from", "codex", "--apply")
        result = self.cli("skill", "enable", "alpha", "--agent", "codex", ok=False)
        self.assertIn("codex", result.stderr)
        self.assertIn(str(config), result.stderr)
        self.assertIn("sync --apply --agent codex", result.stderr)

    def test_deleted_projection_is_recreated_and_disabled_without_conflict(self):
        self.cli("sync", "--apply")
        for name in ("alpha", "beta"):
            self.cli("skill", "add", self.package(name))
            self.cli("skill", "enable", name, "--agent", "claude")
        self.link("claude", "alpha").unlink()
        self.link("claude", "beta").unlink()
        self.cli("skill", "disable", "beta", "--agent", "claude")
        self.assertTrue(self.link("claude", "alpha").is_symlink())
        self.assertFalse(self.link("claude", "beta").exists())
        self.link("claude", "alpha").unlink()
        self.cli("restore", "--apply")
        self.assertFalse(self.link("claude", "alpha").exists())

    def test_pending_projection_recovery_preserves_saved_entry_and_links(self):
        self.cli("sync", "--apply")
        self.cli("skill", "add", self.package("alpha"))
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        link = self.link("claude", "alpha")
        target = os.readlink(link)
        pending = link.parent / ".kitmux-alpha.pending"
        pending.mkdir(mode=0o700)
        saved = pending / "entry"
        saved.write_text("saved native content")

        for args in (("sync", "--check"), ("sync", "--apply")):
            result = self.cli(*args, ok=False)
            self.assertIn(pending.name, result.stderr)
            self.assertEqual(saved.read_text(), "saved native content")
            self.assertEqual(os.readlink(link), target)
        saved.unlink()
        pending.rmdir()
        self.cli("sync", "--check")

    def test_pending_journal_recovers_managed_exchange_without_writing_on_check(self):
        source = self.package("alpha")
        self.cli("sync", "--apply")
        self.cli("skill", "add", source)
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        link = self.link("claude", "alpha")
        current = os.readlink(link)
        journal = self.home / ".local/state/kitmux/adapters.json"
        pending = link.parent / ".kitmux-alpha.pending"

        for state in ("empty", "before-exchange", "after-exchange"):
            with self.subTest(state=state):
                data = json.loads(journal.read_text())
                data["pending"] = True
                data["links"][str(link)].update(previous=str(source), after=current)
                journal.write_text(json.dumps(data))
                pending.mkdir(mode=0o700)
                entry = pending / "entry"
                if state == "before-exchange":
                    link.unlink()
                    link.symlink_to(source)
                    entry.symlink_to(current)
                elif state == "after-exchange":
                    entry.symlink_to(source)

                self.cli("sync", "--check", ok=False)
                self.assertTrue(pending.is_dir())
                self.assertEqual(entry.is_symlink(), state != "empty")
                self.cli("sync", "--apply")
                self.assertFalse(pending.exists())
                self.assertEqual(os.readlink(link), current)
                self.assertFalse(json.loads(journal.read_text())["pending"])
                self.cli("sync", "--check")

    def test_changed_projection_conflict_names_path_and_refresh_resolves_it(self):
        self.cli("sync", "--apply")
        self.cli("skill", "add", self.package("alpha"))
        self.cli("skill", "add", self.package("beta"))
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        other = self.package("alpha", "Different native package", parent="other")
        self.link("claude", "alpha").unlink()
        self.link("claude", "alpha").symlink_to(other)
        result = self.cli("skill", "enable", "beta", "--agent", "codex", ok=False)
        self.assertIn(str(self.link("claude", "alpha")), result.stderr)
        self.assertIn("import --refresh --from claude", result.stderr)
        self.cli("import", "--refresh", "--from", "claude", "--apply")
        self.cli("sync", "--apply")
        self.assertEqual(os.readlink(self.link("claude", "alpha")), str(other))
        self.assertEqual(self.skill("alpha")["agents"]["claude"], "off")
        self.cli("sync", "--check")

    def test_kitmux_enable_overrides_native_opencode_deny_until_restore(self):
        source = self.package("alpha")
        (self.home / ROOTS["opencode"]).mkdir(parents=True)
        self.link("opencode", "alpha").symlink_to(source)
        original = '{"permissions":[{"action":"skill","resource":"alpha","effect":"deny"}]}\n'
        self.write(OPENCODE, original)
        self.cli("import", "--from", "opencode", "--apply")
        self.assertEqual(self.skill("alpha")["agents"], {"opencode": "off"})
        self.cli("skill", "enable", "alpha", "--agent", "opencode")
        self.assertTrue(opencode_allows(self.rules(), "alpha"))
        self.assertTrue(self.link("opencode", "alpha").is_symlink())
        self.cli("restore", "--apply")
        self.assertEqual(json.loads((self.home / OPENCODE).read_text()), json.loads(original))

    def test_git_ignores_inherited_repository_environment(self):
        bare, commit = self.repository({"alpha/SKILL.md": "---\nname: alpha\ndescription: Git\n---\nBody\n"})
        decoy = self.home / "decoy.git"
        self.git("init", "-q", "--bare", decoy)
        before = sorted(path.name for path in decoy.iterdir())
        environment = dict(os.environ, GIT_DIR=str(decoy), GIT_INDEX_FILE=str(self.home / "index"))
        self.cli("skill", "add", bare, "--path", "alpha", "--ref", commit, env=environment)
        self.assertEqual(sorted(path.name for path in decoy.iterdir()), before)
        self.assertEqual(self.skill("alpha")["revision"], commit)

    def test_codex_path_override_is_enforced_after_alias_removal(self):
        source = self.package("alpha")
        (self.home / ".codex/skills").mkdir(parents=True)
        (self.home / ".codex/skills/alpha").symlink_to(source)
        self.write(".codex/config.toml", f'[[skills.config]]\npath = "{source}/SKILL.md"\nenabled = false\n')
        self.cli("import", "--from", "codex", "--apply")
        self.cli("skill", "inherit", "alpha", "--agent", "codex")
        self.assertFalse((self.home / ".codex/skills/alpha").exists())
        result = self.cli("skill", "enable", "alpha", "--agent", "codex", ok=False)
        self.assertIn("skills.config", result.stderr)
        self.assertFalse((self.home / ".codex/skills/alpha").exists())

    def test_installed_packages_are_read_only_and_revisions_remain_usable(self):
        source = self.package("alpha", "v1")
        self.cli("sync", "--apply")
        self.cli("skill", "add", source)
        self.cli("skill", "enable", "alpha", "--agent", "claude")
        installed = Path(self.skill("alpha")["path"])
        self.assertFalse(os.access(installed / "SKILL.md", os.W_OK))
        self.assertFalse(os.access(installed, os.W_OK))
        with self.assertRaises(PermissionError):
            (self.link("claude", "alpha") / "SKILL.md").write_text("agent edit\n")
        with self.assertRaises(PermissionError):
            (self.link("claude", "alpha") / "notes.md").write_text("agent note\n")
        self.package("alpha", "v2")
        self.cli("skill", "update", "alpha", "--apply")
        self.cli("skill", "rollback", "alpha")
        self.assertEqual(Path(self.skill("alpha")["path"]), installed)
        self.cli("skill", "remove", "alpha")
        self.cli("skill", "add", source)

    def test_cached_package_recovers_root_mode_and_preserves_conflicting_entries(self):
        source = self.package("alpha")
        self.cli("skill", "add", source)
        installed = Path(self.skill("alpha")["path"])
        self.cli("skill", "remove", "alpha")
        installed.chmod(0o700)
        self.cli("skill", "add", source)
        self.assertEqual(installed.stat().st_mode & 0o777, 0o500)

        self.cli("skill", "remove", "alpha")
        installed.chmod(0o700)
        saved = installed.with_name("saved")
        installed.rename(saved)
        installed.symlink_to(saved)
        self.cli("skill", "add", source, ok=False)
        self.assertTrue(installed.is_symlink())
        self.assertEqual(saved.stat().st_mode & 0o777, 0o700)
        installed.unlink()
        installed.mkdir()
        self.cli("skill", "add", source, ok=False)
        self.assertEqual(list(installed.iterdir()), [])

    def test_unprojected_skills_do_not_hide_same_named_native_skills(self):
        self.cli("sync", "--apply")
        self.cli("skill", "add", self.package("alpha"))
        self.cli("skill", "add", self.package("beta"))
        self.cli("skill", "enable", "beta", "--agent", "claude")
        settings = self.home / ".claude/settings.json"
        overrides = json.loads(settings.read_text()).get("skillOverrides", {}) if settings.exists() else {}
        self.assertNotIn("alpha", overrides)
        self.assertTrue(opencode_allows(self.rules(), "alpha"))
        self.assertFalse(opencode_allows(self.rules(), "beta"))

    def test_deep_git_tree_is_rejected_before_deep_extraction(self):
        deep = "/".join(["d"] * 300)
        bare, commit = self.repository({"alpha/SKILL.md": "---\nname: alpha\ndescription: Git\n---\nBody\n",
                                        f"alpha/{deep}/file.txt": "deep\n"})
        hard = resource.getrlimit(resource.RLIMIT_STACK)[1]

        def stack():
            resource.setrlimit(resource.RLIMIT_STACK, (192 * 1024, hard))

        result = self.cli("skill", "add", bare, "--path", "alpha", "--ref", commit, ok=False, limits=stack)
        self.assertGreater(result.returncode, 0)
        self.assertIn("nest", result.stderr)
        leftovers = [path.name for path in (self.home / ".local/share/kitmux/skills").iterdir()]
        self.assertEqual(leftovers, [])

    def test_import_skips_non_package_entries_and_real_directories_with_named_warnings(self):
        self.package("alpha")
        root = self.home / ROOTS["claude"]
        root.mkdir(parents=True)
        (root / "alpha").symlink_to(self.home / "src/alpha")
        (root / "gone").symlink_to(self.home / "src/deleted")
        (root / "README.md").write_text("notes\n")
        real = self.package("local-only", parent=ROOTS["claude"])
        result = self.cli("import", "--from", "claude", "--apply")
        for path in (root / "gone", root / "README.md", real):
            self.assertIn(str(path), result.stderr)
        self.assertIn(f"skill add {real}", result.stderr)
        self.assertEqual(set(self.catalog()["skills"]), {"alpha"})
        self.cli("skill", "add", self.package("local-only", parent="elsewhere"))
        self.cli("sync", "--apply")
        self.assertTrue(real.is_dir() and not real.is_symlink())
        result = self.cli("skill", "enable", "local-only", "--agent", "claude", ok=False)
        self.assertIn(str(real), result.stderr)


if __name__ == "__main__":
    unittest.main()
