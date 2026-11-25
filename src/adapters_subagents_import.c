#include "adapters.h"
#include "subagents.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *value=km_get(parent,key);
    if (!value) { value=json_object_new_object(); json_object_object_add(parent,key,value); }
    return value;
}

static int origin_conflict(Km *km, json_object *resource, const char *agent, const char *id,
                           const char *path, const char *key) {
    json_object *old=km_get(km_get(resource,"origins"),agent);
    if (!old || (!strcmp(km_string(old,"path"),path) && !!km_string(old,"key")==!!key &&
        (!key || !strcmp(km_string(old,"key"),key)))) return 0;
    return km_error(km,"Multiple native %s definitions for %s: %s%s%s and %s%s%s; keep one of them, then rerun the import",
                    agent,id,km_string(old,"path"),km_string(old,"key") ? " " : "",km_string(old,"key") ? km_string(old,"key") : "",
                    path,key ? " " : "",key ? key : "");
}

static void set_origin(json_object *resource, const char *agent, const char *path, const char *key) {
    json_object *origin=json_object_new_object();
    json_object_object_add(origin,"path",json_object_new_string(path));
    if (key) json_object_object_add(origin,"key",json_object_new_string(key));
    json_object_object_add(object_at(resource,"origins"),agent,origin);
}

static json_object *owned(AdRun *run, const char *path, const char *key) {
    if (!key) return km_get(km_get(run->state,"subagent_files"),path);
    return km_get(km_get(km_get(km_get(run->state,"files"),path),"entries"),key);
}

/* Unusable native files are reported and skipped so one stray file cannot hide
 * every other definition; I/O, safety and ownership failures still stop. */
static int skip(AdRun *run, const char *path, const char *key) {
    printf("import: skipped %s%s%s: %s\n",path,key ? " " : "",key ? key : "",run->km->error);
    run->km->error[0]='\0';
    return 0;
}

static bool native_on(json_object *resource, const char *agent) {
    return !json_object_get_boolean(km_get(km_get(km_get(resource,"native"),agent),"disabled"));
}

static int attach(AdRun *run, json_object *resource, const char *agent, const char *id,
                  json_object *parsed, const char *text, bool managed) {
    json_object *variant=km_get(km_get(parsed,"native"),agent), *existing=km_get(km_get(resource,"native"),agent);
    if (!managed && existing && strcmp(km_string(existing,"text"),text))
        return km_error(run->km,"Subagent import conflicts with the existing %s variant of %s; edit that variant with "
                        "`kitmux subagent edit %s --from FILE --format %s` or rename the native definition",agent,id,id,agent);
    if (managed && existing && !run->refresh) return 0;
    json_object_object_add(variant,"description",json_object_new_string(km_string(resource,"description")));
    json_object_object_add(variant,"instructions",json_object_new_string(km_string(resource,"instructions")));
    json_object_object_add(object_at(resource,"native"),agent,json_object_get(variant));
    return 0;
}

/* A new resource takes the native availability directly. Otherwise inherited or
 * group availability stays untouched unless the native state contradicts it.
 * Returns 1 when the definition cannot render; the catalog is left unchanged. */
static int merge_definition(AdRun *run, const char *agent, const char *id, json_object *parsed,
                            const char *text, const char *path, const char *key) {
    json_object *resources=km_resources(run->km,"subagent"), *resource=km_get(resources,id), *backup=NULL;
    bool created=!resource, managed=owned(run,path,key)!=NULL;
    if (!created && origin_conflict(run->km,resource,agent,id,path,key)<0) return -1;
    if (!created && json_object_deep_copy(resource,&backup,NULL)) return km_error(run->km,"out of memory");
    if (created) { resource=json_object_get(parsed); json_object_object_add(resources,id,resource); }
    else if (attach(run,resource,agent,id,parsed,text,managed)<0) { json_object_put(backup); return -1; }
    if (km_subagent_check(run->km,id,resource,NULL)<0) {
        if (created) json_object_object_del(resources,id);
        else json_object_object_add(resources,id,backup);
        return 1;
    }
    json_object_put(backup);
    set_origin(resource,agent,path,key);
    bool on=native_on(resource,agent);
    if (created || km_effective(run->km,"subagent",id,agent,NULL,0)!=on)
        json_object_object_add(object_at(resource,"agents"),agent,json_object_new_string(on ? "on" : "off"));
    return 0;
}

static json_object *parse_definition(AdRun *run, const char *text, const char *format, const char *id) {
    if (!strncmp(text,"\xEF\xBB\xBF",3)) {
        km_error(run->km,"starts with a UTF-8 byte-order mark; save it without one to import it"); return NULL;
    }
    if (!km_get(km_resources(run->km,"subagent"),id) && km_subagent_case_twin(run->km,id)) {
        km_error(run->km,"its ID differs from existing subagent %s only by letter case",km_subagent_case_twin(run->km,id));
        return NULL;
    }
    return km_subagent_parse(run->km,text,format,id);
}

