/**
 * @file exceptions.h
 * @brief Заголовочный файл для обработчиков исключений процессора.
 */

#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

/**
 * @struct registers_t
 * @brief Структура для сохранения состояния регистров при прерывании.
 * 
 * Эта структура соответствует порядку, в котором регистры сохраняются
 * в ассемблерной заглушке `common_isr_stub` (своя для каждой разрядности).
 */
#ifdef __x86_64__
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  // push r8..r15
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;     // push в обратном порядке
    uint64_t int_no, err_code;                      // Номер прерывания и код ошибки
    uint64_t rip, cs, rflags, rsp, ss;              // Сохраняется процессором автоматически
} registers_t;
#else
typedef struct {
    uint32_t ds;                                    // Сегмент данных
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pusha
    uint32_t int_no, err_code;                      // Номер прерывания и код ошибки
    uint32_t eip, cs, eflags, useresp, ss;          // Сохраняется процессором автоматически
} registers_t;
#endif

/**
 * @brief Основной обработчик исключений на C.
 * 
 * Вызывается из ассемблерных заглушек.
 * @param regs Указатель на структуру с сохраненными регистрами.
 */
void exception_handler(registers_t *regs);

#endif // EXCEPTIONS_H 