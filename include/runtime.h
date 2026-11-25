#ifndef KITMUX_RUNTIME_H
#define KITMUX_RUNTIME_H
#include "kitmux.h"
#include <sys/types.h>
#include <time.h>

#define RT_MAX_LINE (1024u * 1024u)
#define RT_MAX_PENDING 64
#define RT_JSON_DEPTH 512
#define RT_MAX_NOISE 64
#define RT_START_SECONDS 30.0
#define RT_RECONNECT_SECONDS (RT_START_SECONDS + 5.0)
#define RT_RETRY_MAX_SECONDS 30.0
#define RT_EOF_GRACE_SECONDS 1.0
#define RT_TERM_GRACE_SECONDS 0.3

/* The current line stays valid until the next rt_next/rt_read on its reader. */
typedef struct {
    char *bytes;
    size_t length;
    size_t consumed;
    const char *line;
    size_t line_length;
    unsigned noise;
} RtReader;
typedef struct {
    bool exists;
    dev_t dev;
    ino_t inode;
    off_t size;
    struct timespec modified;
    struct timespec changed;
} RtStamp;
typedef struct {
    Km *km;
    const char *id;
    const char *agent;
    bool enabled;
    bool initialized;
    bool control_bound;
    bool starting;          /* upstream initialize sent; requests queue meanwhile */
    bool discovering;       /* reconnect discovery request is outstanding */
    bool client_stale;      /* a client tools/list failed; announce recovery */
    bool reconnect_waiting; /* reconnect_fd awaits the discovery result */
    pid_t child;
    int upstream_in;
    int upstream_out;
    char upstream_protocol[16];
    int control;
    int reconnect_fd;
    char socket_path[PATH_MAX];
    char failure[256];
    unsigned failures;
    double start_deadline;
    double retry_at;
    double reconnect_deadline;
    json_object *execution;
    json_object *initialize;
    json_object *pending;
    RtReader downstream;
    RtReader upstream;
    RtStamp catalog;
} Rt;

double rt_clock(void);
int rt_nonblock(int fd);
int rt_send(Km *km, int fd, json_object *message);
int rt_send_line(Km *km, int fd, const char *line, size_t length);
int rt_error(Rt *rt, json_object *id, int code, const char *message);
int rt_result(Rt *rt, json_object *id, json_object *result);
int rt_read(Km *km, int fd, RtReader *reader);
int rt_next(Km *km, RtReader *reader, json_object **message);
bool rt_noise(RtReader *reader);
json_object *rt_line_id(const RtReader *reader);
bool rt_identifier(json_object *value);
bool rt_named_id(json_object *id, const char *name);
void rt_fail_pending(Rt *rt, const char *message);
int rt_start(Rt *rt);
void rt_stop(Rt *rt, const char *message);
void rt_upstream_failed(Rt *rt, const char *message);
int rt_upstream_initialized(Rt *rt, json_object *packet);
int rt_discovered(Rt *rt, json_object *packet);
void rt_tick(Rt *rt);
int rt_refresh(Rt *rt, bool force);
int rt_client(Rt *rt, json_object *message);
int rt_server(Rt *rt, json_object *message);
int rt_changed(Rt *rt);
bool rt_legacy_version(const char *version);
int rt_control_open(Rt *rt);
void rt_control_accept(Rt *rt);
void rt_reconnect_finish(Rt *rt, const char *state, const char *message);
int rt_reconnect(Km *km, const char *id, const char *agent, bool json);
json_object *rt_control_connections(Km *km, const char *id, const char *agent,
                                    const char *command, int timeout_ms, bool *scan_complete);
json_object *rt_connection_status(Rt *rt);
json_object *rt_status(Km *km, const char *id, const char *agent);
#endif
