/**
 * @file elf_loader.c
 * @brief Загрузчик relocatable ELF (.ko) с применением релокаций.
 *
 * Реализует минимально необходимую для модулей часть ELF32 (i686) и ELF64
 * (x86_64):
 *   - разбор заголовка и таблицы секций;
 *   - размещение секций SHF_ALLOC (PROGBITS копируются, NOBITS = .bss);
 *   - разрешение неопределённых символов через ksym_lookup();
 *   - релокации: R_386_32 / R_386_PC32 для i386 и
 *     R_X86_64_64 / R_X86_64_PC32 / R_X86_64_PLT32 / R_X86_64_32 / R_X86_64_32S
 *     для x86_64.
 *
 * Пейджинга (кроме идентичного отображения) нет, поэтому адреса совпадают
 * с физическими и «загруженный адрес» символа равен его реальному адресу.
 */

#include "elf_loader.h"
#include "../console.h"
#include "../video/video.h"
#include "../memory/memory.h"

/* ----------------------------------------------------------------------
 *  Структуры ELF (набор и размеры полей зависят от разрядности)
 * -------------------------------------------------------------------- */
#ifdef __x86_64__

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf_Sym;

/* На x86_64 релокации всегда RELA (с явным аддендом). */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf_Rel_any;

#define ARCH_REL_TYPE  SHT_RELA

#define ELF_R_SYM(info)  ((uint32_t)((info) >> 32))
#define ELF_R_TYPE(info) ((uint32_t)((info) & 0xffffffff))

#define ELF_CLASS   2   /* ELFCLASS64 */
#define ELF_MACHINE 62  /* EM_X86_64 */

/* Типы релокаций x86_64 */
#define R_ARCH_ABS   1   /* R_X86_64_64     */
#define R_ARCH_PC    2   /* R_X86_64_PC32   */
#define R_ARCH_PLT   4   /* R_X86_64_PLT32  */
#define R_ARCH_32    10  /* R_X86_64_32     */
#define R_ARCH_32S   11  /* R_X86_64_32S    */

#else /* i386 */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf_Rel_any;

#define ARCH_REL_TYPE  SHT_REL

#define ELF_R_SYM(info)  ((info) >> 8)
#define ELF_R_TYPE(info) ((info) & 0xff)

#define ELF_CLASS   1  /* ELFCLASS32 */
#define ELF_MACHINE 3  /* EM_386 */

/* Типы релокаций i386 */
#define R_ARCH_ABS 1   /* R_386_32   */
#define R_ARCH_PC  2   /* R_386_PC32 */

#endif

/* Коды типов/флагов/секций */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHT_REL      9

#define SHF_ALLOC 0x2

#define SHN_UNDEF 0x0000
#define SHN_ABS   0xfff1

/* ---------------------------------------------------------------------- */

static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/**
 * @brief Разрешить адрес символа из таблицы символов модуля.
 *
 * @param data    Образ .ko (для доступа к strtab/symtab).
 * @param shdrs   Таблица секций.
 * @param shnum   Число секций.
 * @param symtab  Секция таблицы символов, к которой относится релокация.
 * @param image   Загруженный образ (база).
 * @param sec_off Смещения секций внутри образа.
 * @param symidx  Индекс символа.
 * @param out     Куда записать разрешённый адрес.
 * @return 0 при успехе, -1 при ошибке.
 */
