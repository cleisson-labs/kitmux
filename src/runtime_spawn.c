#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static json_object *definition(Rt *rt) {
    return rt->execution;
}

static int spawn_child(Rt *rt) {
    json_object *server = definition(rt), *command = km_get(server, "command");
    size_t count = json_object_array_length(command);
    if (!count || count > 256) return km_error(rt->km, "Managed MCP requires 1–256 command arguments");
    char *arguments[257];
    for (size_t i = 0; i < count; i++) {
        json_object *value = json_object_array_get_idx(command, i);
        if (!json_object_is_type(value, json_type_string)) return km_error(rt->km, "Invalid MCP command argument");
        arguments[i] = (char *)json_object_get_string(value);
    }
    arguments[count] = NULL;
    int input[2], output[2];
    if (pipe(input)) return km_error(rt->km, "Cannot create MCP input pipe");
    if (pipe(output)) { close(input[0]); close(input[1]); return km_error(rt->km, "Cannot create MCP output pipe"); }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd < 0 || dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
        close(nullfd); close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        const char *cwd = km_string(server, "cwd");
        if (cwd && chdir(cwd)) _exit(126);
        json_object *environment = km_get(server, "env");
        if (environment) {
            json_object_object_foreach(environment, name, value) {
                if (!json_object_is_type(value, json_type_string) || setenv(name, json_object_get_string(value), 1)) _exit(126);
            }
        }
        execvp(arguments[0], arguments);
        _exit(127);
    }
    close(input[0]); close(output[1]);
    if (pid < 0) { close(input[1]); close(output[0]); return km_error(rt->km, "Cannot start MCP process"); }
    setpgid(pid, pid);
    rt->child = pid; rt->upstream_in = input[1]; rt->upstream_out = output[0];
    fcntl(input[1], F_SETFD, FD_CLOEXEC); fcntl(output[0], F_SETFD, FD_CLOEXEC);
    rt_nonblock(input[1]); rt_nonblock(output[0]);
    return 0;
}

/* waitid(WNOWAIT) leaves an exited leader unreaped. Its zombie keeps the PID
 * and process group reserved, so signals never reach a reused process ID.
 * Returns 1 when the leader exited, 0 while it runs, -1 if it is not our child. */
static int leader_state(pid_t pid) {
    siginfo_t observed = {0};
    int status;
    do { status = waitid(P_PID, (id_t)pid, &observed, WEXITED | WNOHANG | WNOWAIT); }
    while (status < 0 && errno == EINTR);
    return status < 0 ? -1 : observed.si_pid == pid;
}

static void pause_briefly(long nanoseconds) {
    struct timespec pause = {0, nanoseconds};
    nanosleep(&pause, NULL);
}

static void await_leader(pid_t pid, double seconds) {
    double until = rt_clock() + seconds;
    while (!leader_state(pid) && rt_clock() < until) pause_briefly(10000000);
}

/* Closed stdin is the MCP stdio shutdown request; signals follow only if the
 * leader does not exit within the grace period. */
static void stop_child(Rt *rt, bool closed_input) {
    if (rt->child <= 0) return;
    if (leader_state(rt->child) >= 0) {
        if (closed_input) await_leader(rt->child, RT_EOF_GRACE_SECONDS);
        bool exited = leader_state(rt->child) != 0;
        kill(-rt->child, SIGTERM);
        if (exited) pause_briefly(50000000);
        else await_leader(rt->child, RT_TERM_GRACE_SECONDS);
        kill(-rt->child, SIGKILL);
        while (waitpid(rt->child, NULL, 0) < 0 && errno == EINTR) {}
    }
    rt->child = 0;
}

void rt_stop(Rt *rt, const char *message) {
    bool closed_input = rt->upstream_in >= 0;
    if (closed_input) close(rt->upstream_in);
    rt->upstream_in = -1;
    /* Client backpressure must never delay stopping the owned process group.
     * The output pipe stays open until then so final writes cannot raise SIGPIPE. */
    stop_child(rt, closed_input);
    if (rt->upstream_out >= 0) close(rt->upstream_out);
    rt->upstream_out = -1;
    rt->upstream = (RtReader){.bytes = rt->upstream.bytes};
    rt->upstream_protocol[0] = '\0';
    rt->starting = rt->discovering = false;
    if (!message) return;
    rt_fail_pending(rt, message);
    rt_reconnect_finish(rt, "failed", message);
}

static bool nonempty_text(json_object *object, const char *key) {
    json_object *value = km_get(object, key);
    const char *text = km_string(object, key);
    return text && *text && strlen(text) == (size_t)json_object_get_string_len(value);
}

/* Only tools are bridged. Other server capabilities are never advertised to
 * the client, so their requests and notifications cannot reach the bridge. */
static bool tools_capability(json_object *caps) {
    return json_object_is_type(km_get(caps, "tools"), json_type_object);
}

static int send_request(Rt *rt, const char *method, const char *id, json_object *params) {
    json_object *packet = json_object_new_object();
    json_object_object_add(packet, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(packet, "method", json_object_new_string(method));
    if (id) json_object_object_add(packet, "id", json_object_new_string(id));
    if (params) json_object_object_add(packet, "params", params);
    int status = rt_send(rt->km, rt->upstream_in, packet);
    json_object_put(packet);
    return status;
}

/* Repeated failures wait 0.5s, 1s, 2s, ... up to 30s before a request may
 * start the upstream again; the first retry is immediate. */
static double retry_delay(unsigned failures) {
    double delay = failures > 1 ? .5 : 0;
    for (unsigned i = 2; i < failures && delay < RT_RETRY_MAX_SECONDS; i++) delay *= 2;
    return delay < RT_RETRY_MAX_SECONDS ? delay : RT_RETRY_MAX_SECONDS;
}

static bool list_pending(Rt *rt) {
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        if (json_object_get_boolean(km_get(record, "list")) && !json_object_get_boolean(km_get(record, "cancelled")))
            return true;
    }
    return false;
}

