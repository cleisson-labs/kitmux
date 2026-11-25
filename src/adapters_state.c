#include "adapters.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static json_object *object_at(json_object *parent, const char *key) {
    json_object *obj=km_get(parent,key);
    if (!obj) { obj=json_object_new_object(); json_object_object_add(parent,key,obj); }
    return obj;
}

int ad_open(AdRun *run, Km *km, bool apply) {
    memset(run,0,sizeof(*run)); run->km=km; run->apply=apply;
    char path[PATH_MAX]; struct stat st;
    if (km_path(km,path,sizeof(path),"%s/adapters.json",km->state)<0) return -1;
    if (lstat(path,&st)==0) {
        char *text=km_read_file(km,path,NULL);
        if (!text) return -1;
        run->state=ad_parse(km,text); free(text);
        if (!run->state) return -1;
    } else if (errno==ENOENT) {
        run->state=json_object_new_object();
        json_object_object_add(run->state,"version",json_object_new_int(1));
        json_object_object_add(run->state,"files",json_object_new_object());
    }
    else return km_error(km,"cannot inspect adapter ownership record");
    if (ad_validate_state(km,run->state)<0) return -1;
    object_at(run->state,"files");
    run->links=json_object_new_array();
    run->messages=json_object_new_array();
    run->created=json_object_new_object();
    run->subagent_actions=json_object_new_array(); return 0;
}

int ad_file_error(Km *km, const char *path) {
    char cause[sizeof(km->error)];
    snprintf(cause,sizeof(cause),"%s",km->error);
    return km_error(km,"%s: %.700s; fix the file, then retry",path,cause);
}

AdFile *ad_file(AdRun *run, const char *agent, const char *relative, bool toml) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/%s",run->km->home,relative)<0) return NULL;
    for (AdFile *f=run->files;f;f=f->next) if (!strcmp(f->path,path)) return f;
    AdFile *file=calloc(1,sizeof(*file));
    if (!file) { km_error(run->km,"out of memory"); return NULL; }
    snprintf(file->path,sizeof(file->path),"%s",path);
    snprintf(file->agent,sizeof(file->agent),"%s",agent); file->toml=toml;
    struct stat st;
    if (lstat(path,&st)==0) {
        file->existed=true;
        file->original=km_read_file(run->km,path,NULL);
        if (!file->original) { free(file); return NULL; }
    } else if (errno==ENOENT) file->original=strdup(toml ? "" : "{}\n");
    else { km_error(run->km,"cannot inspect native configuration for %s",agent); free(file); return NULL; }
    file->text=strdup(file->original);
    if (!file->text) { free(file->original); free(file); km_error(run->km,"out of memory"); return NULL; }
    if (!toml) {
        json_object *doc=ad_parse(run->km,file->text);
        bool valid=doc && json_object_is_type(doc,json_type_object);
        if (doc && !valid) km_error(run->km,"native configuration must be a JSON object");
        if (valid && ad_native_shape(run->km,agent,path,doc)<0) valid=false;
        if (doc) json_object_put(doc);
        if (!valid) { ad_file_error(run->km,path); free(file->text); free(file->original); free(file); return NULL; }
    }
    file->next=run->files; run->files=file; return file;
}

static json_object *file_record(AdRun *run, AdFile *file) {
    json_object *record=object_at(object_at(run->state,"files"),file->path);
    json_object_object_add(record,"agent",json_object_new_string(file->agent));
    json_object_object_add(record,"toml",json_object_new_boolean(file->toml));
    return object_at(record,"entries");
}

char *ad_entry_raw(Km *km, const AdFile *file, const char *key) {
    return file->toml ? ad_toml_raw(km,file->text,key) : ad_json_raw(km,file->text,key);
}

static bool mcp_entry(const AdFile *file, const char *key) {
    return file->toml || !strncmp(key,"mcp/servers/",12);
}

/* Clients drop explicit defaults (Codex enabled = true, OpenCode
 * disabled: false) when they rewrite configuration; that is not drift. */
