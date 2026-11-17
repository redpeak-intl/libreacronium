/**
 * @file e1000.c
 * @brief Драйвер Intel e1000 (QEMU 82540EM).
 *
 * Реализация минимально необходимого набора: сброс, чтение MAC, настройка
 * приёмного и передающего колец, поллинг приёма и передача кадров.
 * Прерывания не используются — приём опрашивается из отдельной задачи ядра.
 */

#include "e1000.h"
#include "pci.h"
#include "../memory/memory.h"
#include "../video/video.h"

/* ------------------------------------------------------------------ */
/* Регистры                                                           */
/* ------------------------------------------------------------------ */

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD   0x0014
#define E1000_ICR    0x00C0
#define E1000_IMS    0x00D0
#define E1000_IMC    0x00D8
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_MTA    0x5200
#define E1000_RAL0   0x5400
#define E1000_RAH0   0x5404

#define CTRL_RST      (1u << 26)
#define CTRL_SLU      (1u << 6)
#define CTRL_FRCSPD   (1u << 11)
#define CTRL_FRCDPLX  (1u << 12)

#define STATUS_LU     (1u << 1)   /* Link Up */

#define RCTL_EN       (1u << 1)
#define RCTL_SBP      (1u << 2)
#define RCTL_UPE      (1u << 3)
#define RCTL_MPE      (1u << 4)
#define RCTL_BAM      (1u << 15)
#define RCTL_SECRC    (1u << 26)

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)

#define ERXD_DD       0x01        /* Descriptor Done */
#define ETXD_DD       0x01
#define TX_CMD        (0x01 | 0x02 | 0x08) /* EOP | IFCS | RS */

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 32
#define BUF_SIZE      2048

/* ------------------------------------------------------------------ */
/* Дескрипторы и буферы (статически, с выравниванием)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

static rx_desc_t rx_ring[RX_DESC_COUNT] __attribute__((aligned(16)));
static tx_desc_t tx_ring[TX_DESC_COUNT] __attribute__((aligned(16)));

static uint8_t rx_buffers[RX_DESC_COUNT][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_DESC_COUNT][BUF_SIZE] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/* Состояние драйвера                                                 */
/* ------------------------------------------------------------------ */

static volatile uint8_t *mmio_base = 0;
static uint8_t            mac_addr[6];
static uint32_t           rx_cur = 0;
static uint32_t           tx_cur = 0;
static int                initialized = 0;

/* ------------------------------------------------------------------ */
/* Доступ к MMIO                                                      */
/* ------------------------------------------------------------------ */

static inline uint32_t mmio_read(uint32_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}

static inline void mmio_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(mmio_base + reg) = value;
}

static inline void io_delay(void) {
    for (volatile int i = 0; i < 1000; i++) { __asm__ volatile(""); }
}

/* ------------------------------------------------------------------ */
/* EEPROM (чтение MAC для 82540EM)                                    */
/* ------------------------------------------------------------------ */

static uint16_t eeprom_read(uint8_t addr) {
    mmio_write(E1000_EERD, (uint32_t)1 | ((uint32_t)addr << 8));
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t v = mmio_read(E1000_EERD);
        if (v & (1u << 4)) {            /* DONE */
            return (uint16_t)(v >> 16);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Настройка приёма/передачи                                          */
/* ------------------------------------------------------------------ */

static void setup_rx(void) {
    for (int i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].status = 0;
    }
    mmio_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    mmio_write(E1000_RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_ring >> 32));
    mmio_write(E1000_RDLEN, RX_DESC_COUNT * sizeof(rx_desc_t));
    mmio_write(E1000_RDH, 0);
    mmio_write(E1000_RDT, RX_DESC_COUNT - 1);
    rx_cur = 0;

    uint32_t rctl = RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE |
                    RCTL_BAM | RCTL_SECRC;
    /* BSIZE = 00 => 2048 байт. */
    mmio_write(E1000_RCTL, rctl);
}

