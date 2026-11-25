#include "adapters.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

AdFile *ad_native_file(AdRun *run, const char *agent) {
    if (!strcmp(agent,"codex")) return ad_file(run,agent,".codex/config.toml",true);
    if (!strcmp(agent,"claude")) return ad_file(run,agent,".claude.json",false);
    char a[PATH_MAX],b[PATH_MAX]; struct stat sa,sb;
    if (km_path(run->km,a,sizeof(a),"%s/.config/opencode/opencode.json",run->km->home)<0 ||
        km_path(run->km,b,sizeof(b),"%s/.config/opencode/opencode.jsonc",run->km->home)<0) return NULL;
    bool first=lstat(a,&sa)==0, second=lstat(b,&sb)==0;
    if (first && second) { km_error(run->km,"consolidate OpenCode JSON and JSONC configs before sync"); return NULL; }
    return ad_file(run,agent,second ? ".config/opencode/opencode.jsonc" : ".config/opencode/opencode.json",false);
}

static json_object *clone(json_object *obj) {
    json_object *copy=NULL;
    if (obj) json_object_deep_copy(obj,&copy,NULL);
    return copy ? copy : json_object_new_object();
}

static int executable(Km *km, char *path, size_t size) {
#ifdef __APPLE__
    uint32_t length=(uint32_t)size;
    if (_NSGetExecutablePath(path,&length)!=0) return km_error(km,"cannot resolve Kitmux executable path");
#else
    ssize_t length=readlink("/proc/self/exe",path,size-1);
    if (length<0) return km_error(km,"cannot resolve Kitmux executable path");
    path[length]=0;
#endif
    char *resolved=realpath(path,NULL);
    if (!resolved) return km_error(km,"cannot resolve Kitmux executable path");
    int result=km_path(km,path,size,"%s",resolved); free(resolved); return result;
}

/* A bridge keeps the executable it already uses, so running another Kitmux
 * binary cannot repoint clients; an executable that is gone is replaced. */
static const char *bridge_program(json_object *native, const char *agent, const char *id) {
    bool opencode=!strcmp(agent,"opencode");
    json_object *args=km_get(native,opencode ? "command" : "args");
    if (!json_object_is_type(args,json_type_array) || json_object_array_length(args)<4) return NULL;
    const char *program=opencode ? json_object_get_string(json_object_array_get_idx(args,0)) : km_string(native,"command");
    const char *tail[]={"bridge",id,"--agent",agent};
    size_t start=json_object_array_length(args)-4;
    for (size_t i=0;i<4;i++) {
        json_object *item=json_object_array_get_idx(args,start+i);
        if (!json_object_is_type(item,json_type_string) || strcmp(json_object_get_string(item),tail[i])) return NULL;
    }
    return program && program[0]=='/' && access(program,X_OK)==0 ? program : NULL;
}

static json_object *command_array(Km *km, json_object *resource, const char *id, const char *agent,
                                  const char *bridge) {
    const char *mode=km_string(resource,"mode");
    if (!mode || strcmp(mode,"managed")) return json_object_get(km_get(resource,"command"));
    char path[PATH_MAX];
    if (bridge ? km_path(km,path,sizeof(path),"%s",bridge)<0 : executable(km,path,sizeof(path))<0) return NULL;
    json_object *array=json_object_new_array();
    json_object_array_add(array,json_object_new_string(path));
    if (km->isolated) {
        json_object_array_add(array,json_object_new_string("--home"));
        json_object_array_add(array,json_object_new_string(km->home));
    }
    json_object_array_add(array,json_object_new_string("bridge"));
    json_object_array_add(array,json_object_new_string(id));
    json_object_array_add(array,json_object_new_string("--agent"));
    json_object_array_add(array,json_object_new_string(agent)); return array;
}

/* Claude's legacy SSE endpoints need an SSE client; Codex and OpenCode connect
 * to remote MCP servers over Streamable HTTP only. */
static bool sse_only(json_object *resource, const char *agent, const char *url) {
    json_object *natives=km_get(resource,"native");
    if (!natives || !strcmp(agent,"claude") || km_get(natives,agent)) return false;
    json_object_object_foreach(natives,client,native) {
        (void)client; const char *type=km_string(native,"type"), *endpoint=km_string(native,"url");
        if (type && endpoint && !strcmp(type,"sse") && !strcmp(endpoint,url)) return true;
    }
    return false;
}

