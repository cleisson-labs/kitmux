#include "adapters_skills.h"
#include "adapters_toml_vendor.h"
#include "skills_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *value=km_get(parent,key);
    if (!value) { value=json_object_new_object(); json_object_object_add(parent,key,value); }
    return value;
}

static bool location_matches(Km *km, const char *expanded, const char *canonical, const char *directory) {
    char candidate[PATH_MAX], resolved[PATH_MAX];
    if (!directory || !*directory) return false;
    if (!strcmp(expanded,directory)) return true;
    if (km_path(km,candidate,sizeof(candidate),"%s/SKILL.md",directory)<0) return false;
    if (!strcmp(expanded,candidate)) return true;
    if (!canonical) return false;
    return (realpath(directory,resolved) && !strcmp(canonical,resolved)) ||
           (realpath(candidate,resolved) && !strcmp(canonical,resolved));
}

/* A path selector matches the projection, legacy/shared aliases, the original
 * source, the installed revision, or an absolute imported alias target, so
 * enforcement does not depend on whether an alias currently exists. */
static bool path_matches(Km *km, const char *configured, const char *source, const char *id) {
    char expanded[PATH_MAX], canonical[PATH_MAX], legacy[PATH_MAX], modern[PATH_MAX];
    if (configured[0]=='~' && configured[1]=='/') {
        if (km_path(km,expanded,sizeof(expanded),"%s/%s",km->home,configured+2)<0) return false;
    } else if (km_path(km,expanded,sizeof(expanded),"%s",configured)<0) return false;
    if (km_path(km,legacy,sizeof(legacy),"%s/.codex/skills/%s",km->home,id)<0 ||
        km_path(km,modern,sizeof(modern),"%s/.agents/skills/%s",km->home,id)<0) return false;
    const char *real=realpath(expanded,canonical) ? canonical : NULL;
    json_object *skill=km_get(km_resources(km,"skill"),id);
    const char *roots[]={source,legacy,modern,km_string(skill,"source"),km_string(skill,"path")};
    for (size_t i=0;i<sizeof(roots)/sizeof(*roots);i++)
        if (location_matches(km,expanded,real,roots[i])) return true;
    const char *fields[]={"discovery","origins"};
    for (size_t f=0;f<2;f++) {
        json_object *bindings=km_get(km_get(skill,fields[f]),"codex");
        if (!json_object_is_type(bindings,json_type_array)) continue;
        for (size_t i=0;i<json_object_array_length(bindings);i++) {
            const char *target=km_string(json_object_array_get_idx(bindings,i),"target");
            if (target && *target=='/' && location_matches(km,expanded,real,target)) return true;
        }
    }
    return false;
}

static int codex_enabled(AdRun *run, const char *id, const char *source, bool *enabled, char *selector, size_t size) {
    AdFile *file=ad_native_file(run,"codex");
    if (!file) return -1;
    char error[256], *copy=strdup(file->text);
    if (!copy) return km_error(run->km,"out of memory");
    toml_table_t *root=toml_parse(copy,error,sizeof(error)); free(copy);
    if (!root) return km_error(run->km,"codex: cannot inspect native skill controls in %s: %s",file->path,error);
    toml_table_t *skills=toml_table_in(root,"skills");
    toml_array_t *entries=skills ? toml_array_in(skills,"config") : NULL;
    int result=0;
    if ((toml_raw_in(root,"skills") || toml_array_in(root,"skills")) ||
        (skills && (toml_raw_in(skills,"config") || toml_table_in(skills,"config"))))
        result=km_error(run->km,"codex: unsupported skills.config layout in %s; use [[skills.config]] tables, then run `kitmux sync --apply --agent codex`",file->path);
    for (int i=0;!result && entries && i<toml_array_nelem(entries);i++) {
        toml_table_t *entry=toml_table_at(entries,i);
        if (!entry) { result=km_error(run->km,"codex: unsupported [[skills.config]] entry %d in %s",i+1,file->path); break; }
        toml_datum_t path=toml_string_in(entry,"path"), name=toml_string_in(entry,"name"), active=toml_bool_in(entry,"enabled");
        if ((!path.ok && !name.ok) || (toml_raw_in(entry,"path") && !path.ok) ||
            (toml_raw_in(entry,"name") && !name.ok) || (toml_raw_in(entry,"enabled") && !active.ok)) {
            if (path.ok) free(path.u.s);
            if (name.ok) free(name.u.s);
            result=km_error(run->km,"codex: unsupported [[skills.config]] entry %d in %s",i+1,file->path); break;
        }
        bool by_name=name.ok && !strcmp(name.u.s,id), matches=by_name;
        if (name.ok && strpbrk(name.u.s,"*?["))
            result=km_error(run->km,"codex: pattern selector name = \"%s\" in %s requires explicit migration; replace it with exact names",name.u.s,file->path);
        if (!result && path.ok && path.u.s[0]!='/' && strncmp(path.u.s,"~/",2))
            result=km_error(run->km,"codex: relative skill path \"%s\" in %s requires explicit migration; use an absolute path",path.u.s,file->path);
        if (!result && path.ok) matches=matches || path_matches(run->km,path.u.s,source,id);
        if (!result && active.ok && !active.u.b && matches) {
            *enabled=false;
            if (selector) snprintf(selector,size,by_name ? "name = \"%s\"" : "path = \"%s\"",by_name ? name.u.s : path.u.s);
        }
        if (path.ok) free(path.u.s);
        if (name.ok) free(name.u.s);
    }
    toml_free(root);
    return result;
}

