# Architecture and verification

Kitmux 0.7.0 is a C17 terminal application for MCP servers, Agent Skills, custom
subagents, groups, and assignments to Codex, Claude Code, OpenCode, and the
Antigravity CLI. See the [README](../README.md) for setup, the [guide](GUIDE.md) for
usage, and [v0.7 notes](V0.7.md) for the current release. Earlier behavior is
documented in [v0.6 notes](V0.6.md), [v0.5 notes](V0.5.md), [v0.4 notes](V0.4.md), [v0.3 notes](V0.3.md),
[v0.2 notes](V0.2.md), and the
[import compatibility record](IMPORT_FIX.md).

V0.3.1 adds native MCP JSON source provisioning, policy-preserving skill source
rebinding, explicit import refresh, read-only sync checks, and idempotent definition
edits. Source
provisioning retains each client's native options and preserves credentials only
at matching endpoints. The Codex adapter patches individual values and nested
tables while retaining comments, then validates the resulting definition.
Refresh changes snapshots without rewriting native bytes or losing the original
restore baseline. Drifted managed bridges remain rejected to protect upstreams.

V0.4 moves groups into their resource tabs and scopes group names, membership,
and actions to MCPs, Skills, or Subagents. Mixed legacy groups migrate explicitly
to matching scoped groups with private backups. Repeated rows share resource
identity, so overlapping memberships never duplicate a bulk resource action.

V0.5 adds bounded resource documents independent of command capture: installed
`SKILL.md`, shared/effective native subagent instructions, and optional MCP Markdown
descriptions. The viewer sanitizes terminal controls, wraps Unicode text, and
reflows on resize. Its file inputs retain the 8 MiB limit; combined subagent
documents allow four such bodies plus 64 KiB of metadata. Basic heading/code
formatting is supported, not full CommonMark rendering. Viewing is read-only and
never launches an MCP or fetches remote content.

MCP descriptions are optional schema-3-compatible metadata. Text/file CLI options
and TUI edits validate input without changing execution variants; re-import,
refresh, and native updates preserve authored text or an explicit empty value.
Existing show/JSON contracts remain unchanged. A native description is adopted
only when local documentation is absent.

## Organization migration

Repository links, badges, the staged installer download URL, and provenance
verification examples now use `cleisson-labs/kitmux`. Published v0.5.0 and v0.6.0
assets predate the attestation-enabled workflow and provide `SHA256SUMS`.

CI uses immutable organization workflows for security, documentation, C static
analysis, and CodeQL. Kitmux keeps its compatibility and sanitizer matrix, records
gcovr coverage on Linux x86_64, and validates the expected job outcomes before
reporting `ci-ok`. Release builds retain the four packaged-binary smoke tests and
reuse the organization asset verifier, attestation, and publisher.

## Implementation boundaries

- `catalog.c`, `policy.c`, and `groups*.c` own catalog validation, persistence,
  availability rules, scoped groups, and legacy migration. Global off and explicit client off win;
  enabled groups combine, and unassigned resources remain off.
- `adapters*.c` preserve native configuration and own only adopted entries,
  projections, and definitions. Their journal records originals, current values,
  and pending writes for conflict detection and recovery. Sync and restore
  checkpoint each client and roll back only a failed client's plan, so other
  clients still apply. `adapters_mcp_import.c` owns MCP import/refresh and
  `adapters_skill_rules.c` owns the OpenCode permission rules Kitmux adds.
- `skills*.c` install complete immutable packages from local directories or pinned
  Git objects. Updates preserve policy, and rollback retains previous packages.
- `subagents*.c` parse and render common instructions and native variants without
  translating away native model, tool, or permission settings.
  `subagents_format_quote.c` emits escapes that every parser accepts, and each
  render is parsed back before it can reach the ownership journal.
- `runtime*.c` implement the optional local stdio MCP bridge and authenticated
  process controls. Direct integrations remain owned by each native client.
