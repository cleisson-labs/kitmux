#include "adapters.h"
#include "subagents.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int default_path(Km *km, char *path, const char *agent, const char *id) {
    const char *root=!strcmp(agent,"codex") ? ".codex/agents" : !strcmp(agent,"claude") ? ".claude/agents" :
                     !strcmp(agent,"antigravity") ? ".gemini/config/agents" : ".config/opencode/agents";
    return km_path(km,path,PATH_MAX,"%s/%s/%s.%s",km->home,root,id,!strcmp(agent,"codex") ? "toml" : "md");
}

static int opencode_collision(AdRun *run, const char *id, bool json_origin);

static char *disabled_json(Km *km, json_object *resource, const char *id) {
    char *desired=strdup(km_string(km_get(km_get(resource,"native"),"opencode"),"text"));
    if (!desired) { km_error(km,"out of memory"); return NULL; }
    json_object *parsed=NULL;
    if (ad_json_edit(km,&desired,"disabled","true")<0 || !(parsed=km_subagent_parse(km,desired,"opencode-json",id))) {
        free(desired); return NULL;
    }
    json_object_put(parsed); return desired;
}

static int sync_json(AdRun *run, json_object *resource, const char *id,
                      json_object *origin, bool enabled) {
    const char *path=km_string(origin,"path"); char key[80];
    if (km_path(run->km,key,sizeof(key),"agents/%s",id)<0) return -1;
    AdFile *file=ad_native_file(run,"opencode");
    if (!file) return -1;
    if (path && strcmp(file->path,path))
        return km_error(run->km,"Native OpenCode subagent %s moved from %s to %s; preserve it and run `kitmux subagent import --from opencode --apply`",id,path,file->path);
    if (!enabled && !origin && !km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key)) return 0;
    if (opencode_collision(run,id,true)<0) return -1;
    char *desired=enabled ? km_subagent_render(run->km,resource,id,"opencode") : disabled_json(run->km,resource,id);
    if (!desired) return -1;
    int result=ad_change(run,file,key,desired,false);
    free(desired);
    if (!result) json_object_object_add(run->state,"version",json_object_new_int(2));
    return result;
}

