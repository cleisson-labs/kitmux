#include "adapters.h"
#include "subagents.h"
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool same(const char *left, const char *right) {
    return (!left || !right) ? left==right : !strcmp(left,right);
}

static int read_current(Km *km, const char *path, char **text) {
    struct stat st; *text=NULL;
    if (ad_subagent_parent_safe(km,path)<0) return -1;
    if (lstat(path,&st)<0) return errno==ENOENT ? 0 : km_error(km,"Cannot inspect native subagent definition %s",path);
    if (!S_ISREG(st.st_mode)) return km_error(km,"Native subagent %s must be a regular file; symlinks are not adopted. Replace it with a regular file, then rerun the command",path);
    size_t length;
    *text=km_read_file(km,path,&length);
    if (!*text) return -1;
    if (strlen(*text)!=length) { free(*text); *text=NULL; return km_error(km,"Native subagent %s contains unsupported NUL bytes; remove them, then rerun the command",path); }
    return 0;
}

typedef enum { CHANGE_SYNC, CHANGE_IMPORT, CHANGE_RESTORE } ChangeMode;

static json_object *records(AdRun *run) {
    json_object *map=km_get(run->state,"subagent_files");
    if (!map) {
        map=json_object_new_object();
        json_object_object_add(run->state,"subagent_files",map);
        json_object_object_add(run->state,"version",json_object_new_int(2));
    }
    return map;
}

static void set_optional(json_object *object, const char *key, const char *value) {
    json_object_object_del(object,key);
    if (value) json_object_object_add(object,key,json_object_new_string(value));
}

static bool pending(AdRun *run) {
    return json_object_get_boolean(km_get(run->state,"pending"));
}

/* A record without a restore baseline or an owned file carries no information;
 * keeping it would turn a later owner-created file at that path into a conflict. */
static bool released(json_object *record) {
    return !km_string(record,"before") && !km_string(record,"after");
}

static bool accepted(AdRun *run, ChangeMode mode, json_object *record, const char *current, const char *desired) {
    if (same(current,km_string(record,"after"))) return true;
    if (pending(run) && same(current,km_string(record,"previous"))) return true;
    if (mode==CHANGE_IMPORT) return run->refresh && same(current,desired);
    if (same(current,desired)) return true;
    return mode==CHANGE_RESTORE && !current; /* restoring into an empty path loses nothing */
}

static int conflict(Km *km, ChangeMode mode, const char *agent, const char *path, const char *id, bool deleted) {
    if (mode==CHANGE_RESTORE)
        return km_error(km,"Native subagent conflict for %s at %s: it changed after Kitmux wrote it. Adopt it with "
                        "`kitmux subagent import --from %s --apply --refresh`, then rerun `kitmux restore --agent %s --apply`",
                        agent,path,agent,agent);
    if (deleted)
        return km_error(km,"Native subagent conflict for %s at %s: the owned file was deleted outside Kitmux. Accept the "
                        "removal with `kitmux subagent disable %s --agent %s`; enabling it again recreates the file",
                        agent,path,id,agent);
    return km_error(km,"Native subagent conflict for %s at %s: the file changed outside Kitmux. Keep it with "
                    "`kitmux subagent import --from %s --apply --refresh`, or move it aside and run `kitmux sync --apply --agent %s`",
                    agent,path,agent,agent);
}

static int recordable(Km *km, const char *path, const char *id, const char *format, const char *desired) {
    if (!desired) return 0;
    json_object *parsed=km_subagent_parse(km,desired,format,id);
    if (parsed) { json_object_put(parsed); return 0; }
    char cause[512]; snprintf(cause,sizeof(cause),"%s",km->error);
    return km_error(km,"Refusing to record an unparseable %s definition at %s (%s); the native file was not changed",format,path,cause);
}

static json_object *new_record(AdRun *run, const char *path, const char *agent, const char *id,
                               const char *format, const char *current) {
    json_object *record=json_object_new_object();
    json_object_object_add(record,"agent",json_object_new_string(agent));
    json_object_object_add(record,"id",json_object_new_string(id));
    json_object_object_add(record,"format",json_object_new_string(format));
    set_optional(record,"before",current);
    json_object_object_add(records(run),path,record);
    return record;
}

static void plan(AdRun *run, json_object *record, const char *path, const char *agent, const char *id,
                 const char *current, const char *desired) {
    set_optional(record,"previous",current); set_optional(record,"after",desired);
    json_object *action=json_object_new_object();
    json_object_object_add(action,"path",json_object_new_string(path));
    json_object_object_add(action,"agent",json_object_new_string(agent));
    json_object_object_add(action,"id",json_object_new_string(id));
    set_optional(action,"before",current); set_optional(action,"after",desired);
    json_object_array_add(run->subagent_actions,action);
}