- `tui*.c` provide grouped resource trees in the ncurses interface; `main.c`
  routes equivalent CLI commands. Row identity distinguishes headers from
  resources, while resource identity deduplicates actions across memberships.
  `tui_theme.c` owns the red/grey palette, glyphs, and client labels; `tui_list.c`
  derives list columns from `km_agents`, so a new client needs no drawing changes.
  `tui_sync.c` reloads the catalog after external saves, checks pending native
  sync for the banner, and builds the Clients tab.
- `tui_details.c` selects allowlisted resource content; `tui_document.c` owns the
  read-only Unicode/Markdown pager. Apply previews retain their explicit review
  behavior and bounded command-output buffer.
- `scripts/*release*.sh` build pinned dependency sources in a private prefix,
  verify runtime linkage/architecture/platform minima, and package executables
  with licenses. The release workflow gates publication on every target and
  stages complete assets in a draft before publishing. Manual runs produce the
  same archives/checksum manifest as workflow artifacts.

The build uses Make, json-c, libyaml, ncurses, and OpenSSL libcrypto. The vendored
TOML parser is documented in [third-party notices](../THIRD_PARTY.md). Python is
used for tests only.

## Persistence and recovery

Catalog mutations automatically synchronize clients previously adopted with
sync or a full import; `subagent import` adopts only that client's subagents. Atomic file replacement and a pending ownership journal make
partial application recoverable; changes across multiple files are not a single
atomic transaction. Conflicting external edits are reported rather than replaced.
Restore recovers the adopted originals and releases the selected clients from
automatic management.

Scoped groups use catalog schema v3. Explicit migration splits legacy groups
without changing resource availability and backs up the original catalog before
replacement. Ownership journals keep their format with two optional keys:
`subagent_agents` (subagent-only adoption) and `skill_rules` (OpenCode permission
rules Kitmux added). Whole-array permission records written by v0.5.0 migrate on
the first sync, and restore accepts both forms. Older Kitmux
builds reject schema v3; keep v0.4 for recovery. Native project and organization
settings keep their own precedence.

## V0.7 Antigravity, interface, and CI

- Antigravity is the fourth client (`antigravity`). Its MCP file is
  `~/.gemini/config/mcp_config.json`; `adapters_skill_excludes.c` owns the names
  Kitmux adds to `~/.gemini/config/skills.json` `exclude` for skills visible through
  the shared `~/.agents/skills`; subagents use an `antigravity` Markdown format in
  `~/.gemini/config/agents`. Behavior was verified against `agy` 1.5.17 under
  `sandbox-exec` with offline commands only, including the blank file `agy` creates
  on first start and its own sorted rewrites of `mcp_config.json`.
- The TUI uses only red, grey, and the terminal's own foreground/background;
  `tests/test_tui_theme.py` asserts the emitted colors, the `NO_COLOR` fallback, and
  ASCII cells outside UTF-8 locales. Documents open with a client summary line.
- CI gained a `ci-ok` gate, actionlint/zizmor, cppcheck (`make analyze`), CodeQL,
  a PR title check, Dependabot, SHA-pinned actions, and release provenance
  attestations. Detailed usage moved from the README into [GUIDE.md](GUIDE.md).
- Compatibility: no catalog schema bump. v0.6 refuses a catalog with `antigravity`
  keys without writing anything; rollback steps are in the v0.7 notes.

## V0.6 QA review fixes

A full review and terminal QA on 25 November 2026 found defects behind three
reported symptoms: checkbox toggles that did not stick, MCPs that stayed on or off
natively after a Kitmux change, and resource details that failed to open. Every
fix below began with a failing regression test in `tests/test_qa_*.py`.

