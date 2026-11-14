/**
 * @file idt.h
 * @brief Заголовочный файл для работы с таблицей дескрипторов прерываний (IDT)
 *
 * Содержит определения структур и констант для настройки IDT,
 * а также объявления функций работы с прерываниями.
 */

#include <stdint.h>

#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

/* Размер таблицы IDT (256 записей - стандарт для x86) */
#define IDT_SIZE 256

/* Атрибуты дескриптора прерывания:
 * P=1 (присутствует), DPL=00 (уровень привилегий 0), 
 * S=0 (системный сегмент), Type=1110 (шлюз прерывания) */
#define INTERRUPT_GATE 0x8e

/* Смещение сегмента кода ядра в GDT */
#define KERNEL_CODE_SEGMENT_OFFSET 0x08

/**
 * @brief Структура дескриптора прерывания в IDT.
 *
 * Формат зависит от разрядности: в 32-битном режиме шлюз занимает 8 байт,
 * в long mode (x86_64) — 16 байт (появляются поля ist и старшие 32 бита адреса).
 */
#ifdef __x86_64__
struct IDT_entry {
    unsigned short int offset_lowerbits;   /**< Младшие 16 бит адреса обработчика */
    unsigned short int selector;           /**< Селектор сегмента кода в GDT */
    unsigned char ist;                     /**< Индекс IST (0 = не использовать) */
    unsigned char type_attr;               /**< Тип и атрибуты шлюза */
    unsigned short int offset_middlebits;  /**< Средние 16 бит адреса обработчика */
    unsigned int offset_highbits;          /**< Старшие 32 бита адреса обработчика */
    unsigned int zero;                     /**< Зарезервировано (0) */
} __attribute__((packed));
#else
struct IDT_entry {
    unsigned short int offset_lowerbits;  /**< Младшие 16 бит адреса обработчика */
    unsigned short int selector;         /**< Селектор сегмента кода в GDT */
    unsigned char zero;                 /**< Всегда 0 */
    unsigned char type_attr;            /**< Тип и атрибуты шлюза */
    unsigned short int offset_higherbits; /**< Старшие 16 бит адреса обработчика */
};
#endif

/* Глобальная таблица IDT */
extern struct IDT_entry IDT[IDT_SIZE];

/**
 * @brief Инициализирует IDT и перенастраивает PIC
 */
void idt_init(void);

/**
 * @brief Загружает IDT (ассемблерная функция)
 * @param idt_ptr Указатель на описатель для команды LIDT
 */
extern void load_idt(void *idt_ptr);

/**
 * @brief Обработчик прерываний клавиатуры (ассемблерная функция)
 */
extern void keyboard_handler(void);

/**
 * @brief Записывает байт в порт ввода-вывода
 * @param port Номер порта
 * @param data Данные для записи
 */
static inline void write_port(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

/**
 * @brief Короткая задержка для старых устройств (PIC, PIT)
 *
 * Запись в неиспользуемый порт 0x80 занимает несколько циклов и даёт
 * медленным контроллерам время обработать предыдущую команду.
 */
static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/**
 * @brief Считывает байт из указанного порта ввода-вывода
 * 
 * Ассемблерная вставка использует инструкцию inb для чтения
 * 8-битного значения с указанного порта ввода-вывода.
 * 
 * @param port Номер порта ввода-вывода
 * @return uint8_t Считанное значение с порта
 */
uint8_t read_port(uint16_t port);

#endif /* KERNEL_IDT_H */