static int opencode_collision(AdRun *run, const char *id, bool json_origin) {
    char config[PATH_MAX];
    if (km_path(run->km,config,sizeof(config),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        ad_subagent_parent_safe(run->km,config)<0) return -1;
    if (json_origin) {
        char path[PATH_MAX]; struct stat st;
        if (default_path(run->km,path,"opencode",id)<0 || ad_subagent_parent_safe(run->km,path)<0) return -1;
        if (lstat(path,&st)==0)
            return km_error(run->km,"OpenCode subagent %s has both %s and a JSON definition; keep one of them, then run `kitmux sync --apply --agent opencode`",id,path);
        return errno==ENOENT ? 0 : km_error(run->km,"Cannot inspect OpenCode subagent discovery %s",path);
    }
    AdFile *file=ad_native_file(run,"opencode"); char key[80];
    if (!file || km_path(run->km,key,sizeof(key),"agents/%s",id)<0) return -1;
    char *raw=ad_json_raw(run->km,file->text,key);
    bool collision=raw!=NULL; free(raw);
    if (run->km->error[0]) return -1;
    return collision ? km_error(run->km,"Unowned OpenCode subagent collision for %s in %s (%s); adopt it with `kitmux subagent import --from opencode --apply` or remove one definition",id,file->path,key) : 0;
}

/* An imported definition stays where it was found; others use the flat layout. */
static int target_path(AdRun *run, char *path, const char *agent, const char *id, json_object *origin) {
    const char *found=origin && !km_string(origin,"key") ? km_string(origin,"path") : NULL;
    if (found && ad_subagent_path_valid(run->km,found,agent,id)) return km_path(run->km,path,PATH_MAX,"%s",found);
    return default_path(run->km,path,agent,id);
}

/* Antigravity reads both <id>.md and <id>/agent.md; never create a second one. */
static int layout_collision(AdRun *run, const char *id, const char *target) {
    char flat[PATH_MAX], folder[PATH_MAX]; struct stat st;
    if (default_path(run->km,flat,"antigravity",id)<0 || ad_subagent_folder_path(run->km,folder,id)<0) return -1;
    const char *other=strcmp(target,flat) ? flat : folder;
    if (km_get(km_get(run->state,"subagent_files"),other) || lstat(other,&st)<0) return 0;
    return km_error(run->km,"Antigravity subagent %s already has a native definition at %s; import it with "
                    "`kitmux subagent import --from antigravity --apply` or remove one definition, then retry",id,other);
}

/* Imported OpenCode Markdown keeps its native disabled form instead of being
 * deleted; Kitmux-created projections are removed when off. */
static char *desired_text(AdRun *run, json_object *resource, const char *id, const char *agent,
                          json_object *origin, bool enabled, bool *absent) {
    *absent=!enabled && !(origin && !strcmp(agent,"opencode"));
    if (*absent) return NULL;
    char *text=km_subagent_render(run->km,resource,id,agent);
    if (enabled || !text) return text;
    char *disabled=km_subagent_markdown_disable(run->km,text); free(text);
    return disabled;
}

static int sync_resource(AdRun *run, const char *agent, const char *id, json_object *resource) {
    if (ad_subagent_origins_valid(run->km,resource,id)<0) return -1;
    if (!km_subagent_compatible(resource,agent,NULL,0)) {
        if (km_subagent_unusable_assignment(run->km,id,agent))
            printf("%s: subagent %s is assigned but has no %s variant; add one with `kitmux subagent edit %s --from FILE --format %s`\n",
                   agent,id,agent,id,agent);
        return 0;
    }
    bool enabled=km_effective(run->km,"subagent",id,agent,NULL,0);
    json_object *origin=km_get(km_get(resource,"origins"),agent);
    const char *format=km_string(km_get(km_get(resource,"native"),agent),"format");
    bool json_target=!strcmp(agent,"opencode") && ((origin && km_string(origin,"key")) || (format && !strcmp(format,"opencode-json")));
    if (json_target) return sync_json(run,resource,id,origin,enabled);
    if (!strcmp(agent,"opencode") && (enabled || origin) && opencode_collision(run,id,false)<0) return -1;
    char path[PATH_MAX];
    if (target_path(run,path,agent,id,origin)<0) return -1;
    if (!strcmp(agent,"antigravity") && (enabled || origin) && layout_collision(run,id,path)<0) return -1;
    json_object *record=km_get(km_get(run->state,"subagent_files"),path);
    if (!enabled && !record) return 0;
    bool absent=false;
    char *desired=desired_text(run,resource,id,agent,origin,enabled,&absent);
    if (!absent && !desired) return -1;
    int result=ad_subagent_change(run,agent,path,id,agent,desired,false);
    free(desired); return result;
}

static int detach_removed_files(AdRun *run, const char *agent) {
    json_object *records=km_get(run->state,"subagent_files"), *resources=km_resources(run->km,"subagent");
    if (!records) return 0;
    json_object_object_foreach(records,path,record) {
        const char *id=km_string(record,"id");
        if (strcmp(agent,km_string(record,"agent")) || km_get(resources,id)) continue;
        /* Already detached: the record only keeps a restore baseline. */
        if (!km_string(record,"after") && !json_object_get_boolean(km_get(run->state,"pending"))) continue;
        if (ad_subagent_change(run,agent,path,id,km_string(record,"format"),NULL,false)<0) return -1;
    }
    return 0;
}

static int detach_removed_json(AdRun *run, const char *agent) {
    if (strcmp(agent,"opencode")) return 0;
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        if (strcmp(agent,km_string(record,"agent"))) continue;
        json_object *entries=km_get(record,"entries");
        json_object_object_foreach(entries,key,entry) {
            (void)entry;
            if (strncmp(key,"agents/",7) || km_get(km_resources(run->km,"subagent"),key+7)) continue;
            AdFile *file=ad_native_file(run,agent);
            if (!file) return -1;
            if (strcmp(path,file->path)) return km_error(run->km,"Native OpenCode subagent origin moved from %s to %s; restore it and rerun `kitmux sync --apply --agent opencode`",path,file->path);
            if (ad_change(run,file,key,NULL,false)<0) return -1;
        }
    }
    return 0;
}

static bool owns_entries(json_object *files, const char *agent) {
    if (!files) return false;
    json_object_object_foreach(files,path,record) {
        (void)path;
        json_object *entries=km_get(record,"entries");
        if (!entries || strcmp(agent,km_string(record,"agent"))) continue;
        json_object_object_foreach(entries,key,entry)
            if (!strncmp(key,"agents/",7) && km_string(entry,"after")) return true;
    }
    return false;
}

/* Layout checks apply only where Kitmux projects or still owns definitions, so an
 * unassigned profile never makes another client's collection a blocker. */
static bool projects_subagents(AdRun *run, const char *agent) {
    if (json_object_get_boolean(km_get(run->state,"pending"))) return true;
    json_object_object_foreach(km_resources(run->km,"subagent"),id,resource) {
        (void)resource;
        if (km_effective(run->km,"subagent",id,agent,NULL,0)) return true;
    }
    json_object *records=km_get(run->state,"subagent_files");
    if (records) { json_object_object_foreach(records,path,record) {
        (void)path;
        if (!strcmp(agent,km_string(record,"agent")) && km_string(record,"after")) return true;
    } }
    return owns_entries(km_get(run->state,"files"),agent);
}

int ad_sync_subagents(AdRun *run, const char *agent) {
    if (projects_subagents(run,agent) && ad_subagent_flat_discovery(run->km,agent)<0) return -1;
    json_object *resources=km_resources(run->km,"subagent");
    if (resources) {
        json_object_object_foreach(resources,id,resource)
            if (sync_resource(run,agent,id,resource)<0) return -1;
    }
    if (detach_removed_files(run,agent)<0) return -1;
    return detach_removed_json(run,agent);
}
