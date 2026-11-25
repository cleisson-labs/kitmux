#include "adapters.h"
#include <stdlib.h>
#include <string.h>

json_object *ad_mcp_canonical(Km *km, const char *agent, json_object *native) {
    if (ad_server_shape(km,agent,native)<0) return NULL;
    json_object *resource=json_object_new_object(), *command=km_get(native,"command");
    const char *url=km_string(native,"url");
    if (url) {
        json_object_object_add(resource,"transport",json_object_new_string("http"));
        json_object_object_add(resource,"url",json_object_new_string(url));
    } else if (command) {
        json_object *array=json_object_new_array();
        if (json_object_is_type(command,json_type_array)) {
            for (size_t i=0;i<json_object_array_length(command);i++)
                json_object_array_add(array,json_object_get(json_object_array_get_idx(command,i)));
        } else if (json_object_is_type(command,json_type_string)) {
            json_object_array_add(array,json_object_get(command));
            json_object *args=km_get(native,"args");
            for (size_t i=0;args && i<json_object_array_length(args);i++)
                json_object_array_add(array,json_object_get(json_object_array_get_idx(args,i)));
        }
        if (!json_object_array_length(array)) { json_object_put(array); goto unsupported; }
        json_object_object_add(resource,"transport",json_object_new_string("stdio"));
        json_object_object_add(resource,"command",array);
    } else goto unsupported;
    json_object *env=km_get(native,!strcmp(agent,"opencode") ? "environment" : "env");
    if (env) json_object_object_add(resource,"env",json_object_get(env));
    const char *cwd=km_string(native,"cwd");
    if (cwd) json_object_object_add(resource,"cwd",json_object_new_string(cwd));
    const char *description=km_string(native,"description");
    if (description && strlen(description)==(size_t)json_object_get_string_len(km_get(native,"description")))
        json_object_object_add(resource,"description",json_object_get(km_get(native,"description")));
    json_object_object_add(resource,"mode",json_object_new_string("direct"));
    json_object_object_add(resource,"enabled",json_object_new_boolean(true));
    json_object_object_add(resource,"agents",json_object_new_object());
    json_object_object_add(resource,"native",json_object_new_object()); return resource;
unsupported:
    json_object_put(resource); km_error(km,"unsupported native MCP definition for %s",agent); return NULL;
}

static bool compatible(json_object *a, json_object *b) {
    const char *keys[]={"transport","command","url","env","cwd"};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
        json_object *left=km_get(a,keys[i]), *right=km_get(b,keys[i]);
        if (!left && !right) continue;
        if (!left || !right || !json_object_equal(left,right)) return false;
    }
    return true;
}

static bool native_enabled(const char *agent, json_object *native) {
    if (!strcmp(agent,"opencode")) return !json_object_get_boolean(km_get(native,"disabled"));
    return !km_get(native,"enabled") || json_object_get_boolean(km_get(native,"enabled"));
}

static json_object *entry_record(AdRun *run, AdFile *file, const char *key) {
    return km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
}

/* Keep shared execution at the catalog level and client differences as variants. */
static int merge_existing(Km *km, json_object *existing, json_object *resource, const char *agent) {
    /* An empty authored description deliberately suppresses imported text. */
    if (!km_get(existing,"description") && km_get(resource,"description"))
        json_object_object_add(existing,"description",json_object_get(km_get(resource,"description")));
    json_object *variants=km_get(existing,"variants");
    if (compatible(existing,resource)) {
        if (variants) json_object_object_del(variants,agent);
        return 0;
    }
    json_object *execution=km_mcp_execution(resource,agent);
    if (!execution) return km_error(km,"cannot retain imported MCP execution");
    json_object_object_del(execution,"mode");
    if (!variants) { variants=json_object_new_object(); json_object_object_add(existing,"variants",variants); }
    json_object_object_add(variants,agent,execution); return 0;
}

static void assign(json_object *resource, const char *agent, bool on) {
    json_object *assignments=km_get(resource,"agents");
    if (!assignments) { assignments=json_object_new_object(); json_object_object_add(resource,"agents",assignments); }
    json_object_object_add(assignments,agent,json_object_new_string(on ? "on" : "off"));
}

/* Import keeps inherited and group availability; it records a client override
 * only for a new resource or when native state contradicts the catalog. */
static void reconcile_assignment(AdRun *run, json_object *resource, const char *id, const char *agent,
                                 json_object *native, bool created) {
    bool wanted=native_enabled(agent,native);
    if (created || km_effective(run->km,"mcp",id,agent,NULL,0)!=wanted) assign(resource,agent,wanted);
}

static bool managed(json_object *resource) {
    const char *mode=km_string(resource,"mode");
    return mode && !strcmp(mode,"managed");
}