static json_object *without_default(Km *km, const AdFile *file, const char *raw) {
    json_object *doc=file->toml ? ad_toml_parse(km,raw) : ad_parse(km,raw);
    const char *field=file->toml ? "enabled" : "disabled";
    json_object *value=km_get(doc,field);
    if (json_object_is_type(value,json_type_boolean) && json_object_get_boolean(value)==file->toml)
        json_object_object_del(doc,field);
    return doc;
}

bool ad_entry_same(Km *km, const AdFile *file, const char *key, const char *a, const char *b) {
    if (!a || !b || !strcmp(a,b) || !mcp_entry(file,key)) return ad_same(km,a,b,file->toml);
    char saved[sizeof(km->error)];
    memcpy(saved,km->error,sizeof(saved));
    json_object *left=without_default(km,file,a), *right=without_default(km,file,b);
    bool same=left && right && json_object_equal(left,right);
    json_object_put(left); json_object_put(right);
    memcpy(km->error,saved,sizeof(saved)); return same;
}

static bool owned(json_object *record) {
    return km_string(record,"before") || km_string(record,"after");
}

static bool accepted(AdRun *run, AdFile *file, const char *key, json_object *record,
                     const char *current, const char *desired) {
    Km *km=run->km;
    /* Ordinary import adopts current content, so only refresh may accept drift. */
    bool settles=!run->importing || run->refresh;
    if (ad_entry_same(km,file,key,current,km_string(record,"after")) ||
        (settles && ad_entry_same(km,file,key,current,desired))) return true;
    return json_object_get_boolean(km_get(run->state,"pending")) &&
           ad_entry_same(km,file,key,current,km_string(record,"previous"));
}

static json_object *claim(AdRun *run, AdFile *file, const char *key, const char *current) {
    json_object *entries=file_record(run,file), *record=km_get(entries,key);
    if (!record) { record=json_object_new_object(); json_object_object_add(entries,key,record); }
    if (current) json_object_object_add(record,"before",json_object_new_string(current));
    return record;
}

static int refuse(Km *km, AdFile *file, const char *key, bool ours) {
    if (ours)
        return km_error(km,"configuration conflict for %s in %s (%s): it changed outside Kitmux. Keep the edit "
                        "with kitmux import --refresh --from %s --apply, or revert it, then retry",
                        file->agent,file->path,key,file->agent);
    return km_error(km,"unowned %s entry %s in %s; adopt it with kitmux import --from %s --apply, "
                    "or rename it, then retry",file->agent,key,file->path,file->agent);
}

/* Kitmux owns an entry once it recorded original or written content. It never
 * edits unowned content, and accepts current content that already matches. */
int ad_change(AdRun *run, AdFile *file, const char *key, const char *desired, bool adopt) {
    Km *km=run->km;
    char *current=ad_entry_raw(km,file,key);
    if (km->error[0]) { free(current); return -1; }
    json_object *record=km_get(km_get(km_get(km_get(run->state,"files"),file->path),"entries"),key);
    bool ours=record && owned(record);
    if (!ours && !desired) { free(current); return 0; }
    if (ours ? !accepted(run,file,key,record,current,desired) :
        (current && !adopt && !ad_entry_same(km,file,key,current,desired))) {
        free(current); return refuse(km,file,key,ours);
    }
    if (!ours) record=claim(run,file,key,current);
    json_object_object_del(record,"previous");
    if (current) json_object_object_add(record,"previous",json_object_new_string(current));
    json_object_object_del(record,"after");
    if (desired) json_object_object_add(record,"after",json_object_new_string(desired));
    /* Sync leaves dropped defaults alone; restore writes the exact baseline. */
    bool same=run->restore ? ad_same(km,current,desired,file->toml) : ad_entry_same(km,file,key,current,desired);
    int result=0;
    if (!same) result=file->toml ? ad_toml_edit(km,&file->text,key,desired) : ad_json_edit(km,&file->text,key,desired);
    free(current); return result;
}

void ad_note(AdRun *run, const char *format, ...) {
    char line[512]; va_list arguments;
    va_start(arguments,format); vsnprintf(line,sizeof(line),format,arguments); va_end(arguments);
    json_object_array_add(run->messages,json_object_new_string(line));
}

/* Plan messages print only once their client's changes are kept. */
void ad_flush(AdRun *run) {
    size_t count=json_object_array_length(run->messages);
    for (size_t i=0;i<count;i++) puts(json_object_get_string(json_object_array_get_idx(run->messages,i)));
    if (count) json_object_array_del_idx(run->messages,0,count);
}

