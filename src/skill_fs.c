#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "skill_fs.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int skill_open_directory(const char *path, bool create) {
    char copy[PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(copy, path, length + 1);
    int fd = open(*path == '/' ? "/" : ".", SKILL_DIR_FLAGS);
    char *save = NULL;
    for (char *name = strtok_r(copy, "/", &save); fd >= 0 && name; name = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, name, SKILL_DIR_FLAGS);
        if (next < 0 && errno == ENOENT && create && (mkdirat(fd, name, 0700) == 0 || errno == EEXIST))
            next = openat(fd, name, SKILL_DIR_FLAGS);
        int saved = errno;
        close(fd);
        errno = saved;
        fd = next;
    }
    return fd;
}

int skill_open_parent(const char *path, bool create, const char **leaf) {
    const char *slash = strrchr(path, '/');
    *leaf = slash ? slash + 1 : path;
    if (!**leaf || !strcmp(*leaf, ".") || !strcmp(*leaf, "..")) {
        errno = EINVAL;
        return -1;
    }
    if (!slash || slash == path) return skill_open_directory(slash ? "/" : ".", false);
    size_t length = (size_t)(slash - path);
    char parent[PATH_MAX];
    if (length >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, path, length);
    parent[length] = 0;
    return skill_open_directory(parent, create);
}

int skill_open_private(int parent, const char *name) {
    int fd = openat(parent, name, SKILL_DIR_FLAGS);
    if (fd < 0) return -1;
    struct stat st;
    if (!fstat(fd, &st) && st.st_uid == geteuid() && (st.st_mode & 077) == 0) return fd;
    close(fd);
    errno = EPERM;
    return -1;
}

bool skill_same_entry(int parent, const char *name, int fd) {
    struct stat opened, named;
    if (fstat(fd, &opened) || fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW)) return false;
    if (opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) return true;
    errno = ESTALE;
    return false;
}

int skill_rename_exclusive(int from_fd, const char *from, int to_fd, const char *to) {
#ifdef __APPLE__
    return renameatx_np(from_fd, from, to_fd, to, RENAME_EXCL);
#else
    return renameat2(from_fd, from, to_fd, to, RENAME_NOREPLACE);
#endif
}

int skill_rename_exchange(int from_fd, const char *from, int to_fd, const char *to) {
#ifdef __APPLE__
    return renameatx_np(from_fd, from, to_fd, to, RENAME_SWAP);
#else
    return renameat2(from_fd, from, to_fd, to, RENAME_EXCHANGE);
#endif
}
