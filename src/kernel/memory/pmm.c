/**
 * @file pmm.c
 * @brief Physical Memory Manager - управление физическими страницами
 * 
 * Реализация менеджера физической памяти с использованием битовой карты
 * для отслеживания свободных и занятых страниц размером 4KB
 */

#include "memory.h"
#include "../video/video.h"

/* Глобальный экземпляр менеджера физической памяти */
pmm_t physical_memory_manager;

/* -------------------------------------------------------------------------
 * Структуры Multiboot (передаются загрузчиком)
 * ---------------------------------------------------------------------- */

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEMORY      0x00000001u /* mem_lower/mem_upper доступны */
#define MULTIBOOT_INFO_MEM_MAP     0x00000040u /* карта памяти (mmap) доступна */
#define MULTIBOOT_MEMORY_AVAILABLE 1

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;   /* Объём памяти ниже 1MB (KB) */
    uint32_t mem_upper;   /* Объём памяти выше 1MB (KB) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint32_t size;        /* Размер записи без самого поля size */
    uint64_t addr;        /* Начало области */
    uint64_t len;         /* Длина области */
    uint32_t type;        /* Тип (1 = доступная память) */
} __attribute__((packed));

/* Максимальный физический адрес, который отслеживает битовая карта (4GB) */
#define MAX_PHYS_ADDR ((uint64_t)MAX_PAGES * PAGE_SIZE)

/**
 * @brief Пометить непрерывный диапазон [start, end) как свободный.
 * @return Количество помеченных страниц.
 * @note Диапазон обрезается по границе 4GB и выравнивается по страницам.
 */
static uint32_t pmm_mark_available(uint64_t start, uint64_t end) {
    uint32_t marked = 0;

    if (end > MAX_PHYS_ADDR) {
        end = MAX_PHYS_ADDR;
    }

    /* Выравниваем границы по страницам */
    start = (start + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
    end = end & ~((uint64_t)PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        pmm_mark_page_free((uint32_t)addr);
        marked++;
    }
    return marked;
}

/**
 * @brief Инициализация менеджера физической памяти
 * @param kernel_end Адрес конца ядра в памяти
 * @param magic Магическое число Multiboot (в eax при загрузке)
 * @param mbi_addr Адрес структуры Multiboot Info (в ebx при загрузке)
 */
void pmm_init(uint32_t kernel_end, uint32_t magic, uint32_t mbi_addr) {
    print_string("PMM Initialization... ");

    physical_memory_manager.kernel_end = kernel_end;

    /* По умолчанию считаем всю память занятой: так мы гарантированно не
       отдадим страницы, которых физически нет. Ниже освободим доступные. */
    memory_set(physical_memory_manager.bitmap, 0xFF,
               sizeof(physical_memory_manager.bitmap));
    physical_memory_manager.free_pages = 0;
    physical_memory_manager.total_pages = 0;

    uint32_t usable_pages = 0;

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        struct multiboot_info *mbi = (struct multiboot_info *)mbi_addr;

        if (mbi->flags & MULTIBOOT_INFO_MEM_MAP) {
            /* Предпочтительный путь — карта памяти от загрузчика. */
            uint32_t offset = 0;
            while (offset < mbi->mmap_length) {
                struct multiboot_mmap_entry *entry =
                    (struct multiboot_mmap_entry *)(mbi->mmap_addr + offset);

                if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    usable_pages += pmm_mark_available(entry->addr,
                                                       entry->addr + entry->len);
                }
                offset += entry->size + sizeof(entry->size);
            }
        } else if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
            /* Запасной вариант: mem_upper — память выше 1MB (в KB). */
            uint64_t upper = (uint64_t)mbi->mem_upper * 1024;
            usable_pages += pmm_mark_available(0x100000, 0x100000 + upper);
        }
    }

    if (usable_pages == 0) {
        /* Карта памяти недоступна — не выделяем ничего за пределами ядра. */
        print_string_color("WARN: no memory map", COLOR_RED, COLOR_BLACK);
    }

    physical_memory_manager.total_pages = usable_pages;

    /* Резервируем первый мегабайт (BIOS, видеопамять, ROM). */
    for (uint32_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        pmm_mark_page_used(addr);
    }

    /* Резервируем область, занятую ядром. */
    uint32_t kernel_pages = (kernel_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (uint32_t i = 0; i < kernel_pages; i++) {
        pmm_mark_page_used(i << PAGE_SHIFT);
    }

    print_string_color("OK\n", COLOR_GREEN, COLOR_BLACK);
    print_string("  - Total pages: ");
    print_hex(physical_memory_manager.total_pages);
    print_string("\n  - Free pages: ");
    print_hex(physical_memory_manager.free_pages);
    print_string("\n");
}

