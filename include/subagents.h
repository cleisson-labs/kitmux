#ifndef KITMUX_SUBAGENTS_H
#define KITMUX_SUBAGENTS_H
#include "kitmux.h"

/* Native snapshots retain exact source text and format (codex, claude,
 * opencode, opencode-json). Their description/instructions are the canonical
 * values at attachment, not necessarily the values in that client's text.
 * Equal canonical values preserve native variants; a later shared edit changes
 * only the edited common field. Rebase these two values when attaching a
 * variant to an existing resource. Imported definitions are not portable.
 */
json_object *km_subagent_parse(Km *km, const char *text, const char *format, const char *id);
char *km_subagent_render(Km *km, json_object *resource, const char *id, const char *client);
int km_subagent_validate(Km *km, const char *id, json_object *resource);
bool km_subagent_compatible(json_object *resource, const char *client, char *reason, size_t size);
/* Renders and parses back every compatible client variant, so anything accepted
 * here can be projected and displayed. Errors name the failing client. When
 * journal_bytes is set, it receives the worst-case ownership-journal footprint. */
int km_subagent_check(Km *km, const char *id, json_object *resource, size_t *journal_bytes);
/* True when policy assigns the profile to client but it has no usable variant. */
bool km_subagent_unusable_assignment(Km *km, const char *id, const char *client);
/* Existing ID equal to id except for letter case, or NULL. */
const char *km_subagent_case_twin(Km *km, const char *id);
/* Prints a stderr warning for each client assigned to id without a usable variant. */
void km_subagent_warn_assignments(Km *km, const char *id);

/* Codec internals. Parsed documents contain description, instructions, optional
 * name and disabled. Editors change only the requested common fields.
 */
json_object *km_subagent_markdown(Km *km, const char *text, const char *client);
char *km_subagent_markdown_mode(Km *km, const char *text);
char *km_subagent_markdown_edit(Km *km, const char *text, const char *client,
                               const char *id, const char *description, const char *instructions);
json_object *km_subagent_toml(Km *km, const char *text);
char *km_subagent_toml_edit(Km *km, const char *text, const char *id,
                           const char *description, const char *instructions);
char *km_subagent_markdown_disable(Km *km, const char *text);
/* Double-quoted scalars that every Kitmux parser and native client accepts. */
char *km_subagent_yaml_quote(const char *value);
char *km_subagent_toml_quote(const char *value);
#endif
