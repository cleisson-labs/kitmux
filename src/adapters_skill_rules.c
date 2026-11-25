#include "adapters_skills.h"
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

/* Kitmux owns only the OpenCode skill rules it appended. Their identities are
 * recorded in adapters.json under skill_rules/<config path>; every other rule
 * in the permissions array is user content and is never a conflict.
 *   deny  - the skill is off for OpenCode but visible through a shared root;
 *   allow - the skill is on for OpenCode but native rules would deny it. */

typedef struct { size_t start, end; bool owned, keep; } Element;

typedef struct {
    AdRun *run;
    AdFile *file;
    char *raw;          /* current permissions array text, or NULL when absent */
    json_object *rules; /* parsed array */
    Element *items;
    size_t count;
    json_object *record;
} Rules;

static size_t skip_space(const char *s, size_t p) {
    for (;;) {
        while (s[p]==' ' || s[p]=='\t' || s[p]=='\n' || s[p]=='\r') p++;
        if (s[p]=='/' && s[p+1]=='/') { while (s[p] && s[p]!='\n') p++; continue; }
        if (s[p]=='/' && s[p+1]=='*') { const char *end=strstr(s+p+2,"*/"); p=end ? (size_t)(end-s)+2 : strlen(s); continue; }
        return p;
    }
}

static int invalid(Rules *r) {
    return km_error(r->run->km,"opencode: permissions in %s must be a v2 array of rules; fix it, then run `kitmux sync --apply --agent opencode`",r->file->path);
}

/* Locate each array element in the raw text so removals keep user formatting. */
static int locate(Rules *r) {
    size_t p=skip_space(r->raw,0), i=0;
    if (r->raw[p++]!='[') return invalid(r);
    for (;;) {
        p=skip_space(r->raw,p);
        if (r->raw[p]==']') break;
        if (i==r->count) return invalid(r);
        json_tokener *tok=json_tokener_new();
        if (!tok) return km_error(r->run->km,"out of memory");
        json_object *value=json_tokener_parse_ex(tok,r->raw+p,(int)strlen(r->raw+p));
        size_t used=json_tokener_get_parse_end(tok);
        json_tokener_free(tok);
        if (!value || !used) { json_object_put(value); return invalid(r); }
        json_object_put(value);
        r->items[i++]=(Element){.start=p,.end=p+used};
        p=skip_space(r->raw,p+used);
        if (r->raw[p]==',') p++;
    }
    return i==r->count ? 0 : invalid(r);
}

static bool owned_shape(json_object *rule, const char *resource, const char *effect) {
    const char *action=km_string(rule,"action"), *target=km_string(rule,"resource"), *kind=km_string(rule,"effect");
    return json_object_is_type(rule,json_type_object) && json_object_object_length(rule)==3 && action && target && kind &&
           !strcmp(action,"skill") && !strcmp(target,resource) && !strcmp(kind,effect);
}

static bool matches(json_object *rule, const char *id) {
    const char *action=km_string(rule,"action"), *resource=km_string(rule,"resource");
    return action && resource && !fnmatch(action,"skill",0) && !fnmatch(resource,id,0);
}

/* Mark the last unowned occurrence of a recorded rule; Kitmux appends its rules. */
static void claim(Rules *r, const char *resource, const char *effect) {
    for (size_t i=r->count;i>0;i--) {
        Element *item=&r->items[i-1];
        if (!item->owned && owned_shape(json_object_array_get_idx(r->rules,i-1),resource,effect)) { item->owned=true; return; }
    }
}

static bool valid_list(json_object *list) {
    if (!list) return true;
    if (!json_object_is_type(list,json_type_array)) return false;
    for (size_t i=0;i<json_object_array_length(list);i++) {
        json_object *rule=json_object_array_get_idx(list,i);
        const char *resource=km_string(rule,"resource"), *effect=km_string(rule,"effect");
        if (!km_valid_id(resource) || !effect || (strcmp(effect,"deny") && strcmp(effect,"allow"))) return false;
    }
    return true;
}

static int claim_recorded(Rules *r) {
    json_object *created=km_get(r->record,"created");
    if (!valid_list(km_get(r->record,"rules")) || !valid_list(km_get(r->record,"previous")) ||
        (created && !json_object_is_type(created,json_type_boolean)))
        return km_error(r->run->km,"opencode: invalid skill_rules ownership record for %s in adapters.json; restore the private state backup, then run `kitmux sync --apply --agent opencode`",r->file->path);
    bool pending=json_object_get_boolean(km_get(r->run->state,"pending"));
    const char *fields[]={"rules","previous"};
    for (size_t f=0;f<(pending ? 2u : 1u);f++) {
        json_object *list=km_get(r->record,fields[f]);
        for (size_t i=0;list && i<json_object_array_length(list);i++) {
            json_object *rule=json_object_array_get_idx(list,i);
            claim(r,km_string(rule,"resource"),km_string(rule,"effect"));
        }
    }
    return 0;
}

