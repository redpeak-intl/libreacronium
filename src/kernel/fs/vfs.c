/**
 * @file vfs.c
 * @brief Реализация VFS: монтирования, разбор путей, операции над файлами.
 */

#include "vfs.h"
#include "../memory/memory.h"
#include "../video/video.h"

/* ---- строковые утилиты (без stdlib) ---- */

static uint32_t s_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void s_copy(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* ---- состояние VFS ---- */

static struct vfs_mount *g_mounts = (struct vfs_mount *)0;
static struct vfs_fs    *g_root_fs = (struct vfs_fs *)0;
static char g_cwd[VFS_PATH_MAX] = "/";

void vfs_init(void) {
    g_mounts = (struct vfs_mount *)0;
    g_root_fs = (struct vfs_fs *)0;
    g_cwd[0] = '/';
    g_cwd[1] = '\0';
}

int vfs_mount(const char *path, struct vfs_fs *fs) {
    if (!path || !fs) {
        return VFS_EINVAL;
    }
    struct vfs_mount *m = (struct vfs_mount *)kmalloc(sizeof(struct vfs_mount));
    if (!m) {
        return VFS_ENOSPC;
    }
    s_copy(m->path, path, VFS_PATH_MAX);
    m->fs = fs;
    m->next = g_mounts;
    g_mounts = m;

    if (s_eq(path, "/")) {
        g_root_fs = fs;
        g_cwd[0] = '/';
        g_cwd[1] = '\0';
    }
    return VFS_OK;
}

int vfs_umount(const char *path) {
    struct vfs_mount *prev = (struct vfs_mount *)0;
    struct vfs_mount *cur = g_mounts;
    while (cur) {
        if (s_eq(cur->path, path)) {
            if (prev) prev->next = cur->next; else g_mounts = cur->next;
            if (g_root_fs == cur->fs) g_root_fs = (struct vfs_fs *)0;
            kfree(cur);
            return VFS_OK;
        }
        prev = cur;
        cur = cur->next;
    }
    return VFS_ENOENT;
}

void vfs_list_mounts(void) {
    print_string("Mounted filesystems:\n");
    if (!g_mounts) {
        print_string("  (none)\n");
        return;
    }
    for (struct vfs_mount *m = g_mounts; m; m = m->next) {
        print_string("  ");
        print_string((m->fs && m->fs->type) ? m->fs->type : "?");
        print_string(" on ");
        print_string(m->path);
        if (m->fs && m->fs->dev && m->fs->dev->name) {
            print_string("  (");
            print_string(m->fs->dev->name);
            print_string(")");
        }
        print_string("\n");
    }
}

/* ---- разбор путей ---- */

/* Построить абсолютный путь: относительный приклеиваем к cwd. */
static void make_absolute(const char *path, char *out) {
    if (!path || path[0] == '\0') {
        s_copy(out, g_cwd, VFS_PATH_MAX);
        return;
    }
    if (path[0] == '/') {
        s_copy(out, path, VFS_PATH_MAX);
        return;
    }
    if (s_eq(g_cwd, "/")) {
        out[0] = '/';
        s_copy(out + 1, path, VFS_PATH_MAX - 1);
    } else {
        uint32_t n = s_len(g_cwd);
        s_copy(out, g_cwd, VFS_PATH_MAX);
        if (n + 1 < VFS_PATH_MAX) { out[n] = '/'; out[n + 1] = '\0'; }
        s_copy(out + n + 1, path, VFS_PATH_MAX - n - 1);
    }
}

/* Разрешить абсолютный путь в узел (обход по компонентам через ops->find). */
static int resolve_abs(struct vfs_fs *fs, const char *path, struct vfs_node *out) {
    if (!fs || !fs->ops || !fs->ops->root) {
        return VFS_ENOENT;
    }
    struct vfs_node cur;
    int r = fs->ops->root(fs, &cur);
    if (r != VFS_OK) {
        return r;
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[VFS_NAME_MAX];
        uint32_t i = 0;
        while (*p && *p != '/') {
            if (i + 1 < VFS_NAME_MAX) comp[i++] = *p;
            p++;
        }
        comp[i] = '\0';

        if (i == 0 || (i == 1 && comp[0] == '.')) {
            continue;
        }
        if (!cur.is_dir) {
            return VFS_ENOTDIR;
        }
        if (i == 2 && comp[0] == '.' && comp[1] == '.') {
            struct vfs_node parent;
            r = fs->ops->find(fs, cur.id, "..", &parent);
            if (r != VFS_OK) return r;
            cur = parent;
            continue;
        }

        struct vfs_node next;
        r = fs->ops->find(fs, cur.id, comp, &next);
        if (r != VFS_OK) return r;
        cur = next;
    }
    *out = cur;
    return VFS_OK;
}

int vfs_resolve(const char *path, struct vfs_node *out) {
    if (!g_root_fs) return VFS_ENOENT;
    char abs[VFS_PATH_MAX];
    make_absolute(path, abs);
    return resolve_abs(g_root_fs, abs, out);
}

int vfs_stat(const char *path, struct vfs_node *out) {
    return vfs_resolve(path, out);
}

/* Разбить путь на родительский каталог и последний компонент. */
static int split_parent(const char *path, struct vfs_node *dir_out,
                        char *leaf, uint32_t leaf_len) {
    char abs[VFS_PATH_MAX];
    make_absolute(path, abs);

    /* Срезаем завершающие слэши. */
    uint32_t n = s_len(abs);
    while (n > 1 && abs[n - 1] == '/') { abs[--n] = '\0'; }

    /* Ищем последний слэш. */
    int32_t slash = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (abs[i] == '/') slash = (int32_t)i;
    }
    if (slash < 0) {
        return VFS_EINVAL;
    }

    s_copy(leaf, abs + slash + 1, leaf_len);

    char parent[VFS_PATH_MAX];
    if (slash == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        s_copy(parent, abs, VFS_PATH_MAX);
        parent[slash] = '\0';
    }
    return resolve_abs(g_root_fs, parent, dir_out);
}

