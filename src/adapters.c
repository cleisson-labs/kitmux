#include "adapters.h"
#include <stdlib.h>
#include <string.h>

typedef enum { SCOPE_NONE, SCOPE_SUBAGENTS, SCOPE_ALL } Scope;

typedef struct { json_object *state; size_t links, actions, messages; } Checkpoint;

static bool subagent_entries_only(json_object *entries) {
    bool any=false;
    json_object_object_foreach(entries,key,entry) {
        (void)entry; if (strncmp(key,"agents/",7)) return false;
        any=true;
    }
    return any;
}

/* True when catalog mutations automatically sync this client's MCPs and skills. */
static bool configured(AdRun *run, const char *agent) {
    json_object *managed=km_get(km_get(run->state,"managed_agents"),agent);
    if (managed) return json_object_get_boolean(managed);
    /* Older journals implied adoption by ownership; subagents alone never did. */
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        (void)path; const char *owner=km_string(record,"agent");
        if (owner && !strcmp(owner,agent) && !subagent_entries_only(km_get(record,"entries"))) return true;
    }
    return false;
}

bool ad_client_configured(Km *km, const char *agent) {
    AdRun run;
    bool result=ad_open(&run,km,false)==0 && configured(&run,agent);
    ad_cleanup(&run); return result;
}

/* subagent import adopts only that client's subagents, never its MCPs/skills. */
static json_object *subagent_adoption(AdRun *run) {
    json_object *map=km_get(run->state,"subagent_agents");
    if (!map) { map=json_object_new_object(); json_object_object_add(run->state,"subagent_agents",map); }
    return map;
}

static Scope scope_for(AdRun *run, const char *agent, bool configured_only, bool subagents_only) {
    if (subagents_only) return SCOPE_SUBAGENTS;
    if (!configured_only || configured(run,agent)) return SCOPE_ALL;
    return json_object_get_boolean(km_get(km_get(run->state,"subagent_agents"),agent)) ? SCOPE_SUBAGENTS : SCOPE_NONE;
}

static int selected(Km *km, const char *csv, bool flags[KM_AGENT_COUNT]) {
    for (int i=0;i<KM_AGENT_COUNT;i++) flags[i]=!csv;
    if (!csv) return 0;
    char *copy=strdup(csv), *save=NULL;
    if (!copy) return km_error(km,"out of memory");
    for (char *p=strtok_r(copy,",",&save);p;p=strtok_r(NULL,",",&save)) {
        if (!km_valid_agent(p)) { free(copy); return km_error(km,"unknown client in adapter selection"); }
        for (int i=0;i<KM_AGENT_COUNT;i++) if (!strcmp(p,km_agents[i])) flags[i]=true;
    }
    free(copy); return 0;
}

static int doctor(AdRun *run, bool flags[KM_AGENT_COUNT]) {
    for (int i=0;i<KM_AGENT_COUNT;i++) {
        if (!flags[i]) continue;
        const char *agent=km_agents[i]; AdFile *file=ad_native_file(run,agent);
        if (!file) return -1;
        if (file->toml) {
            json_object *ids=ad_toml_ids(run->km,file->text);
            if (!ids) return -1;
            json_object_put(ids);
        }
        printf("%s: %s config valid; runtime unverified; refresh may require a new session\n",agent,file->existed ? "existing" : "no");
    }
    if (json_object_get_boolean(km_get(run->state,"pending")))
        puts("Interrupted native sync detected; retry sync --apply to recover or inspect conflicts.");
    puts("Scope: user configuration. Project/organization overrides remain native and may restrict availability.");
    return 0;
}

static int process(AdRun *run, const char *verb, const char *agent, Scope scope) {
    bool all=scope==SCOPE_ALL; int result=0;
    if (!strcmp(verb,"import")) {
        if (all) result=ad_import(run,agent);
        if (result==0 && all) result=ad_import_skills(run,agent);
        return result ? result : ad_import_subagents(run,agent);
    }
    if (!strcmp(verb,"restore")) {
        result=ad_restore(run,agent);
        if (result==0) result=ad_restore_links(run,agent);
        return result ? result : ad_restore_subagents(run,agent);
    }
    if (all) result=ad_sync_mcp(run,agent);
    if (result==0 && all) result=ad_sync_skills(run,agent);
    return result ? result : ad_sync_subagents(run,agent);
}

static void record_adoption(AdRun *run, const char *verb, const char *agent, Scope scope) {
    bool restore=!strcmp(verb,"restore");
    if (restore || scope==SCOPE_ALL)
        json_object_object_add(run->management,agent,json_object_new_boolean(!restore));
    if (restore || (scope==SCOPE_SUBAGENTS && !strcmp(verb,"import")))
        json_object_object_add(subagent_adoption(run),agent,json_object_new_boolean(!restore));
}

