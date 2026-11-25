#include "skills_internal.h"
#include "skill_fs.h"
#include <openssl/rand.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#define SKILL_MAX_ENTRIES 4096u
#define SKILL_MAX_TOTAL (32u * 1024u * 1024u)
/* Installed packages are immutable: agents cannot edit them through projections. */
#define SKILL_FILE_MODE 0400
#define SKILL_EXECUTABLE_MODE 0500
#define SKILL_DIRECTORY_MODE 0500

typedef struct {
    Km *km;
    EVP_MD_CTX *digest;
    size_t count;
    size_t bytes;
} Tree;

static int entry_order(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static bool safe_entry_name(const char *name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '\\') return false;
    return true;
}

static int sync_directory(Km *km, const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return km_error(km, "cannot open installed skill directory for sync: %s", strerror(errno));
    int result = fsync(fd);
    int saved = errno;
    close(fd);
    return result ? km_error(km, "cannot sync installed skill directory: %s", strerror(saved)) : 0;
}

static int sync_ancestry(Km *km, const char *parent) {
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s", parent)) return -1;
    for (;;) {
        if (sync_directory(km, path)) return -1;
        if (!strcmp(path, km->home) || !strcmp(path, "/")) return 0;
        char *slash = strrchr(path, '/');
        if (!slash) return km_error(km, "skill store parent is not absolute");
        if (slash == path) path[1] = 0;
        else *slash = 0;
    }
}

static int copy_bytes(Tree *tree, int input, int output, size_t expected) {
    unsigned char buffer[16384];
    size_t total = 0;
    ssize_t got;
    while ((got = read(input, buffer, sizeof(buffer))) != 0) {
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 || (size_t)got > expected - total)
            return km_error(tree->km, "skill file changed while reading");
        total += (size_t)got;
        if (EVP_DigestUpdate(tree->digest, buffer, (size_t)got) != 1)
            return km_error(tree->km, "cannot hash skill package");
        for (ssize_t offset = 0; output >= 0 && offset < got;) {
            ssize_t written = write(output, buffer + offset, (size_t)(got - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return km_error(tree->km, "cannot write skill package: %s", strerror(errno));
            offset += written;
        }
    }
    return total == expected ? 0 : km_error(tree->km, "skill file changed while reading");
}

static int visit_file(Tree *tree, const char *relative, int source, const char *name, const struct stat *before,
                      int destination) {
    if (before->st_size < 0 || (unsigned long long)before->st_size > KM_MAX_FILE ||
        (size_t)before->st_size > SKILL_MAX_TOTAL - tree->bytes)
        return km_error(tree->km, "skill package exceeds file or total size limit");
    int input = openat(source, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat actual;
    if (input < 0) return km_error(tree->km, "cannot read skill file: %s", strerror(errno));
    if (fstat(input, &actual) || !S_ISREG(actual.st_mode) || actual.st_ino != before->st_ino ||
        actual.st_dev != before->st_dev || actual.st_size != before->st_size) {
        close(input);
        return km_error(tree->km, "skill file changed while reading");
    }
    char header[PATH_MAX + 64];
    int count = snprintf(header, sizeof(header), "F:%s:%lld:%u\n", relative,
                         (long long)actual.st_size, (unsigned)!!(actual.st_mode & 0111));
    if (count < 0 || (size_t)count >= sizeof(header) ||
        EVP_DigestUpdate(tree->digest, header, (size_t)count) != 1) {
        close(input);
        return km_error(tree->km, "cannot hash skill package");
    }
    int output = -1;
    if (destination >= 0) {
        output = openat(destination, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        actual.st_mode & 0111 ? SKILL_EXECUTABLE_MODE : SKILL_FILE_MODE);
        if (output < 0) {
            close(input);
            return km_error(tree->km, "cannot create skill file: %s", strerror(errno));
        }
    }
    int result = copy_bytes(tree, input, output, (size_t)actual.st_size);
    if (output >= 0 && !result && fsync(output))
        result = km_error(tree->km, "cannot sync installed skill file: %s", strerror(errno));
    if (output >= 0 && close(output) && !result)
        result = km_error(tree->km, "cannot close installed skill file");
    close(input);
    tree->bytes += (size_t)actual.st_size;
    return result;
}

/* scandirat is unavailable on macOS. Read names from the opened directory and
 * sort by bytes to preserve the existing, locale-independent package digest. */
static int sorted_names(Tree *tree, DIR *directory, char ***names, size_t *count) {
    size_t capacity = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) return km_error(tree->km, "cannot list skill package: %s", strerror(errno));
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git")) continue;
        if (!safe_entry_name(name)) return km_error(tree->km, "skill package has an unsafe filename");
        if (*count >= SKILL_MAX_ENTRIES - tree->count)
            return km_error(tree->km, "skill package has too many entries or nested directories");
        if (*count == capacity) {
            capacity = capacity ? 2 * capacity : 16;
            char **grown = realloc(*names, capacity * sizeof(*grown));
            if (!grown) return km_error(tree->km, "cannot allocate skill directory entries");
            *names = grown;
        }
        (*names)[*count] = strdup(name);
        if (!(*names)[*count]) return km_error(tree->km, "cannot allocate skill directory entry");
        ++*count;
    }
    if (*count > 1) qsort(*names, *count, sizeof(**names), entry_order);
    return 0;
}

