#include "adapters_skills.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *links(AdRun *run) {
    json_object *value=km_get(run->state,"links");
    if (!value) { value=json_object_new_object(); json_object_object_add(run->state,"links",value); }
    return value;
}

static int real_parents(Km *km, const char *agent, const char *path) {
    char copy[PATH_MAX];
    if (km_path(km,copy,sizeof(copy),"%s",path)<0) return -1;
    for (char *p=copy+1;*p;p++) {
        if (*p!='/') continue;
        *p=0; struct stat st;
        if (lstat(copy,&st)==0) {
            if (!S_ISDIR(st.st_mode))
                return km_error(km,"%s: skill discovery directory %s is a symlink or file, which prevents client isolation; replace it with a real directory, then run `kitmux sync --apply --agent %s`",agent,copy,agent);
        } else if (errno!=ENOENT) return km_error(km,"%s: cannot inspect skill discovery directory %s: %s",agent,copy,strerror(errno));
        *p='/';
    }
    return 0;
}

static bool projection_path(Km *km, const char *agent, const char *path, const char *id) {
    const char *roots[3]={NULL,NULL,NULL}; char expected[PATH_MAX];
    if (!strcmp(agent,"codex")) { roots[0]=".agents/skills"; roots[1]=".codex/skills"; }
    else roots[0]=!strcmp(agent,"claude") ? ".claude/skills" : ".config/opencode/skills";
    if (!km_valid_agent(agent) || !km_valid_id(id)) return false;
    for (size_t i=0;roots[i];i++) {
        if (km_path(km,expected,sizeof(expected),"%s/%s/%s",km->home,roots[i],id)<0) return false;
        if (!strcmp(expected,path)) return true;
    }
    return false;
}

typedef struct { bool exists, symlink; char target[PATH_MAX]; } LinkState;

static int read_link(Km *km, const char *agent, const char *path, LinkState *state) {
    memset(state,0,sizeof(*state));
    struct stat st;
    if (lstat(path,&st)<0) return errno==ENOENT ? 0 : km_error(km,"%s: cannot inspect skill projection %s: %s",agent,path,strerror(errno));
    state->exists=true;
    if (!S_ISLNK(st.st_mode)) return 0;
    ssize_t n=readlink(path,state->target,sizeof(state->target)-1);
    if (n<0 || (size_t)n>=sizeof(state->target)-1) return km_error(km,"%s: cannot read skill projection %s",agent,path);
    state->target[n]=0; state->symlink=true; return 0;
}

static bool link_is(const LinkState *state, const char *target) {
    return target ? state->symlink && !strcmp(state->target,target) : !state->exists;
}

static int changed_link(Km *km, const char *agent, const char *path, const LinkState *now) {
    return km_error(km,"%s: skill projection %s was changed outside Kitmux (now %s%s); run `kitmux import --refresh --from %s --apply` to adopt that change, or restore the link and run `kitmux sync --apply --agent %s`",
                    agent,path,now->symlink ? "a link to " : "a real file or directory",now->symlink ? now->target : "",agent,agent);
}

static int unowned_link(Km *km, const char *agent, const char *path, const LinkState *now) {
    if (now->symlink)
        return km_error(km,"%s: %s already links to %s outside Kitmux; adopt it with `kitmux import --from %s --apply` or move it aside, then run `kitmux sync --apply --agent %s`",agent,path,now->target,agent,agent);
    return km_error(km,"%s: %s is an unmanaged skill directory or file; install it with `kitmux skill add %s` and move the original aside, then run `kitmux sync --apply --agent %s`",agent,path,path,agent);
}

/* Reconcile one owned projection. Deleted links are recreated or left absent;
 * only different content at an owned path, or unowned content where Kitmux
 * needs a projection, is a conflict. Unwanted unowned content is left alone. */
