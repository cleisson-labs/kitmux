#include "adapters_skills.h"
#include <stdlib.h>
#include <string.h>

/* Antigravity has no per-skill switch. Kitmux hides a skill that is off for it
 * but visible through the shared ~/.agents/skills root by adding its name to
 * the top-level "exclude" list of ~/.gemini/config/skills.json. Kitmux owns
 * only the names it added; they are recorded in adapters.json under
 * skill_excludes/<manifest path> (names, previous, created, file). Every other
 * manifest entry is user content and is never removed or reordered. */

#define MANIFEST ".gemini/config/skills.json"

typedef struct {
    AdRun *run;
    AdFile *file;
    json_object *record;
    json_object *current; /* exclude array, or NULL when absent */
    bool *owned;
    size_t count;
} Excludes;

static int invalid(Excludes *e) {
    return km_error(e->run->km,"antigravity: \"exclude\" in %s must be an array of skill names; fix it, then run `kitmux sync --apply --agent antigravity`",
                    e->file->path);
}

static bool names_valid(json_object *list, bool ids) {
    if (!json_object_is_type(list,json_type_array)) return false;
    for (size_t i=0;i<json_object_array_length(list);i++) {
        json_object *name=json_object_array_get_idx(list,i);
        if (!json_object_is_type(name,json_type_string) || (ids && !km_valid_id(json_object_get_string(name)))) return false;
    }
    return true;
}

static const char *entry_at(Excludes *e, size_t i) {
    return json_object_get_string(json_object_array_get_idx(e->current,i));
}

/* Recorded names claim the last matching entries; Kitmux appends its names. */
static void claim(Excludes *e, const char *name) {
    for (size_t i=e->count;i>0;i--)
        if (!e->owned[i-1] && !strcmp(entry_at(e,i-1),name)) { e->owned[i-1]=true; return; }
}

static int record_valid(Excludes *e) {
    const char *lists[]={"names","previous"}, *flags[]={"created","file"};
    for (size_t i=0;i<2;i++) {
        json_object *list=km_get(e->record,lists[i]), *flag=km_get(e->record,flags[i]);
        if ((list && !names_valid(list,true)) || (flag && !json_object_is_type(flag,json_type_boolean)))
            return km_error(e->run->km,"antigravity: invalid skill_excludes record for %s in adapters.json; restore the private state backup, then run `kitmux sync --apply --agent antigravity`",
                            e->file->path);
    }
    return 0;
}

static json_object *record_for(AdRun *run, const char *path) {
    json_object *all=km_get(run->state,"skill_excludes");
    if (!all) { all=json_object_new_object(); json_object_object_add(run->state,"skill_excludes",all); }
    if (!json_object_is_type(all,json_type_object)) return NULL;
    json_object *record=km_get(all,path);
    if (!record) { record=json_object_new_object(); json_object_object_add(all,path,record); }
    return record;
}

static int load(Excludes *e, AdRun *run) {
    memset(e,0,sizeof(*e)); e->run=run;
    if (!(e->file=ad_file(run,"antigravity",MANIFEST,false))) return -1;
    if (!(e->record=record_for(run,e->file->path)))
        return km_error(run->km,"antigravity: invalid skill_excludes record in adapters.json; restore the private state backup");
    if (record_valid(e)<0) return -1;
    json_object *doc=ad_parse(run->km,e->file->text);
    if (!doc) return -1;
    json_object *current=km_get(doc,"exclude");
    e->current=current ? json_object_get(current) : NULL;
    json_object_put(doc);
    if (e->current && !names_valid(e->current,false)) return invalid(e);
    if (!km_get(e->record,"names")) {
        json_object_object_add(e->record,"created",json_object_new_boolean(!e->current));
        json_object_object_add(e->record,"file",json_object_new_boolean(!e->file->existed));
    }
    e->count=e->current ? json_object_array_length(e->current) : 0;
    if (!(e->owned=calloc(e->count ? e->count : 1,sizeof(*e->owned)))) return km_error(run->km,"out of memory");
    bool pending=json_object_get_boolean(km_get(run->state,"pending"));
    const char *fields[]={"names","previous"};
    for (size_t f=0;f<(pending ? 2u : 1u);f++) {
        json_object *list=km_get(e->record,fields[f]);
        for (size_t i=0;list && i<json_object_array_length(list);i++) claim(e,json_object_get_string(json_object_array_get_idx(list,i)));
    }
    return 0;
}