static int adopt(AdRun *run, AdFile *file, const char *agent, const char *id, const char *raw, json_object *native) {
    if (!km_valid_id(id)) return km_error(run->km,"native MCP identifier is unsupported");
    json_object *resources=km_resources(run->km,"mcp"), *existing=km_get(resources,id);
    char key[256]; ad_mcp_key(key,sizeof(key),agent,id);
    json_object *record=entry_record(run,file,key);
    /* Re-adopting our projection must never turn a bridge into its own upstream. */
    if (existing && record && ad_entry_same(run->km,file,key,raw,km_string(record,"after")))
        return ad_change(run,file,key,raw,true);
    if (run->refresh && existing && record && managed(existing))
        return km_error(run->km,"Cannot refresh an externally changed managed MCP (%s); reconcile its native bridge before import",id);
    json_object *resource=ad_mcp_canonical(run->km,agent,native);
    if (!resource) return -1;
    bool created=!existing || km_get(run->created,id);
    if (existing && merge_existing(run->km,existing,resource,agent)<0) { json_object_put(resource); return -1; }
    if (existing) { json_object_put(resource); resource=existing; }
    else {
        json_object_object_add(resources,id,resource);
        json_object_object_add(run->created,id,json_object_new_boolean(true));
    }
    json_object *natives=km_get(resource,"native");
    if (!natives) { natives=json_object_new_object(); json_object_object_add(resource,"native",natives); }
    json_object_object_add(natives,agent,json_object_get(native));
    reconcile_assignment(run,resource,id,agent,native,created);
    return ad_change(run,file,key,raw,true);
}

static const char *entry_id(const char *agent, const char *key) {
    if (!strcmp(agent,"codex")) return key;
    const char *prefix=!strcmp(agent,"claude") ? "mcpServers/" : "mcp/servers/";
    return strncmp(key,prefix,strlen(prefix)) ? NULL : key+strlen(prefix);
}

/* Refresh adopts an owned entry deleted outside Kitmux: that client turns off. */
static int adopt_deletions(AdRun *run, AdFile *file, const char *agent) {
    json_object *entries=km_get(km_get(km_get(run->state,"files"),file->path),"entries");
    if (!entries) return 0;
    json_object_object_foreach(entries,key,record) {
        const char *id=entry_id(agent,key);
        json_object *resource=id ? km_get(km_resources(run->km,"mcp"),id) : NULL;
        if (!resource || !km_string(record,"after")) continue;
        char *current=ad_entry_raw(run->km,file,key);
        if (current || run->km->error[0]) { free(current); if (run->km->error[0]) return -1; continue; }
        if (km_effective(run->km,"mcp",id,agent,NULL,0)) assign(resource,agent,false);
        if (ad_change(run,file,key,NULL,true)<0) return -1;
        ad_note(run,"%s: %s MCP %s was removed natively; now off for %s",
                run->apply ? "import" : "preview",agent,id,agent);
    }
    return 0;
}

static json_object *native_ids(AdRun *run, AdFile *file, const char *agent) {
    if (file->toml) {
        json_object *ids=ad_toml_ids(run->km,file->text);
        if (!ids) ad_file_error(run->km,file->path);
        return ids;
    }
    json_object *doc=ad_parse(run->km,file->text), *ids=json_object_new_array();
    json_object *servers=!strcmp(agent,"claude") ? km_get(doc,"mcpServers") : km_get(km_get(doc,"mcp"),"servers");
    if (servers) {
        json_object_object_foreach(servers,id,value) { (void)value; json_object_array_add(ids,json_object_new_string(id)); }
    }
    json_object_put(doc); return ids;
}

int ad_import(AdRun *run, const char *agent) {
    AdFile *file=ad_native_file(run,agent);
    if (!file) return -1;
    json_object *ids=native_ids(run,file,agent);
    if (!ids) return -1;
    int result=0;
    for (size_t i=0;i<json_object_array_length(ids);i++) {
        const char *id=json_object_get_string(json_object_array_get_idx(ids,i));
        char key[256]; ad_mcp_key(key,sizeof(key),agent,id);
        char *raw=file->toml ? ad_toml_raw(run->km,file->text,id) : ad_json_raw(run->km,file->text,key);
        json_object *native=raw ? (file->toml ? ad_toml_parse(run->km,raw) : ad_parse(run->km,raw)) : NULL;
        if (!native) {
            free(raw);
            result=km_error(run->km,"%s: cannot safely locate native MCP %s; keep it in its own [mcp_servers.%s] "
                            "table or JSON object, then retry",file->path,id,id);
            break;
        }
        result=adopt(run,file,agent,id,raw,native); free(raw); json_object_put(native);
        if (result<0) break;
        ad_note(run,"%s: %s MCP %s",run->apply ? "import" : "preview",agent,id);
    }
    json_object_put(ids);
    return result==0 && run->refresh ? adopt_deletions(run,file,agent) : result;
}