/* Earlier versions owned the whole array; adopt only the denies they appended. */
static int migrate_legacy(Rules *r) {
    json_object *entries=km_get(km_get(km_get(r->run->state,"files"),r->file->path),"entries");
    json_object *legacy=km_get(entries,"permissions");
    if (!legacy) return 0;
    const char *before=km_string(legacy,"before");
    json_object *original=before ? ad_parse(r->run->km,before) : NULL;
    if (before && (!original || !json_object_is_type(original,json_type_array))) { json_object_put(original); return invalid(r); }
    for (size_t i=r->count;i>0;i--) {
        json_object *rule=json_object_array_get_idx(r->rules,i-1);
        const char *resource=km_string(rule,"resource");
        if (!km_valid_id(resource) || !owned_shape(rule,resource,"deny")) continue;
        size_t native=0, current=0;
        for (size_t j=0;original && j<json_object_array_length(original);j++)
            native+=owned_shape(json_object_array_get_idx(original,j),resource,"deny");
        for (size_t j=0;j<r->count;j++) current+=r->items[j].owned ? 0 : owned_shape(json_object_array_get_idx(r->rules,j),resource,"deny");
        if (current>native) claim(r,resource,"deny");
    }
    json_object_object_add(r->record,"created",json_object_new_boolean(!before));
    json_object_put(original);
    json_object_object_del(entries,"permissions");
    return 0;
}

static bool native_denies(Rules *r, const char *id) {
    bool denied=false;
    for (size_t i=0;i<r->count;i++) {
        json_object *rule=json_object_array_get_idx(r->rules,i);
        if (!r->items[i].owned && matches(rule,id)) denied=!strcmp(km_string(rule,"effect") ? km_string(rule,"effect") : "","deny");
    }
    return denied;
}

static const char *needed(Rules *r, const char *id, json_object *skill) {
    if (km_effective(r->run->km,"skill",id,"opencode",NULL,0)) return native_denies(r,id) ? "allow" : NULL;
    return ad_skill_shared_visible(r->run,id,skill) ? "deny" : NULL;
}

/* An agent-level skill allow overrides global denies, so isolation is impossible. */
static int agent_override(Rules *r) {
    json_object *document=ad_parse(r->run->km,r->file->text);
    if (!document) return -1;
    json_object *agents=km_get(document,"agents");
    int result=0;
    if (json_object_is_type(agents,json_type_object)) {
        json_object_object_foreach(agents,name,agent) {
            json_object *permissions=km_get(agent,"permissions");
            bool allows=permissions && !json_object_is_type(permissions,json_type_array);
            for (size_t i=0;!allows && permissions && i<json_object_array_length(permissions);i++) {
                json_object *rule=json_object_array_get_idx(permissions,i);
                const char *action=km_string(rule,"action"), *effect=km_string(rule,"effect");
                allows=action && (!strcmp(action,"skill") || !strcmp(action,"*")) && (!effect || strcmp(effect,"deny"));
            }
            if (allows && !result)
                result=km_error(r->run->km,"opencode: agent \"%s\" in %s has a permission rule that allows skills, overriding Kitmux's per-skill denies; remove that agent rule (or enable the skills for opencode), then run `kitmux sync --apply --agent opencode`",name,r->file->path);
        }
    }
    json_object_put(document);
    return result;
}

static bool last_for(Rules *r, size_t index, const char *id) {
    for (size_t i=index+1;i<r->count;i++)
        if (matches(json_object_array_get_idx(r->rules,i),id)) return false;
    return true;
}

static int remove_item(Rules *r, size_t i) {
    size_t start=r->items[i].start, end=r->items[i].end, after=skip_space(r->raw,end);
    if (r->raw[after]==',') end=after+1;
    else if (i>0) {
        size_t comma=skip_space(r->raw,r->items[i-1].end);
        if (r->raw[comma]==',') start=comma;
    }
    return ad_replace(r->run->km,&r->raw,start,end,"");
}

static int append_rule(Rules *r, const char *id, const char *effect, json_object *list) {
    json_object *rule=json_object_new_object();
    json_object_object_add(rule,"action",json_object_new_string("skill"));
    json_object_object_add(rule,"resource",json_object_new_string(id));
    json_object_object_add(rule,"effect",json_object_new_string(effect));
    int result=ad_array_append(r->run->km,&r->raw,json_object_to_json_string_ext(rule,JSON_C_TO_STRING_PLAIN));
    json_object_put(rule);
    json_object *owned=json_object_new_object();
    json_object_object_add(owned,"resource",json_object_new_string(id));
    json_object_object_add(owned,"effect",json_object_new_string(effect));
    json_object_array_add(list,owned);
    return result;
}