static bool user_excludes(Excludes *e, const char *id) {
    for (size_t i=0;i<e->count;i++) if (!e->owned[i] && !strcmp(entry_at(e,i),id)) return true;
    return false;
}

/* User entries keep their order; Kitmux's names follow them. */
static json_object *desired(Excludes *e, json_object *names, bool restore) {
    json_object *list=json_object_new_array();
    for (size_t i=0;i<e->count;i++) if (!e->owned[i]) json_object_array_add(list,json_object_new_string(entry_at(e,i)));
    if (restore) return list;
    json_object_object_foreach(km_resources(e->run->km,"skill"),id,skill) {
        bool on=km_effective(e->run->km,"skill",id,"antigravity",NULL,0), excluded=user_excludes(e,id);
        if (on && excluded)
            ad_note(e->run,"antigravity: skill %s is on but your %s excludes it; remove that entry to show it",id,e->file->path);
        if (on || excluded || !ad_skill_agents_root_visible(e->run,id,skill)) continue;
        json_object_array_add(list,json_object_new_string(id));
        json_object_array_add(names,json_object_new_string(id));
    }
    return list;
}

static bool empty_document(Km *km, const char *text) {
    json_object *doc=ad_parse(km,text);
    bool empty=json_object_is_type(doc,json_type_object) && !json_object_object_length(doc);
    json_object_put(doc); return empty;
}

static int store(Excludes *e, json_object *list, json_object *names) {
    Km *km=e->run->km;
    bool empty=!json_object_array_length(list);
    bool drop=empty && (json_object_get_boolean(km_get(e->record,"created")) || !e->current);
    int result=0;
    if (drop ? e->current!=NULL : !json_object_equal(e->current,list)) {
        const char *text=drop ? NULL : json_object_to_json_string_ext(list,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
        result=ad_json_edit(km,&e->file->text,"exclude",text);
    }
    /* A manifest Kitmux created disappears once it holds nothing else. */
    e->file->remove=!result && drop && json_object_get_boolean(km_get(e->record,"file")) && empty_document(km,e->file->text);
    json_object *previous=km_get(e->record,"names");
    json_object_object_add(e->record,"previous",previous ? json_object_get(previous) : json_object_new_array());
    json_object_object_add(e->record,"names",json_object_get(names));
    return result;
}

static int run_excludes(AdRun *run, bool restore) {
    Excludes e;
    json_object *names=json_object_new_array(), *list=NULL;
    int result=load(&e,run);
    if (!result) { list=desired(&e,names,restore); result=store(&e,list,names); }
    if (!result && restore) json_object_object_del(km_get(run->state,"skill_excludes"),e.file->path);
    json_object_put(list); json_object_put(names); json_object_put(e.current); free(e.owned);
    return result;
}

int ad_sync_skill_excludes(AdRun *run) { return run_excludes(run,false); }

int ad_restore_skill_excludes(AdRun *run) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s",run->km->home,MANIFEST)<0) return -1;
    return km_get(km_get(run->state,"skill_excludes"),path) ? run_excludes(run,true) : 0;
}

int ad_skill_manifest_enabled(AdRun *run, const char *id, bool *enabled) {
    AdFile *file=ad_file(run,"antigravity",MANIFEST,false);
    if (!file) return -1;
    json_object *doc=ad_parse(run->km,file->text);
    if (!doc) return -1;
    json_object *list=km_get(doc,"exclude");
    *enabled=true;
    for (size_t i=0;names_valid(list,false) && i<json_object_array_length(list);i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(list,i)),id)) *enabled=false;
    json_object_put(doc); return 0;
}
