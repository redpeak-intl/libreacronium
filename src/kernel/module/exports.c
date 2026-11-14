/**
 * @file exports.c
 * @brief Экспорт символов ядра для загружаемых модулей.
 *
 * Каждая строка EXPORT_SYMBOL добавляет символ в таблицу .ksymtab,
 * по которой загрузчик разрешает неопределённые ссылки модулей.
 * Это позволяет модулю вызывать функции ядра, не слинкованные с ним
 * статически (ядро и модуль соединяются во время загрузки модуля).
 */

#include "module.h"

/* Прототипы функций ядра, которые мы хотим предоставить модулям. */
extern void     print_string(const char *str);
extern void     print_string_color(const char *str, unsigned char fg, unsigned char bg);
extern void     console_print(const char *str);
extern void     console_println(const char *str);
extern void    *kmalloc(size_t size);
extern void     kfree(void *ptr);
extern void    *krealloc(void *ptr, size_t size);
extern uint32_t pmm_alloc_page(void);
extern void     pmm_free_page(uint32_t page_addr);
extern void     pit_sleep_ms(uint32_t ms);
extern void     kernel_panic(const char *msg);

/* Экспортируем их модулям. */
EXPORT_SYMBOL(print_string);
EXPORT_SYMBOL(print_string_color);
EXPORT_SYMBOL(console_print);
EXPORT_SYMBOL(console_println);
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(krealloc);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
EXPORT_SYMBOL(pit_sleep_ms);
EXPORT_SYMBOL(kernel_panic);
