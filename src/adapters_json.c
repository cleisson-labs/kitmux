#include "adapters.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t key, value, end, comma, previous, close, first, last; bool found, any; } Field;
static size_t whitespace(const char *s, size_t p);
static size_t value_end(const char *s, size_t p);
static int check_keys(Km *km, const char *text, size_t *cursor);

json_object *ad_parse(Km *km, const char *text) {
    struct json_tokener *tok = json_tokener_new();
    if (!tok) { km_error(km, "out of memory"); return NULL; }
    size_t length = strlen(text);
    json_object *doc = json_tokener_parse_ex(tok, text, (int)length+1);
    size_t end = json_tokener_get_parse_end(tok);
    enum json_tokener_error error = json_tokener_get_error(tok);
    json_tokener_free(tok);
    end=whitespace(text,end);
    if (error != json_tokener_success || end != length || !doc) {
        if (doc) json_object_put(doc);
        km_error(km, "invalid native JSON/JSONC configuration"); return NULL;
    }
    size_t cursor=0;
    if (check_keys(km,text,&cursor)<0) { json_object_put(doc); return NULL; }
    return doc;
}

static size_t whitespace(const char *s, size_t p) {
    for (;;) {
        while (isspace((unsigned char)s[p])) p++;
        if (s[p]=='/' && s[p+1]=='/') { while (s[p] && s[p]!='\n') p++; continue; }
        if (s[p]=='/' && s[p+1]=='*') {
            p+=2; while (s[p] && !(s[p]=='*' && s[p+1]=='/')) p++;
            if (s[p]) p+=2;
            continue;
        }
        return p;
    }
}

static size_t string_end(const char *s, size_t p) {
    p++;
    while (s[p]) {
        if (s[p]=='\\' && s[p+1]) p+=2;
        else if (s[p++]=='"') break;
    }
    return p;
}

static size_t value_end(const char *s, size_t p) {
    if (s[p]=='"') return string_end(s,p);
    if (s[p]=='{' || s[p]=='[') {
        char close = s[p++]=='{' ? '}' : ']';
        for (;;) {
            p=whitespace(s,p);
            if (!s[p] || s[p]==close) return s[p] ? p+1 : p;
            if (s[p]=='"') p=string_end(s,p);
            else if (s[p]=='{' || s[p]=='[') p=value_end(s,p);
            else p++;
        }
    }
    while (s[p] && !strchr(",]} \t\r\n",s[p]) && s[p]!='/') p++;
    return p;
}

static int check_keys(Km *km, const char *text, size_t *cursor) {
    size_t p=whitespace(text,*cursor);
    if (text[p]!='{' && text[p]!='[') { *cursor=value_end(text,p); return 0; }
    bool object=text[p]=='{'; char close=object ? '}' : ']';
    json_object *seen=object ? json_object_new_object() : NULL; p++;
    while (text[p]) {
        p=whitespace(text,p);
        if (text[p]==close) { *cursor=p+1; if (seen) json_object_put(seen); return 0; }
        if (object) {
            if (text[p]!='"') break;
            size_t end=string_end(text,p); char *raw=strndup(text+p,end-p);
            json_object *name=json_tokener_parse(raw); free(raw);
            const char *key=name ? json_object_get_string(name) : NULL;
            bool valid=key && strlen(key)==(size_t)json_object_get_string_len(name) && !km_get(seen,key);
            if (!valid) { if (name) json_object_put(name); json_object_put(seen); return km_error(km,"duplicate or invalid key in native JSON/JSONC configuration"); }
            json_object_object_add(seen,key,json_object_new_boolean(true)); json_object_put(name);
            p=whitespace(text,end); if (text[p++]!=':') break;
        }
        if (check_keys(km,text,&p)<0) { if (seen) json_object_put(seen); return -1; }
        p=whitespace(text,p);
        if (text[p]==',') p++;
        else if (text[p]!=close) break;
    }
    if (seen) json_object_put(seen);
    return km_error(km,"unsupported native JSON/JSONC syntax");
}

static bool matching_key(const char *s, size_t begin, size_t end, const char *key) {
    char *raw=strndup(s+begin,end-begin);
    json_object *obj=json_tokener_parse(raw);
    bool equal=obj && json_object_is_type(obj,json_type_string) && !strcmp(json_object_get_string(obj),key);
    if (obj) json_object_put(obj);
    free(raw); return equal;
}