static json_object *render_native(Km *km, json_object *resource, json_object *execution, const char *id,
                                  const char *agent, bool enabled, const char *bridge) {
    json_object *original=km_get(km_get(resource,"native"),agent), *native=clone(original);
    const char *transport=km_string(execution,"transport"), *url=km_string(execution,"url");
    bool opencode=!strcmp(agent,"opencode"), codex=!strcmp(agent,"codex");
    if (transport && !strcmp(transport,"http")) {
        if (!url) { json_object_put(native); km_error(km,"HTTP MCP is missing its URL"); return NULL; }
        if (sse_only(resource,agent,url)) {
            json_object_put(native);
            km_error(km,"MCP %s uses Claude's SSE transport, but %s connects to remote MCP servers over "
                     "Streamable HTTP only; run kitmux mcp disable %s --agent %s",id,agent,id,agent);
            return NULL;
        }
        const char *old=km_string(native,"url");
        if (old && strcmp(old,url)) { json_object_put(native); km_error(km,"changed imported endpoint requires explicit re-import"); return NULL; }
        json_object_object_add(native,"url",json_object_new_string(url));
        if (!codex && !km_get(native,"type")) json_object_object_add(native,"type",json_object_new_string(opencode ? "remote" : "http"));
    } else {
        json_object *array=command_array(km,execution,id,agent,bridge);
        if (!array || !json_object_is_type(array,json_type_array) || !json_object_array_length(array)) {
            if (array) json_object_put(array);
            json_object_put(native); km_error(km,"stdio MCP has no command"); return NULL;
        }
        if (opencode) {
            json_object_object_add(native,"type",json_object_new_string("local"));
            json_object_object_add(native,"command",array);
        } else {
            json_object_object_add(native,"command",json_object_get(json_object_array_get_idx(array,0)));
            json_object *args=json_object_new_array();
            for (size_t i=1;i<json_object_array_length(array);i++)
                json_object_array_add(args,json_object_get(json_object_array_get_idx(array,i)));
            if (json_object_array_length(args) || km_get(native,"args")) json_object_object_add(native,"args",args);
            else json_object_put(args);
            json_object_put(array);
        }
    }
    json_object *env=km_get(execution,"env");
    json_object_object_del(native,opencode ? "environment" : "env");
    if (env) json_object_object_add(native,opencode ? "environment" : "env",json_object_get(env));
    const char *cwd=km_string(execution,"cwd");
    json_object_object_del(native,"cwd");
    if (cwd) json_object_object_add(native,"cwd",json_object_new_string(cwd));
    bool managed=km_string(execution,"mode") && !strcmp(km_string(execution,"mode"),"managed");
    /* Codex treats a missing enabled as true and drops an explicit default. */
    if (codex && (!enabled || km_get(original,"enabled")))
        json_object_object_add(native,"enabled",json_object_new_boolean(enabled));
    if (opencode && (!original || managed || !enabled || km_get(original,"disabled")))
        json_object_object_add(native,"disabled",json_object_new_boolean(!enabled));
    return native;
}

json_object *ad_native(Km *km, json_object *resource, const char *id, const char *agent, bool enabled) {
    json_object *execution=km_mcp_execution(resource,agent);
    if (!execution) { km_error(km,"cannot resolve MCP execution for %s",agent); return NULL; }
    json_object *native=render_native(km,resource,execution,id,agent,enabled,NULL);
    json_object_put(execution); return native;
}

void ad_mcp_key(char *key, size_t size, const char *agent, const char *id) {
    snprintf(key,size,"%s%s",!strcmp(agent,"claude") ? "mcpServers/" : !strcmp(agent,"opencode") ? "mcp/servers/" : "",id);
}

static json_object *entry_record(AdRun *run, AdFile *file, const char *key) {
    return km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
}

static char *render_json(Km *km, json_object *native, const char *base) {
    if (!base) {
        const char *pretty=json_object_to_json_string_ext(native,JSON_C_TO_STRING_PRETTY|
                                                          JSON_C_TO_STRING_SPACED|JSON_C_TO_STRING_NOSLASHESCAPE);
        char *copy=strdup(pretty);
        if (!copy) km_error(km,"out of memory");
        return copy;
    }
    char *result=strdup(base);
    if (!result) { km_error(km,"out of memory"); return NULL; }
    json_object *existing_doc=ad_parse(km,result);
    if (!existing_doc) { free(result); return NULL; }
    json_object_object_foreach(existing_doc,old_key,old_value) {
        (void)old_value; json_object *present=NULL;
        if (!json_object_object_get_ex(native,old_key,&present) && ad_json_edit(km,&result,old_key,NULL)<0) {
            json_object_put(existing_doc); free(result); return NULL;
        }
    }
    json_object_put(existing_doc);
    json_object_object_foreach(native,key,value) {
        char *existing=ad_json_raw(km,result,key);
        const char *desired=json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
        bool same=ad_same(km,existing,desired,false); free(existing);
        if (!same && ad_json_edit(km,&result,key,desired)<0) { free(result); return NULL; }
    }
    return result;
}