static DIR *open_directory_stream(int parent, const char *name) {
    int fd = openat(parent, name, SKILL_DIR_FLAGS);
    if (fd < 0) return NULL;
    DIR *directory = fdopendir(fd);
    if (!directory) {
        int saved = errno;
        close(fd);
        errno = saved;
    }
    return directory;
}

static int walk(Tree *tree, const char *relative, int source, const char *name, unsigned depth, int destination) {
    if (depth > KM_SKILL_MAX_DEPTH || ++tree->count > SKILL_MAX_ENTRIES)
        return km_error(tree->km, "skill package has too many entries or nested directories");
    struct stat st;
    if (fstatat(source, name, &st, AT_SYMLINK_NOFOLLOW))
        return km_error(tree->km, "cannot inspect skill package: %s", strerror(errno));
    if (S_ISLNK(st.st_mode)) return km_error(tree->km, "symlinks are not supported in skill packages");
    if (S_ISREG(st.st_mode)) return visit_file(tree, relative, source, name, &st, destination);
    if (!S_ISDIR(st.st_mode)) return km_error(tree->km, "special files are not supported in skill packages");
    if (EVP_DigestUpdate(tree->digest, relative, strlen(relative) + 1) != 1)
        return km_error(tree->km, "cannot hash skill directory");
    DIR *directory = open_directory_stream(source, name);
    if (!directory) return km_error(tree->km, "cannot list skill directory: %s", strerror(errno));
    int child_dir = destination;
    if (destination >= 0 && depth > 0) {
        if (mkdirat(destination, name, 0700) || (child_dir = openat(destination, name, SKILL_DIR_FLAGS)) < 0) {
            closedir(directory);
            return km_error(tree->km, "cannot create staged skill directory: %s", strerror(errno));
        }
    }
    char **names = NULL;
    size_t count = 0;
    int result = sorted_names(tree, directory, &names, &count);
    for (size_t i = 0; i < count; ++i) {
        if (!result) {
            char child[PATH_MAX];
            result = km_path(tree->km, child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", names[i]);
            if (!result) result = walk(tree, child, dirfd(directory), names[i], depth + 1, child_dir);
        }
        free(names[i]);
    }
    free(names);
    closedir(directory);
    if (child_dir >= 0) {
        /* macOS needs a writable staging root until publication. */
        if (!result && depth > 0 && fchmod(child_dir, SKILL_DIRECTORY_MODE))
            result = km_error(tree->km, "cannot protect staged skill directory: %s", strerror(errno));
        if (!result && fsync(child_dir))
            result = km_error(tree->km, "cannot sync staged skill directory: %s", strerror(errno));
        if (depth > 0) close(child_dir);
    }
    return result;
}

static int hash_directory(Km *km, int root, int destination, char hash[65]) {
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!digest) return km_error(km, "cannot allocate skill hash");
    Tree tree = {.km = km, .digest = digest};
    int result = EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1 ? walk(&tree, "", root, ".", 0, destination) : -1;
    unsigned char bytes[32];
    unsigned count = 0;
    if (!result && (EVP_DigestFinal_ex(digest, bytes, &count) != 1 || count != 32)) result = -1;
    EVP_MD_CTX_free(digest);
    if (result) return *km->error ? -1 : km_error(km, "cannot hash skill package");
    for (unsigned i = 0; i < count; ++i) snprintf(hash + i * 2, 3, "%02x", bytes[i]);
    return 0;
}

