#include "tui.h"
#include "adapters.h"
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#define MODIFIED_NS(st) ((st).st_mtimespec.tv_nsec)
#else
#define MODIFIED_NS(st) ((st).st_mtim.tv_nsec)
#endif

/* Other Kitmux processes (CLI, agents using the Kitmux skill) replace the catalog
 * atomically; inode, size and timestamps together detect those saves cheaply. */
bool ui_catalog_changed(KmUi *ui) {
    char path[PATH_MAX]; struct stat st;
    KmUiWatch now; memset(&now, 0, sizeof(now));
    if (km_path(ui->km, path, sizeof(path), "%s/catalog.json", ui->km->config) == 0 && stat(path, &st) == 0) {
        now.present = true; now.device = st.st_dev; now.inode = st.st_ino; now.size = st.st_size;
        now.modified = st.st_mtime; now.changed = st.st_ctime; now.modified_ns = (long)MODIFIED_NS(st);
    }
    bool changed = now.present != ui->watch.present || now.device != ui->watch.device ||
                   now.inode != ui->watch.inode || now.size != ui->watch.size || now.modified != ui->watch.modified ||
                   now.changed != ui->watch.changed || now.modified_ns != ui->watch.modified_ns;
    ui->watch = now;
    return changed;
}

/* Read-only: reports a native sync that adopted clients still need. */
void ui_check_sync(KmUi *ui) {
    char *args[] = {"sync", "--check", "--configured"};
    char output[1024]; bool truncated;
    if (!ui_capture(ui, 3, args, output, sizeof(output), &truncated)) { ui->sync_error[0] = '\0'; return; }
    snprintf(ui->sync_error, sizeof(ui->sync_error), "%s",
             *ui->km->error ? ui->km->error : "native configuration check failed; run kitmux sync --check");
}

static void append(KmUi *ui, size_t *used, const char *format, ...) {
    if (*used >= sizeof(ui->clients)) return;
    va_list args; va_start(args, format);
    int written = vsnprintf(ui->clients + *used, sizeof(ui->clients) - *used, format, args);
    va_end(args);
    if (written > 0) *used += (size_t)written;
}

static int enabled_count(KmUi *ui, const char *kind, const char *agent) {
    int count = 0;
    json_object_object_foreach(km_resources(ui->km, kind), id, item) {
        (void)item;
        if (km_effective(ui->km, kind, id, agent, NULL, 0)) count++;
    }
    return count;
}

static void client_state(KmUi *ui, size_t *used, const char *agent) {
    bool adopted = ad_client_configured(ui->km, agent);
    append(ui, used, "%s: %s | ON: %d MCPs, %d skills, %d subagents\n", agent,
           adopted ? "adopted" : "not adopted (s previews adoption)", enabled_count(ui, "mcp", agent),
           enabled_count(ui, "skill", agent), enabled_count(ui, "subagent", agent));
    if (!adopted) return;
    char *args[] = {"sync", "--check", "--agent", (char *)agent};
    char output[1024]; bool truncated;
    if (!ui_capture(ui, 4, args, output, sizeof(output), &truncated)) append(ui, used, "  Native configuration: current\n");
    else append(ui, used, "  Native sync pending: %s\n", *ui->km->error ? ui->km->error : "run kitmux sync --check");
}

void ui_clients_refresh(KmUi *ui) {
    size_t used = 0; ui->clients[0] = '\0';
    for (int a = 0; a < KM_AGENT_COUNT; a++) client_state(ui, &used, km_agents[a]);
    append(ui, &used, "\nAdopted clients sync automatically after catalog changes; reload them afterwards.\n");
}
