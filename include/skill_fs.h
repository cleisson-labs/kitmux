#ifndef KITMUX_SKILL_FS_H
#define KITMUX_SKILL_FS_H
#include <stdbool.h>
#include <fcntl.h>

#define SKILL_DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)

/* Walk each parent with O_NOFOLLOW. A missing directory is created only on request. */
int skill_open_directory(const char *path, bool create);
int skill_open_parent(const char *path, bool create, const char **leaf);
int skill_open_private(int parent, const char *name);
bool skill_same_entry(int parent, const char *name, int fd);
int skill_rename_exclusive(int from_fd, const char *from, int to_fd, const char *to);
int skill_rename_exchange(int from_fd, const char *from, int to_fd, const char *to);
#endif