/**
 * @brief Поиск свободной страницы в битовой карте
 * @return Индекс свободной страницы или -1, если нет свободных страниц
 */
static int find_free_page(void) {
    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        if (physical_memory_manager.bitmap[i] != 0xFFFFFFFF) {
            /* Найдена группа с свободными страницами */
            uint32_t bitmap_entry = physical_memory_manager.bitmap[i];
            for (uint32_t j = 0; j < 32; j++) {
                if (!(bitmap_entry & (1 << j))) {
                    return i * 32 + j;
                }
            }
        }
    }
    return -1; /* Нет свободных страниц */
}

/**
 * @brief Выделение одной физической страницы
 * @return Адрес выделенной страницы или 0 при ошибке
 */
uint32_t pmm_alloc_page(void) {
    if (physical_memory_manager.free_pages == 0) {
        return 0; /* Нет свободных страниц */
    }
    
    int page_index = find_free_page();
    if (page_index == -1) {
        return 0; /* Не удалось найти свободную страницу */
    }
    
    uint32_t page_addr = page_index << PAGE_SHIFT;
    pmm_mark_page_used(page_addr);
    
    return page_addr;
}

/**
 * @brief Освобождение физической страницы
 * @param page_addr Адрес страницы для освобождения
 */
void pmm_free_page(uint32_t page_addr) {
    uint32_t page_index = page_addr >> PAGE_SHIFT;
    uint32_t bitmap_index = page_index / 32;
    uint32_t bit_index = page_index % 32;
    
    if (bitmap_index >= BITMAP_SIZE) {
        return; /* Некорректный адрес */
    }
    
    /* Проверяем, была ли страница занята */
    if (!(physical_memory_manager.bitmap[bitmap_index] & (1 << bit_index))) {
        return; /* Страница уже свободна */
    }
    
    /* Освобождаем страницу */
    physical_memory_manager.bitmap[bitmap_index] &= ~(1 << bit_index);
    physical_memory_manager.free_pages++;
    
    /* Очищаем содержимое страницы */
    memory_set((void*)page_addr, 0, PAGE_SIZE);
}

/**
 * @brief Получение количества свободных страниц
 * @return Количество свободных страниц
 */
uint32_t pmm_get_free_pages_count(void) {
    return physical_memory_manager.free_pages;
}

/**
 * @brief Пометить страницу как занятую
 * @param page_addr Адрес страницы
 */
void pmm_mark_page_used(uint32_t page_addr) {
    uint32_t page_index = page_addr >> PAGE_SHIFT;
    uint32_t bitmap_index = page_index / 32;
    uint32_t bit_index = page_index % 32;
    
    if (bitmap_index >= BITMAP_SIZE) {
        return; /* Некорректный адрес */
    }
    
    /* Проверяем, была ли страница свободна */
    if (!(physical_memory_manager.bitmap[bitmap_index] & (1 << bit_index))) {
        physical_memory_manager.bitmap[bitmap_index] |= (1 << bit_index);
        physical_memory_manager.free_pages--;
    }
}

/**
 * @brief Пометить страницу как свободную
 * @param page_addr Адрес страницы
 */
void pmm_mark_page_free(uint32_t page_addr) {
    uint32_t page_index = page_addr >> PAGE_SHIFT;
    uint32_t bitmap_index = page_index / 32;
    uint32_t bit_index = page_index % 32;
    
    if (bitmap_index >= BITMAP_SIZE) {
        return; /* Некорректный адрес */
    }
    
    /* Проверяем, была ли страница занята */
    if (physical_memory_manager.bitmap[bitmap_index] & (1 << bit_index)) {
        physical_memory_manager.bitmap[bitmap_index] &= ~(1 << bit_index);
        physical_memory_manager.free_pages++;
    }
}

/**
 * @brief Вывод информации о состоянии менеджера физической памяти
 */
void pmm_dump_info(void) {
    print_string("Physical Memory Manager Info:\n");
    print_string("  - Total pages: ");
    print_hex(physical_memory_manager.total_pages);
    print_string("\n  - Free pages: ");
    print_hex(physical_memory_manager.free_pages);
    print_string("\n  - Used pages: ");
    print_hex(physical_memory_manager.total_pages - physical_memory_manager.free_pages);
    print_string("\n  - Kernel end: ");
    print_hex(physical_memory_manager.kernel_end);
    print_string("\n");
} 