int ad_codex_skill_allowed(AdRun *run, const char *id, const char *path) {
    bool enabled=true; char selector[PATH_MAX+32]="";
    if (codex_enabled(run,id,path,&enabled,selector,sizeof(selector))<0) return -1;
    if (enabled) return 0;
    AdFile *file=ad_native_file(run,"codex");
    return km_error(run->km,"codex: native Codex skills.config entry %s in %s disables skill %s; remove that entry or set enabled = true, then run `kitmux sync --apply --agent codex`",
                    selector,file ? file->path : "~/.codex/config.toml",id);
}

static int permission_state(Km *km, json_object *rules, const char *id, bool *enabled, bool *matched) {
    if (!rules) return 0;
    if (!json_object_is_type(rules,json_type_array)) return km_error(km,"OpenCode skill import requires v2 permission arrays");
    for (size_t i=0;i<json_object_array_length(rules);i++) {
        json_object *rule=json_object_array_get_idx(rules,i);
        const char *action=km_string(rule,"action"), *resource=km_string(rule,"resource"), *effect=km_string(rule,"effect");
        if (!action || !resource || !effect) return km_error(km,"malformed native OpenCode skill permission rule");
        if (fnmatch(action,"skill",0) || fnmatch(resource,id,0)) continue;
        if (strcmp(effect,"allow") && strcmp(effect,"deny") && strcmp(effect,"ask"))
            return km_error(km,"unsupported native OpenCode skill permission effect");
        *enabled=strcmp(effect,"deny")!=0;
        *matched=true;
    }
    return 0;
}

int ad_skill_native_enabled(AdRun *run, const char *agent, const char *id, const char *source, bool *enabled) {
    *enabled=true;
    if (!strcmp(agent,"codex")) return codex_enabled(run,id,source,enabled,NULL,0);
    if (!strcmp(agent,"antigravity")) return ad_skill_manifest_enabled(run,id,enabled);
    AdFile *file=!strcmp(agent,"claude") ? ad_file(run,agent,".claude/settings.json",false) : ad_native_file(run,agent);
    if (!file) return -1;
    json_object *doc=ad_parse(run->km,file->text);
    if (!doc) return -1;
    int result=0;
    if (!strcmp(agent,"claude")) {
        json_object *value=km_get(km_get(doc,"skillOverrides"),id);
        const char *state=value && json_object_is_type(value,json_type_string) ? json_object_get_string(value) : NULL;
        if (value && (!state || (strcmp(state,"on") && strcmp(state,"off") && strcmp(state,"name-only") && strcmp(state,"user-invocable-only"))))
            result=km_error(run->km,"unsupported native Claude skill visibility; explicit migration required");
        else if (state && !strcmp(state,"off")) *enabled=false;
    } else {
        bool matched=false;
        result=permission_state(run->km,km_get(doc,"permissions"),id,enabled,&matched);
        json_object *agents=km_get(doc,"agents");
        if (!result && agents) {
            if (!json_object_is_type(agents,json_type_object)) result=km_error(run->km,"unsupported OpenCode agent configuration");
            else { json_object_object_foreach(agents,name,agent_config) {
                (void)name; bool state=*enabled, specific=false;
                if (!result) result=permission_state(run->km,km_get(agent_config,"permissions"),id,&state,&specific);
                if (!result && specific && state!=*enabled)
                    result=km_error(run->km,"OpenCode named-agent skill visibility conflicts; explicit migration required");
            } }
        }
    }
    json_object_put(doc);
    return result;
}

