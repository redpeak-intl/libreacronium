/**
 * @file fat32.c
 * @brief Полноценный драйвер FAT32: чтение и запись.
 *
 * Реализует операции struct vfs_ops поверх блочного устройства:
 *  - монтирование (BPB, FSInfo, проверка сигнатур);
 *  - разбор каталогов с поддержкой длинных имён (LFN);
 *  - чтение/запись файлов с ростом и обрезкой цепочек кластеров;
 *  - создание файлов/каталогов, удаление, mkdir;
 *  - аллокацию/освобождение кластеров и обновление двух копий FAT + FSInfo.
 *
 * Имена хранятся в ASCII: при чтении LFN (UCS-2) не-ASCII символы
 * заменяются на '?', при создании имя конвертируется в UCS-2 из ASCII.
 */

#include "fat32.h"
#include "../memory/memory.h"
#include "../video/video.h"

/* --- служебные константы FAT --- */
#define FAT_EOC      0x0FFFFFFF
#define FAT_FREE     0x00000000
#define FAT_BAD      0x0FFFFFF7

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_VOLUME  0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F

#define MAX_LFN_PARTS 20
#define MAX_ENTRIES   40   /* LFN(20) + SFN + запас */

struct fat32_fs {
    struct block_device *dev;

    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size;          /* секторов на одну FAT */
    uint32_t total_sectors;

    uint32_t fat_start;         /* первый сектор первой FAT */
    uint32_t data_start;        /* первый сектор области данных */
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t cluster_size;      /* байт в кластере */

    uint16_t fs_info_sector;
    uint32_t free_count;
    uint32_t next_free;

    uint8_t *secbuf;            /* буфер одного сектора */
    uint8_t *cbuf;              /* буфер одного кластера */
};