/* ---- операции уровня пользователя ---- */

int vfs_list(const char *path) {
    if (!g_root_fs) {
        print_string("no filesystem mounted\n");
        return VFS_ENOENT;
    }
    struct vfs_node dir;
    int r = vfs_resolve(path && path[0] ? path : ".", &dir);
    if (r != VFS_OK) return r;
    if (!dir.is_dir) return VFS_ENOTDIR;

    struct vfs_node e;
    uint32_t i = 0;
    uint32_t total = 0;
    while (g_root_fs->ops->readdir(g_root_fs, dir.id, i, &e) == VFS_OK) {
        if (s_eq(e.name, ".") || s_eq(e.name, "..")) { i++; continue; }
        if (e.is_dir) {
            print_string_color("[DIR]  ", COLOR_LIGHT_CYAN, COLOR_BLACK);
        } else {
            print_string("       ");
        }
        print_string(e.name);
        if (!e.is_dir) {
            print_string("  ");
            print_dec((int)e.size);
            print_string(" bytes");
        }
        print_string("\n");
        i++;
        total++;
    }
    if (total == 0) {
        print_string("(empty)\n");
    }
    return VFS_OK;
}

int vfs_read_file(const char *path, void **out_buf, uint32_t *out_len) {
    if (!g_root_fs) return VFS_ENOENT;
    struct vfs_node node;
    int r = vfs_resolve(path, &node);
    if (r != VFS_OK) return r;
    if (node.is_dir) return VFS_EISDIR;

    if (node.size == 0) {
        *out_buf = (void *)0;
        *out_len = 0;
        return VFS_OK;
    }

    uint8_t *buf = (uint8_t *)kmalloc(node.size + 1);
    if (!buf) return VFS_ENOSPC;
    uint32_t got = 0;
    r = g_root_fs->ops->read(g_root_fs, &node, 0, buf, node.size, &got);
    if (r != VFS_OK) {
        kfree(buf);
        return r;
    }
    buf[got] = '\0';
    *out_buf = buf;
    *out_len = got;
    return VFS_OK;
}