static void package_fields(json_object *resource, KmSkill *skill, const char *path) {
    json_object_object_add(resource,"path",json_object_new_string(path));
    json_object_object_add(resource,"source",json_object_new_string(skill->source));
    json_object_object_add(resource,"revision",json_object_new_string(skill->revision));
    json_object_object_add(resource,"hash",json_object_new_string(skill->hash));
    json_object_object_add(resource,"description",json_object_new_string(skill->description));
    json_object_object_add(resource,"compatibility",json_object_new_string(skill->compatibility));
    json_object_object_add(resource,"license",json_object_new_string(skill->license));
    json_object_object_add(resource,"subdir",json_object_new_string(""));
    json_object_object_add(resource,"ref",json_object_new_string(""));
    json_object_object_add(resource,"enabled",json_object_new_boolean(true));
    json_object_object_add(resource,"agents",json_object_new_object());
}

static int add_binding(Km *km, json_object *resource, const char *agent, const char *path,
                       const char *target, bool owned) {
    const char *field=owned ? "discovery" : "origins";
    json_object *all=object_at(resource,field), *bindings=km_get(all,agent);
    if (!bindings) { bindings=json_object_new_array(); json_object_object_add(all,agent,bindings); }
    if (!json_object_is_type(bindings,json_type_array)) return km_error(km,"invalid imported skill bindings");
    for (size_t i=0;i<json_object_array_length(bindings);i++) {
        json_object *entry=json_object_array_get_idx(bindings,i);
        const char *existing=km_string(entry,"path");
        if (!existing) return km_error(km,"invalid imported skill binding");
        if (!strcmp(existing,path)) return 0;
    }
    json_object *entry=json_object_new_object();
    json_object_object_add(entry,"path",json_object_new_string(path));
    json_object_object_add(entry,"target",json_object_new_string(target));
    json_object_array_add(bindings,entry);
    return 0;
}

/* Marks catalog entries created by the current import run (never serialized). */
static char created_marker;

static void warn_skipped(const char *agent, const char *path, const char *reason) {
    fprintf(stderr,"kitmux: warning: %s: skipped %s: %s\n",agent,path,reason);
}

static void drop_binding(json_object *resource, const char *field, const char *agent, const char *path) {
    json_object *bindings=km_get(km_get(resource,field),agent);
    if (!json_object_is_type(bindings,json_type_array)) return;
    for (size_t i=json_object_array_length(bindings);i>0;i--) {
        const char *bound=km_string(json_object_array_get_idx(bindings,i-1),"path");
        if (bound && !strcmp(bound,path)) json_object_array_del_idx(bindings,i-1,1);
    }
}

/* Refresh adopts a native replacement of an owned projection: Kitmux stops
 * owning that path and keeps its own package off for this client only. */
static void release_client(AdRun *run, const char *agent, const char *name, json_object *resource, const char *path) {
    ad_release_skill_link(run,path);
    drop_binding(resource,"discovery",agent,path);
    drop_binding(resource,"origins",agent,path);
    json_object_object_add(object_at(resource,"agents"),agent,json_object_new_string("off"));
    fprintf(stderr,"kitmux: warning: %s: %s now holds different native content; Kitmux released it and turned %s off for %s\n",
            agent,path,name,agent);
}