void ad_drop_files(AdRun *run, const char *agent) {
    for (AdFile **link=&run->files;*link;) {
        AdFile *file=*link;
        if (strcmp(file->agent,agent)) { link=&file->next; continue; }
        *link=file->next; free(file->original); free(file->text); free(file);
    }
}

static int write_state(AdRun *run, bool pending) {
    char path[PATH_MAX];
    if (km_path(run->km,path,sizeof(path),"%s/adapters.json",run->km->state)<0) return -1;
    json_object_object_add(run->state,"pending",json_object_new_boolean(pending));
    const char *text=json_object_to_json_string_ext(run->state,JSON_C_TO_STRING_PRETTY);
    return km_write_file(run->km,path,text,strlen(text),0600);
}

static int check_file(AdRun *run, AdFile *file) {
    struct stat st; bool exists=lstat(file->path,&st)==0;
    if (!exists && errno!=ENOENT) return km_error(run->km,"cannot recheck native configuration");
    if (exists!=file->existed) return km_error(run->km,"%s changed during sync; run the command again",file->path);
    if (!exists) return 0;
    char *now=km_read_file(run->km,file->path,NULL);
    if (!now) return -1;
    bool same=!strcmp(now,file->original); free(now);
    return same ? 0 : km_error(run->km,"%s changed during sync; run the command again",file->path);
}

static int sync_link_parent(Km *km, const char *path) {
    char parent[PATH_MAX];
    if (km_path(km,parent,sizeof(parent),"%s",path)<0) return -1;
    char *slash=strrchr(parent,'/');
    if (!slash || slash==parent) return km_error(km,"invalid skill projection parent");
    *slash=0;
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return km_error(km,"cannot open skill projection directory %s for sync: %s",parent,strerror(errno));
    int result=fsync(fd); close(fd);
    return result<0 ? km_error(km,"cannot sync skill projection directory %s",parent) : 0;
}

static int apply_link(AdRun *run, json_object *link) {
    const char *path=km_string(link,"path"), *target=km_string(link,"target");
    const char *before=km_string(link,"before");
    struct stat st; char actual[PATH_MAX];
    bool exists=lstat(path,&st)==0;
    if (exists) {
        ssize_t n=readlink(path,actual,sizeof(actual)-1);
        if (n<0 || (size_t)n>=sizeof(actual)-1) return km_error(run->km,"skill projection %s is no longer a link; retry `kitmux sync --apply`",path);
        actual[n]=0;
        if (!before || strcmp(actual,before)) return km_error(run->km,"skill projection %s changed during sync; retry `kitmux sync --apply`",path);
    } else if (errno!=ENOENT || before) return km_error(run->km,"skill projection %s changed during sync; retry `kitmux sync --apply`",path);
    if (target && exists && !strcmp(actual,target)) return 0;
    if (!target) {
        if (!exists) return 0;
        if (unlink(path)<0) return km_error(run->km,"cannot detach skill projection %s: %s",path,strerror(errno));
        return sync_link_parent(run->km,path);
    }
    char parent[PATH_MAX], temporary[PATH_MAX];
    snprintf(parent,sizeof(parent),"%s",path); *strrchr(parent,'/')=0;
    if (km_mkdirs(run->km,parent)<0) return -1;
    if (km_path(run->km,temporary,sizeof(temporary),"%s.kitmux-%ld",path,(long)getpid())<0) return -1;
    if (symlink(target,temporary)<0) return km_error(run->km,"cannot stage skill projection %s: %s",path,strerror(errno));
    if (rename(temporary,path)<0) {
        int saved=errno; unlink(temporary);
        return km_error(run->km,"cannot apply skill projection %s: %s",path,strerror(saved));
    }
    return sync_link_parent(run->km,path);
}

