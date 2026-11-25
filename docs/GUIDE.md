# Kitmux guide

Detailed usage and reference for Kitmux. The [README](../README.md) covers installation
and a quick start; [IMPLEMENTATION.md](IMPLEMENTATION.md) records architecture and
verification.

- [Install with your local agent](#install-with-your-local-agent)
- [Build and run from source](#build-and-run-from-source)
- [First use](#first-use)
- [Configuration as code](#configuration-as-code)
- [Terminal interface](#terminal-interface)
- [Availability rules](#availability-rules)
- [MCP servers](#mcp-servers)
- [Skills](#skills)
- [Custom subagents](#custom-subagents)
- [Configuration safety and scope](#configuration-safety-and-scope)
- [Development and verification](#development-and-verification)

## Install with your local agent

Copy this prompt into your local coding agent:

```text
Install Kitmux from https://github.com/cleisson-labs/kitmux/releases and import my
existing setup. Read the README for the selected stable release. Resolve one
release tag, select the executable for this machine, verify its SHA256 checksum,
and install it at ~/.local/bin/kitmux using the documented atomic upgrade and
backup procedure. Keep an existing newer version; report incompatible platforms
or symlink installations instead of replacing another installer's files.
Ensure Kitmux is reachable on PATH and verify its version.

Detect existing user configurations for Codex, Claude Code, OpenCode, and the
Antigravity CLI (agy), including
their skill/subagent directories. For the detected clients, run import --preview,
inspect the result, and apply the import of supported MCPs, skills, and subagents.
If an older catalog requires migration, use group migrate --preview then --apply.
Preserve client-specific definitions, credentials, disabled states, unrelated
settings, and restore records. Retain project/plugin resources in their native
scope. Do not force conflicts, rewrite the private catalog/journal, or use refresh
to silently adopt conflicting external edits. Report skipped or conflicting items.

Verify with kitmux --version, status, doctor, and sync --check for each imported
client. If a check reports pending changes, inspect sync --preview and apply only
changes consistent with preserving my existing setup. Report imported resource
counts, unresolved conflicts, and client reload steps. Never print credentials
or complete native configuration files. Proceed with installation and supported
imports; ask only if resolving a conflict requires a choice about my setup.
```

## Build and run from source

Requirements: C17 compiler, Make, pkg-config, json-c, libyaml, ncursesw, and
OpenSSL libcrypto. Python 3.11+ is needed for tests only. On macOS, install build
dependencies with `brew install pkgconf json-c libyaml ncurses openssl`. On
Ubuntu/Debian use `build-essential`, `pkg-config`, `libjson-c-dev`, `libyaml-dev`,
`libncurses-dev`, and `libssl-dev`.

```sh
make
./build/kitmux --help
make install PREFIX="$HOME/.local"
```

Without installation, substitute `./build/kitmux` for `kitmux` in the examples.

Run `kitmux --help` for commands. Without a TTY, the application prints
help instead of waiting for input. `NO_COLOR=1` or `--no-color` disables colors.
Reload clients after upgrading so new processes use the installed version.
Older catalogs can be inspected before migration. Preview and apply the group
upgrade before editing groups:

```sh
kitmux group migrate --preview
kitmux group migrate --apply
```

Migration splits mixed groups by resource type, preserves their availability,
and backs up the original catalog before writing schema version 3. Empty legacy
groups become MCP groups, as listed in the preview. Keep v0.4 for subsequent
recovery; older executables cannot read schema version 3.

## First use

Import previews are read-only. Importing adopts existing user MCP definitions,
filesystem skill packages, and custom subagent definitions while retaining native
settings without displaying credentials. A shared MCP name can have different
commands, environment, or working directory per client; Kitmux keeps those
execution variants under one catalog ID.

```sh
kitmux import --from codex,claude,opencode,antigravity --preview
kitmux import --from codex,claude,opencode,antigravity --apply
kitmux status
```

To adopt deliberate external edits to already-owned entries, preview and apply
`import --refresh --from CLIENT`. Refresh retains the native file bytes and the
original restore baseline while updating Kitmux's selected client snapshots.
Ordinary import, sync, and restore continue to reject conflicting edits. Refresh
rejects drifted managed MCP bridges; reconcile their native bridge configuration
before importing so the bridge cannot become its own upstream.

Skill discovery includes legacy Codex `.codex/skills`, shared `.agents/skills`,
Claude `.claude/skills`, OpenCode `.config/opencode/skills` plus its compatible
shared roots, and Antigravity `.gemini/config/skills` plus the shared `.agents/skills`. Identical complete packages are deduplicated while native client
visibility and alias locations are retained. Existing leaf symlinks are adopted
without changing them during import; later sync manages the selected aliases,
and restore reinstates their original targets. Canonical source packages remain
untouched. Bundled `.system`, synced collections, and plugin caches are excluded.

Import skips entries it cannot adopt safely and keeps importing the rest: dangling
links, files that are not skill packages, and real directories in owned discovery
locations each print a warning naming the path. For a real directory, run
`kitmux skill add PATH`, move the original aside, then enable the skill for its
clients. Different package contents sharing one skill ID still stop the import and
name the path; choose one package with `skill update --source`. Unusable subagent
files are skipped the same way. Native Codex skill-disable controls remain in force,
whether they select the skill by name or by any of its paths; re-enabling a blocked
skill requires changing that specific native override before retrying sync.

Create a group and assign it to any subset of the clients:

```sh
kitmux mcp group add communication
kitmux mcp group include communication teams slack
kitmux mcp group assign communication --agents codex,claude,opencode,antigravity
kitmux mcp group enable communication
```

The example assumes those server IDs exist. Imported resources preserve their
existing direct agent assignments; use `mcp inherit ID --agent AGENT` to let group
membership control them instead.

For a new catalog, preview and apply native configuration once:

```sh
kitmux sync --preview
kitmux sync --apply
```

Use `--agent codex`, `--agent claude`, or `--agent opencode` to adopt one client.
After adoption, catalog mutations automatically sync **only adopted clients**.
Each client is synchronized independently: a failure in one client never blocks
native changes for the others, and the command exits nonzero naming each failed
client. An external edit to an owned entry produces a conflict only when the entry
differs from what Kitmux wants; dropped client defaults (Codex `enabled = true`,
OpenCode `disabled: false`) and deletions of entries Kitmux also wants gone are not
conflicts. Kitmux retains your desired catalog change, reports that native sync is
pending, and names the client, file, entry, and recovery command. Use
`import --refresh --from CLIENT` to adopt an edit or deletion for that client only;
inherited and group availability are left unchanged unless the native state
contradicts them.

Native configuration changes may require reloading the client or starting a new
session. `status` reports desired availability alongside observed managed bridge
processes. Native connection state and client tool refresh remain unverified;
writing configuration is not proof of a connection.
For automation, `kitmux sync --check [--agent CLIENT]` performs the same read-only
planning and returns zero only when selected projections are current and no
journal recovery is pending. A planned change, conflict, or pending journal
returns nonzero. `--check` cannot be combined with `--apply`.

## Configuration as code

Use native MCP source files when provisioning options such as tool allowlists,
timeouts, or per-client environment values:

```sh
kitmux mcp add docs --from ./docs-codex.json --format codex
kitmux mcp edit docs --from ./docs-claude.json --format claude
kitmux skill update reviewer --source ./skills/reviewer --preview
kitmux skill update reviewer --source ./skills/reviewer --apply
```

For every format, the MCP source is **one JSON object**, not a complete client
configuration: the contents of Codex's `mcp_servers.ID`, Claude's `mcpServers.ID`,
or OpenCode's `mcp.servers.ID`. For example, a Codex source can contain
`{"url":"https://example.invalid/mcp","enabled_tools":["search"]}`. Local Codex
and Claude definitions use a command string plus an optional `args` array;
OpenCode uses a command array. A source must select exactly one command or URL.

Native edits change only that client's execution and options. Existing global
availability, client assignments, groups, and runtime mode remain in force;
new resources start unassigned. At an unchanged command/argument/working-directory
or URL endpoint,
source fields recursively merge with that client's existing native options,
preserving omitted credentials and local settings. A changed endpoint replaces
its native snapshot using only the supplied source; old credentials are not
transferred. Other clients retain their own variants. Keep credential-bearing
source files private and provide credentials again only for the intended endpoint.

Skill updates with `--source DIRECTORY` retain policy and rollback history and
rebind the update source even when package contents match. Repeated identical
native MCP, subagent, and skill updates leave the catalog revision unchanged.
An explicit new skill source resets the previous source's subdirectory and Git
ref unless `--path` or `--ref` is supplied again.
`status --json` exposes each resource's global `enabled` and raw `assignments`
alongside effective availability; it omits MCP options and credentials.

## Terminal interface

| Key | Action |
|---|---|
| `1`–`5`, Tab | MCPs, Skills, Subagents, Clients, Status |
| Arrows, `j`/`k` | Move selection |
| Enter | Expand/collapse a group; open a resource document |
| Left/Right | Collapse/expand a group |
| `v` | Show group details or open a resource document |
| Space | Select multiple rows |
| `/`, Esc | Filter; clear filter or cancel |
| `n`, `x`, `d` | Add, edit, remove |
| `g` | Create a group inside the current resource tab |
| `p` on Subagents | Duplicate a definition with a new ID |
| `e`, `a`, `m` | Toggle, assign clients, edit group memberships |
| `r` | Reconnect selected MCP |
| `s`, `i` | Preview/apply native sync or import |
| `u`, `b` | Preview/apply skill update or roll back |
| `c`, `?`, `q` | Check configuration, help, quit |
| `f` on Status, Clients | Refresh the status snapshot or client state |

The interface needs at least 74 columns and 20 rows. It handles terminal resize,
normal exit, and interruption without leaving echo disabled. Red marks the active
tab, the selected row, and warnings; grey carries secondary text, OFF states, and
hints; everything else uses the terminal's own foreground and background, so light
and dark themes both work. `NO_COLOR=1` or `--no-color` switches to bold, dim, and
reverse video. UTF-8 terminals show ■ for ON, · for OFF, ◐ for a mixed group, and !
when a subagent needs a native variant; other locales show `[x]`, `[ ]`, `[-]`, and
`[!]`. Columns follow the supported clients, and the name column fits the longest
visible name.
Resource documents support arrows or `j`/`k`, Page Up/Down, Space, Home/End or
`g`/`G`; Enter, Esc, or `q` returns to the selected row. Headings and fenced code
receive terminal formatting, while other Markdown remains readable as text.
Skills show the installed `SKILL.md`, including documents larger than 32 KiB.
Subagents show shared instructions and labeled effective native variants. Each
document opens with a one-line client summary and ends with per-client reasons. MCPs
show their description, transport, mode, and availability without endpoint
arguments, environment values, or native authentication fields. Opening details
does not run servers or change configuration.
Groups show their matching resources as indented rows; uncategorized resources
appear under Ungrouped. Search finds resources even inside collapsed groups.
An item appearing in several groups is one resource: edits apply everywhere,
and bulk actions process it once. Removing a group preserves its resources.
Groups can show mixed effective client availability when member overrides differ.
In the assign dialog (`a`), only clients you change become explicit per-client
overrides; unchanged clients keep following their groups, and a globally OFF item
keeps its assignments. Space marks rows for bulk actions; marks stay selected when
their group is collapsed or filtered out. The list reloads when another Kitmux
process changes the catalog. A banner reports pending native sync, and the Clients
tab shows each client's adoption, ON counts, and native check result.
Local MCP forms offer direct/managed mode selection. Reconnect asks which client
to target and shows its result. Status is a captured snapshot; press `f` to query
again.

## Availability rules

- A globally disabled resource is off for every client.
- An explicit per-client `off` wins over group membership.
- Otherwise, direct `on` or membership in any enabled assigned group makes it
  available. Multiple groups do not duplicate a resource.
- `inherit` removes a per-client override. No assignment means off.

```sh
kitmux mcp disable teams --agent claude
kitmux mcp inherit teams --agent claude
kitmux mcp group disable communication
kitmux status --json
```

Assignment shares definitions. It does not merge accounts, authentication stores,
permissions, or conversation state. These controls are not a sandbox against an
agent that also has ordinary filesystem/shell access.

## MCP servers

Register an existing command or an HTTP endpoint:

```sh
kitmux mcp add example --command /absolute/path/to/mcp-server -- --stdio
kitmux mcp add docs --url https://example.test/mcp
kitmux mcp edit example --cwd /absolute/project/path
kitmux mcp edit example --description-file ./server-guide.md
kitmux mcp edit docs --description "Search the team's documentation"
kitmux mcp reconnect example --agent codex
kitmux mcp status example --agent codex --json
kitmux mcp remove example
```

Registration does not install the MCP server. Direct mode leaves connection
ownership with the client; reconnect returns an actionable instruction when
Kitmux cannot control that client connection. HTTP/OAuth remains direct.
Use one of `--description TEXT` or `--description-file FILE` with add/edit, including
native `--from` provisioning. The TUI form offers text, Markdown file, keep/skip,
and clear choices. Descriptions remain local catalog metadata; imports and refresh
preserve authored text. Existing entries without documentation show a fallback.

Imported endpoints cannot be changed to a different command/URL under the same
ID. Add a new ID and review its settings; authentication from the previous
endpoint must not silently follow it.

For Antigravity, Kitmux writes `serverUrl` for HTTP endpoints (keeping `url` when an
imported entry used it) and represents off as `"disabled": true` on imported entries;
entries Kitmux created are removed. `agy` rewrites this file itself, so dropped
defaults such as `disabled: false` or empty `args` are not conflicts. The empty file
`agy` creates on first start reads as no servers. `httpUrl` and legacy SSE are not
supported; reload direct servers from the `/mcp` panel in `agy`.

### Managed local bridge

For a compatible local server, use `--mode managed` when adding or editing it.
Native settings then launch a Kitmux bridge under that server's original name.
The bridge starts its own upstream process during capability discovery, observes
catalog policy changes, and accepts `mcp reconnect` through a private local socket.
Startup is asynchronous: requests queue for up to 30 seconds while the upstream
initializes, a failed start is retried on the next request with backoff (0.5 s
doubling to 30 s), and one `tools/list_changed` follows recovery after a failed
tool listing. Codex currently ignores that notification, so a running Codex session
shows toggled tools only after a restart.
It stays registered while disabled, returns an empty tool list, and rejects stale
calls. Removing the resource removes its owned native entry.

Each downstream connection has a separate upstream process. Reconnect fails
interrupted calls with an unknown-completion error and never replays them.
Only tracked child process groups are stopped. There is no permanent daemon.

Inspect processes without starting or reconnecting them:

```sh
kitmux mcp status example --agent codex
kitmux status --json
```

Authenticated local queries report each bridge PID, its actual upstream PID,
pending requests, protocol, and the last upstream error. `waiting` means the client
has not initialized; `starting` means the upstream is initializing; `idle` means
initialized with no upstream; `running` means a live upstream was
observed; `disabled` means the bridge has observed off policy. An `inactive` result
means no bridge was found. Sockets left by exited bridges are removed during
status and reconnect scans; a socket whose owner is alive but unresponsive is
`unreachable`, and a mix of reachable and unreachable
connections is `partial`. Direct connections remain `unverified`. These process
observations do not certify that the client's tool list refreshed.
Queries have a 500ms/64-endpoint budget per MCP/client; JSON marks incomplete
scans with `scan_complete: false` and excludes unqueried endpoints from counts.

**Supported bridge contract:** newline-delimited stdio, tools only, MCP revisions
`2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25`. Unknown version offers
in legacy initialization negotiate to `2025-11-25`; modern discovery probes
receive method-not-found so clients can fall back. The 2026 stateless lifecycle,
resources, prompts, roots, sampling, and elicitation are not supported. Upstream
servers may select any of the supported legacy revisions.

Native lifecycle qualification uses isolated Codex app-server, Claude Code headless,
and OpenCode private-server surfaces with read-only fixture tools and synthetic
providers. It does not certify arbitrary servers or authenticated integrations.
Keep session-bound integrations such as ai-memory direct. See the
[compatibility evidence and qualification limits](RUNTIME_COMPATIBILITY.md)
and [exact native client test results](NATIVE_CLIENT_QUALIFICATION.md).

Cancellation is forwarded only for outstanding calls. Cancelled calls retain a
bounded record until upstream completion or reconnect, so late results cannot be
mistaken for another call. They count toward the 64-request limit. Only correlated
progress is forwarded. Upstream initialization instructions are not surfaced by
the lazy tools-only bridge; use direct mode for servers that require them.

Upstreams may advertise other capabilities, but only tools are bridged; an
upstream without the tools capability is rejected with its own error. Messages are
forwarded as their original lines, blank lines and up to 64 consecutive non-JSON
upstream lines are skipped, and `ping` is answered before initialization. Stopping
an upstream closes its stdin first and waits up to 1 second before terminating its
process group. Reconnect reports each failing bridge's reason.

Upstream stderr is discarded to avoid exposing credentials. Limits include a
1 MiB protocol message, 512 levels of JSON nesting, 64 pending requests, and
bounded startup/shutdown. Control
sockets require a private, sufficiently short state path; the CLI reports an
actionable error if the platform socket-path limit is exceeded.

## Skills

```sh
kitmux skill add /absolute/path/to/meeting-notes
kitmux skill add owner/repository --path skills/meeting-notes --ref COMMIT
kitmux skill group add communication
kitmux skill group include communication meeting-notes
kitmux skill update meeting-notes --preview
kitmux skill update meeting-notes --apply --ref NEW_COMMIT
kitmux skill rollback meeting-notes
```

Complete skill packages are copied into a SHA256-addressed store outside agent
discovery roots. Downloads use Git objects rather than a checkout; package scripts,
checkout filters, and hooks do not execute during installation. Source revisions
are pinned and packages start unassigned. Updates preserve agent choices and show
the revision plus changed files before applying. Installed revisions are read-only
(files 0400/0500, directories 0500); restore write permission before deleting the
store by hand. Git runs without inherited repository variables such as `GIT_DIR`.

The installer checks standard YAML metadata, rejects package-internal symlinks and
special files, and bounds files, total size, directory depth, and entry count.
Use import to adopt existing native leaf aliases with a restore record; directory
aliases or unsupported collection layouts require deliberate migration.

Client adapters own only their projections and visibility entries. OpenCode's
compatibility discovery from Claude/shared skill directories is handled through
explicit skill permissions; disabling suggestions alone is insufficient. Kitmux
owns only the OpenCode permission rules it adds: other edits to the array are kept,
a Kitmux allow rule overrides a native deny while Kitmux enables the skill, and
restore removes Kitmux's rules. Name-based hides (Claude `skillOverrides`, OpenCode
denies) are written only for skills whose Kitmux projection that client can see. A
deleted Kitmux link is recreated, or left absent when the skill is off. A skill
already loaded into a conversation remains in its context until a fresh session.

Antigravity has no per-skill switch. Kitmux links skills into
`~/.gemini/config/skills`, and when a skill is off for Antigravity but visible through
the shared `~/.agents/skills`, it adds the name to the top-level `exclude` list in
`~/.gemini/config/skills.json`. Kitmux owns only the names it adds and removes a
manifest it created once it is empty. Skills in `~/.gemini/skills` and
`~/.gemini/antigravity-cli/skills` stay native-managed and cannot be hidden by Kitmux.

Removing a skill detaches its owned projections after sync and cleans group
membership. Source repositories and cached revisions remain. Cache pruning and
full textual update diffs are not implemented yet.

### Kitmux usage skill

The included [Kitmux skill](../skills/kitmux/SKILL.md) teaches agents the CLI workflow,
group policy, safe imports, reconnect limits, and native refresh requirements.
Its default invocation policy permits automatic selection for relevant tasks.
Use the same package for every client:

```sh
kitmux skill add /path/to/kitmux/skills/kitmux
kitmux skill enable kitmux --agent codex
kitmux skill enable kitmux --agent claude
kitmux skill enable kitmux --agent opencode
kitmux skill enable kitmux --agent antigravity
```

These direct assignments make the guide available by default across the clients;
normal per-client controls can disable it. Clients may need a refresh or new
session to discover a newly installed skill.

## Custom subagents

Press `3` for Subagents, select a profile, and press `a` to choose its clients.
Space toggles a checkbox; Enter saves only the clients you changed. `e` controls
global availability. Subagents
use the same group union, explicit per-client off, and global off rules as skills
and MCPs. Assignments control future native discovery, not running tasks.

Import only existing subagent definitions. This adopts the client's subagents
only; it does not opt that client into MCP or skill management:

```sh
kitmux subagent import --from codex,claude,opencode,antigravity --preview
kitmux subagent import --from codex,claude,opencode,antigravity --apply
kitmux subagent list
kitmux subagent show code-reviewer
```

Create a portable profile from a plain Markdown instructions file:

```sh
kitmux subagent add code-reviewer --from ./review-instructions.md \
  --description "Review code changes for correctness and regressions"
kitmux subagent group add engineering
kitmux subagent group include engineering code-reviewer
kitmux subagent group assign engineering --agents codex,claude,opencode,antigravity
kitmux sync --preview
kitmux sync --apply
```

New profiles begin unassigned. Portable profiles contain a description and
instructions; the target client supplies its normal native defaults. They do not
translate models, grant tools, or preload resources merely through group membership.

```sh
kitmux subagent disable code-reviewer --agent claude
kitmux subagent inherit code-reviewer --agent claude
kitmux subagent edit code-reviewer --from ./updated-instructions.md
kitmux subagent duplicate code-reviewer release-reviewer
kitmux subagent remove release-reviewer
```

Duplicating copies the definition and native variants but starts with no client
assignments or group memberships. Editing shared instructions preserves native
options. Use `--format codex`, `--format claude`, or `--format opencode` when the
source is a native TOML or Markdown definition. A native-format edit supplies a
variant for that client. Imported profiles keep separate native variants under
one ID; assigning an imported profile to a client with no variant reports it as
incompatible until an appropriate native variant is supplied.

User definition paths are `~/.codex/agents/*.toml`, `~/.claude/agents/*.md`,
`~/.config/opencode/agents/*.md`, and Antigravity `~/.gemini/config/agents/<id>.md`
or `<id>/agent.md`; OpenCode v2 also supports `agents` entries in its JSON/JSONC
configuration. Antigravity definitions need `name` and `description` frontmatter;
portable profiles render only those fields and the body. `--format antigravity`
supplies a native variant. Imports preserve originals for `restore` and detect
conflicting external edits. V0.3 manages flat, regular-file collections; nested
collections, symlinks, and filename/native-name mismatches need explicit migration.
Changing an adopted or projected OpenCode profile between Markdown and JSON is
rejected before saving; edit it in its current format. Project and plugin
definitions keep native precedence.
After applying changes, refresh the client or start a new session; disabling a
definition does not terminate a running subagent or erase its context.

## Configuration safety and scope

Kitmux manages **user-level** Codex, Claude Code, OpenCode, and Antigravity configuration.
Project and organization settings retain their native precedence and can change
effective availability. OpenCode integration targets the v2 schema. Native
configuration syntax and conflicts are checked; unsupported constructs produce
an error instead of a whole-file rewrite.

Imported Codex stdio definitions can switch between direct and managed mode while
preserving native policy and comments. Changes to multiline command/argument
values or unsupported TOML key layouts fail closed and retain the native file.

Kitmux writes an MCP into a client's configuration only when it is ON for that
client or the client already has that entry, so environment values and credentials
never spread to unassigned clients. Entries Kitmux created are removed when turned
off; imported Codex and OpenCode entries keep their native disabled form, while
Claude entries are removed. Codex entries omit the default `enabled = true`, new
JSON entries follow the file's indentation, and a managed bridge keeps its existing
Kitmux executable while that file exists. Claude SSE endpoints are not projected
into Codex or OpenCode, which support only Streamable HTTP.

Owned entries have before/after records, while unrelated settings and JSONC/TOML
comments are preserved. Writes use a catalog lock, atomic replacement, private
permissions, and a journal for native updates. Partial multi-client application
is recoverable; it is not claimed to be one atomic transaction.

```sh
kitmux doctor
kitmux restore --agent claude --preview
kitmux restore --agent claude --apply
```

Restore checks for concurrent edits and restores owned entries/projections to
their original state. It never overwrites newer unrelated changes. Restored
clients leave automatic management until explicitly adopted again with sync.

Paths honor XDG overrides for Kitmux's own files:

```text
~/.config/kitmux/catalog.json
~/.local/share/kitmux/skills/
~/.local/state/kitmux/adapters.json
~/.local/state/kitmux/r/
```

The catalog and ownership journal can contain private native settings. Keep them
local; do not publish them or add them to a dotfiles repository. Normal status and
preview output omit command arguments, environment values, and native auth fields.

## Development and verification

```sh
make test
make sanitize
```

Optional installed-client qualification uses isolated homes and synthetic loopback
providers without live accounts:

```sh
make test-native
```

See [native client qualification](NATIVE_CLIENT_QUALIFICATION.md) for exact
tested releases and surfaces. Missing clients are reported as skipped tests.

Tests use temporary homes and stub processes. `--home DIR` requires an existing
absolute directory and isolates all Kitmux/native test paths beneath it without
changing the shell's HOME. No live client configuration is used by the suite.

The test suite covers policy precedence, client assignment combinations, malformed
input, native preservation/conflicts, skill provenance/update/rollback, runtime
reconnect/cancellation boundaries, and terminal restoration. Sanitizer builds use
AddressSanitizer and UndefinedBehaviorSanitizer.

See [implementation progress](IMPLEMENTATION.md), the original
[product plan](PLAN.md), the [concept diagram](kitmux-concept.html), and
[third-party notices](../THIRD_PARTY.md).
The [v0.7 release notes](V0.7.md) describe the current release.

### Continuous integration and release provenance

GitHub Actions runs a single required gate, `ci-ok`, which passes only when workflow
linting (actionlint and zizmor), cppcheck static analysis (`make analyze`), and the
ordinary and ASan/UBSan suites on macOS 15 and Ubuntu 22.04 (arm64 and x86_64) all
succeed; pull requests that change only documentation skip the build jobs. CodeQL
scans the C sources and Python tests on pull requests and weekly, pull request titles
follow Conventional Commits, and Dependabot proposes pinned action updates weekly.
Every action is pinned to a commit SHA, and each job receives only the token
permissions it needs.

Releases built by the attestation-enabled workflow carry SLSA build provenance for
every archive and `SHA256SUMS`. Verify a download before installing it (GitHub CLI
2.49 or later):

```sh
gh attestation verify kitmux-vX.Y.Z-OS-ARCH.tar.gz --repo cleisson-labs/kitmux \
  --signer-workflow cleisson-labs/kitmux/.github/workflows/release.yml \
  --source-ref refs/tags/vX.Y.Z
```

Published v0.5.0 and v0.6.0 assets predate the attestation-enabled workflow.
Use their `SHA256SUMS` for checksum verification.