- Native sync: one client's conflict or write failure no longer blocks the others.
  Dropped client defaults and deletions that match the desired state are not
  conflicts. Errors name the client, file, entry, and recovery command, and
  `import --refresh` adopts deletions for one client. MCPs are written only into
  clients where they are ON or already present, so credentials no longer spread to
  unassigned clients. Imports leave adopted clients in sync. Re-import and refresh
  keep group and inherited availability. `subagent import` no longer adopts MCP or
  skill management. Codex comments next to edited tables survive, and JSON inserts
  follow the file's indentation.
- Skills: OpenCode permission ownership is limited to Kitmux rules. Deleted links
  are recreated, or left absent when the skill is off. Native denies yield to a
  Kitmux enable until restore. Codex path selectors are enforced by every location.
  Packages are read-only. Git ignores inherited repository variables, and extraction
  depth is bounded. Import skips unusable entries with named warnings.
- Subagents: renders escape every control character and are parsed back before
  they are recorded, so ANSI or C1 text can no longer corrupt the ownership journal.
  Journals corrupted by v0.5.0 recover on the next sync. Layout checks apply only to
  clients with Kitmux-managed subagents. Stale records are released. Unusable native
  files are skipped by name. Edits are validated before saving. Imported OpenCode
  Markdown is disabled in place. Case-only ID collisions are rejected.
- Managed bridge: upstreams with extra capabilities are accepted (tools only are
  bridged). Startup is asynchronous with a 30-second budget, retry, and backoff, and
  status gains `starting`. Crash relist loops are gone. Stale sockets of exited
  bridges are pruned. Messages are forwarded byte-for-byte with 512-level nesting.
  Upstreams receive EOF before termination. In-place catalog edits are observed.
  Reconnect reports each failure reason.
- TUI: the assign dialog changes only the clients the user edits and keeps
  assignments of globally OFF items. Bulk actions continue when the catalog saved
  but native sync failed, and they include marked rows hidden by collapse or search.
  The list reloads after external catalog changes; this also fixes a SIGSEGV after an
  external group removal. A banner and the Clients tab report pending native sync.
  Esc responds immediately, cancelled searches keep the previous filter, long IDs
  widen with the terminal, and doctor output opens in a viewer.

Local qualification on macOS arm64 at the final revision: `make -j8 test` ran 367
cases (361 passed, 6 opt-in native cases skipped), and `make -j8 sanitize` ran the
same 367 cases with ASan/UBSan and no sanitizer reports. Hosted CI at revision
`21f7a5f` passed the ordinary and ASan/UBSan suites on macOS 15 arm64/x86_64 and
Ubuntu 22.04 arm64/x86_64, including the ARM64 leak checks. An earlier revision failed
only on macOS 15: it refuses to rename a directory without write permission, so
read-only skill packages are now protected after they are published.

Remaining limits: Codex only logs `tools/list_changed`, so a running Codex session
shows managed tool changes after a restart. Bridge retries happen on the next
client request or reconnect, with no background timer. Clients that were made
fully managed by an earlier `subagent import` stay managed until
`restore --agent CLIENT` and re-adoption. Native client behavior was verified from
installed client binaries and documentation, not by `make test-native` in this
pass.

## V0.5 verification and release gates

Local qualification on 12 November 2026:

| Platform / command | Result |
|---|---|
| macOS arm64: `make -j4 test` | 274 passed; 6 opt-in native cases skipped; 280 discovered |
| macOS arm64: `make -j4 sanitize` | 274 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| Ubuntu 22.04 arm64: `make -j4 BUILD=build-linux PYTHON=python3.11 test` | 274 passed; 6 opt-in cases skipped; 280 discovered |
| Ubuntu 22.04 arm64: `ASAN_OPTIONS=detect_leaks=0 make -j4 BUILD=build-linux-sanitize SANITIZE_BUILD=build-linux-sanitize PYTHON=python3.11 sanitize` | 274 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| Ubuntu 22.04 arm64: `sh scripts/check-leaks.sh /work/build-linux-sanitize/kitmux` | 5 representative workflows passed with `detect_leaks=1` |
| arm64 release builds on macOS and Ubuntu 22.04: `make release VERSION=v0.5.0` | Both versioned archives built; runtime linkage, architecture, and minimum-platform verification passed |
| Packaged executables: `sh scripts/smoke-release.sh BINARY` | Import, skill hashing, managed MCP reconnect, terminal restoration, xterm/tmux startup passed on both systems |
| Packaged macOS executable: `KITMUX_BINARY=build-release-darwin-arm64/kitmux PYTHONPATH=tests python3 -m unittest test_install.ReleaseInstallTests.test_documented_install_import_check_and_restore_with_real_binary -v` | Passed using staged downloads and an isolated home |