/* ---- little-endian доступ ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ---- строковые утилиты ---- */
static uint32_t s_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void s_copy(char *d, const char *s, uint32_t max) {
    uint32_t i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char lo(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (up(*a) != up(*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ---- чтение/запись кластеров ---- */
static uint32_t sector_of_cluster(struct fat32_fs *fs, uint32_t cl) {
    return fs->data_start + (cl - 2) * fs->sectors_per_cluster;
}
static uint32_t cluster_of_lba(struct fat32_fs *fs, uint32_t lba) {
    return 2 + (lba - fs->data_start) / fs->sectors_per_cluster;
}
static int read_cluster(struct fat32_fs *fs, uint32_t cl, uint8_t *buf) {
    return block_read(fs->dev, sector_of_cluster(fs, cl), buf,
                      fs->sectors_per_cluster);
}
static int write_cluster(struct fat32_fs *fs, uint32_t cl, const uint8_t *buf) {
    return block_write(fs->dev, sector_of_cluster(fs, cl), buf,
                       fs->sectors_per_cluster);
}
static int zero_cluster(struct fat32_fs *fs, uint32_t cl) {
    memory_set(fs->cbuf, 0, fs->cluster_size);
    return write_cluster(fs, cl, fs->cbuf);
}

/* ---- FAT ---- */
static int fat_get(struct fat32_fs *fs, uint32_t cl, uint32_t *out) {
    uint32_t off = cl * 4;
    uint32_t sect = fs->fat_start + off / fs->bytes_per_sector;
    uint32_t in = off % fs->bytes_per_sector;
    if (block_read(fs->dev, sect, fs->secbuf, 1) != 0) return -1;
    *out = rd32(fs->secbuf + in) & 0x0FFFFFFF;
    return 0;
}

static int fat_set(struct fat32_fs *fs, uint32_t cl, uint32_t value) {
    uint32_t off = cl * 4;
    uint32_t sect_in = off / fs->bytes_per_sector;
    uint32_t in = off % fs->bytes_per_sector;
    for (uint8_t f = 0; f < fs->num_fats; f++) {
        uint32_t sect = fs->fat_start + (uint32_t)f * fs->fat_size + sect_in;
        if (block_read(fs->dev, sect, fs->secbuf, 1) != 0) return -1;
        uint32_t old = rd32(fs->secbuf + in);
        uint32_t nv = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        wr32(fs->secbuf + in, nv);
        if (block_write(fs->dev, sect, fs->secbuf, 1) != 0) return -1;
    }
    return 0;
}

/*
 * Следующий кластер цепочки:
 *   0 — есть продолжение (в *out_next), 1 — конец цепочки, -1 — ошибка.
 */
static int cluster_next(struct fat32_fs *fs, uint32_t cl, uint32_t *out_next) {
    uint32_t v;
    if (fat_get(fs, cl, &v) != 0) return -1;
    if (v == 0 || v == FAT_BAD) return 1;
    if (v >= 0x0FFFFFF8) return 1;
    *out_next = v;
    return 0;
}

static int nth_cluster(struct fat32_fs *fs, uint32_t start, uint32_t index,
                       uint32_t *out) {
    uint32_t cl = start;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) return -1;
        cl = nxt;
    }
    *out = cl;
    return 0;
}

/* ---- FSInfo ---- */
static void fsinfo_save(struct fat32_fs *fs) {
    if (!fs->fs_info_sector) return;
    if (block_read(fs->dev, fs->fs_info_sector, fs->secbuf, 1) != 0) return;
    if (rd32(fs->secbuf + 0) != 0x41615252) return;   /* нет сигнатуры */
    wr32(fs->secbuf + 488, fs->free_count);
    wr32(fs->secbuf + 492, fs->next_free);
    block_write(fs->dev, fs->fs_info_sector, fs->secbuf, 1);
}

/* ---- аллокация/освобождение кластеров ---- */
static int alloc_cluster(struct fat32_fs *fs, uint32_t *out) {
    for (uint32_t i = 0; i < fs->cluster_count; i++) {
        uint32_t cl = 2 + ((fs->next_free + i) % fs->cluster_count);
        uint32_t v;
        if (fat_get(fs, cl, &v) != 0) return -1;
        if (v == FAT_FREE) {
            if (fat_set(fs, cl, FAT_EOC) != 0) return -1;
            if (fs->free_count > 0) fs->free_count--;
            fs->next_free = cl + 1;
            fsinfo_save(fs);
            zero_cluster(fs, cl);
            *out = cl;
            return 0;
        }
    }
    return -1;
}

static void free_chain(struct fat32_fs *fs, uint32_t start) {
    uint32_t cl = start;
    while (cl >= 2 && cl < fs->cluster_count + 2) {
        uint32_t nxt;
        int end = cluster_next(fs, cl, &nxt);
        fat_set(fs, cl, FAT_FREE);
        fs->free_count++;
        if (end != 0) break;
        cl = nxt;
    }
    fsinfo_save(fs);
}

/* Урезать цепочку до keep кластеров, освободив хвост. */
static void truncate_chain(struct fat32_fs *fs, uint32_t start, uint32_t keep) {
    uint32_t cl = start;
    for (uint32_t i = 0; i + 1 < keep; i++) {
        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) { keep = i + 1; break; }
        cl = nxt;
    }
    uint32_t nxt;
    if (cluster_next(fs, cl, &nxt) == 0) {
        fat_set(fs, cl, FAT_EOC);
        free_chain(fs, nxt);
    }
}

/* Удлинить цепочку до не менее n кластеров. */
static int extend_chain(struct fat32_fs *fs, uint32_t start, uint32_t n) {
    uint32_t count = 1, cl = start, nxt;
    while (cluster_next(fs, cl, &nxt) == 0) { cl = nxt; count++; }
    while (count < n) {
        uint32_t nc;
        if (alloc_cluster(fs, &nc) != 0) return -1;
        if (fat_set(fs, cl, nc) != 0) return -1;
        cl = nc;
        count++;
    }
    return 0;
}

/* ---- имена каталогов ---- */
static uint8_t lfn_checksum(const uint8_t *sfn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    }
    return sum;
}

static void sfn_to_name(const uint8_t *e, char *out) {
    uint8_t nt = e[12];
    int k = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = e[i];
        if (c == ' ') break;
        if (i == 0 && c == 0x05) c = 0xE5;
        if (nt & 0x08) c = (uint8_t)lo((char)c);
        out[k++] = (char)c;
    }
    int any = 0;
    for (int i = 8; i < 11; i++) if (e[i] != ' ') { any = 1; break; }
    if (any) {
        out[k++] = '.';
        for (int i = 8; i < 11; i++) {
            uint8_t c = e[i];
            if (c == ' ') break;
            if (nt & 0x10) c = (uint8_t)lo((char)c);
            out[k++] = (char)c;
        }
    }
    out[k] = '\0';
}

/* Собрать отображаемое имя: из LFN-частей (если есть) или из SFN. */
static void build_name(const uint8_t *e, const uint16_t lfn[MAX_LFN_PARTS][13],
                       int have_lfn, int maxseq, char *out) {
    if (have_lfn && maxseq >= 1) {
        int k = 0;
        for (int s = 0; s < maxseq && s < MAX_LFN_PARTS; s++) {
            for (int j = 0; j < 13; j++) {
                uint16_t c = lfn[s][j];
                if (c == 0x0000) { out[k] = '\0'; return; }
                if (c == 0xFFFF) continue;
                out[k++] = (c >= 0x20 && c < 0x80) ? (char)c : '?';
                if (k >= VFS_NAME_MAX - 1) { out[k] = '\0'; return; }
            }
        }
        out[k] = '\0';
        return;
    }
    sfn_to_name(e, out);
}