/* A link into Kitmux's own store for this skill is a (possibly stale) projection. */
static bool kitmux_revision(Km *km, const char *name, const char *root) {
    char store[PATH_MAX];
    if (km_path(km,store,sizeof(store),"%s/skills/%s/",km->data,name)<0) return false;
    return !strncmp(root,store,strlen(store));
}

/* Import records a direct assignment for a resource it creates. For existing
 * resources it writes one only when native visibility contradicts Kitmux's
 * effective availability, so group and inherited control survives re-import. */
static void record_availability(AdRun *run, const char *agent, const char *name, json_object *resource, bool native) {
    json_object *agents=object_at(resource,"agents");
    const char *override=km_string(agents,agent);
    bool created=json_object_get_userdata(resource)==&created_marker;
    if (created) {
        if (!override) json_object_object_add(agents,agent,json_object_new_string(native ? "on" : "off"));
        return;
    }
    json_object *global=km_get(resource,"enabled");
    if (global && !json_object_get_boolean(global)) return;
    bool effective=km_effective(run->km,"skill",name,agent,NULL,0);
    bool direct=override && strcmp(override,"inherit");
    if (native!=effective && (!direct || run->refresh))
        json_object_object_add(agents,agent,json_object_new_string(native ? "on" : "off"));
}

static int import_package(AdRun *run, const char *agent, const char *name,
                          const char *path, const char *target, bool owned) {
    KmSkill skill={0};
    if (km_skill_acquire(run->km,path,NULL,NULL,&skill)<0 || km_skill_inspect(run->km,&skill)<0) {
        char reason[sizeof(run->km->error)];
        snprintf(reason,sizeof(reason),"%s",run->km->error);
        run->km->error[0]=0;
        warn_skipped(agent,path,reason);
        return 0;
    }
    if (strcmp(name,skill.name)) {
        char reason[256];
        snprintf(reason,sizeof(reason),"SKILL.md names \"%s\"; rename the entry to %s or fix the name, then retry the import",skill.name,skill.name);
        warn_skipped(agent,path,reason);
        return 0;
    }
    json_object *resources=km_resources(run->km,"skill"), *resource=km_get(resources,name);
    if (resource && ad_skill_bindings_valid(run->km,resource,name)<0) return -1;
    const char *hash=km_string(resource,"hash");
    if (resource && (!hash || strcmp(hash,skill.hash)) && !kitmux_revision(run->km,name,skill.root)) {
        if (owned && run->refresh && ad_skill_link_owned(run,path)) { release_client(run,agent,name,resource,path); return 0; }
        return km_error(run->km,"%s: skill import conflict: %s at %s differs from the installed %s package; run `kitmux skill update %s --source %s --apply` to adopt it, or move it aside (sources preserved)",
                        agent,path,skill.source,name,name,skill.source);
    }
    bool enabled=true;
    if (ad_skill_native_enabled(run,agent,name,path,&enabled)<0) return -1;
    if (!resource) {
        char installed[PATH_MAX];
        int result=run->apply ? km_skill_store(run->km,&skill,installed,sizeof(installed)) :
            km_path(run->km,installed,sizeof(installed),"%s/skills/%s/%s",run->km->data,name,skill.hash);
        if (result<0) return -1;
        resource=json_object_new_object();
        package_fields(resource,&skill,installed);
        json_object_set_userdata(resource,&created_marker,NULL);
        json_object_object_add(resources,name,resource);
    }
    if (add_binding(run->km,resource,agent,path,target,false)<0 ||
        (owned && (add_binding(run->km,resource,agent,path,target,true)<0 || ad_adopt_skill_link(run,agent,path,target)<0))) return -1;
    record_availability(run,agent,name,resource,enabled);
    printf("%s: %s skill %s (%s)\n",run->apply ? "import" : "preview",agent,name,enabled ? "available" : "native off");
    return 0;
}

/* Real directories in owned roots stay unmanaged; refresh releases a Kitmux
 * projection that a user replaced with one. */