static int import_text(AdRun *run, const char *agent, const char *id, const char *path, const char *text) {
    if (!strcmp(agent,"opencode")) {
        char *mode=km_subagent_markdown_mode(run->km,text);
        if (!mode) return skip(run,path,NULL);
        bool subagent=!strcmp(mode,"subagent"); free(mode);
        if (!subagent) { printf("import: opencode primary/all agent skipped (subagents only): %s\n",path); return 0; }
    }
    json_object *parsed=parse_definition(run,text,agent,id);
    int result=parsed ? merge_definition(run,agent,id,parsed,text,path,NULL) : 1;
    json_object_put(parsed);
    if (result>0) return skip(run,path,NULL);
    if (!result) result=ad_subagent_change(run,agent,path,id,agent,text,true);
    if (!result) printf("import: %s subagent %s (%s)\n",agent,id,km_effective(run->km,"subagent",id,agent,NULL,0) ? "available" : "disabled");
    return result;
}

static int import_file(AdRun *run, const char *agent, const char *id, const char *path) {
    size_t length; char *text=km_read_file(run->km,path,&length);
    if (!text) return -1;
    int result;
    if (strlen(text)==length) result=import_text(run,agent,id,path,text);
    else { km_error(run->km,"contains NUL bytes"); result=skip(run,path,NULL); }
    free(text); return result;
}

static int import_entry(AdRun *run, const char *root, const char *name, const char *suffix, const char *agent) {
    size_t length=strlen(name), extension=strlen(suffix);
    if (name[0]=='.' || length<=extension || strcmp(name+length-extension,suffix)) return 0;
    char id[65], path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s",root,name)<0) return -1;
    if (length-extension>=sizeof(id)) {
        km_error(run->km,"the file name must be a Kitmux ID of at most 64 characters"); return skip(run,path,NULL);
    }
    memcpy(id,name,length-extension); id[length-extension]=0;
    if (!km_valid_id(id)) {
        km_error(run->km,"the file name must be a Kitmux ID (letters, digits, hyphens, underscores)"); return skip(run,path,NULL);
    }
    return import_file(run,agent,id,path);
}

static int scan_directory(AdRun *run, const char *agent) {
    const char *relative=!strcmp(agent,"codex") ? ".codex/agents" : !strcmp(agent,"claude") ? ".claude/agents" : ".config/opencode/agents";
    const char *suffix=!strcmp(agent,"codex") ? ".toml" : ".md";
    char root[PATH_MAX], sample[PATH_MAX]; struct stat st;
    if (km_path(run->km,root,sizeof(root),"%s/%s",run->km->home,relative)<0 ||
        km_path(run->km,sample,sizeof(sample),"%s/validation",root)<0 || ad_subagent_parent_safe(run->km,sample)<0) return -1;
    if (lstat(root,&st)<0) return errno==ENOENT ? 0 : km_error(run->km,"Cannot inspect native subagent directory %s",root);
    if (!S_ISDIR(st.st_mode)) return km_error(run->km,"Native subagent discovery %s must be a real directory",root);
    DIR *dir=opendir(root);
    if (!dir) return km_error(run->km,"Cannot read native subagent discovery directory %s",root);
    int result=0; struct dirent *entry;
    while (!result && (entry=readdir(dir))) result=import_entry(run,root,entry->d_name,suffix,agent);
    closedir(dir); return result;
}

static int import_json_entry(AdRun *run, AdFile *file, const char *id, json_object *definition) {
    char key[80];
    if (km_path(run->km,key,sizeof(key),"agents/%s",id)<0) return -1;
    if (!km_valid_id(id) || !json_object_is_type(definition,json_type_object)) {
        km_error(run->km,"agent names must be Kitmux IDs (letters, digits, hyphens, underscores)");
        return skip(run,file->path,key);
    }
    char *raw=ad_json_raw(run->km,file->text,key);
    if (!raw) return -1;
    json_object *parsed=parse_definition(run,raw,"opencode-json",id);
    int result=parsed ? merge_definition(run,"opencode",id,parsed,raw,file->path,key) : 1;
    json_object_put(parsed);
    if (!result) result=ad_change(run,file,key,raw,true);
    free(raw);
    if (result>0) return skip(run,file->path,key);
    if (result<0) return result;
    json_object_object_add(run->state,"version",json_object_new_int(2));
    printf("import: opencode subagent %s (%s)\n",id,km_effective(run->km,"subagent",id,"opencode",NULL,0) ? "available" : "disabled");
    return 0;
}

static int import_config(AdRun *run) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        ad_subagent_parent_safe(run->km,path)<0) return -1;
    AdFile *file=ad_native_file(run,"opencode");
    if (!file) return -1;
    json_object *doc=ad_parse(run->km,file->text), *agents=km_get(doc,"agents");
    if (!doc) return -1;
    if (agents && !json_object_is_type(agents,json_type_object)) {
        json_object_put(doc); return km_error(run->km,"OpenCode agents in %s must be an object",file->path);
    }
    int result=0;
    if (agents) { json_object_object_foreach(agents,id,definition) {
        const char *mode=km_string(definition,"mode");
        if (!mode || strcmp(mode,"subagent")) { printf("import: opencode primary/all agent skipped (subagents only): %s\n",id); continue; }
        if ((result=import_json_entry(run,file,id,definition))<0) break;
    } }
    json_object_put(doc); return result;
}

int ad_import_subagents(AdRun *run, const char *agent) {
    if (ad_subagent_flat_discovery(run->km,agent)<0) return -1;
    if (scan_directory(run,agent)<0) return -1;
    return !strcmp(agent,"opencode") ? import_config(run) : 0;
}