static int slot_free(const uint8_t *b) { return b[0] == 0x00 || b[0] == 0xE5; }

/* Проверить, занято ли короткое имя в каталоге. */
static int short_exists(struct fat32_fs *fs, uint32_t dir_id, const uint8_t *sfn) {
    uint32_t cl = dir_id;
    uint32_t epc = fs->cluster_size / 32;
    for (;;) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return 0;
        for (uint32_t e = 0; e < epc; e++) {
            uint8_t *p = fs->cbuf + e * 32;
            if (p[0] == 0x00) return 0;
            if (p[0] == 0xE5) continue;
            if (p[11] == ATTR_LFN || (p[11] & ATTR_VOLUME)) continue;
            int same = 1;
            for (int i = 0; i < 11; i++) if (p[i] != sfn[i]) { same = 0; break; }
            if (same) return 1;
        }
        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) return 0;
        cl = nxt;
    }
}

static int valid_char(uint8_t c) {
    if (c <= 0x20 || c >= 0x7F) return 0;
    const char *bad = "\"*+,/:;<=>?[\\]|";
    for (const char *p = bad; *p; p++) if ((char)c == *p) return 0;
    return 1;
}

/*
 * Построить короткое имя 8.3 (11 байт) и определить, нужна ли LFN.
 * name — ASCII-имя, которое ввёл пользователь.
 * nt — байт флагов регистра (e[12]): 0x08 — база в нижнем регистре,
 *      0x10 — расширение в нижнем регистре. Позволяет хранить имена вида
 *      "test1.txt" прямо в SFN, без лишних LFN-записей.
 */
static int make_short_name(struct fat32_fs *fs, uint32_t dir_id,
                           const char *name, uint8_t *sfn, uint8_t *nt,
                           int *need_lfn) {
    *need_lfn = 0;
    *nt = 0;

    /* Разбить на базу и расширение. */
    const char *dot = (const char *)0;
    int dots = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') { dots++; dot = p; }
    }
    if (dot == name) dot = (const char *)0; /* ".hidden" — без расширения */

    char base[64]; char ext[16];
    uint32_t bl = 0, el = 0;
    uint32_t base_limit = (dot ? (uint32_t)(dot - name) : s_len(name));
    for (uint32_t i = 0; i < base_limit && bl < sizeof(base) - 1; i++) base[bl++] = name[i];
    base[bl] = '\0';
    if (dot) {
        for (const char *p = dot + 1; *p && el < sizeof(ext) - 1; p++) ext[el++] = *p;
        ext[el] = '\0';
    } else {
        ext[0] = '\0';
    }

    int changed = 0;
    int base_lower = 0, ext_lower = 0;
    for (uint32_t i = 0; i < bl; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') { base[i] = up(c); base_lower = 1; }
        else if (!valid_char((uint8_t)c)) { base[i] = '_'; changed = 1; }
    }
    for (uint32_t i = 0; i < el; i++) {
        char c = ext[i];
        if (c >= 'a' && c <= 'z') { ext[i] = up(c); ext_lower = 1; }
        else if (!valid_char((uint8_t)c)) { ext[i] = '_'; changed = 1; }
    }

    int fits = (bl >= 1 && bl <= 8 && el <= 3 && dots <= 1);
    if (!fits) changed = 1;

    if (!changed) {
        memory_set(sfn, ' ', 11);
        for (uint32_t i = 0; i < bl; i++) sfn[i] = (uint8_t)base[i];
        for (uint32_t i = 0; i < el; i++) sfn[8 + i] = (uint8_t)ext[i];
        if (!short_exists(fs, dir_id, sfn)) {
            *nt = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
            return VFS_OK;
        }
        changed = 1;
    }

    /* Мангling: <base~N>.EXT */
    *need_lfn = 1;
    for (uint32_t n = 1; n < 1000000; n++) {
        char num[8]; int nl = 0;
        uint32_t t = n;
        char tmp[8]; int tl = 0;
        while (t) { tmp[tl++] = (char)('0' + t % 10); t /= 10; }
        while (tl) num[nl++] = tmp[--tl];
        num[nl] = '\0';

        uint32_t keep = 8 - 1 - (uint32_t)nl;
        if (keep > bl) keep = bl;
        if (keep < 1) keep = 1;

        memory_set(sfn, ' ', 11);
        for (uint32_t i = 0; i < keep; i++) sfn[i] = (uint8_t)base[i];
        sfn[keep] = '~';
        for (int i = 0; i < nl; i++) sfn[keep + 1 + i] = (uint8_t)num[i];
        for (uint32_t i = 0; i < el && i < 3; i++) sfn[8 + i] = (uint8_t)ext[i];

        if (!short_exists(fs, dir_id, sfn)) return VFS_OK;
    }
    return VFS_EEXIST;
}

