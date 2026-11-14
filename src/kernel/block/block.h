/**
 * @file block.h
 * @brief Блочный слой: абстракция над устройствами хранения.
 *
 * Файловые системы (FAT32 и будущие) работают не с «железом», а с
 * устройством блока: умеет читать/писать сектора по LBA. Конкретный
 * драйвер (ATA PIO, RAM-диск, ...) подставляет свою реализацию.
 *
 * Это и есть «не монолит»: ФС и драйвер диска знают только этот интерфейс.
 */

#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include <stdint.h>

/* Стандартный размер сектора. */
#define BLOCK_SECTOR_SIZE 512

/**
 * @brief Блочное устройство.
 */
struct block_device {
    const char *name;          /* имя устройства ("hda")                */
    uint32_t    sector_count;  /* число секторов                        */
    int       (*read)(struct block_device *dev, uint32_t lba,
                      uint8_t *buffer, uint32_t count);
    int       (*write)(struct block_device *dev, uint32_t lba,
                       const uint8_t *buffer, uint32_t count);
};

/**
 * @brief Инициализировать блочный слой (и, при желании, драйверы).
 */
void block_init(void);

/**
 * @brief Зарегистрировать блочное устройство в таблице.
 * @return 0 при успехе, -1 если таблица заполнена.
 */
int block_register(struct block_device *dev);

/**
 * @brief Найти устройство по имени.
 * @return Указатель на устройство или NULL.
 */
struct block_device *block_get(const char *name);

/**
 * @brief Первое зарегистрированное устройство (или NULL).
 */
struct block_device *block_first(void);

/**
 * @brief Прочитать/записать сектора через устройство.
 * @return 0 при успехе, -1 при выходе за границы устройства.
 */
int block_read(struct block_device *dev, uint32_t lba, void *buffer, uint32_t count);
int block_write(struct block_device *dev, uint32_t lba, const void *buffer, uint32_t count);

/**
 * @brief Вывести список блочных устройств.
 */
void block_dump_info(void);

#endif /* KERNEL_BLOCK_H */