/* Keep owned rules that are still needed and decisive; remove the rest, then append missing ones. */
static int reconcile(Rules *r, json_object *list, bool restore) {
    json_object *resources=km_resources(r->run->km,"skill");
    bool denies=false;
    for (size_t i=0;i<r->count && !restore;i++) {
        json_object *rule=json_object_array_get_idx(r->rules,i);
        const char *id=km_string(rule,"resource");
        if (!r->items[i].owned || !km_get(resources,id)) continue;
        const char *want=needed(r,id,km_get(resources,id));
        r->items[i].keep=want && !strcmp(want,km_string(rule,"effect")) && last_for(r,i,id);
    }
    for (size_t i=r->count;i>0;i--)
        if (r->items[i-1].owned && !r->items[i-1].keep && remove_item(r,i-1)<0) return -1;
    if (restore) return 0;
    json_object_object_foreach(resources,id,skill) {
        const char *want=needed(r,id,skill);
        if (!want) continue;
        denies=denies || !strcmp(want,"deny");
        bool present=false;
        for (size_t i=0;i<r->count && !present;i++)
            present=r->items[i].keep && !strcmp(km_string(json_object_array_get_idx(r->rules,i),"resource"),id);
        if (present) {
            json_object *owned=json_object_new_object();
            json_object_object_add(owned,"resource",json_object_new_string(id));
            json_object_object_add(owned,"effect",json_object_new_string(want));
            json_object_array_add(list,owned);
        } else if (append_rule(r,id,want,list)<0) return -1;
    }
    return denies ? agent_override(r) : 0;
}

static int load(Rules *r, AdRun *run) {
    memset(r,0,sizeof(*r));
    r->run=run;
    r->file=ad_native_file(run,"opencode");
    if (!r->file) return -1;
    json_object *all=km_get(run->state,"skill_rules");
    if (!all) { all=json_object_new_object(); json_object_object_add(run->state,"skill_rules",all); }
    if (!json_object_is_type(all,json_type_object))
        return km_error(run->km,"opencode: invalid skill_rules record in adapters.json; restore the private state backup");
    r->record=km_get(all,r->file->path);
    if (!r->record) { r->record=json_object_new_object(); json_object_object_add(all,r->file->path,r->record); }
    r->raw=ad_json_raw(run->km,r->file->text,"permissions");
    if (!km_get(r->record,"created") && !km_get(r->record,"rules"))
        json_object_object_add(r->record,"created",json_object_new_boolean(!r->raw));
    if (!r->raw && !(r->raw=strdup("[]"))) return km_error(run->km,"out of memory");
    r->rules=ad_parse(run->km,r->raw);
    if (!json_object_is_type(r->rules,json_type_array)) return invalid(r);
    r->count=json_object_array_length(r->rules);
    r->items=calloc(r->count ? r->count : 1,sizeof(*r->items));
    if (!r->items) return km_error(run->km,"out of memory");
    if (locate(r)<0 || migrate_legacy(r)<0) return -1;
    return claim_recorded(r);
}

static void release(Rules *r) {
    free(r->raw); free(r->items);
    if (r->rules) json_object_put(r->rules);
}

static int store(Rules *r, json_object *list) {
    char *current=ad_json_raw(r->run->km,r->file->text,"permissions");
    json_object *array=ad_parse(r->run->km,r->raw);
    bool empty=array && !json_object_array_length(array);
    json_object_put(array);
    bool created=json_object_get_boolean(km_get(r->record,"created"));
    const char *desired=empty && (created || !current) ? NULL : r->raw;
    int result=0;
    if (!ad_same(r->run->km,current,desired,false))
        result=ad_json_edit(r->run->km,&r->file->text,"permissions",desired);
    free(current);
    json_object *previous=km_get(r->record,"rules");
    json_object_object_add(r->record,"previous",previous ? json_object_get(previous) : json_object_new_array());
    json_object_object_add(r->record,"rules",list);
    return result;
}

static int run_rules(AdRun *run, bool restore) {
    Rules r;
    json_object *list=json_object_new_array();
    int result=load(&r,run);
    if (!result) result=reconcile(&r,list,restore);
    if (!result) { result=store(&r,list); list=NULL; }
    if (list) json_object_put(list);
    if (!result && restore) json_object_object_del(km_get(run->state,"skill_rules"),r.file->path);
    release(&r);
    return result;
}

int ad_sync_skill_rules(AdRun *run) { return run_rules(run,false); }

int ad_restore_skill_rules(AdRun *run) { return run_rules(run,true); }