/* Сформировать LFN-части + запись SFN. */
static int build_entries(const char *name, const uint8_t *sfn, int need_lfn,
                         uint8_t attr, uint8_t nt, uint32_t cluster,
                         uint32_t size, uint8_t *ents) {
    static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int n = 0;

    if (need_lfn) {
        uint16_t u[VFS_NAME_MAX];
        int ul = 0;
        for (uint32_t i = 0; name[i] && i < VFS_NAME_MAX - 1; i++) u[ul++] = (uint8_t)name[i];
        int nlfn = (ul + 12) / 13;
        if (nlfn < 1) nlfn = 1;
        uint8_t csum = lfn_checksum(sfn);
        for (int seq = nlfn; seq >= 1; seq--) {
            uint8_t *e = ents + n * 32;
            memory_set(e, 0, 32);
            e[0] = (uint8_t)(seq | (seq == nlfn ? 0x40 : 0x00));
            e[11] = ATTR_LFN;
            e[13] = csum;
            for (int j = 0; j < 13; j++) {
                int gi = (seq - 1) * 13 + j;
                uint16_t v = (gi < ul) ? u[gi] : (gi == ul ? 0x0000 : 0xFFFF);
                e[pos[j]] = (uint8_t)(v & 0xFF);
                e[pos[j] + 1] = (uint8_t)(v >> 8);
            }
            n++;
        }
    }

    uint8_t *e = ents + n * 32;
    memory_set(e, 0, 32);
    memory_copy(e, sfn, 11);
    e[11] = attr;
    e[12] = nt;
    wr16(e + 20, (uint16_t)((cluster >> 16) & 0x0FFF));
    wr16(e + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(e + 28, size);
    n++;
    return n;
}

/* ---- размещение записей в каталоге ---- */
static int dir_reserve(struct fat32_fs *fs, uint32_t dir_id, uint32_t n,
                       uint32_t *oc, uint32_t *oi) {
    uint32_t epc = fs->cluster_size / 32;
    uint32_t placed = 0;
    uint32_t cl = dir_id;

    while (placed < n) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint32_t i = 0;
        while (i < epc && placed < n) {
            if (!slot_free(fs->cbuf + i * 32)) { i++; continue; }
            uint32_t j = i, run = 0;
            for (;;) {
                if (j >= epc) { run = epc - i; break; }
                uint8_t b0 = fs->cbuf[j * 32];
                if (b0 == 0x00) { run = epc - i; break; }
                if (b0 == 0xE5) { j++; continue; }
                run = j - i; break;
            }
            if (run >= (n - placed)) {
                uint32_t need = n - placed;
                for (uint32_t k = 0; k < need; k++) {
                    oc[placed] = cl; oi[placed] = i + k; placed++;
                }
                break;
            }
            if (run == epc - i) {
                for (uint32_t k = i; k < epc && placed < n; k++) {
                    oc[placed] = cl; oi[placed] = k; placed++;
                }
                break;
            }
            i = i + run;   /* упёрлись в занятую запись — ищем дальше */
        }
        if (placed >= n) break;

        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) == 0) {
            cl = nxt;
        } else {
            uint32_t nc;
            if (alloc_cluster(fs, &nc) != 0) return VFS_ENOSPC;
            if (fat_set(fs, cl, nc) != 0) return VFS_EIO;
            fsinfo_save(fs);
            cl = nc;
        }
    }
    return VFS_OK;
}