static int link_change(AdRun *run, const char *agent, const char *path, const char *target, bool adopt) {
    const char *id=strrchr(path,'/');
    if (!id || !projection_path(run->km,agent,path,id+1)) return km_error(run->km,"%s: unsafe skill projection path %s",agent,path);
    if (real_parents(run->km,agent,path)<0) return -1;
    LinkState now;
    if (read_link(run->km,agent,path,&now)<0) return -1;
    json_object *record=km_get(links(run),path);
    if (record) {
        bool pending=json_object_get_boolean(km_get(run->state,"pending"));
        bool expected=link_is(&now,km_string(record,"after"));
        bool prior=pending && link_is(&now,km_string(record,"previous"));
        bool adopted=adopt && run->refresh && now.symlink;
        if (now.exists && !expected && !prior && !adopted) return target ? changed_link(run->km,agent,path,&now) : 0;
    } else {
        if (!target) return 0;
        if (now.exists && !(adopt && now.symlink) && !link_is(&now,target)) return unowned_link(run->km,agent,path,&now);
        record=json_object_new_object();
        json_object_object_add(record,"agent",json_object_new_string(agent));
        if (now.symlink) json_object_object_add(record,"before",json_object_new_string(now.target));
        json_object_object_add(links(run),path,record);
    }
    const char *current=now.symlink ? now.target : NULL;
    json_object_object_del(record,"previous"); json_object_object_del(record,"after");
    if (current) json_object_object_add(record,"previous",json_object_new_string(current));
    if (target) json_object_object_add(record,"after",json_object_new_string(target));
    if (link_is(&now,target)) return 0;
    json_object *action=json_object_new_object();
    json_object_object_add(action,"path",json_object_new_string(path));
    if (current) json_object_object_add(action,"before",json_object_new_string(current));
    if (target) json_object_object_add(action,"target",json_object_new_string(target));
    json_object_array_add(run->links,action); return 0;
}

int ad_adopt_skill_link(AdRun *run, const char *agent, const char *path, const char *target) {
    LinkState now;
    if (read_link(run->km,agent,path,&now)<0) return -1;
    if (!link_is(&now,target)) return km_error(run->km,"%s: native skill alias %s changed during import; retry the import",agent,path);
    return link_change(run,agent,path,target,true);
}

/* Refresh adopts a native replacement by releasing Kitmux ownership of that path. */
void ad_release_skill_link(AdRun *run, const char *path) {
    json_object_object_del(links(run),path);
}

static const char *skill_root(const char *agent) {
    return !strcmp(agent,"codex") ? ".agents/skills" : !strcmp(agent,"claude") ? ".claude/skills" : ".config/opencode/skills";
}

static bool binding_text(json_object *entry, const char *key) {
    json_object *value=km_get(entry,key);
    const char *text=km_string(entry,key);
    return text && *text && strlen(text)==(size_t)json_object_get_string_len(value);
}

static bool origin_path(Km *km, const char *agent, const char *path, const char *id) {
    if (projection_path(km,agent,path,id)) return true;
    if (strcmp(agent,"opencode")) return false;
    if (projection_path(km,"claude",path,id)) return true;
    char shared[PATH_MAX];
    return km_path(km,shared,sizeof(shared),"%s/.agents/skills/%s",km->home,id)==0 && !strcmp(shared,path);
}

int ad_skill_bindings_valid(Km *km, json_object *skill, const char *id) {
    const char *fields[]={"discovery","origins"};
    for (size_t f=0;f<2;f++) {
        json_object *all=km_get(skill,fields[f]);
        if (!all) continue;
        if (!json_object_is_type(all,json_type_object)) return km_error(km,"invalid imported skill bindings");
        json_object_object_foreach(all,agent,bindings) {
            if (!km_valid_agent(agent) || !json_object_is_type(bindings,json_type_array) || json_object_array_length(bindings)>4)
                return km_error(km,"invalid imported skill binding client or count");
            for (size_t i=0;i<json_object_array_length(bindings);i++) {
                json_object *entry=json_object_array_get_idx(bindings,i);
                const char *path=km_string(entry,"path");
                if (!binding_text(entry,"path") || !binding_text(entry,"target") ||
                    !(f ? origin_path(km,agent,path,id) : projection_path(km,agent,path,id)))
                    return km_error(km,"unsafe imported skill discovery binding");
                for (size_t prior=0;prior<i;prior++) {
                    const char *previous=km_string(json_object_array_get_idx(bindings,prior),"path");
                    if (!strcmp(previous,path)) return km_error(km,"duplicate imported skill discovery binding");
                }
            }
        }
    }
    return 0;
}

