/**
 * @file kmod_blob.c
 * @brief Реестр встроенных в образ ядра модулей (.ko).
 *
 * Каждый .ko собирается отдельно (см. каталог modules/) и встраивается в
 * образ ядра как «сырой» блок байт (objcopy -I binary). Здесь описывается,
 * какой блок какому имени соответствует и от чего он зависит.
 *
 * Это временный «источник» модулей: чтобы позже грузить их из ФС, достаточно
 * заменить поиск в этом массиве на чтение файла с диска — остальная часть
 * подсистемы (ELF-загрузчик, реестр, shell-команды) остаётся без изменений.
 */

#include "module.h"

/* Символы границ блоков создаются objcopy при встраивании .ko. */
extern const uint8_t kmod_hello_start[];
extern const uint8_t kmod_hello_end[];
extern const uint8_t kmod_world_start[];
extern const uint8_t kmod_world_end[];

static const struct embedded_module g_embedded_modules[] = {
    { "hello", NULL,    kmod_hello_start, kmod_hello_end },
    { "world", "hello", kmod_world_start, kmod_world_end },
};

const struct embedded_module *embedded_modules_begin(void) {
    return g_embedded_modules;
}

const struct embedded_module *embedded_modules_end(void) {
    return g_embedded_modules +
           (sizeof(g_embedded_modules) / sizeof(g_embedded_modules[0]));
}