static void skip_directory(AdRun *run, const char *agent, const char *name, const char *path) {
    json_object *resource=km_get(km_resources(run->km,"skill"),name);
    if (run->refresh && resource && ad_skill_link_owned(run,path)) { release_client(run,agent,name,resource,path); return; }
    char reason[3*PATH_MAX];
    snprintf(reason,sizeof(reason),"real skill directory; Kitmux manages only linked packages here. To manage it, run `kitmux skill add %s`, move %s aside, and run `kitmux skill enable %s --agent %s`",
             path,path,name,agent);
    warn_skipped(agent,path,reason);
}

static int scan_entry(AdRun *run, const char *agent, const char *root, const char *name, bool owned) {
    char path[PATH_MAX], metadata[PATH_MAX], target[PATH_MAX]; struct stat st;
    if (km_path(run->km,path,sizeof(path),"%s/%s",root,name)<0 ||
        km_path(run->km,metadata,sizeof(metadata),"%s/SKILL.md",path)<0) return -1;
    if (lstat(path,&st)<0) { warn_skipped(agent,path,"it changed during discovery; retry the import"); return 0; }
    if (!S_ISLNK(st.st_mode) && !S_ISDIR(st.st_mode)) {
        warn_skipped(agent,path,"not a skill package directory; move flat skill files into NAME/SKILL.md packages");
        return 0;
    }
    bool link=S_ISLNK(st.st_mode);
    if (!km_valid_id(name)) { warn_skipped(agent,path,"the entry name is not a valid skill ID"); return 0; }
    if (stat(metadata,&st)<0 || !S_ISREG(st.st_mode)) {
        warn_skipped(agent,path,link ? "the link is dangling or its target has no SKILL.md" : "no SKILL.md (nested collections need explicit migration)");
        return 0;
    }
    if (owned && !link) { skip_directory(run,agent,name,path); return 0; }
    if (link) {
        ssize_t size=readlink(path,target,sizeof(target)-1);
        if (size<0 || (size_t)size>=sizeof(target)-1) { warn_skipped(agent,path,"cannot read the link"); return 0; }
        target[size]=0;
    } else if (km_path(run->km,target,sizeof(target),"%s",path)<0) return -1;
    return import_package(run,agent,name,path,target,owned);
}

static int scan_root(AdRun *run, const char *agent, const char *relative, bool owned) {
    char root[PATH_MAX]; struct stat st;
    if (km_path(run->km,root,sizeof(root),"%s/%s",run->km->home,relative)<0) return -1;
    if (lstat(root,&st)<0) return errno==ENOENT ? 0 : km_error(run->km,"%s: cannot inspect skill discovery root %s: %s",agent,root,strerror(errno));
    if (!S_ISDIR(st.st_mode))
        return km_error(run->km,"%s: skill discovery root %s is a symlink or file; replace it with a real directory of skill links, then retry `kitmux import --from %s`",agent,root,agent);
    DIR *directory=opendir(root);
    if (!directory) return km_error(run->km,"%s: cannot read skill discovery root %s: %s",agent,root,strerror(errno));
    int result=0; size_t count=0; struct dirent *entry;
    while (!result && (entry=readdir(directory))) {
        const char *name=entry->d_name;
        if (name[0]=='.' || (!strcmp(relative,".claude/skills") && !strcmp(name,"synced"))) continue;
        if (++count>4096) { result=km_error(run->km,"%s: skill discovery root %s exceeds 4096 entries",agent,root); break; }
        result=scan_entry(run,agent,root,name,owned);
    }
    closedir(directory);
    return result;
}

int ad_import_skills(AdRun *run, const char *agent) {
    if (!strcmp(agent,"codex")) {
        if (scan_root(run,agent,".codex/skills",true)<0) return -1;
        return scan_root(run,agent,".agents/skills",true);
    }
    if (!strcmp(agent,"claude")) return scan_root(run,agent,".claude/skills",true);
    if (!strcmp(agent,"antigravity"))
        return scan_root(run,agent,".gemini/config/skills",true)<0 ? -1 : scan_root(run,agent,".agents/skills",false);
    if (scan_root(run,agent,".config/opencode/skills",true)<0 ||
        scan_root(run,agent,".claude/skills",false)<0) return -1;
    return scan_root(run,agent,".agents/skills",false);
}