static int dir_commit(struct fat32_fs *fs, uint32_t *oc, uint32_t *oi,
                      const uint8_t *ents, int n) {
    uint32_t epc = fs->cluster_size / 32;
    int start = 0;
    while (start < n) {
        uint32_t cl = oc[start];
        int end = start;
        while (end < n && oc[end] == cl) end++;

        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint32_t last_idx = 0;
        for (int k = start; k < end; k++) {
            memory_copy(fs->cbuf + oi[k] * 32, ents + k * 32, 32);
            last_idx = oi[k];
        }
        if (end == n && last_idx + 1 < epc) {
            fs->cbuf[(last_idx + 1) * 32] = 0x00;
        }
        if (write_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        start = end;
    }
    return VFS_OK;
}

/* ---- поиск и листинг ---- */
static int fat_find(struct vfs_fs *vfs, uint32_t dir_id, const char *name,
                    struct vfs_node *out) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    uint32_t epc = fs->cluster_size / 32;
    uint32_t cl = dir_id;

    for (;;) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint16_t lfn[MAX_LFN_PARTS][13];
        int have_lfn = 0, maxseq = 0;

        for (uint32_t e = 0; e < epc; e++) {
            uint8_t *p = fs->cbuf + e * 32;
            if (p[0] == 0x00) return VFS_ENOENT;
            if (p[0] == 0xE5) { have_lfn = 0; maxseq = 0; continue; }
            if (p[11] == ATTR_LFN) {
                int seq = p[0] & 0x3F;
                if (seq >= 1 && seq <= MAX_LFN_PARTS) {
                    static const int lpos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    uint16_t *slot = lfn[seq - 1];
                    for (int j = 0; j < 13; j++) slot[j] = rd16(p + lpos[j]);
                    if (seq > maxseq) maxseq = seq;
                    have_lfn = 1;
                }
                continue;
            }
            if (p[11] & ATTR_VOLUME) { have_lfn = 0; maxseq = 0; continue; }

            char disp[VFS_NAME_MAX];
            build_name(p, lfn, have_lfn, maxseq, disp);
            char shortc[VFS_NAME_MAX];
            sfn_to_name(p, shortc);

            if (eq_ci(name, disp) || eq_ci(name, shortc)) {
                uint32_t clu = (rd16(p + 20) & 0x0FFF) << 16 | rd16(p + 26);
                int is_dir = (p[11] & ATTR_DIR) ? 1 : 0;
                if (clu == 0 && is_dir) clu = fs->root_cluster; /* ".." корня */
                s_copy(out->name, disp, VFS_NAME_MAX);
                out->is_dir = (uint8_t)is_dir;
                out->size = is_dir ? 0 : rd32(p + 28);
                out->id = clu;
                out->parent_id = dir_id;
                out->dir_lba = sector_of_cluster(fs, cl) + (e * 32) / fs->bytes_per_sector;
                out->dir_off = (e * 32) % fs->bytes_per_sector;
                return VFS_OK;
            }
            have_lfn = 0; maxseq = 0;
        }

        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) break;
        cl = nxt;
    }
    return VFS_ENOENT;
}

static int fat_readdir(struct vfs_fs *vfs, uint32_t dir_id, uint32_t index,
                       struct vfs_node *out) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    uint32_t epc = fs->cluster_size / 32;
    uint32_t cl = dir_id;
    uint32_t real = 0;

    for (;;) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint16_t lfn[MAX_LFN_PARTS][13];
        int have_lfn = 0, maxseq = 0;

        for (uint32_t e = 0; e < epc; e++) {
            uint8_t *p = fs->cbuf + e * 32;
            if (p[0] == 0x00) return VFS_ENOENT;
            if (p[0] == 0xE5) { have_lfn = 0; maxseq = 0; continue; }
            if (p[11] == ATTR_LFN) {
                int seq = p[0] & 0x3F;
                if (seq >= 1 && seq <= MAX_LFN_PARTS) {
                    static const int lpos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
                    uint16_t *slot = lfn[seq - 1];
                    for (int j = 0; j < 13; j++) slot[j] = rd16(p + lpos[j]);
                    if (seq > maxseq) maxseq = seq;
                    have_lfn = 1;
                }
                continue;
            }
            if (p[11] & ATTR_VOLUME) { have_lfn = 0; maxseq = 0; continue; }

            char disp[VFS_NAME_MAX];
            build_name(p, lfn, have_lfn, maxseq, disp);
            uint8_t attr = p[11];
            have_lfn = 0; maxseq = 0;

            if (disp[0] == '.' && (disp[1] == '\0' || (disp[1] == '.' && disp[2] == '\0')))
                continue;

            if (real == index) {
                uint32_t clu = (rd16(p + 20) & 0x0FFF) << 16 | rd16(p + 26);
                int is_dir = (attr & ATTR_DIR) ? 1 : 0;
                s_copy(out->name, disp, VFS_NAME_MAX);
                out->is_dir = (uint8_t)is_dir;
                out->size = is_dir ? 0 : rd32(p + 28);
                out->id = clu;
                out->parent_id = dir_id;
                out->dir_lba = sector_of_cluster(fs, cl) + (e * 32) / fs->bytes_per_sector;
                out->dir_off = (e * 32) % fs->bytes_per_sector;
                return VFS_OK;
            }
            real++;
        }

        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) break;
        cl = nxt;
    }
    return VFS_ENOENT;
}