/* An explicit availability key already in the client's file stays explicit. */
static void keep_explicit_default(json_object *native, json_object *existing, const char *agent, bool enabled) {
    bool codex=!strcmp(agent,"codex");
    const char *key=codex ? "enabled" : !strcmp(agent,"opencode") ? "disabled" : NULL;
    if (key && km_get(existing,key) && !km_get(native,key))
        json_object_object_add(native,key,json_object_new_boolean(codex ? enabled : !enabled));
}

static json_object *project(AdRun *run, AdFile *file, json_object *resource, const char *id,
                            bool visible, const char *current) {
    Km *km=run->km; const char *agent=file->agent;
    json_object *existing=current ? (file->toml ? ad_toml_parse(km,current) : ad_parse(km,current)) : NULL;
    if (current && !existing) return NULL;
    json_object *execution=km_mcp_execution(resource,agent), *native=NULL;
    if (!execution) km_error(km,"cannot resolve MCP execution for %s",agent);
    else native=render_native(km,resource,execution,id,agent,visible,bridge_program(existing,agent,id));
    if (native) keep_explicit_default(native,existing,agent,visible);
    json_object_put(execution); json_object_put(existing); return native;
}

/* An off MCP stays only where the client already has it: a disabled bridge
 * remains registered, and imported Codex/OpenCode entries keep their native
 * off form. A definition and its credentials never spread to other clients. */
static bool keep_entry(const char *agent, bool enabled, bool managed, const char *current, const char *original) {
    if (enabled) return true;
    if (!current) return false;
    return managed || (original && strcmp(agent,"claude"));
}

static bool managed_stdio(json_object *resource) {
    const char *mode=km_string(resource,"mode"), *transport=km_string(resource,"transport");
    return mode && transport && !strcmp(mode,"managed") && !strcmp(transport,"stdio");
}

static char *desired_entry(AdRun *run, AdFile *file, json_object *resource, const char *id,
                           bool visible, const char *current, const char *original) {
    json_object *native=project(run,file,resource,id,visible,current);
    if (!native) return NULL;
    const char *base=current ? current : original;
    char *desired=file->toml ? ad_toml_render(run->km,native,id,base) : render_json(run->km,native,base);
    json_object_put(native); return desired;
}

static int sync_one(AdRun *run, AdFile *file, const char *id, json_object *resource) {
    Km *km=run->km; const char *agent=file->agent; char key[256];
    ad_mcp_key(key,sizeof(key),agent,id);
    bool enabled=km_effective(km,"mcp",id,agent,NULL,0), managed=managed_stdio(resource);
    char *current=ad_entry_raw(km,file,key), *desired=NULL;
    if (km->error[0]) { free(current); return -1; }
    json_object *record=entry_record(run,file,key);
    const char *original=km_string(record,"before");
    if (keep_entry(agent,enabled,managed,current,original)) {
        desired=desired_entry(run,file,resource,id,enabled || managed,current,original);
        if (!desired || km->error[0]) { free(current); free(desired); return -1; }
    } else if (!original && !km_string(record,"after")) { free(current); return 0; }
    if (!ad_entry_same(km,file,key,current,desired))
        ad_note(run,"%s: %s MCP %s -> %s%s",run->apply ? "apply" : "preview",agent,id,
                enabled ? "enabled" : "disabled",managed && desired ? " (bridge remains registered)" : "");
    int result=ad_change(run,file,key,desired,false); free(current); free(desired); return result;
}

static int remove_missing(AdRun *run, AdFile *file, const char *agent) {
    json_object *record=km_get(km_get(run->state,"files"),file->path);
    json_object *entries=km_get(record,"entries");
    if (!entries) return 0;
    json_object_object_foreach(entries,key,entry) {
        (void)entry; const char *id=strrchr(key,'/'); id=id ? id+1 : key;
        if (km_get(km_resources(run->km,"mcp"),id)) continue;
        if (!strcmp(agent,"codex") || !strncmp(key,"mcpServers/",11) || !strncmp(key,"mcp/servers/",12))
            if (ad_change(run,file,key,NULL,false)<0) return -1;
    }
    return 0;
}

int ad_sync_mcp(AdRun *run, const char *agent) {
    AdFile *file=ad_native_file(run,agent);
    if (!file) return -1;
    if (file->toml) {
        json_object *ids=ad_toml_ids(run->km,file->text);
        if (!ids) return ad_file_error(run->km,file->path);
        json_object_put(ids);
    }
    json_object_object_foreach(km_resources(run->km,"mcp"),id,resource)
        if (sync_one(run,file,id,resource)<0) return -1;
    return remove_missing(run,file,agent);
}
