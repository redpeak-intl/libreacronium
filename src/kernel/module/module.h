/**
 * @file module.h
 * @brief Загружаемые модули ядра libreacronium (аналог Linux-модулей).
 *
 * Идея та же, что и в Linux:
 *   - модуль — это отдельный relocatable ELF (.ko), который НЕ входит в
 *     монолитное ядро, а подгружается в рантайме;
 *   - ядро экспортирует символы (EXPORT_SYMBOL), модуль их использует;
 *   - модуль объявляет точки входа module_init()/module_exit();
 *   - загрузкой/выгрузкой управляют insmod/rmmod/lsmod/modprobe.
 *
 * На текущем этапе модули лежат прямо в образе ядра (встроенные blob'ы),
 * но загрузчик работает с настоящим ELF и релокациями, поэтому позже
 * достаточно будет подменить источник blob'а на чтение файла из ФС.
 */

#ifndef KERNEL_MODULE_H
#define KERNEL_MODULE_H

#include <stdint.h>
#include <stddef.h>

/* ======================================================================
 *  Таблица экспортируемых символов ядра (EXPORT_SYMBOL)
 * ==================================================================== */

/**
 * @brief Одна запись в таблице символов ядра.
 */
struct ksym {
    const char *name;   /* имя символа, как оно видно модулю */
    void       *addr;   /* его абсолютный адрес */
};

/**
 * @brief Экспортировать символ ядра для использования в модулях.
 *
 * Запись попадает в секцию .ksymtab, которую линкер собирает в один
 * непрерывный массив между __ksymtab_start и __ksymtab_end.
 */
#define EXPORT_SYMBOL(sym)                                                  \
    const struct ksym __ksym_##sym                                          \
        __attribute__((section(".ksymtab"), used, aligned(4))) = {          \
            #sym, (void *)&sym                                              \
        }

extern const struct ksym __ksymtab_start[];
extern const struct ksym __ksymtab_end[];

/**
 * @brief Найти адрес экспортированного символа ядра.
 * @return Адрес символа или NULL, если символ не экспортирован.
 */
void *ksym_lookup(const char *name);

/* ======================================================================
 *  Реестр загруженных модулей
 * ==================================================================== */

/**
 * @brief Описание загруженного в память модуля.
 */
struct kernel_module {
    const char           *name;        /* имя модуля                  */
    const char           *depends;     /* зависимости (имена)         */
    uint8_t              *image;       /* образ загруженного кода     */
    uint32_t              image_size;  /* размер образа (для kfree)   */
    int                 (*init)(void); /* аналог module_init          */
    void                (*exit)(void); /* аналог module_exit          */
    uint32_t              refcount;    /* >0 — модуль используется    */
    struct kernel_module *next;        /* следующий в списке          */
};

/* ======================================================================
 *  Встроенные (embedded) модули — источник .ko на текущем этапе
 * ==================================================================== */

/**
 * @brief Встроенный в образ ядра модуль (не загружен, но доступен).
 */
struct embedded_module {
    const char    *name;     /* имя для insmod/modprobe   */
    const char    *depends;  /* "a,b" или NULL            */
    const uint8_t *data;     /* начало ELF-образа .ko     */
    const uint8_t *end;      /* конец ELF-образа .ko      */
};

const struct embedded_module *embedded_modules_begin(void);
const struct embedded_module *embedded_modules_end(void);

/* ======================================================================
 *  Макросы, используемые ВНУТРИ модулей
 * ==================================================================== */

/**
 * @brief Объявить функцию инициализации модуля (аналог module_init).
 */
#define module_init(fn)                                                     \
    static int (*const __mod_init_ptr)(void)                                \
        __attribute__((section(".module.init"), used, aligned(4))) = (fn)

/**
 * @brief Объявить функцию выгрузки модуля (аналог module_exit).
 */
#define module_exit(fn)                                                     \
    static void (*const __mod_exit_ptr)(void)                               \
        __attribute__((section(".module.exit"), used, aligned(4))) = (fn)

/* ======================================================================
 *  API подсистемы модулей (используется ядром / shell)
 * ==================================================================== */

/**
 * @brief Инициализация подсистемы модулей (вызывается из kmain).
 */
void module_subsystem_init(void);

/**
 * @brief Загрузить модуль по имени (команда insmod).
 * @return 0 при успехе, -1 при ошибке.
 */
int module_load(const char *name);

/**
 * @brief Выгрузить модуль по имени (команда rmmod).
 * @return 0 при успехе, -1 при ошибке.
 */
int module_unload(const char *name);

/**
 * @brief Загрузить модуль вместе с зависимостями (команда modprobe).
 * @return 0 при успехе, -1 при ошибке.
 */
int module_probe(const char *name);

/**
 * @brief Вывести список загруженных модулей (команда lsmod).
 */
void module_list(void);

#endif /* KERNEL_MODULE_H */
