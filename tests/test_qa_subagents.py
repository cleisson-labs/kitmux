"""Regression coverage for custom subagent QA findings."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

BINARY = os.environ.get('KITMUX_BINARY', str(Path(__file__).parents[1] / 'build/kitmux'))
ESC = '\x1b'


class QaSubagentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()

    def cli(self, *args, ok=True, home=None):
        result = subprocess.run([BINARY, '--home', str(home or self.home), *args],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def write(self, relative, text, home=None):
        path = (home or self.home) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def adopt(self):
        """Make every client configured, as a daily user's home already is."""
        self.cli('sync', '--apply')

    def portable(self, ident, instructions='Review carefully.\n', description='Review code'):
        source = self.write(f'{ident}-source.md', instructions)
        self.cli('subagent', 'add', ident, '--from', str(source), '--description', description)

    def catalog(self):
        return json.loads((self.home / '.config/kitmux/catalog.json').read_text())

    def reimport(self, relative, client):
        other = Path(tempfile.mkdtemp(dir=self.home, prefix='check-'))
        self.write(relative, (self.home / relative).read_text(), home=other)
        self.cli('subagent', 'import', '--from', client, '--apply', home=other)
        return json.loads(self.cli('subagent', 'show', Path(relative).stem, '--json', home=other).stdout)

    # Finding 1: rendered control characters must never poison the journal.
    def test_ansi_escape_in_codex_instructions_keeps_journal_valid(self):
        self.adopt()
        instructions = f'Colors: {ESC}[1mbold{ESC}[0m and vt\x0b\n'
        self.portable('esc', instructions)
        self.cli('subagent', 'enable', 'esc', '--agent', 'codex')
        path = self.home / '.codex/agents/esc.toml'
        self.assertEqual(tomllib.loads(path.read_text())['developer_instructions'], instructions)
        self.assertEqual(self.reimport('.codex/agents/esc.toml', 'codex')['instructions'], instructions)
        self.cli('sync', '--preview')
        self.cli('subagent', 'disable', 'esc', '--agent', 'codex')
        self.assertFalse(path.exists())

    def test_delete_character_in_codex_instructions_is_escaped(self):
        self.adopt()
        instructions = 'Delete \x7f marker\n'
        self.portable('del', instructions)
        self.cli('subagent', 'enable', 'del', '--agent', 'codex')
        text = (self.home / '.codex/agents/del.toml').read_text()
        self.assertNotIn('\x7f', text)
        self.assertEqual(tomllib.loads(text)['developer_instructions'], instructions)
        self.cli('sync', '--preview')

    def test_c1_control_description_round_trips_through_frontmatter(self):
        self.adopt()
        description = 'C1 \u009b control and line separator'
        self.portable('cone', description=description)
        for client, relative in (('claude', '.claude/agents/cone.md'),
                                 ('opencode', '.config/opencode/agents/cone.md')):
            with self.subTest(client=client):
                self.cli('subagent', 'enable', 'cone', '--agent', client)
                self.assertEqual(self.reimport(relative, client)['description'], description)
        self.cli('sync', '--preview')

    def test_journal_written_by_an_earlier_build_recovers_on_sync(self):
        self.adopt()
        self.portable('legacy', 'Delete \x7f marker\n')
        self.cli('subagent', 'enable', 'legacy', '--agent', 'codex')
        path = self.home / '.codex/agents/legacy.toml'
        legacy = 'name = "legacy"\ndescription = "Review code"\ndeveloper_instructions = "Delete \x7f marker\\n"\n'
        path.write_text(legacy)
        journal = self.home / '.local/state/kitmux/adapters.json'
        state = json.loads(journal.read_text())
        state['subagent_files'][str(path)].update(after=legacy, previous=legacy)
        journal.write_text(json.dumps(state))
        self.cli('sync', '--apply')
        self.assertEqual(tomllib.loads(path.read_text())['developer_instructions'], 'Delete \x7f marker\n')

    # Finding 2: layout checks apply only where Kitmux projects files.
    def test_unassigned_subagent_ignores_unrelated_layouts(self):
        (self.home / '.claude/agents/archive').mkdir(parents=True)
        dotfiles = self.home / 'dotfiles/opencode'
        dotfiles.mkdir(parents=True)
        (self.home / '.config').mkdir()
        (self.home / '.config/opencode').symlink_to(dotfiles)
        self.adopt()
        self.portable('rev')
        self.cli('subagent', 'enable', 'rev', '--agent', 'codex')
        self.assertTrue((self.home / '.codex/agents/rev.toml').exists())

    def test_dot_entries_do_not_block_an_assigned_client(self):
        self.adopt()
        (self.home / '.claude/agents/.git').mkdir(parents=True)
        self.portable('rev')
        self.cli('subagent', 'enable', 'rev', '--agent', 'claude')
        self.assertTrue((self.home / '.claude/agents/rev.md').exists())

    def test_nested_directory_error_names_its_path(self):
        self.adopt()
        nested = self.home / '.claude/agents/archive'
        nested.mkdir(parents=True)
        self.portable('rev')
        result = self.cli('subagent', 'enable', 'rev', '--agent', 'claude', ok=False)
        self.assertIn(str(nested), result.stderr)

    # Finding 3 and R2: ownership records follow the resource lifecycle.
    def test_removed_subagent_releases_its_native_path(self):
        self.adopt()
        self.portable('foo')
        self.cli('subagent', 'enable', 'foo', '--agent', 'claude')
        self.cli('subagent', 'remove', 'foo')
        mine = self.write('.claude/agents/foo.md', '---\nname: foo\ndescription: Mine\n---\nMine.\n')
        self.portable('bar')
        self.cli('subagent', 'enable', 'bar', '--agent', 'claude')
        self.assertTrue((self.home / '.claude/agents/bar.md').exists())
        self.cli('restore', '--agent', 'claude', '--apply')
        self.assertEqual(mine.read_text(), '---\nname: foo\ndescription: Mine\n---\nMine.\n')

    def test_refresh_adoption_records_a_baseline_that_restore_keeps(self):
        self.adopt()
        self.portable('foo')
        self.cli('subagent', 'enable', 'foo', '--agent', 'claude')
        path = self.home / '.claude/agents/foo.md'
        edited = '---\nname: foo\ndescription: Owner edit\n---\nOwner instructions.\n'
        path.write_text(edited)
        result = self.cli('subagent', 'disable', 'foo', '--agent', 'claude', ok=False)
        self.assertIn(str(path), result.stderr)
        self.assertIn('--refresh', result.stderr)
        self.cli('subagent', 'import', '--from', 'claude', '--apply', '--refresh')
        self.cli('restore', '--agent', 'claude', '--apply')
        self.assertEqual(path.read_text(), edited)

    def test_owned_file_deleted_outside_kitmux_can_be_disabled(self):
        self.adopt()
        self.portable('foo')
        self.cli('subagent', 'enable', 'foo', '--agent', 'claude')
        (self.home / '.claude/agents/foo.md').unlink()
        self.cli('subagent', 'disable', 'foo', '--agent', 'claude')

    # Finding 5: one unusable file does not abort a client's import.
    def test_import_skips_unusable_files_and_names_them(self):
        agents = self.home / '.claude/agents'
        self.write('.claude/agents/rev.md', '---\nname: rev\ndescription: Review\n---\nReview.\n')
        skipped = {
            'README.md': '# My agents\n',
            'bom.md': '﻿---\nname: bom\ndescription: Bom\n---\nBody\n',
            'empty.md': '---\nname: empty\ndescription: Empty\n---\n',
            'gen.md': '---\nname: gen\ndescription: Use when. Examples: Context: x\n---\nBody\n',
            'my.agent.md': '---\nname: my.agent\ndescription: Dots\n---\nBody\n',
            ('x' * 70) + '.md': '---\nname: long\ndescription: Long\n---\nBody\n',
            'other.md': '---\nname: renamed\ndescription: Mismatch\n---\nBody\n',
        }
        for name, text in skipped.items():
            self.write(f'.claude/agents/{name}', text)
        result = self.cli('subagent', 'import', '--from', 'claude', '--apply')
        self.assertEqual(set(self.catalog()['subagents']), {'rev'})
        for name in skipped:
            self.assertIn(str(agents / name), result.stdout + result.stderr)

    def test_invalid_opencode_json_entry_is_skipped_by_name(self):
        config = self.write('.config/opencode/opencode.json', json.dumps({'agents': {
            'good': {'description': 'Good', 'mode': 'subagent', 'prompt': 'Good.'},
            'bad.name': {'description': 'Bad', 'mode': 'subagent', 'prompt': 'Bad.'},
            'empty': {'description': 'Empty', 'mode': 'subagent', 'prompt': ''}}}))
        result = self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        self.assertEqual(set(self.catalog()['subagents']), {'good'})
        self.assertIn(str(config), result.stdout + result.stderr)
        self.assertIn('bad.name', result.stdout + result.stderr)

    # Finding 6: an edit that cannot render is rejected before saving.
    def test_edit_that_cannot_render_is_rejected_before_save(self):
        self.write('.claude/agents/rev.md',
                   '---\nname: rev\ndescription: &d Anchored\nother: *d\n---\nBody\n')
        self.cli('subagent', 'import', '--from', 'claude', '--apply')
        before = self.catalog()
        result = self.cli('subagent', 'edit', 'rev', '--description', 'New', ok=False)
        self.assertIn('claude', result.stderr)
        self.assertEqual(self.catalog(), before)
        self.cli('sync', '--apply')

    # Finding 7: assignments without a native variant are reported.
    def test_incompatible_assignment_warns_at_enable_and_sync(self):
        self.write('.codex/agents/rev.toml',
                   'name = "rev"\ndescription = "Codex"\ndeveloper_instructions = "Review."\n')
        self.cli('subagent', 'import', '--from', 'codex', '--apply')
        enabled = self.cli('subagent', 'enable', 'rev', '--agent', 'claude')
        self.assertIn('--format claude', enabled.stdout + enabled.stderr)
        synced = self.cli('sync', '--apply')
        self.assertIn('claude', synced.stdout)
        self.assertIn('--format claude', synced.stdout)

    # Finding 8: oversized definitions fail before they break the journal.
    def test_definition_that_cannot_fit_the_journal_is_rejected_at_add(self):
        self.adopt()
        source = self.write('big.md', 'Review line with words.\n' * 130000)
        result = self.cli('subagent', 'add', 'big', '--from', str(source), '--description', 'Big', ok=False)
        self.assertIn('8 MiB', result.stderr)
        self.assertEqual(json.loads(self.cli('subagent', 'list', '--json').stdout), {})

    # Finding 9 and R3: native disabled state and availability are preserved.
    def test_natively_disabled_opencode_markdown_is_preserved(self):
        self.adopt()
        original = '---\ndescription: OC\nmode: subagent\ndisabled: true\n---\nBody\n'
        path = self.write('.config/opencode/agents/rev.md', original)
        self.cli('subagent', 'import', '--from', 'opencode', '--apply')
        self.portable('other')
        self.cli('subagent', 'enable', 'other', '--agent', 'codex')
        self.assertEqual(path.read_text(), original)
        self.cli('subagent', 'enable', 'rev', '--agent', 'opencode')
        self.assertIn('disabled: false', path.read_text())
        self.cli('subagent', 'disable', 'rev', '--agent', 'opencode')
        self.assertIn('disabled: true', path.read_text())
        self.cli('restore', '--agent', 'opencode', '--apply')
        self.assertEqual(path.read_text(), original)

    def test_refresh_does_not_turn_group_availability_into_an_override(self):
        self.adopt()
        self.portable('rev')
        self.cli('subagent', 'group', 'add', 'eng')
        self.cli('subagent', 'group', 'include', 'eng', 'rev')
        self.cli('subagent', 'group', 'assign', 'eng', '--agents', 'claude')
        self.assertTrue((self.home / '.claude/agents/rev.md').exists())
        self.cli('subagent', 'import', '--from', 'claude', '--apply', '--refresh')
        self.assertNotIn('claude', self.catalog()['subagents']['rev']['agents'])

    def test_ids_differing_only_by_case_are_rejected(self):
        self.portable('Rev')
        source = self.write('lower.md', 'Lower.\n')
        result = self.cli('subagent', 'add', 'rev', '--from', str(source), '--description', 'Lower', ok=False)
        self.assertIn('case', result.stderr.lower())
        result = self.cli('subagent', 'duplicate', 'Rev', 'REV', ok=False)
        self.assertIn('case', result.stderr.lower())


if __name__ == '__main__':
    unittest.main()