Linux qualification ran in Ubuntu 22.04 containers under a process reaper. On
Linux ARM64, LeakSanitizer exit scans added about 1.1 seconds per CLI locally and
about 3 seconds on the hosted runner. The initial hosted full-suite run reached
its 30-minute limit and reported failures in shutdown/status timing tests. An
inactive status snapshot could finish its leak scan after a test's entire polling
window elapsed. Switching compilers offered only a modest improvement.

ARM64 Linux CI therefore runs the complete suite under ASan/UBSan with
`ASAN_OPTIONS=detect_leaks=0`, then runs focused non-timed import, skill, subagent,
metadata, and document workflows with `detect_leaks=1`. All original timing
assertions remain in the full ordinary and ASan/UBSan suites. The three affected
runtime cases passed unchanged under that profile. Linux x86_64 retains leak
detection across the complete sanitizer suite. Full-suite ARM64 leak coverage is
a remaining toolchain limitation, not a claim made by this release.

The Linux archive passed smoke tests in a fresh Ubuntu 22.04/glibc 2.35 runtime
containing terminal data and Python fixtures but no development libraries. On
macOS, the packaged executable also passed version/import/skill-hash/status checks
with Homebrew reads denied. This machine's pyenv Python depends on Homebrew
gettext, preventing the entire Python smoke harness from starting inside that
sandbox; its ordinary fixture smoke passed separately. Hosted release tests use
the setup-python interpreter and deny Homebrew reads for the full smoke harness.

Red tests preceded document display, MCP metadata validation/import/editing, and
release packaging behavior. Independent review added reproduced regressions for
metadata-only TUI edits of distinct imported client variants, failed binary
inspectors being swallowed by shell pipelines, filename-based architecture false
positives, and backup-directory/symlink handling in the installer. All were fixed
and focused re-review found no remaining blockers. Documentation/build-harness
changes used their applicable test-first exceptions.

The README installation block is executed directly by `tests/test_install.py`.
Staged fixtures cover all four platform mappings, checksum/download/version
failures, atomic upgrades and rollback, installation errors, and symlink/directory
preservation. A real executable completes installation, preview/apply import,
sync preview/apply/check, and restore with synthetic MCP/skill/subagent data.
Tests use temporary homes and do not mutate live client settings or install host
dependencies. Release/linkage tests reject missing notices, mismatched versions,
wrong architectures, newer platform requirements, and non-system dynamic libraries.

Hosted qualification on 12 November 2026 at revision `36098f7`:

| Native target | Ordinary tests | ASan/UBSan |
|---|---|---|
| macOS 15 arm64 | Passed | Passed |
| macOS 15 x86_64 | Passed | Passed |
| Ubuntu 22.04 arm64 | Passed | Passed; separate leak checks passed |
| Ubuntu 22.04 x86_64 | Passed | Passed, including full-suite leak detection |

