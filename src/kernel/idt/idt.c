/**
 * @file idt.c 
 * @brief Реализация таблицы дескрипторов прерываний (IDT)
 *
 * Содержит инициализацию IDT и настройку контроллера прерываний (PIC).
 * Обрабатывает аппаратные прерывания, включая прерывания клавиатуры.
 */

#include "idt.h"
#include <stdint.h>
#include "../video/video.h"
#include "exceptions.h" // Подключаем заголовок с обработчиками

/* Объявление внешних ассемблерных обработчиков-заглушек */
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

/* Объявление обработчика прерывания PIT (заглушка таймера/планировщика) */
extern void pit_handler_asm();

/* Объявление заглушки добровольной отдачи процессора (вектор 0x30) */
extern void sched_yield_asm();

/* Глобальная таблица IDT */
struct IDT_entry IDT[IDT_SIZE];

/**
 * @brief Описатель IDT для команды LIDT.
 *
 * В 32-битном режиме база 32-битная, в long mode — 64-битная. Структура
 * всегда упакована (без выравнивания) — так требует формат LIDT.
 */
struct idt_ptr {
    uint16_t limit;
#ifdef __x86_64__
    uint64_t base;
#else
    uint32_t base;
#endif
} __attribute__((packed));

void idt_set_gate(int n, unsigned long handler) {
#ifdef __x86_64__
    uint64_t h = (uint64_t)handler;
    IDT[n].offset_lowerbits  = h & 0xffff;
    IDT[n].selector          = KERNEL_CODE_SEGMENT_OFFSET;
    IDT[n].ist               = 0;
    IDT[n].type_attr         = INTERRUPT_GATE;
    IDT[n].offset_middlebits = (h >> 16) & 0xffff;
    IDT[n].offset_highbits   = (uint32_t)((h >> 32) & 0xffffffff);
    IDT[n].zero              = 0;
#else
    IDT[n].offset_lowerbits = handler & 0xffff;
    IDT[n].selector = KERNEL_CODE_SEGMENT_OFFSET;
    IDT[n].zero = 0;
    IDT[n].type_attr = INTERRUPT_GATE;
    IDT[n].offset_higherbits = (handler >> 16) & 0xffff;
#endif
}

/**
 * @brief Инициализация IDT и PIC
 * 
 * Функция выполняет:
 * 1. Настройку дескриптора прерывания клавиатуры (IRQ1)
 * 2. Переназначение векторов прерываний в PIC
 * 3. Загрузку IDT с помощью lidt
 */
void idt_init(void)
{
    print_string("IDT Initialization... ");  // Добавлено: статусное сообщение
    
    /* Установка обработчиков исключений (ISR 0-31) */
    idt_set_gate(0, (unsigned long)isr0);
    idt_set_gate(1, (unsigned long)isr1);
    idt_set_gate(2, (unsigned long)isr2);
    idt_set_gate(3, (unsigned long)isr3);
    idt_set_gate(4, (unsigned long)isr4);
    idt_set_gate(5, (unsigned long)isr5);
    idt_set_gate(6, (unsigned long)isr6);
    idt_set_gate(7, (unsigned long)isr7);
    idt_set_gate(8, (unsigned long)isr8);
    idt_set_gate(9, (unsigned long)isr9);
    idt_set_gate(10, (unsigned long)isr10);
    idt_set_gate(11, (unsigned long)isr11);
    idt_set_gate(12, (unsigned long)isr12);
    idt_set_gate(13, (unsigned long)isr13);
    idt_set_gate(14, (unsigned long)isr14);
    idt_set_gate(15, (unsigned long)isr15);
    idt_set_gate(16, (unsigned long)isr16);
    idt_set_gate(17, (unsigned long)isr17);
    idt_set_gate(18, (unsigned long)isr18);
    idt_set_gate(19, (unsigned long)isr19);
    idt_set_gate(20, (unsigned long)isr20);
    idt_set_gate(21, (unsigned long)isr21);
    idt_set_gate(22, (unsigned long)isr22);
    idt_set_gate(23, (unsigned long)isr23);
    idt_set_gate(24, (unsigned long)isr24);
    idt_set_gate(25, (unsigned long)isr25);
    idt_set_gate(26, (unsigned long)isr26);
    idt_set_gate(27, (unsigned long)isr27);
    idt_set_gate(28, (unsigned long)isr28);
    idt_set_gate(29, (unsigned long)isr29);
    idt_set_gate(30, (unsigned long)isr30);
    idt_set_gate(31, (unsigned long)isr31);

    /* 1. Настройка обработчика клавиатуры (IRQ1 -> INT 0x21) */
    idt_set_gate(0x21, (unsigned long)keyboard_handler);

    /* Настройка обработчика системного таймера (IRQ0 -> INT 0x20) */
    idt_set_gate(0x20, (unsigned long)pit_handler_asm);

    /* Вектор 0x30 — программный "yield" планировщика (INT 0x30) */
    idt_set_gate(0x30, (unsigned long)sched_yield_asm);

    /* 2. Перенастройка PIC (Programmable Interrupt Controller) */
    
    /* ICW1 - начало инициализации */
    write_port(0x20, 0x11);  // Основной PIC
    io_wait();
    write_port(0xA0, 0x11);  // Вторичный PIC
    io_wait();

    /* ICW2 - переназначение базовых векторов */
    write_port(0x21, 0x20);  // Основные прерывания начиная с 0x20
    io_wait();
    write_port(0xA1, 0x28);  // Вторичные прерывания начиная с 0x28
    io_wait();

    /* ICW3 - настройка каскадирования */
    write_port(0x21, 0x04);  // Ведомый PIC подключён к линии IRQ2 мастера
    io_wait();
    write_port(0xA1, 0x02);  // Ведомый PIC подключён к линии IRQ2
    io_wait();

    /* ICW4 - дополнительная информация */
    write_port(0x21, 0x01);  // Режим 8086/88
    io_wait();
    write_port(0xA1, 0x01);  // Режим 8086/88
    io_wait();

    /* Маскирование всех прерываний */
    write_port(0x21, 0xff);  // Основной PIC
    write_port(0xA1, 0xff);  // Вторичный PIC

    /* 3. Загрузка IDT */
    struct idt_ptr idtp;
    idtp.limit = (uint16_t)((sizeof(struct IDT_entry) * IDT_SIZE) - 1);
    idtp.base = (unsigned long)(uintptr_t)IDT;

    load_idt(&idtp);  // Ассемблерная функция загрузки IDT

    print_string_color("OK\n", COLOR_GREEN, COLOR_BLACK);  // Добавлено: успешный статус
}

/**
 * @brief Считывает байт из указанного порта ввода-вывода
 * 
 * Использует инструкцию inb для чтения 8-битного значения
 * из порта ввода-вывода с заданным адресом.
 * 
 * @param port Номер порта ввода-вывода (16 бит)
 * @return uint8_t Считанное 8-битное значение с порта
 * 
 * @note Используется для взаимодействия с аппаратурой на уровне портов.
 */
uint8_t read_port(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}