/* ---- создать узел (файл или каталог) ---- */
static int fat_create_node(struct vfs_fs *vfs, uint32_t dir_id, const char *name,
                           int is_dir, struct vfs_node *out) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;

    struct vfs_node tmp;
    if (fat_find(vfs, dir_id, name, &tmp) == VFS_OK) return VFS_EEXIST;
    if (name[0] == '\0') return VFS_EINVAL;

    uint8_t sfn[11];
    uint8_t nt = 0;
    int need_lfn = 0;
    int r = make_short_name(fs, dir_id, name, sfn, &nt, &need_lfn);
    if (r != VFS_OK) return r;

    uint32_t first_cluster = 0;
    if (is_dir) {
        if (alloc_cluster(fs, &first_cluster) != 0) return VFS_ENOSPC;
    }

    uint8_t ents[MAX_ENTRIES * 32];
    int total = build_entries(name, sfn, need_lfn,
                              is_dir ? ATTR_DIR : ATTR_ARCHIVE, nt,
                              first_cluster, 0, ents);

    uint32_t oc[MAX_ENTRIES], oi[MAX_ENTRIES];
    r = dir_reserve(fs, dir_id, (uint32_t)total, oc, oi);
    if (r != VFS_OK) return r;
    r = dir_commit(fs, oc, oi, ents, total);
    if (r != VFS_OK) return r;

    /* Заполнить узел. */
    s_copy(out->name, name, VFS_NAME_MAX);
    out->is_dir = (uint8_t)(is_dir ? 1 : 0);
    out->size = 0;
    out->id = first_cluster;
    out->parent_id = dir_id;
    uint32_t sfn_cl = oc[total - 1], sfn_i = oi[total - 1];
    out->dir_lba = sector_of_cluster(fs, sfn_cl) +
                   (sfn_i * 32) / fs->bytes_per_sector;
    out->dir_off = (sfn_i * 32) % fs->bytes_per_sector;

    /* Для каталога создать записи "." и "..". */
    if (is_dir) {
        memory_set(fs->cbuf, 0, fs->cluster_size);
        uint8_t *dot = fs->cbuf;
        memory_set(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = ATTR_DIR;
        wr16(dot + 20, (uint16_t)((first_cluster >> 16) & 0x0FFF));
        wr16(dot + 26, (uint16_t)(first_cluster & 0xFFFF));

        uint8_t *dotdot = fs->cbuf + 32;
        memory_set(dotdot, ' ', 11);
        dotdot[0] = '.';
        dotdot[1] = '.';
        dotdot[11] = ATTR_DIR;
        uint32_t parent = (dir_id == fs->root_cluster) ? 0 : dir_id;
        wr16(dotdot + 20, (uint16_t)((parent >> 16) & 0x0FFF));
        wr16(dotdot + 26, (uint16_t)(parent & 0xFFFF));

        fs->cbuf[64] = 0x00;
        if (write_cluster(fs, first_cluster, fs->cbuf) != 0) return VFS_EIO;
    }
    return VFS_OK;
}

/* ---- обновление записи каталога ---- */
static int write_entry_fields(struct fat32_fs *fs, struct vfs_node *node) {
    if (node->dir_lba == 0 && node->dir_off == 0) return VFS_EIO;
    if (block_read(fs->dev, node->dir_lba, fs->secbuf, 1) != 0) return VFS_EIO;
    uint8_t *e = fs->secbuf + node->dir_off;
    wr16(e + 20, (uint16_t)((node->id >> 16) & 0x0FFF));
    wr16(e + 26, (uint16_t)(node->id & 0xFFFF));
    wr32(e + 28, node->size);
    if (block_write(fs->dev, node->dir_lba, fs->secbuf, 1) != 0) return VFS_EIO;
    return VFS_OK;
}

/* ---- чтение/запись файла ---- */
static int fat_read(struct vfs_fs *vfs, struct vfs_node *node, uint32_t offset,
                    void *buf, uint32_t len, uint32_t *out_read) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    *out_read = 0;
    if (node->is_dir) return VFS_EISDIR;
    if (offset >= node->size || node->id == 0) return VFS_OK;
    if (offset + len > node->size) len = node->size - offset;
    if (len == 0) return VFS_OK;

    uint32_t cs = fs->cluster_size;
    uint32_t cl;
    if (nth_cluster(fs, node->id, offset / cs, &cl) != 0) return VFS_EIO;
    uint32_t within = offset % cs;
    uint32_t done = 0;

    while (done < len) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint32_t chunk = cs - within;
        if (chunk > len - done) chunk = len - done;
        memory_copy((uint8_t *)buf + done, fs->cbuf + within, chunk);
        done += chunk;
        within = 0;
        if (done < len) {
            uint32_t nxt;
            if (cluster_next(fs, cl, &nxt) != 0) return VFS_EIO;
            cl = nxt;
        }
    }
    *out_read = done;
    return VFS_OK;
}

