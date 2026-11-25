#include "subagents.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Quoting for native definitions. Both quoters emit only escapes that Kitmux's
 * own parsers (libyaml and the vendored TOML parser) and the clients accept, so
 * every rendered definition parses back. Hex escapes are uppercase because the
 * vendored TOML parser historically rejected lowercase digits. */

typedef struct { char *text; size_t length, capacity; } Buffer;

static bool put(Buffer *out, const char *bytes, size_t count) {
    if (out->length + count + 1 > out->capacity) {
        size_t capacity = (out->length + count + 1) * 2;
        char *text = realloc(out->text, capacity);
        if (!text) return false;
        out->text = text; out->capacity = capacity;
    }
    memcpy(out->text + out->length, bytes, count);
    out->length += count; out->text[out->length] = '\0';
    return true;
}

static const char *short_escape(unsigned char ch) {
    switch (ch) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\b': return "\\b";
    case '\t': return "\\t";
    case '\n': return "\\n";
    case '\f': return "\\f";
    case '\r': return "\\r";
    default: return NULL;
    }
}

/* Decodes one UTF-8 sequence; returns its length or 0 when malformed. */
static size_t decode(const unsigned char *p, unsigned long *code) {
    size_t length = *p < 0x80 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
    if (!length) return 0;
    *code = length == 1 ? *p : *p & (0x7f >> length);
    for (size_t i = 1; i < length; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        *code = (*code << 6) | (p[i] & 0x3f);
    }
    return length;
}

/* YAML treats C1 controls, NEL, LS, PS, BOM and U+FFFE/FFFF specially. */
static bool yaml_escaped(unsigned long code) {
    return code < 0x20 || code == 0x7f || (code >= 0x80 && code <= 0x9f) ||
           code == 0x2028 || code == 0x2029 || code == 0xfeff || code == 0xfffe || code == 0xffff;
}

static char *quote(const char *value, bool yaml) {
    Buffer out = {0};
    bool ok = put(&out, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)value; ok && *p;) {
        unsigned long code = *p; size_t length = yaml ? decode(p, &code) : 1;
        if (!length) length = 1;
        const char *escape = short_escape(*p);
        char hex[16];
        if (escape) ok = put(&out, escape, strlen(escape));
        else if (yaml ? yaml_escaped(code) : (code < 0x20 || code == 0x7f)) {
            snprintf(hex, sizeof(hex), "\\u%04lX", code);
            ok = put(&out, hex, strlen(hex));
        } else ok = put(&out, (const char *)p, length);
        p += length;
    }
    if (ok) ok = put(&out, "\"", 1);
    if (!ok) { free(out.text); return NULL; }
    return out.text;
}

char *km_subagent_yaml_quote(const char *value) { return quote(value, true); }

char *km_subagent_toml_quote(const char *value) { return quote(value, false); }