static int change(AdRun *run, ChangeMode mode, const char *agent, const char *target,
                  const char *identifier, const char *kind, const char *desired) {
    Km *km=run->km;
    /* Callers may pass strings owned by the record that a release deletes below. */
    char path[PATH_MAX], id[80], format[32];
    if (km_path(km,path,sizeof(path),"%s",target)<0 || km_path(km,id,sizeof(id),"%s",identifier)<0 ||
        km_path(km,format,sizeof(format),"%s",kind)<0) return -1;
    if (!ad_subagent_path_valid(km,path,agent,id)) return km_error(km,"Unsafe native subagent target %s",path);
    if (recordable(km,path,id,format,desired)<0) return -1;
    char *current=NULL;
    if (read_current(km,path,&current)<0) return -1;
    json_object *map=km_get(run->state,"subagent_files"), *record=km_get(map,path);
    if (record && released(record) && !pending(run)) { json_object_object_del(map,path); record=NULL; }
    int result=0;
    if (!record && !desired) { free(current); return 0; }
    if (record && !accepted(run,mode,record,current,desired))
        result=conflict(km,mode,agent,path,id,!current);
    else if (!record && current && mode!=CHANGE_IMPORT)
        result=km_error(km,"Unowned subagent collision for %s at %s. Adopt it with `kitmux subagent import --from %s --apply`, "
                        "or move it aside and run `kitmux sync --apply --agent %s`",agent,path,agent,agent);
    if (result<0) { free(current); return result; }
    /* A refresh that adopts an owner's file records it as the restore baseline. */
    if (record && mode==CHANGE_IMPORT && current && !km_string(record,"before") && !same(current,km_string(record,"after")))
        set_optional(record,"before",current);
    if (!record) record=new_record(run,path,agent,id,format,current);
    plan(run,record,path,agent,id,current,desired);
    free(current); return 0;
}

int ad_subagent_change(AdRun *run, const char *agent, const char *path,
                       const char *id, const char *format, const char *desired, bool adopt) {
    return change(run,adopt ? CHANGE_IMPORT : CHANGE_SYNC,agent,path,id,format,desired);
}

static int check_action(AdRun *run, json_object *action) {
    char *current=NULL;
    if (read_current(run->km,km_string(action,"path"),&current)<0) return -1;
    bool equal=same(current,km_string(action,"before")); free(current);
    return equal ? 0 : km_error(run->km,"Native subagent file %s changed during sync; nothing was overwritten. Rerun `kitmux sync --apply`",km_string(action,"path"));
}

int ad_subagent_check_actions(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++)
        if (check_action(run,json_object_array_get_idx(run->subagent_actions,i))<0) return -1;
    return 0;
}

static int sync_parent(Km *km, const char *path) {
    char parent[PATH_MAX];
    if (km_path(km,parent,sizeof(parent),"%s",path)<0) return -1;
    *strrchr(parent,'/')=0;
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return km_error(km,"Cannot open native subagent directory %s for sync",parent);
    int result=fsync(fd); close(fd);
    return result<0 ? km_error(km,"Native subagent directory %s sync failed",parent) : 0;
}

static int apply_action(AdRun *run, json_object *action) {
    const char *path=km_string(action,"path"), *desired=km_string(action,"after");
    const char *before=km_string(action,"before");
    if (check_action(run,action)<0) return -1;
    if (same(before,desired)) return 0;
    if (desired) {
        if (km_write_file(run->km,path,desired,strlen(desired),0600)<0) return -1;
    } else if (before && (unlink(path)<0 || sync_parent(run->km,path)<0))
        return km_error(run->km,"Cannot remove owned native subagent %s; check its permissions, then rerun `kitmux sync --apply`",path);
    printf("%s: subagent %s %s; reload required\n",km_string(action,"agent"),km_string(action,"id"),desired ? "available" : "disabled");
    return 0;
}

static void release_records(AdRun *run) {
    json_object *map=km_get(run->state,"subagent_files");
    if (!map) return;
    for (;;) {
        const char *stale=NULL;
        json_object_object_foreach(map,path,record) if (released(record)) { stale=path; break; }
        if (!stale) return;
        json_object_object_del(map,stale);
    }
}

int ad_subagent_apply_actions(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++)
        if (apply_action(run,json_object_array_get_idx(run->subagent_actions,i))<0) return -1;
    release_records(run);
    return 0;
}

void ad_subagent_preview(AdRun *run) {
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++) {
        json_object *action=json_object_array_get_idx(run->subagent_actions,i);
        if (!same(km_string(action,"before"),km_string(action,"after")))
            printf("preview: %s subagent %s %s\n",km_string(action,"agent"),km_string(action,"id"),km_string(action,"after") ? "available" : "disabled");
    }
}

int ad_restore_subagents(AdRun *run, const char *agent) {
    json_object *map=km_get(run->state,"subagent_files");
    if (!map) return 0;
    json_object_object_foreach(map,path,record) {
        const char *owner=km_string(record,"agent");
        if (strcmp(owner,agent)) continue;
        const char *raw=km_string(record,"before"); char *copy=raw ? strdup(raw) : NULL;
        int result=change(run,CHANGE_RESTORE,agent,path,km_string(record,"id"),km_string(record,"format"),copy);
        free(copy); if (result<0) return -1;
    }
    return 0;
}
