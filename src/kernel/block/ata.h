/**
 * @file ata.h
 * @brief Драйвер ATA PIO (primary bus, master, LBA28).
 */

#ifndef KERNEL_ATA_H
#define KERNEL_ATA_H

#include "block.h"

/**
 * @brief Определить ATA-диск и зарегистрировать его как блочное устройство.
 * @return Указатель на устройство или NULL, если диск не найден.
 */
struct block_device *ata_init(void);

#endif /* KERNEL_ATA_H */