static int checkpoint(AdRun *run, Checkpoint *point) {
    point->state=NULL;
    if (json_object_deep_copy(run->state,&point->state,NULL)) return km_error(run->km,"out of memory");
    point->links=json_object_array_length(run->links);
    point->actions=json_object_array_length(run->subagent_actions);
    point->messages=json_object_array_length(run->messages); return 0;
}

static void truncate_array(json_object *array, size_t length) {
    size_t count=json_object_array_length(array);
    if (count>length) json_object_array_del_idx(array,length,count-length);
}

/* Discard one client's planned changes so the other clients still apply. */
static void rollback(AdRun *run, Checkpoint *point, const char *agent) {
    json_object_put(run->state); run->state=point->state; point->state=NULL;
    truncate_array(run->links,point->links);
    truncate_array(run->subagent_actions,point->actions);
    truncate_array(run->messages,point->messages);
    ad_drop_files(run,agent);
}

static void add_failure(Km *km, char *failed, size_t size, const char *agent) {
    size_t used=strlen(failed);
    snprintf(failed+used,size-used,"%s%s: %.400s",used ? "; " : "",agent,km->error);
    km->error[0]=0;
}

/* Sync and restore isolate clients; import stays all-or-nothing because it
 * saves the catalog together with ownership. */
static int run_agents(AdRun *run, const char *verb, const bool flags[KM_AGENT_COUNT], bool configured_only,
                      bool subagents_only, char *failed, size_t size) {
    bool isolated=strcmp(verb,"import")!=0;
    for (int i=0;i<KM_AGENT_COUNT;i++) {
        const char *agent=km_agents[i];
        Scope scope=flags[i] ? scope_for(run,agent,configured_only,subagents_only) : SCOPE_NONE;
        Checkpoint point={0};
        if (scope==SCOPE_NONE) continue;
        if (isolated && checkpoint(run,&point)<0) return -1;
        if (process(run,verb,agent,scope)<0) {
            if (!isolated) return -1;
            add_failure(run->km,failed,size,agent); rollback(run,&point,agent); continue;
        }
        json_object_put(point.state);
        record_adoption(run,verb,agent,scope);
        ad_flush(run);
    }
    return 0;
}

static int report_failures(Km *km, const char *verb, const char *failed, int result) {
    char cause[sizeof(km->error)];
    snprintf(cause,sizeof(cause),"%s",result<0 ? km->error : "");
    return km_error(km,"%s failed for %.600s%s%.300s; other selected clients were processed",
                    verb,failed,*cause ? "; " : "",cause);
}

int km_adapter_command(Km *km, int argc, char **argv) {
    if (!argc) return km_error(km,"expected adapter command");
    const char *verb=argv[0], *csv=NULL;
    bool apply=false, apply_requested=false, check=false, configured_only=false;
    bool subagents_only=false, refresh=false, flags[KM_AGENT_COUNT];
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--apply")) { apply=true; apply_requested=true; }
        else if (!strcmp(argv[i],"--preview")) apply=false;
        else if (!strcmp(argv[i],"--configured")) configured_only=true;
        else if (!strcmp(argv[i],"--subagents")) subagents_only=true;
        else if (!strcmp(argv[i],"--refresh")) refresh=true;
        else if (!strcmp(argv[i],"--check")) check=true;
        else if ((!strcmp(argv[i],"--agent") || !strcmp(argv[i],"--from")) && i+1<argc) csv=argv[++i];
        else return km_error(km,"unknown adapter argument");
    }
    if (strcmp(verb,"sync") && strcmp(verb,"import") && strcmp(verb,"restore") && strcmp(verb,"doctor"))
        return km_error(km,"unknown adapter command");
    if (subagents_only && strcmp(verb,"import")) return km_error(km,"subagent selection requires import");
    if (refresh && strcmp(verb,"import")) return km_error(km,"--refresh requires explicit import");
    if (check && (strcmp(verb,"sync") || apply_requested))
        return km_error(km,"Use sync --check without --apply");
    if (selected(km,csv,flags)<0) return -1;
    AdRun run;
    if (ad_open(&run,km,apply)<0) { ad_cleanup(&run); return -1; }
    run.refresh=refresh;
    run.importing=!strcmp(verb,"import");
    run.restore=!strcmp(verb,"restore");
    run.check=check;
    run.management=json_object_new_object();
    int result=0;
    if (!strcmp(verb,"doctor")) result=doctor(&run,flags);
    else {
        char failed[sizeof(km->error)]="";
        result=run_agents(&run,verb,flags,configured_only,subagents_only,failed,sizeof(failed));
        /* Never claim native ownership without the desired imported resources:
         * a later sync could otherwise remove entries after a failed save. */
        if (result==0 && apply && !strcmp(verb,"import")) result=km_save(km);
        if (result==0 && !(check && *failed)) result=ad_finish(&run);
        if (*failed) result=report_failures(km,verb,failed,result);
    }
    ad_cleanup(&run); return result;
}