static int elf_resolve_symbol(const uint8_t *data, const Elf_Shdr *shdrs,
                              uint16_t shnum, const Elf_Shdr *symtab,
                              const uint8_t *image, const uint32_t *sec_off,
                              uint32_t symidx, uintptr_t *out)
{
    if (symidx >= symtab->sh_size / sizeof(Elf_Sym)) {
        console_println("elf: bad symbol index");
        return -1;
    }

    const Elf_Sym *sym =
        ((const Elf_Sym *)(data + symtab->sh_offset)) + symidx;

    const Elf_Shdr *strtab = &shdrs[symtab->sh_link];
    const char *name = (const char *)(data + strtab->sh_offset + sym->st_name);

    /* Неопределённый символ — ищем в таблице экспорта ядра. */
    if (sym->st_shndx == SHN_UNDEF) {
        if (sym->st_name == 0) {
            *out = 0;
            return 0;
        }
        void *addr = ksym_lookup(name);
        if (!addr) {
            console_print("elf: unresolved symbol: ");
            console_println(name);
            return -1;
        }
        *out = (uintptr_t)addr;
        return 0;
    }

    /* Абсолютный символ — значение уже готовый адрес. */
    if (sym->st_shndx == SHN_ABS) {
        *out = (uintptr_t)sym->st_value;
        return 0;
    }

    /* Символ в неразмещаемой секции (например, debug) — не трогаем. */
    if (sym->st_shndx >= shnum || !(shdrs[sym->st_shndx].sh_flags & SHF_ALLOC)) {
        *out = (uintptr_t)sym->st_value;
        return 0;
    }

    /* Обычный символ: адрес = база образа + смещение секции + значение. */
    *out = (uintptr_t)(image + sec_off[sym->st_shndx]) + (uintptr_t)sym->st_value;
    return 0;
}

