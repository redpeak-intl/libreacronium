/**
 * @file hello.c
 * @brief Пример загружаемого модуля ядра libreacronium.
 *
 * Модуль НЕ размещается в монолитном ядре: он компилируется в отдельный
 * relocatable ELF (.ko) и загружается в рантайме. Вызов print_string() —
 * это ссылка на символ, экспортированный ядром через EXPORT_SYMBOL();
 * загрузчик подставит его адрес при релокации.
 */

#include "module.h"

/* Символ, экспортированный ядром (см. exports.c). */
extern void print_string(const char *str);

static int hello_init(void) {
    print_string("[hello] module loaded: hello from a loadable module!\n");
    return 0;
}

static void hello_exit(void) {
    print_string("[hello] module unloaded. bye!\n");
}

module_init(hello_init);
module_exit(hello_exit);