static void setup_tx(void) {
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)tx_buffers[i];
        tx_ring[i].status = ETXD_DD;  /* свободен */
        tx_ring[i].cmd = 0;
        tx_ring[i].length = 0;
    }
    mmio_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    mmio_write(E1000_TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_ring >> 32));
    mmio_write(E1000_TDLEN, TX_DESC_COUNT * sizeof(tx_desc_t));
    mmio_write(E1000_TDH, 0);
    mmio_write(E1000_TDT, 0);
    tx_cur = 0;

    mmio_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20));
    mmio_write(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                      */
/* ------------------------------------------------------------------ */

int e1000_init(void) {
    pci_device_t dev;

    /* Ищем 82540EM (0x100E). Если не нашли — любой сетевой контроллер Intel. */
    if (pci_find_device(PCI_VENDOR_INTEL, 0x100E, &dev) != 0) {
        print_string_color("e1000: устройство не найдено\n", COLOR_RED, COLOR_BLACK);
        return -1;
    }

    print_string("e1000: PCI ");
    print_hex(dev.bus);
    print_string(":");
    print_hex(dev.slot);
    print_string(".");
    print_hex(dev.func);
    print_string(" dev=");
    print_hex(dev.device_id);
    print_string("\n");

    pci_enable_mmio_busmaster(&dev);
    uint32_t bar0 = pci_bar_mmio_base(&dev, 0);
    if (bar0 == 0) {
        print_string_color("e1000: BAR0 не является MMIO\n", COLOR_RED, COLOR_BLACK);
        return -1;
    }
    mmio_base = (volatile uint8_t *)(uintptr_t)bar0;

    print_string("e1000: MMIO=");
    print_hex(bar0);
    print_string("\n");

    /* Маскируем все прерывания — работаем в режиме опроса. */
    mmio_write(E1000_IMC, 0xFFFFFFFF);

    /* Сброс. */
    mmio_write(E1000_CTRL, mmio_read(E1000_CTRL) | CTRL_RST);
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(mmio_read(E1000_CTRL) & CTRL_RST)) break;
    }
    io_delay();

    /* Читаем MAC из EEPROM. */
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    mac_addr[0] = (uint8_t)(w0 & 0xFF);
    mac_addr[1] = (uint8_t)(w0 >> 8);
    mac_addr[2] = (uint8_t)(w1 & 0xFF);
    mac_addr[3] = (uint8_t)(w1 >> 8);
    mac_addr[4] = (uint8_t)(w2 & 0xFF);
    mac_addr[5] = (uint8_t)(w2 >> 8);

    /* Если EEPROM не ответил — берём адрес из RAL/RAH. */
    uint32_t ral = mmio_read(E1000_RAL0);
    uint32_t rah = mmio_read(E1000_RAH0);
    if (mac_addr[0] == 0 && mac_addr[1] == 0 && mac_addr[2] == 0 &&
        mac_addr[3] == 0 && mac_addr[4] == 0 && mac_addr[5] == 0) {
        mac_addr[0] = (uint8_t)(ral & 0xFF);
        mac_addr[1] = (uint8_t)((ral >> 8) & 0xFF);
        mac_addr[2] = (uint8_t)((ral >> 16) & 0xFF);
        mac_addr[3] = (uint8_t)((ral >> 24) & 0xFF);
        mac_addr[4] = (uint8_t)(rah & 0xFF);
        mac_addr[5] = (uint8_t)((rah >> 8) & 0xFF);
    }

    /* Прописываем адрес в фильтр приёма (RAH.AV = бит 31). */
    mmio_write(E1000_RAL0,
               (uint32_t)mac_addr[0] | ((uint32_t)mac_addr[1] << 8) |
               ((uint32_t)mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24));
    mmio_write(E1000_RAH0,
               (uint32_t)mac_addr[4] | ((uint32_t)mac_addr[5] << 8) |
               (1u << 31));

    /* Очищаем таблицу многоадресной рассылки (128 записей). */
    for (uint32_t i = 0; i < 128; i++) {
        mmio_write(E1000_MTA + i * 4, 0);
    }

    /* Поднимаем линк, принудительно full duplex. */
    mmio_write(E1000_CTRL,
               mmio_read(E1000_CTRL) | CTRL_SLU | CTRL_FRCSPD | CTRL_FRCDPLX);

    setup_rx();
    setup_tx();

    mmio_write(E1000_ICR, 0xFFFFFFFF); /* сбрасываем флаги прерываний */

    initialized = 1;

    print_string("e1000: MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) print_string(":");
        uint8_t b = mac_addr[i];
        char hex[3];
        hex[0] = "0123456789abcdef"[(b >> 4) & 0xF];
        hex[1] = "0123456789abcdef"[b & 0xF];
        hex[2] = '\0';
        print_string(hex);
    }
    print_string("\n");

    return 0;
}

void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

int e1000_present(void) {
    return initialized;
}

int e1000_link_up(void) {
    if (!initialized) return 0;
    return (mmio_read(E1000_STATUS) & STATUS_LU) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Передача                                                           */
/* ------------------------------------------------------------------ */

int e1000_transmit(const void *data, uint32_t len) {
    if (!initialized) return -1;
    if (len > BUF_SIZE) return -1;

    /* Ждём, пока дескриптор освободится. */
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if (tx_ring[tx_cur].status & ETXD_DD) break;
    }
    if (!(tx_ring[tx_cur].status & ETXD_DD)) {
        return -1; /* Тайм-аут */
    }

    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) tx_buffers[tx_cur][i] = src[i];

    tx_ring[tx_cur].length = (uint16_t)len;
    tx_ring[tx_cur].cso    = 0;
    tx_ring[tx_cur].cmd    = TX_CMD;
    tx_ring[tx_cur].status = 0;

    uint32_t next = (tx_cur + 1) % TX_DESC_COUNT;
    __asm__ volatile("" ::: "memory");     /* барьер перед дверным звонком */
    mmio_write(E1000_TDT, next);
    tx_cur = next;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Приём                                                              */
/* ------------------------------------------------------------------ */

int e1000_receive(void *buffer, uint32_t *len) {
    if (!initialized) return 0;

    uint32_t i = rx_cur;
    if (!(rx_ring[i].status & ERXD_DD)) {
        return 0; /* Кадров нет */
    }

    uint32_t length = rx_ring[i].length;
    if (length > *len) length = *len;

    uint8_t *dst = (uint8_t *)buffer;
    for (uint32_t j = 0; j < length; j++) {
        dst[j] = rx_buffers[i][j];
    }
    *len = length;

    /* Возвращаем дескриптор железу. */
    rx_ring[i].status = 0;
    __asm__ volatile("" ::: "memory");
    mmio_write(E1000_RDT, i);
    rx_cur = (i + 1) % RX_DESC_COUNT;

    return 1;
}