static int fat_write(struct vfs_fs *vfs, struct vfs_node *node, uint32_t offset,
                     const void *buf, uint32_t len, uint32_t *out_written) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    *out_written = 0;
    if (node->is_dir) return VFS_EISDIR;
    if (len == 0) return VFS_OK;

    uint32_t cs = fs->cluster_size;
    uint32_t end = offset + len;
    uint32_t need = (end + cs - 1) / cs;

    if (node->id == 0) {
        uint32_t c;
        if (alloc_cluster(fs, &c) != 0) return VFS_ENOSPC;
        node->id = c;
        write_entry_fields(fs, node);
    }
    if (extend_chain(fs, node->id, need) != 0) return VFS_ENOSPC;

    uint32_t cl;
    if (nth_cluster(fs, node->id, offset / cs, &cl) != 0) return VFS_EIO;
    uint32_t within = offset % cs;
    uint32_t done = 0;

    while (done < len) {
        if (read_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        uint32_t chunk = cs - within;
        if (chunk > len - done) chunk = len - done;
        memory_copy(fs->cbuf + within, (const uint8_t *)buf + done, chunk);
        if (write_cluster(fs, cl, fs->cbuf) != 0) return VFS_EIO;
        done += chunk;
        within = 0;
        if (done < len) {
            uint32_t nxt;
            if (cluster_next(fs, cl, &nxt) != 0) return VFS_EIO;
            cl = nxt;
        }
    }

    if (end > node->size) {
        node->size = end;
        write_entry_fields(fs, node);
    }
    *out_written = done;
    return VFS_OK;
}

static int fat_truncate(struct vfs_fs *vfs, struct vfs_node *node, uint32_t size) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    if (node->is_dir) return VFS_EISDIR;
    if (size == node->size) return VFS_OK;

    uint32_t cs = fs->cluster_size;

    if (size == 0) {
        if (node->id) free_chain(fs, node->id);
        node->id = 0;
        node->size = 0;
        return write_entry_fields(fs, node);
    }

    uint32_t need = (size + cs - 1) / cs;
    if (size > node->size) {
        if (node->id == 0) {
            uint32_t c;
            if (alloc_cluster(fs, &c) != 0) return VFS_ENOSPC;
            node->id = c;
        }
        if (extend_chain(fs, node->id, need) != 0) return VFS_ENOSPC;
    } else if (node->id) {
        truncate_chain(fs, node->id, need);
        uint32_t within = size % cs;
        if (within != 0) {
            uint32_t last;
            if (nth_cluster(fs, node->id, need - 1, &last) == 0) {
                if (read_cluster(fs, last, fs->cbuf) == 0) {
                    memory_set(fs->cbuf + within, 0, cs - within);
                    write_cluster(fs, last, fs->cbuf);
                }
            }
        }
    }
    node->size = size;
    return write_entry_fields(fs, node);
}

/* ---- удаление ---- */
static void clear_slot(struct fat32_fs *fs, uint32_t cl, uint32_t idx) {
    if (read_cluster(fs, cl, fs->cbuf) != 0) return;
    fs->cbuf[idx * 32] = 0xE5;
    write_cluster(fs, cl, fs->cbuf);
}

static uint32_t prev_cluster(struct fat32_fs *fs, uint32_t start, uint32_t target) {
    uint32_t cl = start;
    for (;;) {
        uint32_t nxt;
        if (cluster_next(fs, cl, &nxt) != 0) return 0;
        if (nxt == target) return cl;
        cl = nxt;
    }
}

static void delete_entry(struct fat32_fs *fs, uint32_t dir_id,
                         uint32_t lba, uint32_t off) {
    uint32_t cl = cluster_of_lba(fs, lba);
    uint32_t epc = fs->cluster_size / 32;
    int idx = (int)(off / 32);
    clear_slot(fs, cl, (uint32_t)idx);

    /* Стереть предшествующие LFN-части. */
    int e = idx - 1;
    uint32_t c = cl;
    while (1) {
        if (e < 0) {
            uint32_t pv = prev_cluster(fs, dir_id, c);
            if (pv == 0) break;
            c = pv;
            e = (int)epc - 1;
        }
        if (read_cluster(fs, c, fs->cbuf) != 0) break;
        uint8_t *p = fs->cbuf + e * 32;
        if (p[0] != 0xE5 && p[11] == ATTR_LFN) {
            clear_slot(fs, c, (uint32_t)e);
            e--;
        } else {
            break;
        }
    }
}

static int fat_unlink(struct vfs_fs *vfs, struct vfs_node *node) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;

    if (node->is_dir) {
        /* Каталог должен быть пуст (readdir пропускает "." и ".."). */
        struct vfs_node e;
        if (fat_readdir(vfs, node->id, 0, &e) == VFS_OK) {
            return VFS_ENOTEMPTY;
        }
        if (node->id) free_chain(fs, node->id);
    } else if (node->id) {
        free_chain(fs, node->id);
    }

    delete_entry(fs, node->parent_id, node->dir_lba, node->dir_off);
    return VFS_OK;
}

/* ---- синхронизация ---- */
static void fat_sync(struct vfs_fs *vfs) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    fsinfo_save(fs);
}

