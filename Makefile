SHELL := /bin/bash

# Целевая архитектура: i686 (32-бит) или x86_64 (amd64, 64-бит).
# Переключается через `make ARCH=x86_64 ...`; по умолчанию собирается i686.
ARCH ?= i686

ifeq ($(filter $(ARCH),i686 x86_64),)
$(error Неподдерживаемая ARCH=$(ARCH). Используйте i686 или x86_64)
endif

# Директории
SRCDIR := src
BUILDDIR := build
OBJDIR := $(BUILDDIR)/$(ARCH)

# Загружаемые модули (.ko) — собираются отдельно и встраиваются в образ ядра.
# Исходники лежат в modules/, имя модуля = имя файла без расширения.
MODULES := hello world

# Архитектурно-зависимые параметры сборки
ifeq ($(ARCH),x86_64)
  NASMFLAGS   := -f elf64 -g
  CFLAGS      := -m64 -c -ffreestanding -nostdlib -Wall -Wextra -g \
                 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-80387 \
                 -fno-pic -fno-pie -fno-stack-protector -mno-stack-arg-probe
  MODULE_CFLAGS := -m64 -c -ffreestanding -nostdlib -Wall -Wextra -O2 \
                 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-80387 \
                 -fno-pic -fno-pie -fno-common -fno-stack-protector \
                 -fno-asynchronous-unwind-tables -Isrc/kernel/module
  LDFORMAT    := elf_x86_64
  LINKER      := linker64.ld
  BLOB_FORMAT := elf64-x86-64
  BLOB_ARCH   := i386:x86-64
  QEMU        := qemu-system-x86_64
  BOOT_ASM    := src/boot/boot64.asm
  IDT_ASM     := src/kernel/idt/idt_load64.asm \
                 src/kernel/idt/exception_handlers64.asm \
                 src/kernel/sched/sched_switch64.asm
  KERNEL      := kernel-x86_64
else
  NASMFLAGS   := -f elf32 -g
  CFLAGS      := -m32 -c -ffreestanding -nostdlib -Wall -Wextra -g
  MODULE_CFLAGS := -m32 -c -ffreestanding -nostdlib -Wall -Wextra -O2 \
                 -fno-pic -fno-pie -fno-common -fno-stack-protector \
                 -fno-asynchronous-unwind-tables -Isrc/kernel/module
  LDFORMAT    := elf_i386
  LINKER      := linker.ld
  BLOB_FORMAT := elf32-i386
  BLOB_ARCH   := i386
  QEMU        := qemu-system-i386
  BOOT_ASM    := src/boot/boot.asm
  IDT_ASM     := src/kernel/idt/idt_load.asm \
                 src/kernel/idt/exception_handlers.asm \
                 src/kernel/sched/sched_switch.asm
  KERNEL      := kernel-i686
endif

# Компиляторы и утилиты
NASM    := nasm
CC      := gcc
LD      := ld
OBJCOPY := objcopy
GDB     := gdb

# Общие пути включения для исходников ядра (относительно корня проекта).
KINCLUDES := -Isrc/kernel -Isrc/kernel/net \
             -Isrc/kernel/net/lwip/include -Isrc/kernel/net/lwip/port
CFLAGS += $(KINCLUDES)

LDFLAGS := -m $(LDFORMAT) -T $(LINKER)

# Устройства QEMU: e1000 в пользовательской сети (DHCP-сервер slirp).
QEMU_NET := -netdev user,id=net0 -device e1000,netdev=net0

# Полные флаги для QEMU (включая режим вывода и ядро)
QEMUFLAGS_RUN := -display curses -kernel $(KERNEL) $(QEMU_NET)
QEMUFLAGS_DEBUG := -display curses -kernel $(KERNEL) $(QEMU_NET) -s -S

