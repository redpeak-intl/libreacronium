/**
 * @file vfs.h
 * @brief Виртуальная файловая система: монтирование ФС и работа с путями.
 *
 * VFS ничего не знает о внутренностях FAT32. Драйвер ФС реализует набор
 * операций (struct vfs_ops), а VFS занимается разбором путей, текущим
 * каталогом и единой точкой входа для shell. Так ФС остаётся «сменным
 * модулем»: завтра рядом встанет ext4 с теми же операциями.
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdint.h>
#include "../block/block.h"

#define VFS_NAME_MAX 260
#define VFS_PATH_MAX 512
#define VFS_MAX_MOUNTS 8

/* Коды возврата VFS. */
#define VFS_OK         0
#define VFS_ENOENT    -1   /* нет такого файла/каталога */
#define VFS_EEXIST    -2   /* уже существует           */
#define VFS_ENOSPC    -3   /* нет места                */
#define VFS_EIO       -4   /* ошибка ввода-вывода      */
#define VFS_ENOTDIR   -5   /* не каталог               */
#define VFS_EISDIR    -6   /* это каталог              */
#define VFS_ENOTEMPTY -7   /* каталог не пуст          */
#define VFS_EINVAL    -8   /* неверные аргументы       */

struct vfs_fs;

/**
 * @brief Описание узла (файла/каталога), возвращаемое ФС.
 *
 * Помимо имени/размера узел несёт «приватные» поля ФС: для FAT32 это
 * первый кластер (id) и физическое расположение записи каталога
 * (dir_lba/dir_off), чтобы потом можно было обновить размер или удалить.
 */
struct vfs_node {
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint8_t  is_dir;
    uint32_t id;         /* идентификатор узла в ФС (FAT32: первый кластер) */
    uint32_t parent_id;  /* идентификатор родительского каталога             */
    uint32_t dir_lba;    /* сектор с записью каталога (0 — нет)              */
    uint32_t dir_off;    /* смещение записи внутри сектора                   */
};

/**
 * @brief Набор операций, которые реализует драйвер файловой системы.
 */
struct vfs_ops {
    int (*root)(struct vfs_fs *fs, struct vfs_node *out);
    int (*find)(struct vfs_fs *fs, uint32_t dir_id, const char *name,
                struct vfs_node *out);
    int (*readdir)(struct vfs_fs *fs, uint32_t dir_id, uint32_t index,
                   struct vfs_node *out);
    int (*create)(struct vfs_fs *fs, uint32_t dir_id, const char *name,
                  struct vfs_node *out);
    int (*mkdir)(struct vfs_fs *fs, uint32_t dir_id, const char *name,
                 struct vfs_node *out);
    int (*unlink)(struct vfs_fs *fs, struct vfs_node *node);
    int (*read)(struct vfs_fs *fs, struct vfs_node *node, uint32_t offset,
                void *buf, uint32_t len, uint32_t *out_read);
    int (*write)(struct vfs_fs *fs, struct vfs_node *node, uint32_t offset,
                 const void *buf, uint32_t len, uint32_t *out_written);
    int (*truncate)(struct vfs_fs *fs, struct vfs_node *node, uint32_t size);
    void (*sync)(struct vfs_fs *fs);
};

/**
 * @brief Смонтированная файловая система (тип + драйвер + приватные данные).
 */
struct vfs_fs {
    const char *type;                /* "fat32"                    */
    struct block_device *dev;        /* носитель                   */
    struct vfs_ops *ops;             /* операции драйвера          */
    void *priv;                      /* данные драйвера ФС         */
};

/* Точка монтирования. */
struct vfs_mount {
    char path[VFS_PATH_MAX];
    struct vfs_fs *fs;
    struct vfs_mount *next;
};

/* Инициализация VFS (сброс таблицы монтирований и cwd). */
void vfs_init(void);

/* Монтирование/размонтирование. */
int  vfs_mount(const char *path, struct vfs_fs *fs);
int  vfs_umount(const char *path);
void vfs_list_mounts(void);

/* Разрешение пути (с учётом текущего каталога). */
int  vfs_resolve(const char *path, struct vfs_node *out);
int  vfs_stat(const char *path, struct vfs_node *out);

/* Операции уровня пользователя. */
int  vfs_list(const char *path);
int  vfs_read_file(const char *path, void **out_buf, uint32_t *out_len);
int  vfs_write_file(const char *path, const void *buf, uint32_t len, int create);
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);
int  vfs_chdir(const char *path);
const char *vfs_getcwd(void);
void vfs_mount_root(struct vfs_fs *fs);

const char *vfs_strerror(int err);

#endif /* KERNEL_VFS_H */