static int fat_root(struct vfs_fs *vfs, struct vfs_node *out) {
    struct fat32_fs *fs = (struct fat32_fs *)vfs->priv;
    s_copy(out->name, "/", VFS_NAME_MAX);
    out->is_dir = 1;
    out->size = 0;
    out->id = fs->root_cluster;
    out->parent_id = fs->root_cluster;
    out->dir_lba = 0;
    out->dir_off = 0;
    return VFS_OK;
}

static int fat_create(struct vfs_fs *vfs, uint32_t dir_id, const char *name,
                      struct vfs_node *out) {
    return fat_create_node(vfs, dir_id, name, 0, out);
}

static int fat_mkdir(struct vfs_fs *vfs, uint32_t dir_id, const char *name,
                     struct vfs_node *out) {
    return fat_create_node(vfs, dir_id, name, 1, out);
}

static struct vfs_ops g_fat32_ops = {
    fat_root, fat_find, fat_readdir,
    fat_create, fat_mkdir, fat_unlink,
    fat_read, fat_write, fat_truncate, fat_sync
};

/* ---- монтирование ---- */
struct vfs_fs *fat32_mount(struct block_device *dev) {
    print_string("FAT32: mounting ");
    print_string(dev->name);
    print_string("... ");

    uint8_t bs[512];
    if (block_read(dev, 0, bs, 1) != 0) {
        print_string_color("read error\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }

    uint16_t bps = rd16(bs + 11);
    uint8_t  spc = bs[13];
    uint16_t reserved = rd16(bs + 14);
    uint8_t  nfats = bs[16];
    uint32_t fatsz = rd32(bs + 36);
    uint32_t rootcl = rd32(bs + 44);
    uint32_t total = rd16(bs + 19);
    if (total == 0) total = rd32(bs + 32);

    if (bps != 512 || spc == 0 || (spc & (spc - 1)) != 0) {
        print_string_color("bad BPB\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }
    if (nfats == 0 || fatsz == 0 || rootcl < 2) {
        print_string_color("not FAT32\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }

    struct fat32_fs *fs = (struct fat32_fs *)kmalloc(sizeof(struct fat32_fs));
    if (!fs) {
        print_string_color("no memory\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }
    struct vfs_fs *vfs = (struct vfs_fs *)kmalloc(sizeof(struct vfs_fs));
    if (!vfs) {
        kfree(fs);
        print_string_color("no memory\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }

    fs->dev = dev;
    fs->bytes_per_sector = bps;
    fs->sectors_per_cluster = spc;
    fs->reserved_sectors = reserved;
    fs->num_fats = nfats;
    fs->fat_size = fatsz;
    fs->total_sectors = total;
    fs->fat_start = reserved;
    fs->data_start = reserved + (uint32_t)nfats * fatsz;
    fs->cluster_size = (uint32_t)bps * spc;
    fs->root_cluster = rootcl;
    fs->fs_info_sector = rd16(bs + 48);
    fs->cluster_count = (total - fs->data_start) / spc;
    fs->free_count = 0;
    fs->next_free = 2;

    if (fs->cluster_count == 0 || rootcl >= fs->cluster_count + 2) {
        kfree(vfs); kfree(fs);
        print_string_color("bad geometry\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }

    fs->secbuf = (uint8_t *)kmalloc(bps);
    fs->cbuf = (uint8_t *)kmalloc(fs->cluster_size);
    if (!fs->secbuf || !fs->cbuf) {
        if (fs->secbuf) kfree(fs->secbuf);
        if (fs->cbuf) kfree(fs->cbuf);
        kfree(vfs); kfree(fs);
        print_string_color("no memory\n", COLOR_RED, COLOR_BLACK);
        return (struct vfs_fs *)0;
    }

    /* Прочитать FSInfo, если он есть и валиден. */
    if (fs->fs_info_sector) {
        if (block_read(dev, fs->fs_info_sector, fs->secbuf, 1) == 0 &&
            rd32(fs->secbuf + 0) == 0x41615252 &&
            rd32(fs->secbuf + 484) == 0x61417272) {
            uint32_t fc = rd32(fs->secbuf + 488);
            uint32_t nf = rd32(fs->secbuf + 492);
            if (fc <= fs->cluster_count) fs->free_count = fc;
            if (nf >= 2 && nf < fs->cluster_count + 2) fs->next_free = nf;
        }
    }

    vfs->type = "fat32";
    vfs->dev = dev;
    vfs->ops = &g_fat32_ops;
    vfs->priv = fs;

    print_string_color("OK\n", COLOR_GREEN, COLOR_BLACK);
    print_string("  - cluster size: ");
    print_dec((int)fs->cluster_size);
    print_string(" bytes, clusters: ");
    print_dec((int)fs->cluster_count);
    print_string(", free: ");
    print_dec((int)fs->free_count);
    print_string("\n");

    return vfs;
}
