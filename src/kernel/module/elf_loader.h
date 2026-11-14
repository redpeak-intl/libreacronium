/**
 * @file elf_loader.h
 * @brief Загрузчик relocatable ELF (.ko) для подсистемы модулей.
 */

#ifndef KERNEL_ELF_LOADER_H
#define KERNEL_ELF_LOADER_H

#include <stdint.h>
#include "module.h"

/**
 * @brief Загрузить relocatable ELF-модуль в память и применить релокации.
 *
 * Функция:
 *   1. проверяет ELF-заголовок (ELF32, EM_386, ET_REL);
 *   2. размещает все секции с флагом SHF_ALLOC в одном блоке памяти;
 *   3. разрешает неопределённые символы через таблицу ядра (EXPORT_SYMBOL);
 *   4. применяет релокации R_386_32 и R_386_PC32;
 *   5. извлекает указатели на .module.init / .module.exit.
 *
 * @param data    Указатель на ELF-образ .ko.
 * @param size    Размер образа в байтах.
 * @param name    Имя модуля (для реестра).
 * @param depends Строка зависимостей "a,b" или NULL.
 * @return Описание загруженного модуля или NULL при ошибке.
 *
 * @note При ошибке выделенная память освобождается внутри функции.
 *       При успехе вызывающая сторона обязана освободить image и структуру.
 */
struct kernel_module *elf_load_module(const uint8_t *data, uint32_t size,
                                      const char *name, const char *depends);

#endif /* KERNEL_ELF_LOADER_H */
