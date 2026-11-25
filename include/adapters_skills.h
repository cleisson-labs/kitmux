#ifndef KITMUX_ADAPTERS_SKILLS_H
#define KITMUX_ADAPTERS_SKILLS_H
#include "adapters.h"

/* Internal contracts shared by the skill projection, import, and OpenCode rule modules. */
void ad_release_skill_link(AdRun *run, const char *path);
bool ad_skill_link_owned(AdRun *run, const char *path);
int ad_codex_skill_allowed(AdRun *run, const char *id, const char *path);
bool ad_skill_shared_visible(AdRun *run, const char *id, json_object *skill);
int ad_sync_skill_rules(AdRun *run);
int ad_restore_skill_rules(AdRun *run);
bool ad_skill_agents_root_visible(AdRun *run, const char *id, json_object *skill);
int ad_sync_skill_excludes(AdRun *run);
int ad_restore_skill_excludes(AdRun *run);
int ad_skill_manifest_enabled(AdRun *run, const char *id, bool *enabled);
#endif
