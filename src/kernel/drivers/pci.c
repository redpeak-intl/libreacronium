/**
 * @file pci.c
 * @brief Реализация минимального драйвера шины PCI (механизм Configuration Space #1).
 *
 * Доступ к пространству конфигурации выполняется через пару портов
 * 0xCF8 (адрес) и 0xCFC (данные). Такое устройство называется "механизм #1"
 * и поддерживается всеми x86-машинами (включая QEMU).
 */

#include "pci.h"
#include "../idt/idt.h"
#include "../video/video.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* ------------------------------------------------------------------ */
/* Порт ввода-вывода                                                  */
/* ------------------------------------------------------------------ */

static inline void pci_outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ------------------------------------------------------------------ */
/* Доступ к configuration space                                       */
/* ------------------------------------------------------------------ */

static uint32_t pci_make_address(uint8_t bus, uint8_t slot, uint8_t func,
                                 uint8_t offset) {
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func <<  8)
         | (offset & 0xFCu);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    pci_outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    return pci_inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
    uint32_t v = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset) {
    uint32_t v = pci_config_read32(bus, slot, func, offset);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint32_t value) {
    pci_outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    pci_outl(PCI_CONFIG_DATA, value);
}

static void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t offset, uint16_t value) {
    uint32_t old = pci_config_read32(bus, slot, func, offset);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t nv = (old & ~mask) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, offset, nv);
}

/* ------------------------------------------------------------------ */
/* Поиск устройств                                                    */
/* ------------------------------------------------------------------ */

static void pci_fill_device(uint8_t bus, uint8_t slot, uint8_t func,
                            pci_device_t *out) {
    uint32_t id = pci_config_read32(bus, slot, func, 0x00);
    out->bus       = bus;
    out->slot      = slot;
    out->func      = func;
    out->vendor_id = (uint16_t)(id & 0xFFFF);
    out->device_id = (uint16_t)(id >> 16);

    uint32_t cc = pci_config_read32(bus, slot, func, 0x08);
    out->revision  = (uint8_t)(cc & 0xFF);
    out->prog_if   = (uint8_t)((cc >> 8) & 0xFF);
    out->subclass  = (uint8_t)((cc >> 16) & 0xFF);
    out->class_code= (uint8_t)((cc >> 24) & 0xFF);

    out->irq_line  = pci_config_read8(bus, slot, func, 0x3C);

    for (int i = 0; i < 6; i++) {
        out->bar[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
    }
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    if (!out) return -1;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read32((uint8_t)bus, slot, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) {
                continue; /* Устройства нет */
            }
            if (device_id == 0xFFFF || (uint16_t)(id >> 16) == device_id) {
                if ((uint16_t)(id & 0xFFFF) == vendor_id) {
                    pci_fill_device((uint8_t)bus, slot, 0, out);
                    return 0;
                }
            }

            /* Проверяем многофункциональность по Header Type */
            uint8_t header_type = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
            if (header_type & 0x80) {
                for (uint8_t func = 1; func < 8; func++) {
                    uint32_t fid = pci_config_read32((uint8_t)bus, slot, func, 0x00);
                    if ((fid & 0xFFFF) == 0xFFFF) continue;
                    if ((fid & 0xFFFF) == vendor_id &&
                        (device_id == 0xFFFF || (uint16_t)(fid >> 16) == device_id)) {
                        pci_fill_device((uint8_t)bus, slot, func, out);
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Command register                                                   */
/* ------------------------------------------------------------------ */

void pci_enable_mmio_busmaster(const pci_device_t *dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0002; /* Memory Space Enable */
    cmd |= 0x0004; /* Bus Master Enable  */
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

uint32_t pci_bar_mmio_base(const pci_device_t *dev, int bar_index) {
    if (!dev || bar_index < 0 || bar_index > 5) return 0;

    uint32_t bar = dev->bar[bar_index];
    if (bar & 0x1) {
        return 0; /* Пространство ввода-вывода, не MMIO */
    }

    uint32_t type = (bar >> 1) & 0x3;
    if (type == 0x2) {
        /* 64-битный BAR: старшая часть в следующем регистре. В нашем случае
           адресное пространство < 4 GiB, поэтому берём младшие 32 бита. */
        return bar & 0xFFFFFFF0u;
    }
    return bar & 0xFFFFFFF0u;
}