static Field field(const char *s, const char *key) {
    Field result={0};
    size_t p=whitespace(s,0), previous=0;
    if (s[p++]!='{') return result;
    for (;;) {
        p=whitespace(s,p);
        if (s[p]=='}') { result.close=p; result.previous=previous; return result; }
        if (s[p]!='"') return result;
        size_t start=p, key_end=string_end(s,p);
        p=whitespace(s,key_end);
        if (s[p++]!=':') return result;
        size_t value=whitespace(s,p), end=value_end(s,value), comma=whitespace(s,end);
        if (!result.any) result.first=start;
        result.any=true; result.last=end;
        if (matching_key(s,start,key_end,key)) {
            result.key=start; result.value=value; result.end=end;
            result.comma=s[comma]==',' ? comma : 0;
            result.previous=previous; result.found=true; return result;
        }
        previous=s[comma]==',' ? comma : 0;
        p=s[comma]==',' ? comma+1 : comma;
    }
}

int ad_replace(Km *km, char **text, size_t start, size_t end, const char *value) {
    size_t length=strlen(*text), added=value ? strlen(value) : 0;
    if (start>end || end>length || length-(end-start)+added>KM_MAX_FILE)
        return km_error(km,"native configuration exceeds supported size");
    char *next=malloc(length-(end-start)+added+1);
    if (!next) return km_error(km,"out of memory");
    memcpy(next,*text,start);
    if (added) memcpy(next+start,value,added);
    memcpy(next+start+added,*text+end,length-end+1);
    free(*text); *text=next; return 0;
}

static int split_path(Km *km, const char *path, char *key, size_t size, const char **rest) {
    const char *slash=strchr(path,'/');
    size_t n=slash ? (size_t)(slash-path) : strlen(path);
    if (!n || n>=size) return km_error(km,"invalid native configuration path");
    memcpy(key,path,n); key[n]=0; *rest=slash ? slash+1 : NULL; return 0;
}

char *ad_json_raw(Km *km, const char *text, const char *path) {
    char key[256]; const char *rest;
    if (split_path(km,path,key,sizeof(key),&rest)<0) return NULL;
    Field f=field(text,key);
    if (!f.found) return NULL;
    char *raw=strndup(text+f.value,f.end-f.value);
    if (!raw) { km_error(km,"out of memory"); return NULL; }
    if (!rest) return raw;
    char *nested=ad_json_raw(km,raw,rest); free(raw); return nested;
}

typedef struct { char text[128]; } Indent;

static size_t line_start(const char *s, size_t p) {
    while (p && s[p-1]!='\n') p--;
    return p;
}

static bool starts_line(const char *s, size_t p) {
    for (size_t i=line_start(s,p);i<p;i++) if (s[i]!=' ' && s[i]!='\t') return false;
    return true;
}

/* Nested raw values start mid-line, so their first line inherits base. */
static Indent line_indent(const char *s, size_t p, const char *base) {
    Indent indent; size_t start=line_start(s,p), n=0;
    while (s[start+n]==' ' || s[start+n]=='\t') n++;
    snprintf(indent.text,sizeof(indent.text),"%s%.*s",start ? "" : base,(int)n,s+start);
    return indent;
}

static Indent member_indent(const char *s, Field f, const char *base) {
    if (f.any && starts_line(s,f.first)) return line_indent(s,f.first,base);
    Indent indent=line_indent(s,f.close,base);
    size_t n=strlen(indent.text);
    snprintf(indent.text+n,sizeof(indent.text)-n,"%s",strchr(indent.text,'\t') ? "\t" : "  ");
    return indent;
}

/* Rebase a multi-line value, whose closing line sets its own indentation. */
static char *reindent(Km *km, const char *value, const char *indent) {
    const char *last=strrchr(value,'\n');
    size_t strip=0, lines=0, width=strlen(indent);
    while (last && (last[1+strip]==' ' || last[1+strip]=='\t')) strip++;
    for (const char *c=value;*c;c++) lines+=*c=='\n';
    char *out=malloc(strlen(value)+lines*width+1), *w=out;
    if (!out) { km_error(km,"out of memory"); return NULL; }
    for (const char *c=value;*c;) {
        *w++=*c;
        if (*c++!='\n') continue;
        for (size_t i=0;i<strip && (*c==' ' || *c=='\t');i++) c++;
        memcpy(w,indent,width); w+=width;
    }
    *w=0; return out;
}

