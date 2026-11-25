#include "adapters_skills.h"
#include "skill_fs.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pending_name(Km *km, const char *leaf, char name[NAME_MAX + 1]) {
    return km_path(km, name, NAME_MAX + 1, ".kitmux-%s.pending", leaf);
}

static int needs_recovery(Km *km, const char *path, const char *pending) {
    return km_error(km,
                    "skill projection %s has an unfinished change; inspect its sibling %s "
                    "and recover any saved entry before retrying",
                    path, pending);
}

static bool points_to(int fd, const char *name, const char *target) {
    char actual[PATH_MAX];
    ssize_t n = readlinkat(fd, name, actual, sizeof(actual));
    if (!target) return n < 0 && errno == ENOENT;
    return n >= 0 && (size_t)n == strlen(target) && !memcmp(actual, target, (size_t)n);
}

static int finish_recovery(int parent, const char *pending, int recovery) {
    if (fsync(recovery) || fsync(parent) || !skill_same_entry(parent, pending, recovery)) return -1;
    return unlinkat(parent, pending, AT_REMOVEDIR) || fsync(parent) ? -1 : 0;
}

/* Before exchange, entry contains the desired target; after exchange/detach,
 * it holds the previous target. The recorded pair and live name distinguish the
 * two crash states. A preview or an unrecognized state never discards an entry. */
static int resume_change(AdRun *run, const char *path, int parent, const char *leaf, const char *pending, int recovery,
                         json_object *record) {
    const char *before = km_string(record, "previous"), *after = km_string(record, "after");
    if (!json_object_get_boolean(km_get(run->state, "pending")) || !before || (after && !strcmp(before, after)))
        return needs_recovery(run->km, path, pending);
    bool prior = points_to(parent, leaf, before), current = points_to(parent, leaf, after);
    bool empty = points_to(recovery, "entry", NULL);
    if ((!prior && !current) || (!empty && !(current && points_to(recovery, "entry", before)) &&
                                 !(prior && after && points_to(recovery, "entry", after))))
        return needs_recovery(run->km, path, pending);
    if (!run->apply)
        return km_error(run->km, "skill projection %s has a pending change in %s; run kitmux sync --apply", path,
                        pending);
    if ((!empty && unlinkat(recovery, "entry", 0)) || finish_recovery(parent, pending, recovery))
        return needs_recovery(run->km, path, pending);
    return 0;
}

int ad_recover_skill_link(AdRun *run, const char *path, json_object *record) {
    const char *leaf;
    int parent = skill_open_parent(path, false, &leaf);
    if (parent < 0)
        return errno == ENOENT ? 0 : km_error(run->km, "cannot open skill projection parent: %s", strerror(errno));
    char pending[NAME_MAX + 1];
    int result = pending_name(run->km, leaf, pending);
    if (!result) {
        int recovery = skill_open_private(parent, pending);
        if (recovery >= 0) {
            result = resume_change(run, path, parent, leaf, pending, recovery, record);
            close(recovery);
        } else if (errno != ENOENT)
            result = needs_recovery(run->km, path, pending);
    }
    close(parent);
    return result;
}

/* Only remove the actual entry captured by the atomic operation, and only if it
 * is the expected managed link. A rollback that encounters another entry keeps
 * it in the per-projection recovery directory for inspection. */
static int change_existing(int parent, const char *leaf, int recovery, const char *before, const char *target) {
    const char *saved = "entry";
    if (target) {
        if (symlinkat(target, recovery, saved)) return -1;
        if (skill_rename_exchange(recovery, saved, parent, leaf)) {
            int error = errno;
            unlinkat(recovery, saved, 0);
            errno = error;
            return -1;
        }
    } else if (skill_rename_exclusive(parent, leaf, recovery, saved))
        return -1;
    if (points_to(recovery, saved, before)) return unlinkat(recovery, saved, 0);

    /* Prefer restoring into an empty slot. Exchange only if the installed
     * replacement is still visible; inspect what the exchange actually moved. */
    if (skill_rename_exclusive(recovery, saved, parent, leaf) < 0 && errno == EEXIST && target &&
        points_to(parent, leaf, target) && skill_rename_exchange(recovery, saved, parent, leaf) == 0 &&
        points_to(recovery, saved, target))
        unlinkat(recovery, saved, 0);
    errno = EAGAIN;
    return -1;
}

static int replace_owned(Km *km, const char *path, int parent, const char *leaf, const char *before,
                         const char *target) {
    if (!points_to(parent, leaf, before))
        return km_error(km, "skill projection %s changed during sync; retry `kitmux sync --apply`", path);
    if (target && !strcmp(before, target)) return 0;
    char pending[NAME_MAX + 1];
    if (pending_name(km, leaf, pending)) return -1;
    if (mkdirat(parent, pending, 0700)) {
        if (errno == EEXIST) return needs_recovery(km, path, pending);
        return km_error(km, "cannot create skill projection recovery directory: %s", strerror(errno));
    }
    int recovery = skill_open_private(parent, pending);
    if (recovery < 0) return needs_recovery(km, path, pending);
    int result = fsync(parent);
    if (!result) result = change_existing(parent, leaf, recovery, before, target);
    int saved = errno;
    if (finish_recovery(parent, pending, recovery)) {
        close(recovery);
        return needs_recovery(km, path, pending);
    }
    close(recovery);
    return result ? km_error(km, "cannot change skill projection %s: %s", path, strerror(saved)) : 0;
}

int ad_apply_skill_link(AdRun *run, json_object *link) {
    const char *path = km_string(link, "path"), *target = km_string(link, "target");
    const char *before = km_string(link, "before"), *leaf;
    int parent = skill_open_parent(path, target && !before, &leaf);
    if (parent < 0) {
        if (errno == ENOENT && !before && !target) return 0;
        return km_error(run->km, "cannot open skill projection parent: %s", strerror(errno));
    }
    int result = 0;
    if (before)
        result = replace_owned(run->km, path, parent, leaf, before, target);
    else if (target && symlinkat(target, parent, leaf))
        result = km_error(run->km, "cannot create skill projection %s: %s", path, strerror(errno));
    if (!result && fsync(parent))
        result = km_error(run->km, "cannot sync skill projection %s: %s", path, strerror(errno));
    close(parent);
    return result;
}