int vfs_write_file(const char *path, const void *buf, uint32_t len, int create) {
    if (!g_root_fs) return VFS_ENOENT;

    struct vfs_node node;
    int r = vfs_resolve(path, &node);
    if (r == VFS_ENOENT && create) {
        struct vfs_node dir;
        char leaf[VFS_NAME_MAX];
        r = split_parent(path, &dir, leaf, VFS_NAME_MAX);
        if (r != VFS_OK) return r;
        if (!dir.is_dir) return VFS_ENOTDIR;
        r = g_root_fs->ops->create(g_root_fs, dir.id, leaf, &node);
        if (r != VFS_OK) return r;
    } else if (r != VFS_OK) {
        return r;
    }
    if (node.is_dir) return VFS_EISDIR;

    uint32_t written = 0;
    r = g_root_fs->ops->write(g_root_fs, &node, 0, buf, len, &written);
    if (r != VFS_OK) return r;
    /* Обрезаем возможный «хвост» старого файла. */
    r = g_root_fs->ops->truncate(g_root_fs, &node, written);
    if (r != VFS_OK) return r;
    return (int)written;
}

int vfs_mkdir(const char *path) {
    if (!g_root_fs) return VFS_ENOENT;
    struct vfs_node dir;
    char leaf[VFS_NAME_MAX];
    int r = split_parent(path, &dir, leaf, VFS_NAME_MAX);
    if (r != VFS_OK) return r;
    if (!dir.is_dir) return VFS_ENOTDIR;
    if (leaf[0] == '\0') return VFS_EINVAL;
    struct vfs_node out;
    return g_root_fs->ops->mkdir(g_root_fs, dir.id, leaf, &out);
}

int vfs_unlink(const char *path) {
    if (!g_root_fs) return VFS_ENOENT;
    struct vfs_node node;
    int r = vfs_resolve(path, &node);
    if (r != VFS_OK) return r;
    if (s_eq(node.name, "/")) return VFS_EINVAL; /* корень не удаляем */
    return g_root_fs->ops->unlink(g_root_fs, &node);
}

int vfs_chdir(const char *path) {
    if (!g_root_fs) return VFS_ENOENT;
    char abs[VFS_PATH_MAX];
    make_absolute(path, abs);

    struct vfs_node node;
    int r = resolve_abs(g_root_fs, abs, &node);
    if (r != VFS_OK) return r;
    if (!node.is_dir) return VFS_ENOTDIR;

    /* Нормализуем cwd: убираем "/./" и разрешаем "/../" через обход. */
    char norm[VFS_PATH_MAX];
    norm[0] = '/';
    norm[1] = '\0';
    const char *p = abs;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[VFS_NAME_MAX];
        uint32_t i = 0;
        while (*p && *p != '/') { if (i + 1 < VFS_NAME_MAX) comp[i++] = *p; p++; }
        comp[i] = '\0';
        if (i == 1 && comp[0] == '.') continue;
        if (i == 2 && comp[0] == '.' && comp[1] == '.') {
            uint32_t l = s_len(norm);
            while (l > 1 && norm[l - 1] != '/') l--;
            if (l > 1) norm[l - 1] = '\0';
            if (norm[0] == '\0') { norm[0] = '/'; norm[1] = '\0'; }
            continue;
        }
        uint32_t l = s_len(norm);
        if (l > 1) { if (l + 1 < VFS_PATH_MAX) { norm[l] = '/'; norm[l + 1] = '\0'; l++; } }
        s_copy(norm + l, comp, VFS_PATH_MAX - l);
    }
    s_copy(g_cwd, norm, VFS_PATH_MAX);
    return VFS_OK;
}

const char *vfs_getcwd(void) {
    return g_cwd;
}

void vfs_mount_root(struct vfs_fs *fs) {
    g_root_fs = fs;
}

const char *vfs_strerror(int err) {
    switch (err) {
        case VFS_OK:        return "ok";
        case VFS_ENOENT:    return "no such file or directory";
        case VFS_EEXIST:    return "file exists";
        case VFS_ENOSPC:    return "no space left";
        case VFS_EIO:       return "input/output error";
        case VFS_ENOTDIR:   return "not a directory";
        case VFS_EISDIR:    return "is a directory";
        case VFS_ENOTEMPTY: return "directory not empty";
        case VFS_EINVAL:    return "invalid argument";
        default:            return "unknown error";
    }
}