static char *member_text(Km *km, const char *key, const char *value, const char *indent, bool own_line,
                         const char *closing) {
    json_object *quoted=json_object_new_string(key);
    const char *name=json_object_to_json_string_ext(quoted,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    char *body=reindent(km,value,indent), *text=NULL;
    size_t n=body ? strlen(name)+strlen(body)+2*strlen(indent)+strlen(closing)+8 : 0;
    if (body && (text=malloc(n))) {
        if (own_line) snprintf(text,n,"%s%s: %s\n",indent,name,body);
        else snprintf(text,n,"\n%s%s: %s\n%s",indent,name,body,closing);
    } else if (body) km_error(km,"out of memory");
    json_object_put(quoted); free(body); return text;
}

static int insert_field(Km *km, char **text, const char *key, const char *value, Field f, const char *base) {
    if (!f.close && (*text)[whitespace(*text,0)]!='{') return km_error(km,"native parent must be an object");
    bool own_line=starts_line(*text,f.close);
    Indent indent=member_indent(*text,f,base), closing=line_indent(*text,f.close,base);
    char *insert=member_text(km,key,value,indent.text,own_line,closing.text);
    if (!insert) return -1;
    size_t at=own_line ? line_start(*text,f.close) : f.close;
    int result=ad_replace(km,text,at,at,insert); free(insert);
    if (!result && f.any && !f.previous) result=ad_replace(km,text,f.last,f.last,",");
    return result;
}

/* Remove a member's whole line when nothing else shares it. */
static void widen_to_lines(const char *s, size_t *start, size_t *end) {
    size_t p=*end;
    while (s[p]==' ' || s[p]=='\t' || s[p]=='\r') p++;
    if (!starts_line(s,*start) || !line_start(s,*start) || (s[p] && s[p]!='\n')) return;
    *start=line_start(s,*start); *end=s[p] ? p+1 : p;
}

static int edit_leaf(Km *km, char **text, const char *key, const char *value, const char *base) {
    Field f=field(*text,key);
    if (!f.found) return value ? insert_field(km,text,key,value,f,base) : 0;
    if (value) return ad_replace(km,text,f.value,f.end,value);
    size_t start=f.key, end=f.comma ? f.comma+1 : f.end;
    widen_to_lines(*text,&start,&end);
    if (ad_replace(km,text,start,end,"")<0) return -1;
    return !f.comma && f.previous ? ad_replace(km,text,f.previous,f.previous+1,"") : 0;
}

static int edit_path(Km *km, char **text, const char *path, const char *value, const char *base) {
    char key[256]; const char *rest;
    if (split_path(km,path,key,sizeof(key),&rest)<0) return -1;
    if (!rest) return edit_leaf(km,text,key,value,base);
    Field f=field(*text,key);
    if (!f.found && !value) return 0;
    char *nested=f.found ? strndup(*text+f.value,f.end-f.value) : strdup("{}");
    if (!nested) return km_error(km,"out of memory");
    /* A new object is rebased when inserted; an existing one keeps its column. */
    Indent inner=f.found ? line_indent(*text,f.value,base) : (Indent){""};
    int result=edit_path(km,&nested,rest,value,inner.text);
    if (result==0) result=edit_leaf(km,text,key,nested,base);
    free(nested); return result;
}

int ad_json_edit(Km *km, char **text, const char *path, const char *value) {
    return edit_path(km,text,path,value,"");
}

/* Comparison never fails a run: text that does not parse simply differs. */
bool ad_same(Km *km, const char *a, const char *b, bool toml) {
    if (!a || !b) return a==b;
    if (!strcmp(a,b)) return true;
    char saved[sizeof(km->error)];
    memcpy(saved,km->error,sizeof(saved));
    json_object *left=toml ? ad_toml_parse(km,a) : ad_parse(km,a);
    json_object *right=toml ? ad_toml_parse(km,b) : ad_parse(km,b);
    bool same=left && right && json_object_equal(left,right);
    if (left) json_object_put(left);
    if (right) json_object_put(right);
    memcpy(km->error,saved,sizeof(saved));
    return same;
}

int ad_array_append(Km *km, char **text, const char *value) {
    size_t p=whitespace(*text,0); bool any=false, comma=false;
    if ((*text)[p++]!='[') return km_error(km,"native permissions must be an array");
    for (;;) {
        p=whitespace(*text,p);
        if ((*text)[p]==']') break;
        if (!(*text)[p]) return km_error(km,"invalid native array");
        p=value_end(*text,p); p=whitespace(*text,p); any=true;
        comma=(*text)[p]==',';
        if (comma) p++;
    }
    size_t n=strlen(value)+8; char *insert=malloc(n);
    if (!insert) return km_error(km,"out of memory");
    snprintf(insert,n,"%s\n%s\n",any && !comma ? "," : "",value);
    int result=ad_replace(km,text,p,p,insert); free(insert); return result;
}