static int hash_tree(Km *km, const char *root, int destination, char hash[65]) {
    int source = open(root, SKILL_DIR_FLAGS);
    if (source < 0) return km_error(km, "cannot open skill package: %s", strerror(errno));
    int result = hash_directory(km, source, destination, hash);
    close(source);
    return result;
}

static bool skill_name(const char *name) {
    size_t length = strlen(name);
    if (!length || length > 64 || name[0] == '-' || name[length - 1] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-')) return false;
        if (i && name[i] == '-' && name[i - 1] == '-') return false;
    }
    return true;
}

static int frontmatter_fields(Km *km, yaml_document_t *document, KmSkill *skill) {
    yaml_node_t *root = yaml_document_get_root_node(document);
    if (!root || root->type != YAML_MAPPING_NODE) return km_error(km, "skill frontmatter must be a YAML mapping");
    for (yaml_node_pair_t *pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top; ++pair) {
        yaml_node_t *key = yaml_document_get_node(document, pair->key);
        yaml_node_t *value = yaml_document_get_node(document, pair->value);
        if (!key || key->type != YAML_SCALAR_NODE) return km_error(km, "skill frontmatter keys must be strings");
        if (memchr(key->data.scalar.value, 0, key->data.scalar.length)) return km_error(km, "skill frontmatter keys cannot contain NUL");
        for (yaml_node_pair_t *earlier = root->data.mapping.pairs.start; earlier < pair; ++earlier) {
            yaml_node_t *other = yaml_document_get_node(document, earlier->key);
            if (other && other->type == YAML_SCALAR_NODE && key->data.scalar.length == other->data.scalar.length &&
                !memcmp(key->data.scalar.value, other->data.scalar.value, key->data.scalar.length))
                return km_error(km, "duplicate key in skill frontmatter");
        }
        bool name = !strcmp((char *)key->data.scalar.value, "name");
        bool description = !strcmp((char *)key->data.scalar.value, "description");
        bool compatibility = !strcmp((char *)key->data.scalar.value, "compatibility");
        bool license = !strcmp((char *)key->data.scalar.value, "license");
        if (!name && !description && !compatibility && !license) continue;
        size_t maximum = name ? sizeof(skill->name) - 1 : description ? sizeof(skill->description) - 1 :
                         compatibility ? sizeof(skill->compatibility) - 1 : sizeof(skill->license) - 1;
        if (!value || value->type != YAML_SCALAR_NODE || !value->data.scalar.length ||
            value->data.scalar.length > maximum || memchr(value->data.scalar.value, 0, value->data.scalar.length))
            return km_error(km, "skill metadata fields must be non-empty scalar strings within standard limits");
        char *target = name ? skill->name : description ? skill->description :
                       compatibility ? skill->compatibility : skill->license;
        memcpy(target, value->data.scalar.value, value->data.scalar.length);
        target[value->data.scalar.length] = 0;
    }
    return skill_name(skill->name) && *skill->description ? 0 :
        km_error(km, "skill needs a valid lowercase name and non-empty description");
}

