/**
 * @file fat32.h
 * @brief Драйвер файловой системы FAT32.
 */

#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "vfs.h"
#include "../block/block.h"

/**
 * @brief Смонтировать FAT32 с блочного устройства.
 *
 * Читает загрузочный сектор и FSInfo, проверяет сигнатуры, заполняет
 * структуру vfs_fs и готовит её к работе с путями/файлами.
 *
 * @param dev блочное устройство
 * @return указатель на vfs_fs или NULL при ошибке (диагностика печатается)
 */
struct vfs_fs *fat32_mount(struct block_device *dev);

#endif /* KERNEL_FAT32_H */