/* A failure never announces tools/list_changed itself: clients that refresh
 * on the notification would otherwise restart a crashing upstream forever. */
void rt_upstream_failed(Rt *rt, const char *message) {
    if (message != rt->failure) snprintf(rt->failure, sizeof(rt->failure), "%s", message);
    rt->failures++;
    rt->retry_at = rt_clock() + retry_delay(rt->failures);
    if (list_pending(rt)) rt->client_stale = true;
    rt_stop(rt, rt->failure);
}

static int start_failed(Rt *rt, const char *message) {
    rt_upstream_failed(rt, message);
    return km_error(rt->km, "%s", rt->failure);
}

/* Starting is asynchronous: requests queue in rt->pending until the upstream
 * answers initialize, so the bridge keeps serving its client and control socket. */
int rt_start(Rt *rt) {
    if (rt->child > 0) return 0;
    if (!rt->initialize) return km_error(rt->km, "Waiting for the client's MCP initialization");
    double wait = rt->retry_at - rt_clock();
    if (wait > 0) return km_error(rt->km, "MCP upstream failed recently (%s); retrying after %.1fs", rt->failure, wait);
    if (spawn_child(rt)) return start_failed(rt, rt->km->error);
    json_object *params = NULL;
    if (json_object_deep_copy(rt->initialize, &params, NULL)) return start_failed(rt, "Cannot retain upstream MCP initialization");
    json_object_object_add(params, "capabilities", json_object_new_object());
    rt->starting = true;
    rt->start_deadline = rt_clock() + RT_START_SECONDS;
    if (send_request(rt, "initialize", "kitmux/initialize", params))
        return start_failed(rt, "MCP upstream exited during startup; use direct mode for diagnostics");
    return 0;
}

static int flush_queue(Rt *rt, bool *listed) {
    json_object_object_foreach(rt->pending, key, record) {
        (void)key;
        json_object *line = km_get(record, "queued");
        if (!line) continue;
        *listed |= json_object_get_boolean(km_get(record, "list"));
        if (rt_send_line(rt->km, rt->upstream_in, json_object_get_string(line), (size_t)json_object_get_string_len(line)))
            return -1;
        json_object_object_del(record, "queued");
    }
    return 0;
}

int rt_upstream_initialized(Rt *rt, json_object *packet) {
    json_object *result = km_get(packet, "result"), *info = km_get(result, "serverInfo"), *reserved = NULL;
    const char *version = km_string(result, "protocolVersion");
    bool legacy = rt_named_id(km_get(packet, "id"), "kitmux/initialize") &&
                  !json_object_object_get_ex(packet, "error", &reserved) &&
                  nonempty_text(result, "protocolVersion") && rt_legacy_version(version) &&
                  nonempty_text(info, "name") && nonempty_text(info, "version");
    bool tools = tools_capability(km_get(result, "capabilities"));
    if (!legacy || !tools) {
        rt_upstream_failed(rt, !legacy ? "Managed bridge requires a legacy MCP endpoint; use direct mode"
                                       : "Managed bridge requires an MCP endpoint with the tools capability; use direct mode");
        return 0;
    }
    snprintf(rt->upstream_protocol, sizeof(rt->upstream_protocol), "%s", version);
    rt->starting = false;
    rt->failures = 0;
    rt->retry_at = 0;
    rt->failure[0] = '\0';
    bool listed = false;
    if (send_request(rt, "notifications/initialized", NULL, NULL) || flush_queue(rt, &listed) ||
        (rt->discovering && send_request(rt, "tools/list", "kitmux/discovery", NULL))) {
        rt_upstream_failed(rt, "MCP disconnected; completion unknown, call not replayed");
        return 0;
    }
    if (rt->discovering || !rt->client_stale) return 0;
    rt->client_stale = false;
    /* A queued tools/list already receives the recovered list. */
    return listed ? 0 : rt_changed(rt);
}

int rt_discovered(Rt *rt, json_object *packet) {
    rt->discovering = false;
    if (!json_object_is_type(km_get(km_get(packet, "result"), "tools"), json_type_array)) {
        rt_upstream_failed(rt, "MCP discovery failed after reconnect");
        return 0;
    }
    rt->client_stale = false;
    rt_reconnect_finish(rt, "reconnected", "Upstream discovery succeeded; client tool refresh may still be required");
    return rt_changed(rt);
}

void rt_tick(Rt *rt) {
    double now = rt_clock();
    if (rt->starting && now >= rt->start_deadline) {
        char message[128];
        snprintf(message, sizeof(message), "MCP upstream did not finish initialization within %.0fs; use direct mode for diagnostics",
                 RT_START_SECONDS);
        rt_upstream_failed(rt, message);
    }
    if (rt->reconnect_waiting && now >= rt->reconnect_deadline)
        rt_reconnect_finish(rt, "failed", "MCP discovery did not finish before the reconnect deadline");
}