static int project_path(AdRun *run, const char *agent, const char *id, json_object *skill, const char *path) {
    char reason[256];
    if (!projection_path(run->km,agent,path,id)) return km_error(run->km,"%s: unsafe imported skill discovery binding %s for %s",agent,path,id);
    bool enabled=km_effective(run->km,"skill",id,agent,reason,sizeof(reason));
    const char *target=enabled ? km_string(skill,"path") : NULL;
    if (enabled) {
        struct stat st;
        if (!target || stat(target,&st)<0 || !S_ISDIR(st.st_mode))
            return km_error(run->km,"%s: installed package for skill %s is missing at %s; run `kitmux skill update %s --apply` or `kitmux skill remove %s`",
                            agent,id,target ? target : "(no path)",id,id);
        if (!strcmp(agent,"codex") && ad_codex_skill_allowed(run,id,path)<0) return -1;
    }
    return link_change(run,agent,path,target,false);
}

static int project(AdRun *run, const char *agent, const char *id, json_object *skill) {
    if (ad_skill_bindings_valid(run->km,skill,id)<0) return -1;
    json_object *bindings=km_get(km_get(skill,"discovery"),agent);
    if (bindings) {
        if (!json_object_is_type(bindings,json_type_array)) return km_error(run->km,"invalid imported skill discovery bindings");
        if (json_object_array_length(bindings)) {
            for (size_t i=0;i<json_object_array_length(bindings);i++) {
                const char *path=km_string(json_object_array_get_idx(bindings,i),"path");
                if (!path || project_path(run,agent,id,skill,path)<0) return -1;
            }
            return 0;
        }
    }
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s/%s",run->km->home,skill_root(agent),id)<0) return -1;
    return project_path(run,agent,id,skill,path);
}

bool ad_skill_link_owned(AdRun *run, const char *path) {
    json_object *record=km_get(km_get(run->state,"links"),path);
    return record && (km_get(record,"after") || km_get(record,"before") || km_get(record,"previous"));
}

static bool claude_managed(AdRun *run, const char *id) {
    char path[PATH_MAX];
    return km_path(run->km,path,sizeof(path),"%s/.claude/skills/%s",run->km->home,id)==0 && ad_skill_link_owned(run,path);
}

static bool binding_at(json_object *skill, const char *field, const char *agent, const char *path) {
    json_object *bindings=km_get(km_get(skill,field),agent);
    if (!json_object_is_type(bindings,json_type_array)) return false;
    for (size_t i=0;i<json_object_array_length(bindings);i++) {
        const char *bound=km_string(json_object_array_get_idx(bindings,i),"path");
        if (bound && !strcmp(bound,path)) return true;
    }
    return false;
}

static bool codex_shared_projection(json_object *skill, const char *shared) {
    json_object *bindings=km_get(km_get(skill,"discovery"),"codex");
    bool defaults=!json_object_is_type(bindings,json_type_array) || !json_object_array_length(bindings);
    return defaults || binding_at(skill,"discovery","codex",shared);
}

/* OpenCode also reads ~/.claude/skills and ~/.agents/skills. A Kitmux skill is
 * visible there when Kitmux projects it for Claude/Codex, still owns a link
 * there, or imported it for OpenCode from that shared location. */
