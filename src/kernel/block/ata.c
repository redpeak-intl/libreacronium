/**
 * @file ata.c
 * @brief Драйвер ATA PIO: primary bus, master, LBA28.
 *
 * Умеет то, что нужно файловым системам: определить диск (IDENTIFY DEVICE),
 * прочитать и записать сектор(ы) по LBA в режиме PIO. Регистрирует себя
 * как блочное устройство, поэтому FAT32 работает поверх абстракции blk_*.
 */

#include "ata.h"
#include "../idt/idt.h"
#include "../video/video.h"

/* Порты primary ATA-канала. */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_ALT_STATUS  0x3F6

/* Биты регистра состояния. */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Команды. */
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC

/* Чтение/запись 16-битного регистра данных. */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

/* 400 нс задержка: 4 чтения альтернативного регистра состояния. */
static void ata_delay400(void) {
    for (int i = 0; i < 4; i++) {
        (void)inw(ATA_ALT_STATUS);
    }
}

/* Ждём, пока диск сбросит BSY. Возвращает 0 при успехе, -1 при таймауте. */
static int ata_wait_bsy(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if ((read_port(ATA_STATUS) & ATA_SR_BSY) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Ждём готовности к обмену данными: BSY=0 и DRQ=1. */
static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = read_port(ATA_STATUS);
        if (status & ATA_SR_BSY) {
            continue;
        }
        if (status & (ATA_SR_DF | ATA_SR_ERR)) {
            return -1;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
    }
    return -1;
}

/* Выбрать диск и выставить LBA (LBA28, master). */
static void ata_select(uint32_t lba) {
    write_port(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay400();
}

static int ata_read(struct block_device *dev, uint32_t lba,
                    uint8_t *buffer, uint32_t count) {
    (void)dev;
    if (ata_wait_bsy() != 0) {
        return -1;
    }

    write_port(ATA_SECTOR_CNT, (uint8_t)count);
    write_port(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    write_port(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    write_port(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    ata_select(lba);
    write_port(ATA_COMMAND, ATA_CMD_READ_PIO);

    uint16_t *buf = (uint16_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < 256; i++) {
            uint16_t word = inw(ATA_DATA);
            *buf++ = word;
        }
        ata_delay400();
    }
    return 0;
}

static int ata_write(struct block_device *dev, uint32_t lba,
                     const uint8_t *buffer, uint32_t count) {
    (void)dev;
    if (ata_wait_bsy() != 0) {
        return -1;
    }

    write_port(ATA_SECTOR_CNT, (uint8_t)count);
    write_port(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    write_port(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    write_port(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    ata_select(lba);
    write_port(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    const uint16_t *buf = (const uint16_t *)buffer;
    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < 256; i++) {
            outw(ATA_DATA, *buf++);
        }
        ata_delay400();
    }

    /* Сброс кэша записи: команда FLUSH CACHE (0xE7). */
    write_port(ATA_COMMAND, 0xE7);
    (void)ata_wait_bsy();
    return 0;
}

/* Одно блочное устройство для primary master. */
static struct block_device g_ata_hda = {
    "hda", 0, ata_read, ata_write
};

struct block_device *ata_init(void) {
    print_string("ATA: probing primary master... ");

    /* Выбираем master и ждём, пока диск успокоится. */
    write_port(ATA_DRIVE_HEAD, 0xA0);
    ata_delay400();
    if (ata_wait_bsy() != 0) {
        print_string_color("no device\n", COLOR_YELLOW, COLOR_BLACK);
        return (struct block_device *)0;
    }

    /* Некоторые диски возвращают 0 в LBA_MID/LBA_HIGH только для ATA. */
    write_port(ATA_LBA_LOW, 0);
    write_port(ATA_LBA_MID, 0);
    write_port(ATA_LBA_HIGH, 0);
    write_port(ATA_COMMAND, ATA_CMD_IDENTIFY);

    /* Диска нет: статус == 0. */
    if (read_port(ATA_STATUS) == 0) {
        print_string_color("no drive\n", COLOR_YELLOW, COLOR_BLACK);
        return (struct block_device *)0;
    }

    if (ata_wait_drq() != 0) {
        print_string_color("identify failed\n", COLOR_YELLOW, COLOR_BLACK);
        return (struct block_device *)0;
    }

    /* Читаем 256 слов IDENTIFY DEVICE. */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_DATA);
    }

    /* Слова 60-61: количество секторов LBA28 (little-endian). */
    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    if (sectors == 0) {
        print_string_color("zero capacity\n", COLOR_YELLOW, COLOR_BLACK);
        return (struct block_device *)0;
    }

    g_ata_hda.sector_count = sectors;
    if (block_register(&g_ata_hda) != 0) {
        print_string_color("register failed\n", COLOR_YELLOW, COLOR_BLACK);
        return (struct block_device *)0;
    }

    print_string_color("OK\n", COLOR_GREEN, COLOR_BLACK);
    print_string("  - hda: ");
    print_dec((int)(sectors / 2048));
    print_string(" MiB (");
    print_dec((int)sectors);
    print_string(" sectors)\n");

    return &g_ata_hda;
}
