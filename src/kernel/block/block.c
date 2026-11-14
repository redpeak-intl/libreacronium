/**
 * @file block.c
 * @brief Реализация блочного слоя: таблица устройств + обёртки чтения/записи.
 */

#include "block.h"
#include "../video/video.h"

/* Небольшая таблица устройств — этого хватает для настольного ядра. */
#define MAX_BLOCK_DEVICES 8

static struct block_device *g_devices[MAX_BLOCK_DEVICES];
static int g_device_count = 0;

void block_init(void) {
    g_device_count = 0;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        g_devices[i] = (struct block_device *)0;
    }
}

int block_register(struct block_device *dev) {
    if (!dev || g_device_count >= MAX_BLOCK_DEVICES) {
        return -1;
    }
    g_devices[g_device_count++] = dev;
    return 0;
}

struct block_device *block_get(const char *name) {
    if (!name) {
        return (struct block_device *)0;
    }
    for (int i = 0; i < g_device_count; i++) {
        struct block_device *d = g_devices[i];
        const char *a = d->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            return d;
        }
    }
    return (struct block_device *)0;
}

struct block_device *block_first(void) {
    return g_device_count > 0 ? g_devices[0] : (struct block_device *)0;
}

int block_read(struct block_device *dev, uint32_t lba, void *buffer, uint32_t count) {
    if (!dev || !dev->read || count == 0) {
        return -1;
    }
    if (lba + count > dev->sector_count) {
        return -1;
    }
    return dev->read(dev, lba, (uint8_t *)buffer, count);
}

int block_write(struct block_device *dev, uint32_t lba, const void *buffer, uint32_t count) {
    if (!dev || !dev->write || count == 0) {
        return -1;
    }
    if (lba + count > dev->sector_count) {
        return -1;
    }
    return dev->write(dev, lba, (const uint8_t *)buffer, count);
}

void block_dump_info(void) {
    print_string("Block devices: ");
    print_dec(g_device_count);
    print_string("\n");
    for (int i = 0; i < g_device_count; i++) {
        struct block_device *d = g_devices[i];
        print_string("  - ");
        print_string(d->name);
        print_string(": ");
        print_dec((int)(d->sector_count / 2048));
        print_string(" MiB (");
        print_dec((int)d->sector_count);
        print_string(" sectors)\n");
    }
}
