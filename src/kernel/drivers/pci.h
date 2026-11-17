/**
 * @file pci.h
 * @brief Минимальный драйвер шины PCI (механизм configuration space #1).
 *
 * Позволяет найти устройство по Vendor/Device ID и прочитать его BAR-ы,
 * линию прерывания и т.п. Используется сетевым драйвером (e1000).
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_VENDOR_INTEL 0x8086

/* Найденное PCI-устройство */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];       /* Сырые значения BAR-ов */
} pci_device_t;

/* Доступ к configuration space */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Найти первое устройство с заданными Vendor/Device ID. Возвращает 0 при успехе. */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/* Включить доступ к MMIO (Memory Space) и Bus Master для устройства. */
void pci_enable_mmio_busmaster(const pci_device_t *dev);

/* Получить базовый физический адрес MMIO из BAR (для 32/64-битных BAR). */
uint32_t pci_bar_mmio_base(const pci_device_t *dev, int bar_index);

#endif /* PCI_H */
