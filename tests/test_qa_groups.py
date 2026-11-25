"""Regression tests for group-level QA findings."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("KITMUX_BINARY", str(Path(__file__).parents[1] / "build/kitmux"))


class GroupAssignmentWarningTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)

    def cli(self, *args):
        result = subprocess.run([BINARY, "--home", str(self.home), *args], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_group_assignment_warns_about_clients_without_a_native_variant(self):
        source = self.home / "reviewer.md"
        source.write_text("---\nname: reviewer\ndescription: Review code\n---\nReview code.\n")
        self.cli("subagent", "add", "reviewer", "--from", str(source), "--format", "claude")
        self.cli("subagent", "group", "add", "team")
        self.cli("subagent", "group", "include", "team", "reviewer")
        result = self.cli("subagent", "group", "assign", "team", "--agents", "codex,claude")
        self.assertIn("subagent reviewer has no codex variant", result.stderr)
        self.assertNotIn("no claude variant", result.stderr)


if __name__ == "__main__":
    unittest.main()
