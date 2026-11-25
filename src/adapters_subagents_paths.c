#include "adapters.h"
#include "subagents.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *agent_root(const char *agent) {
    if (!strcmp(agent,"codex")) return ".codex/agents";
    if (!strcmp(agent,"claude")) return ".claude/agents";
    if (!strcmp(agent,"opencode")) return ".config/opencode/agents";
    if (!strcmp(agent,"antigravity")) return ".gemini/config/agents";
    return NULL;
}

/* Antigravity also discovers <root>/<id>/agent.md, one level deep. */
int ad_subagent_folder_path(Km *km, char *path, const char *id) {
    return km_path(km,path,PATH_MAX,"%s/.gemini/config/agents/%s/agent.md",km->home,id);
}

bool ad_subagent_path_valid(Km *km, const char *path, const char *agent, const char *id) {
    const char *root=agent_root(agent); char expected[PATH_MAX];
    if (!root || !path || !km_valid_id(id)) return false;
    if (km_path(km,expected,sizeof(expected),"%s/%s/%s.%s",km->home,root,id,
                !strcmp(agent,"codex") ? "toml" : "md")<0) return false;
    if (!strcmp(path,expected)) return true;
    return !strcmp(agent,"antigravity") && ad_subagent_folder_path(km,expected,id)==0 && !strcmp(path,expected);
}

static bool config_origin(Km *km, const char *path, const char *key, const char *id) {
    char expected[PATH_MAX], entry[80];
    if (!path || !key || km_path(km,entry,sizeof(entry),"agents/%s",id)<0 || strcmp(key,entry)) return false;
    const char *names[]={"opencode.json","opencode.jsonc"};
    for (size_t i=0;i<2;i++) {
        if (km_path(km,expected,sizeof(expected),"%s/.config/opencode/%s",km->home,names[i])<0) return false;
        if (!strcmp(path,expected)) return true;
    }
    return false;
}

static bool text_field(json_object *object, const char *key, bool required) {
    json_object *value=NULL;
    if (!json_object_object_get_ex(object,key,&value)) return !required;
    const char *text=km_string(object,key);
    return text && strlen(text)==(size_t)json_object_get_string_len(value);
}

int ad_subagent_origins_valid(Km *km, json_object *resource, const char *id) {
    json_object *origins=NULL;
    if (!json_object_object_get_ex(resource,"origins",&origins)) return 0;
    if (!json_object_is_type(origins,json_type_object)) goto invalid;
    json_object_object_foreach(origins,agent,origin) {
        if (!km_valid_agent(agent) || !json_object_is_type(origin,json_type_object) ||
            !text_field(origin,"path",true) || !text_field(origin,"key",false)) goto invalid;
        const char *path=km_string(origin,"path"), *key=km_string(origin,"key");
        if (key ? (strcmp(agent,"opencode") || !config_origin(km,path,key,id)) :
                  !ad_subagent_path_valid(km,path,agent,id)) goto invalid;
        const char *format=km_string(km_get(km_get(resource,"native"),agent),"format");
        if (!format) goto invalid;
        if (strcmp(format,key ? "opencode-json" : agent))
            return km_error(km,"Native %s format of subagent %s conflicts with its discovery location %s; run `kitmux restore --agent %s --apply` and reimport before changing format",agent,id,path,agent);
    }
    return 0;
invalid:
    return km_error(km,"Invalid or unsafe native subagent origin for %s",id);
}

int ad_subagent_parent_safe(Km *km, const char *path) {
    char parent[PATH_MAX]; struct stat st;
    if (km_path(km,parent,sizeof(parent),"%s",path)<0) return -1;
    for (char *p=parent+1;*p;p++) {
        if (*p!='/') continue;
        *p=0;
        int result=lstat(parent,&st);
        *p='/';
        if (result<0 && errno==ENOENT) return 0;
        if (result<0 || !S_ISDIR(st.st_mode))
            return km_error(km,"Unsafe subagent discovery directory %s: expected a real directory, not a symlink or file. "
                            "Kitmux skips subagent writes for this client until it is a real directory",parent);
    }
    return 0;
}

