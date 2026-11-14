/**
 * @file module.c
 * @brief Реестр модулей, таблица символов ядра и команды управления.
 *
 * Реализует Linux-подобный интерфейс:
 *   - ksym_lookup()        — поиск символа, экспортированного ядром;
 *   - module_load()        — insmod;
 *   - module_unload()      — rmmod;
 *   - module_probe()       — modprobe (с зависимостями);
 *   - module_list()        — lsmod.
 */

#include "module.h"
#include "elf_loader.h"
#include "../console.h"
#include "../video/video.h"
#include "../memory/memory.h"

/* Голова списка загруженных модулей. */
static struct kernel_module *g_modules = NULL;

/* ----------------------------------------------------------------------
 *  Локальные строковые утилиты (freestanding)
 * -------------------------------------------------------------------- */

static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int kstreq(const char *a, const char *b) {
    if (!a || !b) return 0;
    return kstrcmp(a, b) == 0;
}

/* ----------------------------------------------------------------------
 *  Разрешение символов ядра (EXPORT_SYMBOL)
 * -------------------------------------------------------------------- */

void *ksym_lookup(const char *name) {
    for (const struct ksym *k = __ksymtab_start; k < __ksymtab_end; k++) {
        if (k->name && kstreq(k->name, name)) {
            return k->addr;
        }
    }
    return NULL;
}

static uint32_t ksym_count(void) {
    return (uint32_t)(__ksymtab_end - __ksymtab_start);
}

/* ----------------------------------------------------------------------
 *  Поиск в реестре и среди встроенных модулей
 * -------------------------------------------------------------------- */

static struct kernel_module *module_find(const char *name) {
    for (struct kernel_module *m = g_modules; m; m = m->next) {
        if (kstreq(m->name, name)) {
            return m;
        }
    }
    return NULL;
}

static const struct embedded_module *embedded_find(const char *name) {
    for (const struct embedded_module *e = embedded_modules_begin();
         e < embedded_modules_end(); e++) {
        if (kstreq(e->name, name)) {
            return e;
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------------
 *  Публичный API
 * -------------------------------------------------------------------- */

int module_load(const char *name) {
    if (!name || !name[0]) {
        console_println("insmod: no module name given");
        return -1;
    }
    if (module_find(name)) {
        console_print("insmod: module already loaded: ");
        console_println(name);
        return -1;
    }

    const struct embedded_module *e = embedded_find(name);
    if (!e) {
        console_print("insmod: module not found: ");
        console_println(name);
        return -1;
    }

    /* Загрузка ELF-образа с релокацией. */
    struct kernel_module *m = elf_load_module(
        e->data, (uint32_t)(e->end - e->data), e->name, e->depends);
    if (!m) {
        console_print("insmod: failed to load ");
        console_println(name);
        return -1;
    }

    /* Вызов точки входа (аналог module_init). */
    if (m->init) {
        int rc = m->init();
        if (rc != 0) {
            console_print("insmod: init() failed for ");
            console_println(name);
            kfree(m->image);
            kfree(m);
            return -1;
        }
    }

    /* Регистрация в списке загруженных модулей. */
    m->next = g_modules;
    g_modules = m;

    console_print("insmod: loaded ");
    console_print(name);
    console_print(" (");
    print_dec((int)m->image_size);
    console_println(" bytes)");
    return 0;
}

int module_unload(const char *name) {
    if (!name || !name[0]) {
        console_println("rmmod: no module name given");
        return -1;
    }

    struct kernel_module **pp = &g_modules;
    while (*pp) {
        struct kernel_module *m = *pp;
        if (kstreq(m->name, name)) {
            if (m->refcount > 0) {
                console_print("rmmod: module in use: ");
                console_println(name);
                return -1;
            }
            if (m->exit) {
                m->exit();
            }
            *pp = m->next;
            kfree(m->image);
            kfree(m);
            console_print("rmmod: removed ");
            console_println(name);
            return 0;
        }
        pp = &(*pp)->next;
    }

    console_print("rmmod: module not loaded: ");
    console_println(name);
    return -1;
}

/**
 * @brief Рекурсивно загрузить модуль и его зависимости (modprobe).
 */
static int module_probe_recursive(const char *name) {
    /* Уже загружен — считать зависимость выполненной. */
    if (module_find(name)) {
        return 0;
    }

    const struct embedded_module *e = embedded_find(name);
    if (!e) {
        console_print("modprobe: module not found: ");
        console_println(name);
        return -1;
    }

    /* Сначала зависимости (список имён через запятую). */
    if (e->depends && e->depends[0]) {
        const char *p = e->depends;
        char dep[64];
        while (*p) {
            while (*p == ' ' || *p == ',') { p++; }
            if (!*p) { break; }

            int n = 0;
            while (*p && *p != ',' && n < (int)sizeof(dep) - 1) {
                dep[n++] = *p++;
            }
            dep[n] = '\0';
            while (n > 0 && dep[n - 1] == ' ') { dep[--n] = '\0'; }

            if (n > 0 && module_probe_recursive(dep) != 0) {
                return -1;
            }
        }
    }

    return module_load(name);
}

int module_probe(const char *name) {
    if (!name || !name[0]) {
        console_println("modprobe: no module name given");
        return -1;
    }
    return module_probe_recursive(name);
}

void module_list(void) {
    if (!g_modules) {
        console_println("No modules loaded.");
        return;
    }

    console_println("Name      Size    Refs  Depends");
    for (struct kernel_module *m = g_modules; m; m = m->next) {
        console_print("  ");
        console_print(m->name);
        console_print("      ");
        print_dec((int)m->image_size);
        console_print("    ");
        print_dec((int)m->refcount);
        console_print("     ");
        console_println(m->depends ? m->depends : "-");
    }
}

void module_subsystem_init(void) {
    console_println("");
    console_println("Module subsystem ready (insmod/rmmod/lsmod/modprobe).");
    console_print("  - exported kernel symbols: ");
    print_dec((int)ksym_count());
    console_println("");
    console_print("  - embedded modules: ");
    print_dec((int)(embedded_modules_end() - embedded_modules_begin()));
    console_println("");
}