# Поиск исходников (arch-зависимые asm выбираются по ARCH)
ASM_SOURCES := $(BOOT_ASM) $(IDT_ASM)
C_SOURCES = $(wildcard src/kernel/*.c) \
            $(wildcard src/kernel/video/*.c) \
            $(wildcard src/kernel/idt/*.c) \
            $(wildcard src/kernel/drivers/*.c) \
            $(wildcard src/kernel/memory/*.c) \
            $(wildcard src/kernel/module/*.c) \
            $(wildcard src/kernel/block/*.c) \
            $(wildcard src/kernel/fs/*.c) \
            $(wildcard src/kernel/sched/*.c) \
            $(wildcard src/kernel/net/*.c) \
            $(wildcard src/kernel/net/lwip/core/*.c) \
            $(wildcard src/kernel/net/lwip/core/ipv4/*.c) \
            $(wildcard src/kernel/net/lwip/api/*.c) \
            src/kernel/net/lwip/netif/ethernet.c \
            $(wildcard src/kernel/net/lwip/port/*.c)

MODULE_KO := $(patsubst %, $(OBJDIR)/modules/%.ko, $(MODULES))
MODULE_BLOBS := $(patsubst %, $(OBJDIR)/modules/%_blob.o, $(MODULES))

# Не удалять промежуточные .ko — их удобно смотреть через readelf/objdump.
.PRECIOUS: $(MODULE_KO)

# Объектные файлы (в build/$(ARCH)/)
ASM_OBJECTS = $(patsubst src/%.asm, $(OBJDIR)/%.o, $(ASM_SOURCES))
C_OBJECTS = $(patsubst src/%.c, $(OBJDIR)/%.o, $(C_SOURCES))
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS) $(MODULE_BLOBS)

# Основные цели
.PHONY: all i686 x86_64 clean run debug help

# Сборка обоих таргетов
all: i686 x86_64

# Сборка отдельного таргета (рекурсивный вызов с нужной ARCH)
i686:
	@$(MAKE) --no-print-directory ARCH=i686 kernel-i686

x86_64:
	@$(MAKE) --no-print-directory ARCH=x86_64 kernel-x86_64

# Запуск в QEMU (для текущей ARCH)
run: $(KERNEL)
	@echo -e "\n🚀 \033[1;36mЗапуск ядра ($(KERNEL)) в QEMU...\033[0m"
	@$(QEMU) $(QEMUFLAGS_RUN)

# Отладка (QEMU + GDB, для текущей ARCH)
debug: $(KERNEL)
	@echo -e "\n🐞 \033[1;35mОтладка ($(KERNEL)):\033[0m"
	@echo -e "  1. QEMU ждёт подключения GDB на порту :1234"
	@echo -e "  2. В новом терминале выполните: \033[1;33mgdb -x .gdbinit $(KERNEL)\033[0m"
	@$(QEMU) $(QEMUFLAGS_DEBUG)

# Линковка ядра текущей архитектуры
$(KERNEL): $(OBJECTS)
	@echo -e "\n🔗 \033[1;34mЛинковка $(ARCH)...\033[0m"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo -e "✅ \033[1;32mСобрано: $@\033[0m"

# Правила компиляции
$(OBJDIR)/%.o: $(SRCDIR)/%.asm
	@mkdir -p $(dir $@)
	@echo -e "🔨 \033[1;34mASM [$(ARCH)]:\033[0m $< → $@"
	@$(NASM) $(NASMFLAGS) $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo -e "🔧 \033[1;34mC [$(ARCH)]:\033[0m $< → $@"
	@$(CC) $(CFLAGS) $< -o $@

# Компиляция модуля в relocatable ELF (.ko)
$(OBJDIR)/modules/%.ko: modules/%.c
	@mkdir -p $(dir $@)
	@echo -e "🧩 \033[1;33mMOD [$(ARCH)]:\033[0m $< → $@"
	@$(CC) $(MODULE_CFLAGS) $< -o $@

# Встраивание .ko в образ ядра как «сырой» блок данных (.kmod)
$(OBJDIR)/modules/%_blob.o: $(OBJDIR)/modules/%.ko
	@cd $(dir $@) && $(OBJCOPY) -I binary -O $(BLOB_FORMAT) -B $(BLOB_ARCH) \
	    --rename-section .data=.kmod,alloc,load,readonly,data,contents \
	    --add-section .note.GNU-stack=/dev/null \
	    --set-section-flags .note.GNU-stack=noload,readonly \
	    --redefine-sym _binary_$*_ko_start=kmod_$*_start \
	    --redefine-sym _binary_$*_ko_end=kmod_$*_end \
	    $*.ko $*_blob.o
	@echo -e "📦 \033[1;33mEMBED [$(ARCH)]:\033[0m $*.ko → $@"

# Очистка
clean:
	@echo -e "\n🧹 \033[1;31mУдаляю build/ и kernel-*...\033[0m"
	@rm -rf $(BUILDDIR) kernel kernel-i686 kernel-x86_64

# Помощь
help:
	@echo -e "\n\033[1;35m📜 Помощь:\033[0m"
	@echo -e "  \033[1;36mmake all\033[0m              — собрать ядро под обе архитектуры (i686 + x86_64)"
	@echo -e "  \033[1;36mmake i686\033[0m             — собрать только 32-битное ядро (kernel-i686)"
	@echo -e "  \033[1;36mmake x86_64\033[0m           — собрать только 64-битное ядро (kernel-x86_64)"
	@echo -e "  \033[1;36mmake ARCH=x86_64 run\033[0m   — запустить выбранную архитектуру в QEMU"
	@echo -e "  \033[1;36mmake debug\033[0m            — отладка (QEMU + GDB)"
	@echo -e "  \033[1;36mmake clean\033[0m            — очистить проект"
	@echo -e "  \033[1;36mmake help\033[0m             — эта справка"
	@echo -e "\n\033[3mУдачи в разработке! 🚀\033[0m"