Each full suite discovered 280 cases: 274 passed and 6 opt-in native cases were
skipped. The [four-target CI run](https://github.com/cleisson-labs/kitmux/actions/runs/43237072560)
completed successfully. The ARM64 Linux sanitizer profile and native-client
qualification limits above apply to these results.

The [release workflow](../.github/workflows/release.yml) additionally verifies
packaged executables, combines four archives with `SHA256SUMS`, and publishes
only when every required job passes.

All four native build jobs in the
[manual release qualification](https://github.com/cleisson-labs/kitmux/actions/runs/43237129299)
also passed ordinary/sanitizer suites, standalone archive validation, and packaged
runtime smoke tests. Both macOS architectures passed the complete smoke harness
with Homebrew reads denied. Both Linux architectures passed it in Ubuntu 22.04
without development libraries, including managed MCP lifecycle, installed skill
hashing, fixture imports, and terminal startup/restoration.

The first hosted Linux release build received HTTP 403 from the original ncurses
archive host. Release downloads now use the official GNU mirror; its archive was
verified against the same pinned SHA256 before changing the URL.

## V0.4 verification baseline

Release verification on macOS arm64, 11 November 2026:

| Command | Result |
|---|---|
| `make -j4 test` | 236 passed; 6 opt-in native cases skipped; 242 discovered |
| `make -j4 sanitize` | 236 passed; 6 opt-in cases skipped; ASan/UBSan enabled |
| `make test-native` | 6 passed; no skips |

The tests use temporary homes, stub MCP processes, and synthetic local providers.
They cover all assignment subsets, policy precedence, imports, native setting
preservation, package updates and rollback, profile editing and duplication,
conflicts, pending recovery, malformed inputs, runtime lifecycle boundaries, and
terminal restoration. No live accounts or billed models are required.

Behavioral work used failing contract tests before implementation and regressions
before fixes. Important regressions include shared native names with different
commands, invalid native metadata, Unicode YAML offsets, JSON/Markdown projection
changes, nested discovery collisions, ambiguous interrupted calls, blocked
shutdown output, and incomplete process scans. Build scaffolding, protocol test
harnesses, and documentation were the test-first exceptions.

The v0.3.1 provisioning work began with failing subprocess contracts for native
MCP source loading, skill source rebinding, explicit drift refresh, and idempotent
profile edits. Additional regressions cover same-content source changes, stale
subdirectories, unsupported Codex values before commit, endpoint credential
isolation (including changed stdio working directories), nested native comments,
policy inspection without native secrets, preservation of raw assignments during
refresh, and read-only checks of planned projections and interrupted journals.
The focused command is `PYTHONPATH=tests python3 -m unittest test_provisioning
test_import_refresh test_sync_check -v`; all 25 provisioning cases pass.

V0.4 adds 10 scoped-group and migration cases and 12 terminal interaction cases.
Red failures preceded the new command/navigation behavior and fixes for adding a
resource inside a collapsed group, read-only implicit lists, backup location,
and failed migration exit status. Coverage includes original-byte backups,
native byte/mtime preservation, rejected cross-type mutations, overlapping
membership, group removal, search, cancellation, and terminal restoration.
Independent review confirmed backend migration preservation and all 26 terminal
workflows; the complete normal and sanitizer suites include them. Installed-client
qualification was rerun for this release as recorded above.

See [native client qualification](NATIVE_CLIENT_QUALIFICATION.md) for exact client
versions and tested surfaces and [runtime compatibility](RUNTIME_COMPATIBILITY.md)
for the supported MCP lifecycle.

## Qualification limits

User scope is the adapter boundary. Project/plugin/organization definitions retain
native precedence, loaded skills retain existing context, and removing a subagent
definition does not stop running work. Managed MCPs support tools-only legacy
stdio; unsupported capabilities and remote HTTP/OAuth require direct mode.

Release targets are macOS 15+ and glibc 2.35+ Linux, each on arm64 and x86_64.
Source builds and fixture qualification do not certify arbitrary native client
versions or live services. The isolated Claude subagent native probe remains
macOS-only because it depends on `sandbox-exec`; its Linux equivalent is not
implemented. Cache pruning and full textual skill diffs remain follow-up work.
The historical v0.3
[architecture diagram](kitmux-concept.html) and [original design plan](PLAN.md)
provide further context; implemented contracts are defined by the current README,
code, and tests.