bool ad_skill_shared_visible(AdRun *run, const char *id, json_object *skill) {
    char claude[PATH_MAX], shared[PATH_MAX];
    if (km_path(run->km,claude,sizeof(claude),"%s/.claude/skills/%s",run->km->home,id)<0 ||
        km_path(run->km,shared,sizeof(shared),"%s/.agents/skills/%s",run->km->home,id)<0) return true;
    if (km_effective(run->km,"skill",id,"claude",NULL,0)) return true;
    if (km_effective(run->km,"skill",id,"codex",NULL,0) && codex_shared_projection(skill,shared)) return true;
    const char *paths[]={claude,shared};
    for (size_t i=0;i<2;i++) {
        if (km_get(km_get(km_get(run->state,"links"),paths[i]),"after")) return true;
        if (binding_at(skill,"origins","opencode",paths[i])) return true;
    }
    return false;
}

static int claude_overrides(AdRun *run) {
    AdFile *file=ad_file(run,"claude",".claude/settings.json",false);
    if (!file) return -1;
    json_object_object_foreach(km_resources(run->km,"skill"),id,skill) {
        (void)skill; char key[256],reason[256]; snprintf(key,sizeof(key),"skillOverrides/%s",id);
        bool enabled=km_effective(run->km,"skill",id,"claude",reason,sizeof(reason));
        json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
        char *current=ad_json_raw(run->km,file->text,key);
        const char *before=record ? km_string(record,"before") : current;
        json_object *original=before ? ad_parse(run->km,before) : NULL;
        bool was_off=original && json_object_is_type(original,json_type_string) && !strcmp(json_object_get_string(original),"off");
        /* "off" is keyed by name and also hides plugin/project skills, so only
         * write it where Kitmux manages (or managed) the Claude projection. */
        const char *desired=enabled ? (was_off ? NULL : before) : claude_managed(run,id) ? "\"off\"" : before;
        int result=ad_change(run,file,key,desired,true);
        json_object_put(original); free(current);
        if (result<0) return -1;
    }
    json_object *record=km_get(km_get(run->state,"files"),file->path), *entries=km_get(record,"entries");
    if (entries) {
        json_object_object_foreach(entries,key,entry) {
            const char *id=strrchr(key,'/');
            if (id && !km_get(km_resources(run->km,"skill"),id+1)) {
                const char *before=km_string(entry,"before"); char *copy=before ? strdup(before) : NULL;
                int result=ad_change(run,file,key,copy,true); free(copy); if (result<0) return -1;
            }
        }
    }
    return 0;
}

int ad_sync_skills(AdRun *run, const char *agent) {
    json_object_object_foreach(km_resources(run->km,"skill"),id,skill)
        if (project(run,agent,id,skill)<0) return -1;
    json_object *records=links(run);
    json_object_object_foreach(records,path,record) {
        if (strcmp(km_string(record,"agent"),agent)) continue;
        const char *id=strrchr(path,'/');
        if (id && !km_get(km_resources(run->km,"skill"),id+1) && link_change(run,agent,path,NULL,false)<0) return -1;
    }
    if (!strcmp(agent,"claude")) return claude_overrides(run);
    if (!strcmp(agent,"opencode")) return ad_sync_skill_rules(run);
    return 0;
}

int ad_restore_links(AdRun *run, const char *agent) {
    json_object *records=links(run);
    json_object_object_foreach(records,path,record) {
        const char *owner=km_string(record,"agent");
        if (agent && strcmp(agent,owner)) continue;
        if (strncmp(path,run->km->home,strlen(run->km->home)) || path[strlen(run->km->home)]!='/')
            return km_error(run->km,"%s: skill ownership record %s points outside the selected home",owner,path);
        const char *before=km_string(record,"before");
        if (link_change(run,owner,path,before,false)<0) return -1;
    }
    return !agent || !strcmp(agent,"opencode") ? ad_restore_skill_rules(run) : 0;
}
