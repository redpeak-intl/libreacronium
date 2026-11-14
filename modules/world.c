/**
 * @file world.c
 * @brief Второй пример модуля — демонстрирует зависимости (modprobe).
 *
 * Модуль world зависит от hello: команда `modprobe world` должна сначала
 * загрузить hello, а затем world (как modprobe в Linux).
 */

#include "module.h"

extern void print_string(const char *str);

static int world_init(void) {
    print_string("[world] module loaded (depends on hello).\n");
    return 0;
}

static void world_exit(void) {
    print_string("[world] module unloaded.\n");
}

module_init(world_init);
module_exit(world_exit);