/* One unwritable file must not stop the other clients' native writes. */
static void write_native(AdRun *run, char *failed, size_t size) {
    for (AdFile *f=run->files;f;f=f->next) {
        if (!strcmp(f->original,f->text)) continue;
        if (check_file(run,f)<0 || km_write_file(run->km,f->path,f->text,strlen(f->text),0600)<0) {
            size_t used=strlen(failed);
            snprintf(failed+used,size-used,"%s%.300s",used ? "; " : "",run->km->error);
            run->km->error[0]=0; continue;
        }
        printf("%s: configuration applied; reload required\n",f->agent);
    }
}

static bool planned_changes(AdRun *run) {
    if (json_object_get_boolean(km_get(run->state,"pending")) || json_object_array_length(run->links)) return true;
    for (AdFile *file=run->files;file;file=file->next)
        if (strcmp(file->original,file->text)) return true;
    for (size_t i=0;i<json_object_array_length(run->subagent_actions);i++) {
        json_object *action=json_object_array_get_idx(run->subagent_actions,i);
        const char *before=km_string(action,"before"), *after=km_string(action,"after");
        if ((!before || !after) ? before!=after : strcmp(before,after)!=0) return true;
    }
    return false;
}

int ad_finish(AdRun *run) {
    if (ad_subagent_check_actions(run)<0) return -1;
    for (AdFile *f=run->files;f;f=f->next) {
        if (check_file(run,f)<0) return -1;
        json_object *checked=f->toml ? ad_toml_ids(run->km,f->text) : ad_parse(run->km,f->text);
        if (!checked) return -1;
        int valid=f->toml ? 0 : ad_native_shape(run->km,f->agent,f->path,checked);
        json_object_put(checked);
        if (valid<0) return -1;
    }
    if (run->check) {
        if (planned_changes(run)) return km_error(run->km,"Native sync changes are pending; run sync --preview or sync --apply");
        puts("Selected native projections are current."); return 0;
    }
    if (!run->apply) {
        for (AdFile *f=run->files;f;f=f->next)
            if (strcmp(f->original,f->text)) printf("preview: %s configuration update\n",f->agent);
        printf("preview: %zu skill projection actions; run with --apply\n",json_object_array_length(run->links));
        ad_subagent_preview(run);
        return 0;
    }
    if (write_state(run,true)<0) return -1;
    char failed[sizeof(run->km->error)]="";
    write_native(run,failed,sizeof(failed));
    for (size_t i=0;i<json_object_array_length(run->links);i++)
        if (apply_link(run,json_object_array_get_idx(run->links,i))<0) return -1;
    if (ad_subagent_apply_actions(run)<0) return -1;
    if (run->management) {
        json_object *management=object_at(run->state,"managed_agents");
        json_object_object_foreach(run->management,agent,value)
            json_object_object_add(management,agent,json_object_get(value));
    }
    if (!*failed) return write_state(run,false);
    /* Pending stays set so the next sync can recover the unwritten files. */
    if (write_state(run,true)<0) return -1;
    return km_error(run->km,"%.800s; other native changes were applied. Fix the cause, then run kitmux sync --apply",failed);
}

int ad_restore(AdRun *run, const char *agent) {
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        (void)path; const char *owner=km_string(record,"agent");
        if (agent && strcmp(agent,owner)) continue;
        if (strncmp(path,run->km->home,strlen(run->km->home)) || path[strlen(run->km->home)]!='/')
            return km_error(run->km,"ownership record points outside the selected home");
        const char *relative=path+strlen(run->km->home)+1;
        AdFile *file=ad_file(run,owner,relative,json_object_get_boolean(km_get(record,"toml")));
        if (!file) return -1;
        json_object *entries=km_get(record,"entries");
        json_object_object_foreach(entries,key,entry) {
            const char *raw=km_string(entry,"before"); char *copy=raw ? strdup(raw) : NULL;
            int result=ad_change(run,file,key,copy,true); free(copy);
            if (result<0) return -1;
        }
    }
    return 0;
}

void ad_cleanup(AdRun *run) {
    while (run->files) {
        AdFile *f=run->files; run->files=f->next;
        free(f->original); free(f->text); free(f);
    }
    if (run->state) json_object_put(run->state);
    if (run->links) json_object_put(run->links);
    if (run->messages) json_object_put(run->messages);
    if (run->created) json_object_put(run->created);
    if (run->management) json_object_put(run->management);
    if (run->subagent_actions) json_object_put(run->subagent_actions);
}