static int inspect_frontmatter(Km *km, KmSkill *skill) {
    char path[PATH_MAX];
    if (km_path(km, path, sizeof(path), "%s/SKILL.md", skill->root)) return -1;
    size_t size;
    char *text = km_read_file(km, path, &size);
    if (!text) return -1;
    size_t start = size >= 4 && !memcmp(text, "---\n", 4) ? 4 :
                   size >= 5 && !memcmp(text, "---\r\n", 5) ? 5 : 0;
    size_t end = start;
    while (start && end < size && end - start <= 65536) {
        if ((end == start || text[end - 1] == '\n') && end + 3 <= size && !memcmp(text + end, "---", 3) &&
            (end + 3 == size || text[end + 3] == '\n' || (end + 4 < size && !memcmp(text + end + 3, "\r\n", 2)))) break;
        ++end;
    }
    if (!start || end >= size || end - start > 65536) {
        free(text);
        return km_error(km, "SKILL.md needs bounded YAML frontmatter between --- lines");
    }
    yaml_parser_t parser;
    yaml_document_t document;
    if (!yaml_parser_initialize(&parser)) { free(text); return km_error(km, "cannot allocate YAML parser"); }
    yaml_parser_set_input_string(&parser, (unsigned char *)text + start, end - start);
    int loaded = yaml_parser_load(&parser, &document);
    int result = loaded ? frontmatter_fields(km, &document, skill) : km_error(km, "invalid YAML skill frontmatter");
    if (loaded) yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    free(text);
    return result;
}

int km_skill_inspect(Km *km, KmSkill *skill) {
    if (hash_tree(km, skill->root, -1, skill->hash) || inspect_frontmatter(km, skill)) return -1;
    if (!*skill->revision) snprintf(skill->revision, sizeof(skill->revision), "local-%s", skill->hash);
    return 0;
}

static int remove_tree_at(int parent, const char *name);

static int remove_directory(int parent, const char *name, DIR *directory) {
    int fd = dirfd(directory);
    /* Only the opened directory needs write permission to remove its children. */
    struct stat st;
    int result = fstat(fd, &st);
    if (!result && st.st_uid != geteuid()) {
        errno = EPERM;
        result = -1;
    }
    if (!result && (st.st_mode & 0777) != 0700) result = fchmod(fd, 0700);
    while (!result) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) result = -1;
            break;
        }
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) result = remove_tree_at(fd, entry->d_name);
    }
    if (!result && (!skill_same_entry(parent, name, fd) || unlinkat(parent, name, AT_REMOVEDIR)) && errno != ENOENT)
        result = -1;
    int saved = errno;
    if (closedir(directory) && !result) return -1;
    errno = saved;
    return result;
}

static int remove_tree_at(int parent, const char *name) {
    DIR *directory = open_directory_stream(parent, name);
    if (directory) return remove_directory(parent, name, directory);
    if (errno == ENOENT) return 0;
    if (errno != ENOTDIR && errno != ELOOP) return -1;
    return unlinkat(parent, name, 0) == 0 || errno == ENOENT ? 0 : -1;
}

int km_skill_remove_tree(const char *path) {
    const char *name;
    int parent = skill_open_parent(path, false, &name);
    if (parent < 0) return errno == ENOENT ? 0 : -1;
    int result = remove_tree_at(parent, name);
    close(parent);
    return result;
}

void km_skill_cleanup(KmSkill *skill) {
    if (*skill->temporary && !km_skill_remove_tree(skill->temporary)) skill->temporary[0] = 0;
}

/* Verify a published cache entry before reuse, including a previous install that
 * stopped between its exclusive rename and the final read-only permission change. */
