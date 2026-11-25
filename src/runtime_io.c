#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double rt_clock(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

int rt_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int write_bytes(Km *km, int fd, const char *bytes, size_t length) {
    double deadline = rt_clock() + 5;
    while (length) {
        double remaining = deadline - rt_clock();
        if (remaining <= 0) return km_error(km, "MCP output timed out");
        struct pollfd item = {fd, POLLOUT, 0};
        int ready = poll(&item, 1, (int)(remaining * 1000));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || rt_clock() >= deadline)
            return km_error(km, "MCP output timed out");
        ssize_t n = write(fd, bytes, length > 4096 ? 4096 : length);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return km_error(km, "MCP connection closed while writing");
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

int rt_send_line(Km *km, int fd, const char *line, size_t length) {
    if (length > RT_MAX_LINE) return km_error(km, "MCP message exceeds 1 MiB");
    if (write_bytes(km, fd, line, length)) return -1;
    return write_bytes(km, fd, "\n", 1);
}

int rt_send(Km *km, int fd, json_object *message) {
    const char *text = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
    return rt_send_line(km, fd, text, strlen(text));
}

static json_object *envelope(json_object *id) {
    json_object *packet = json_object_new_object();
    json_object_object_add(packet, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(packet, "id", json_object_get(id));
    return packet;
}

int rt_error(Rt *rt, json_object *id, int code, const char *message) {
    json_object *packet = envelope(id), *error = json_object_new_object();
    json_object_object_add(error, "code", json_object_new_int(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(packet, "error", error);
    int result = rt_send(rt->km, STDOUT_FILENO, packet);
    json_object_put(packet);
    return result;
}

int rt_result(Rt *rt, json_object *id, json_object *result) {
    json_object *packet = envelope(id);
    json_object_object_add(packet, "result", result);
    int status = rt_send(rt->km, STDOUT_FILENO, packet);
    json_object_put(packet);
    return status;
}

static void compact(RtReader *reader) {
    if (!reader->consumed) return;
    memmove(reader->bytes, reader->bytes + reader->consumed, reader->length - reader->consumed);
    reader->length -= reader->consumed;
    reader->consumed = 0;
    reader->line = NULL;
    reader->line_length = 0;
}

int rt_read(Km *km, int fd, RtReader *reader) {
    char bytes[4096];
    compact(reader);
    size_t room = RT_MAX_LINE - reader->length;
    if (!room) return km_error(km, "MCP message exceeds 1 MiB");
    ssize_t n = read(fd, bytes, room < sizeof(bytes) ? room : sizeof(bytes));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 1;
    if (n <= 0) return n == 0 ? 0 : km_error(km, "MCP input failed");
    if (memchr(bytes, '\0', (size_t)n)) return km_error(km, "MCP input contains a NUL byte");
    memcpy(reader->bytes + reader->length, bytes, (size_t)n);
    reader->length += (size_t)n;
    return 1;
}

bool rt_identifier(json_object *value) {
    return json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_string);
}

bool rt_named_id(json_object *id, const char *name) {
    return json_object_is_type(id, json_type_string) && strlen(name) == (size_t)json_object_get_string_len(id) &&
           !strcmp(json_object_get_string(id), name);
}

static bool valid_envelope(json_object *message) {
    if (!json_object_is_type(message, json_type_object)) return false;
    const char *version = km_string(message, "jsonrpc");
    json_object *id = NULL;
    bool has_id = json_object_object_get_ex(message, "id", &id);
    return version && !strcmp(version, "2.0") && (!has_id || id == NULL || rt_identifier(id));
}

static json_object *parse(const char *text, size_t length, enum json_tokener_error *error) {
    json_tokener *parser = json_tokener_new_ex(RT_JSON_DEPTH);
    if (!parser) { *error = json_tokener_error_parse_unexpected; return NULL; }
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *value = json_tokener_parse_ex(parser, text, (int)length);
    *error = json_tokener_get_error(parser);
    size_t used = json_tokener_get_parse_end(parser);
    json_tokener_free(parser);
    if (*error == json_tokener_success && used == length) return value;
    if (*error == json_tokener_success) *error = json_tokener_error_parse_unexpected;
    json_object_put(value);
    return NULL;
}

/* json-c cannot represent integers beyond 64 bits. Inspect a private copy in
 * which such literals become 0.0, which is never a valid ID or progress token;
 * forwarding always uses the original line, so no precision is lost. */
static json_object *parse_wide_integers(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length;) {
        if (text[i] == '"') {
            size_t start = i++;
            while (i < length && text[i] != '"') i += text[i] == '\\' ? 2 : 1;
            i = i < length ? i + 1 : length;
            memcpy(copy + used, text + start, i - start); used += i - start;
        } else if (text[i] == '-' || isdigit((unsigned char)text[i])) {
            size_t start = i, first = i + (text[i] == '-');
            for (i = first; i < length && isdigit((unsigned char)text[i]); i++) {}
            size_t digits = i - first;
            bool integer = !(i < length && strchr(".eE", text[i]));
            while (i < length && strchr("0123456789.eE+-", text[i])) i++;
            if (integer && digits > 18 && text[first] != '0') { memcpy(copy + used, "0.0", 3); used += 3; }
            else { memcpy(copy + used, text + start, i - start); used += i - start; }
        } else copy[used++] = text[i++];
    }
    enum json_tokener_error error;
    json_object *value = parse(copy, used, &error);
    free(copy);
    return value;
}

int rt_next(Km *km, RtReader *reader, json_object **message) {
    *message = NULL;
    for (;;) {
        compact(reader);
        char *newline = memchr(reader->bytes, '\n', reader->length);
        if (!newline) return 0;
        size_t length = (size_t)(newline - reader->bytes), start = 0;
        reader->consumed = length + 1;
        while (length && isspace((unsigned char)reader->bytes[length - 1])) length--;
        while (start < length && isspace((unsigned char)reader->bytes[start])) start++;
        reader->bytes[length] = '\0';
        reader->line = reader->bytes + start;
        reader->line_length = length - start;
        if (!reader->line_length) continue;
        enum json_tokener_error error;
        *message = parse(reader->line, reader->line_length, &error);
        if (!*message && error == json_tokener_error_parse_number)
            *message = parse_wide_integers(reader->line, reader->line_length);
        if (valid_envelope(*message)) return 1;
        json_object_put(*message);
        *message = NULL;
        return km_error(km, "Invalid MCP JSON-RPC message");
    }
}

bool rt_noise(RtReader *reader) {
    return reader->line && reader->line[0] != '{' && ++reader->noise <= RT_MAX_NOISE;
}

static size_t string_end(const char *text, size_t length, size_t i) {
    for (i++; i < length; i++) {
        if (text[i] == '\\') i++;
        else if (text[i] == '"') return i;
    }
    return length;
}

static json_object *id_value(const char *text, size_t length, size_t i) {
    while (i < length && isspace((unsigned char)text[i])) i++;
    if (i < length && text[i] == '"') {
        size_t end = string_end(text, length, i);
        enum json_tokener_error error;
        json_object *value = end < length ? parse(text + i, end + 1 - i, &error) : NULL;
        if (json_object_is_type(value, json_type_string)) return value;
        json_object_put(value);
        return NULL;
    }
    char digits[24];
    size_t count = 0;
    while (i < length && count + 1 < sizeof(digits) && (isdigit((unsigned char)text[i]) || (!count && text[i] == '-')))
        digits[count++] = text[i++];
    digits[count] = '\0';
    if (!count || (i < length && !strchr(",} \t\r", text[i]))) return NULL;
    errno = 0;
    char *end = NULL;
    long long number = strtoll(digits, &end, 10);
    return errno || *end ? NULL : json_object_new_int64(number);
}

/* Recover the top-level ID of a line json-c rejected (for example, one nested
 * beyond RT_JSON_DEPTH) so the error can be correlated with its request. */
json_object *rt_line_id(const RtReader *reader) {
    const char *text = reader->line;
    size_t length = reader->line_length;
    int depth = 0;
    for (size_t i = 0; text && i < length; i++) {
        if (text[i] == '"') {
            size_t end = string_end(text, length, i), next = end + 1;
            if (end >= length) return NULL;
            while (next < length && isspace((unsigned char)text[next])) next++;
            if (depth == 1 && next < length && text[next] == ':' && end - i == 3 && !memcmp(text + i + 1, "id", 2))
                return id_value(text, length, next + 1);
            i = end;
        } else if (text[i] == '{' || text[i] == '[') depth++;
        else if (text[i] == '}' || text[i] == ']') depth--;
    }
    return NULL;
}

void rt_fail_pending(Rt *rt, const char *message) {
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        if (!json_object_get_boolean(km_get(record, "cancelled")) &&
            rt_error(rt, km_get(record, "id"), -32000, message)) break;
    }
    json_object_put(rt->pending);
    rt->pending = json_object_new_object();
}