static int flat_collection(Km *km, const char *agent, const char *relative, bool legacy, bool folders) {
    char root[PATH_MAX], path[PATH_MAX]; struct stat st;
    if (km_path(km,root,sizeof(root),"%s/%s",km->home,relative)<0 ||
        km_path(km,path,sizeof(path),"%s/validation",root)<0 || ad_subagent_parent_safe(km,path)<0) return -1;
    if (lstat(root,&st)<0) return errno==ENOENT ? 0 : km_error(km,"Cannot inspect native subagent collection %s",root);
    if (!S_ISDIR(st.st_mode)) return km_error(km,"Native subagent discovery %s must be a real directory",root);
    DIR *dir=opendir(root);
    if (!dir) return km_error(km,"Cannot read native subagent collection %s",root);
    int result=0; struct dirent *entry;
    while ((entry=readdir(dir))) {
        /* Dot entries (.git, .DS_Store) are not client definitions. */
        if (entry->d_name[0]=='.') continue;
        if (km_path(km,path,sizeof(path),"%s/%s",root,entry->d_name)<0) { result=-1; break; }
        if (lstat(path,&st)<0) { result=km_error(km,"Native subagent collection %s changed; retry",root); break; }
        if (folders && S_ISDIR(st.st_mode)) continue;
        if (legacy || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            result=km_error(km,"Unsupported nested, linked, or legacy %s subagent collection at %s; migrate profiles into "
                            "flat regular files under ~/%s before import or sync for %s",agent,path,agent_root(agent),agent);
            break;
        }
    }
    closedir(dir); return result;
}

int ad_subagent_flat_discovery(Km *km, const char *agent) {
    if (flat_collection(km,agent,agent_root(agent),false,!strcmp(agent,"antigravity"))<0) return -1;
    return !strcmp(agent,"opencode") ? flat_collection(km,agent,".config/opencode/agent",true,false) : 0;
}

/* Only "before" is ever written back (by restore), so it must parse. "after" and
 * "previous" are comparison baselines; earlier builds could record rendered texts
 * that their own parser rejected, and refusing them would brick every sync. */
static int record_valid(Km *km, const char *path, json_object *record) {
    if (!json_object_is_type(record,json_type_object) || !text_field(record,"agent",true) ||
        !text_field(record,"id",true) || !text_field(record,"format",true))
        return km_error(km,"Invalid subagent ownership record for %s: agent, id and format must be strings",path);
    const char *agent=km_string(record,"agent"), *id=km_string(record,"id"), *format=km_string(record,"format");
    if (!km_valid_agent(agent) || strcmp(agent,format) || !ad_subagent_path_valid(km,path,agent,id))
        return km_error(km,"Unsafe subagent ownership record for %s: it does not match %s's discovery path for %s",path,agent,id);
    const char *fields[]={"before","after","previous"};
    for (size_t i=0;i<3;i++)
        if (!text_field(record,fields[i],false))
            return km_error(km,"Invalid subagent ownership record for %s: field %s must be text",path,fields[i]);
    const char *before=km_string(record,"before");
    json_object *parsed=before ? km_subagent_parse(km,before,format,id) : NULL;
    if (before && !parsed)
        return km_error(km,"Subagent ownership record for %s has a restore baseline (field before) that is not a valid %s "
                        "definition, so Kitmux will not restore it; inspect that record in %s/adapters.json",path,agent,km->state);
    json_object_put(parsed);
    return 0;
}

int ad_subagent_state_valid(Km *km, json_object *state) {
    json_object *records=NULL;
    if (!json_object_object_get_ex(state,"subagent_files",&records)) return 0;
    if (json_object_get_int(km_get(state,"version"))!=2 || !json_object_is_type(records,json_type_object))
        return km_error(km,"Invalid subagent ownership journal: subagent_files needs state version 2");
    json_object_object_foreach(records,path,record)
        if (record_valid(km,path,record)<0) return -1;
    return 0;
}