struct kernel_module *elf_load_module(const uint8_t *data, uint32_t size,
                                      const char *name, const char *depends)
{
    uint32_t     *sec_off = NULL;
    uint8_t      *image   = NULL;
    struct kernel_module *m = NULL;

    /* --- Проверка заголовка ELF --- */
    if (!data || size < sizeof(Elf_Ehdr)) { goto fail; }
    const Elf_Ehdr *eh = (const Elf_Ehdr *)data;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        console_println("elf: bad magic");
        goto fail;
    }
    if (eh->e_ident[4] != ELF_CLASS || eh->e_type != 1 /*ET_REL*/ ||
        eh->e_machine != ELF_MACHINE) {
        console_println("elf: unsupported file (wrong class/machine)");
        goto fail;
    }
    if (eh->e_shoff == 0 || eh->e_shnum == 0) { goto fail; }
    if ((uint64_t)eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf_Shdr) > size) {
        console_println("elf: section table out of range");
        goto fail;
    }

    const Elf_Shdr *shdrs = (const Elf_Shdr *)(data + eh->e_shoff);
    uint16_t shnum = eh->e_shnum;

    /* --- Расчёт размеров секций и смещений внутри единого образа --- */
    sec_off = (uint32_t *)kmalloc(shnum * sizeof(uint32_t));
    if (!sec_off) { goto fail; }
    memory_set(sec_off, 0, shnum * sizeof(uint32_t));

    uint32_t total = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        const Elf_Shdr *s = &shdrs[i];
        if (s->sh_type == SHT_NULL || !(s->sh_flags & SHF_ALLOC)) { continue; }

        uint32_t align = s->sh_addralign ? (uint32_t)s->sh_addralign : 1;
        if (align < 4) { align = 4; }
        total = (total + align - 1) & ~(align - 1);
        sec_off[i] = total;
        total += (uint32_t)s->sh_size;
    }
    total = (total + 15) & ~15u;   /* выравниваем образ целиком */

    if (total == 0) {
        console_println("elf: nothing to load");
        goto fail;
    }

    image = (uint8_t *)kmalloc(total);
    if (!image) { goto fail; }
    memory_set(image, 0, total);   /* .bss (NOBITS) остаётся нулевым */

    /* --- Копирование PROGBITS-секций --- */
    for (uint16_t i = 0; i < shnum; i++) {
        const Elf_Shdr *s = &shdrs[i];
        if (!(s->sh_flags & SHF_ALLOC) || s->sh_type != SHT_PROGBITS) { continue; }
        if ((uint64_t)s->sh_offset + s->sh_size > size) {
            console_println("elf: section content out of range");
            goto fail;
        }
        memory_copy(image + sec_off[i], data + s->sh_offset, s->sh_size);
    }

    /* --- Применение релокаций --- */
    for (uint16_t i = 0; i < shnum; i++) {
        const Elf_Shdr *rel = &shdrs[i];
        if (rel->sh_type != ARCH_REL_TYPE) { continue; }

        uint16_t target = rel->sh_info;                 /* к какой секции */
        if (target >= shnum || !(shdrs[target].sh_flags & SHF_ALLOC)) { continue; }

        const Elf_Shdr *symtab = &shdrs[rel->sh_link];
        const Elf_Rel_any *rels = (const Elf_Rel_any *)(data + rel->sh_offset);
        uint32_t nrel = (uint32_t)(rel->sh_size / sizeof(Elf_Rel_any));

        for (uint32_t r = 0; r < nrel; r++) {
            uint32_t r_sym  = ELF_R_SYM(rels[r].r_info);
            uint32_t r_type = ELF_R_TYPE(rels[r].r_info);

            uintptr_t S;
            if (elf_resolve_symbol(data, shdrs, shnum, symtab, image, sec_off,
                                   r_sym, &S) != 0) {
                goto fail;
            }

            uint8_t *place = image + sec_off[target] + rels[r].r_offset;
            uintptr_t P = (uintptr_t)place;

#ifdef __x86_64__
            int64_t A = (int64_t)rels[r].r_addend;
            if (r_type == R_ARCH_ABS) {
                /* 64-битная абсолютная: S + A */
                *(uint64_t *)place = (uint64_t)(S + (uintptr_t)A);
            } else if (r_type == R_ARCH_PC || r_type == R_ARCH_PLT) {
                /* 32-битная относительно PC: S + A - P */
                *(int32_t *)place = (int32_t)((intptr_t)S + A - (intptr_t)P);
            } else if (r_type == R_ARCH_32 || r_type == R_ARCH_32S) {
                /* 32-битная абсолютная: S + A */
                *(int32_t *)place = (int32_t)((intptr_t)S + A);
            } else
#else
            /* На i386 адденд лежит прямо в месте релокации. */
            int64_t A = (int64_t)*(int32_t *)place;
            if (r_type == R_ARCH_ABS) {
                *(uint32_t *)place = (uint32_t)(S + (uintptr_t)A);
            } else if (r_type == R_ARCH_PC) {
                *(uint32_t *)place = (uint32_t)(S + (uintptr_t)A - P);
            } else
#endif
            {
                console_print("elf: unsupported relocation type ");
                print_dec((int)r_type);
                console_println("");
                goto fail;
            }
        }
    }

    /* --- Поиск точек входа module_init/module_exit --- */
    const char *shstr = (const char *)(data + shdrs[eh->e_shstrndx].sh_offset);
    int  (*init)(void) = NULL;
    void (*exit)(void) = NULL;

    for (uint16_t i = 0; i < shnum; i++) {
        const Elf_Shdr *s = &shdrs[i];
        if (s->sh_type == SHT_NULL || !(s->sh_flags & SHF_ALLOC)) { continue; }

        const char *sname = shstr + s->sh_name;
        if (kstrcmp(sname, ".module.init") == 0 && s->sh_size >= sizeof(void *)) {
            init = (int (*)(void))*(void **)(image + sec_off[i]);
        } else if (kstrcmp(sname, ".module.exit") == 0 && s->sh_size >= sizeof(void *)) {
            exit = (void (*)(void))*(void **)(image + sec_off[i]);
        }
    }

    /* --- Формирование записи реестра --- */
    m = (struct kernel_module *)kmalloc(sizeof(struct kernel_module));
    if (!m) { goto fail; }

    m->name       = name;
    m->depends    = depends;
    m->image      = image;
    m->image_size = total;
    m->init       = init;
    m->exit       = exit;
    m->refcount   = 0;
    m->next       = NULL;

    kfree(sec_off);
    return m;

fail:
    if (sec_off) { kfree(sec_off); }
    if (image)   { kfree(image); }
    return NULL;
}