static int reuse_installed(Km *km, int parent, int fd, const char *path, const char *expected) {
    char actual[65];
    if (hash_directory(km, fd, -1, actual)) return -1;
    if (strcmp(actual, expected)) return km_error(km, "installed skill package was modified");
    if (!skill_same_entry(parent, expected, fd)) return km_error(km, "skill store entry changed during verification");
    struct stat st;
    if (fstat(fd, &st) || ((st.st_mode & 07777) != SKILL_DIRECTORY_MODE && fchmod(fd, SKILL_DIRECTORY_MODE)) ||
        fsync(fd))
        return km_error(km, "cannot protect installed skill directory %s: %s", path, strerror(errno));
    return 0;
}

static int install_at(Km *km, KmSkill *skill, int parent, const char *path) {
    unsigned char random[12];
    if (RAND_bytes(random, sizeof(random)) != 1) return km_error(km, "cannot generate skill staging name");
    char staging[sizeof(".install-") + 2 * sizeof(random)] = ".install-";
    for (size_t i = 0; i < sizeof(random); ++i)
        snprintf(staging + sizeof(".install-") - 1 + 2 * i, 3, "%02x", random[i]);
    if (mkdirat(parent, staging, 0700)) return km_error(km, "cannot stage skill package: %s", strerror(errno));
    int fd = skill_open_private(parent, staging);
    if (fd < 0) return km_error(km, "cannot open skill staging directory %s: %s", staging, strerror(errno));
    char copied[65];
    int result = hash_tree(km, skill->root, fd, copied);
    if (!result && strcmp(copied, skill->hash)) result = km_error(km, "source changed during skill installation");
    if (!result && skill_same_entry(parent, staging, fd) &&
        skill_rename_exclusive(parent, staging, parent, skill->hash) == 0) {
        if (!skill_same_entry(parent, skill->hash, fd) || fchmod(fd, SKILL_DIRECTORY_MODE) || fsync(fd))
            result = km_error(km, "cannot protect installed skill directory %s: %s", path, strerror(errno));
        close(fd);
        return result;
    }
    if (!result) {
        int winner = -1;
        if (errno == EEXIST || errno == ENOTEMPTY) winner = openat(parent, skill->hash, SKILL_DIR_FLAGS);
        if (winner < 0)
            result = km_error(km, "cannot publish skill package: %s", strerror(errno));
        else {
            result = reuse_installed(km, parent, winner, path, skill->hash);
            close(winner);
        }
    }
    if (!skill_same_entry(parent, staging, fd)) {
        close(fd);
        return km_error(km, "staged skill package %s changed before cleanup", staging);
    }
    DIR *directory = fdopendir(fd);
    if (!directory) {
        int error = errno;
        close(fd);
        return km_error(km, "cannot list staged skill package %s: %s", staging, strerror(error));
    }
    if (remove_directory(parent, staging, directory))
        return km_error(km, "cannot clean staged skill package %s: %s", staging, strerror(errno));
    return result;
}

int km_skill_store(Km *km, KmSkill *skill, char *out, size_t size) {
    char parent[PATH_MAX];
    if (km_path(km, parent, sizeof(parent), "%s/skills/%s", km->data, skill->name) ||
        km_path(km, out, size, "%s/%s", parent, skill->hash))
        return -1;
    int dir = skill_open_directory(parent, true);
    if (dir < 0) return km_error(km, "cannot open skill store: %s", strerror(errno));
    int existing = openat(dir, skill->hash, SKILL_DIR_FLAGS);
    int result;
    if (existing >= 0) {
        result = reuse_installed(km, dir, existing, out, skill->hash);
        close(existing);
    } else if (errno == ENOENT) {
        result = install_at(km, skill, dir, out);
    } else
        result = km_error(km, "skill store path is not a regular directory: %s", strerror(errno));
    if (!result && fsync(dir)) result = km_error(km, "cannot sync skill store: %s", strerror(errno));
    close(dir);
    return result ? result : sync_ancestry(km, parent);
}
