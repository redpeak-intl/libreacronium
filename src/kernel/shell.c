/**
 * @file shell.c
 * @brief Простейший командный интерпретатор ядра libreacronium.
 */

#include "shell.h"
#include "console.h"
#include "video/video.h"
#include "memory/memory.h"
#include "drivers/pit.h"
#include "module/module.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "net/net.h"

/* kernel_panic определён в kernel.c */
void kernel_panic(const char* msg);

/* Локальные утилиты работы со строками (без стандартной библиотеки) */
static int str_len(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static const char* str_skip_spaces(const char *s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/**
 * @brief Проверить, начинается ли команда с отдельного слова `verb`.
 *
 * Если да — записать в *arg указатель на остаток строки (аргумент).
 * Позволяет разбирать команды вида "insmod hello".
 */
static int parse_cmd(const char *cmd, const char *verb, const char **arg) {
    int i = 0;
    while (verb[i] != '\0') {
        if (cmd[i] != verb[i]) return 0;
        i++;
    }
    if (cmd[i] != '\0' && cmd[i] != ' ' && cmd[i] != '\t') return 0;
    *arg = str_skip_spaces(cmd + i);
    return 1;
}

/**
 * @brief Разбить аргумент на первый токен и остаток строки.
 *
 * Копирует первый токен (до пробела) в tok, возвращает указатель на
 * остаток (без ведущих пробелов). Нужно для "write FILE TEXT".
 */
static const char* split_token(const char *arg, char *tok, int tokmax) {
    const char *p = str_skip_spaces(arg);
    int i = 0;
    while (p[i] != '\0' && p[i] != ' ' && p[i] != '\t') {
        if (i < tokmax - 1) tok[i] = p[i];
        i++;
    }
    tok[i < tokmax ? i : tokmax - 1] = '\0';
    return str_skip_spaces(p + i);
}

/* Разобрать беззнаковое десятичное число (для "ping IP COUNT"). */
static uint32_t parse_u32(const char *s, uint32_t dflt) {
    uint32_t v = 0;
    int any = 0;
    if (!s) return dflt;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
        any = 1;
    }
    return any ? v : dflt;
}

static void shell_print_help(void) {
    console_println("");
    console_println("libreacronium shell commands:");
    console_println("  help      - show this help");
    console_println("  clear     - clear screen");
    console_println("  meminfo   - show physical memory info");
    console_println("  heapinfo  - show kernel heap info");
    console_println("  timerinfo - show PIT timer info");
    console_println("  lsmod     - list loaded modules");
    console_println("  insmod N  - load module N (no dependencies)");
    console_println("  modprobe N- load module N with dependencies");
    console_println("  rmmod N   - unload module N");
    console_println("  ls [path] - list directory");
    console_println("  cd [path] - change directory");
    console_println("  pwd       - print working directory");
    console_println("  cat FILE  - print file contents");
    console_println("  mkdir DIR - create directory");
    console_println("  rm PATH   - remove file/directory");
    console_println("  write F T - create/overwrite file F with text T");
    console_println("  mount     - list mounted filesystems");
    console_println("  ifconfig  - show network interface");
    console_println("  netstat   - show network stats and tasks");
    console_println("  ping IP   - send ICMP echo: ping IP [count]");
    console_println("  dhcp      - restart DHCP client");
    console_println("  sched     - list kernel tasks");
    console_println("  panic     - trigger kernel panic");
}

void shell_execute(const char *cmd) {
    cmd = str_skip_spaces(cmd);
    int len = str_len(cmd);
    if (len == 0) {
        /* пустой ввод — ничего не делаем */
        return;
    }

    const char *arg = 0;

    if (str_eq(cmd, "help")) {
        shell_print_help();
    } else if (str_eq(cmd, "clear")) {
        clear_screen();
    } else if (str_eq(cmd, "meminfo")) {
        pmm_dump_info();
    } else if (str_eq(cmd, "heapinfo")) {
        heap_dump_info();
    } else if (str_eq(cmd, "timerinfo")) {
        pit_dump_info();
    } else if (str_eq(cmd, "lsmod")) {
        module_list();
    } else if (parse_cmd(cmd, "insmod", &arg)) {
        module_load(arg);
    } else if (parse_cmd(cmd, "modprobe", &arg)) {
        module_probe(arg);
    } else if (parse_cmd(cmd, "rmmod", &arg)) {
        module_unload(arg);
    } else if (parse_cmd(cmd, "ls", &arg)) {
        int r = vfs_list(arg[0] ? arg : ".");
        if (r != VFS_OK) {
            console_print("ls: ");
            console_println(vfs_strerror(r));
        }
    } else if (parse_cmd(cmd, "cd", &arg)) {
        int r = vfs_chdir(arg[0] ? arg : "/");
        if (r != VFS_OK) {
            console_print("cd: ");
            console_println(vfs_strerror(r));
        }
    } else if (str_eq(cmd, "pwd")) {
        console_println(vfs_getcwd());
    } else if (parse_cmd(cmd, "cat", &arg)) {
        if (!arg[0]) {
            console_println("cat: missing operand");
        } else {
            char name[128];
            split_token(arg, name, sizeof(name));
            void *buf = 0;
            uint32_t n = 0;
            int r = vfs_read_file(name, &buf, &n);
            if (r != VFS_OK) {
                console_print("cat: ");
                console_println(vfs_strerror(r));
            } else {
                if (buf && n > 0) {
                    console_print((const char *)buf);
                    console_println("");
                } else {
                    console_println("(empty file)");
                }
                if (buf) kfree(buf);
            }
        }
    } else if (parse_cmd(cmd, "mkdir", &arg)) {
        if (!arg[0]) {
            console_println("mkdir: missing operand");
        } else {
            char name[128];
            split_token(arg, name, sizeof(name));
            int r = vfs_mkdir(name);
            if (r != VFS_OK) {
                console_print("mkdir: ");
                console_println(vfs_strerror(r));
            }
        }
    } else if (parse_cmd(cmd, "rm", &arg)) {
        if (!arg[0]) {
            console_println("rm: missing operand");
        } else {
            char name[128];
            split_token(arg, name, sizeof(name));
            int r = vfs_unlink(name);
            if (r != VFS_OK) {
                console_print("rm: ");
                console_println(vfs_strerror(r));
            }
        }
    } else if (parse_cmd(cmd, "write", &arg)) {
        char name[128];
        const char *text = split_token(arg, name, sizeof(name));
        if (!name[0]) {
            console_println("write: usage: write FILE TEXT");
        } else {
            int r = vfs_write_file(name, text, (uint32_t)str_len(text), 1);
            if (r < 0) {
                console_print("write: ");
                console_println(vfs_strerror(r));
            } else {
                console_print("wrote ");
                print_dec(r);
                console_println(" bytes");
            }
        }
    } else if (str_eq(cmd, "mount")) {
        vfs_list_mounts();
    } else if (str_eq(cmd, "ifconfig")) {
        net_ifconfig();
    } else if (str_eq(cmd, "netstat")) {
        net_stat();
    } else if (str_eq(cmd, "sched")) {
        sched_dump();
    } else if (str_eq(cmd, "dhcp")) {
        if (net_dhcp_renew() == 0) {
            console_println("dhcp: клиент перезапущен");
        } else {
            console_println("dhcp: сеть недоступна");
        }
    } else if (parse_cmd(cmd, "ping", &arg)) {
        char host[64];
        const char *rest = split_token(arg, host, sizeof(host));
        if (!host[0]) {
            console_println("ping: usage: ping IP [count]");
        } else {
            net_ping(host, parse_u32(rest, 4));
        }
    } else if (str_eq(cmd, "panic")) {
        kernel_panic("Manual panic triggered from shell.\n");
    } else {
        console_print("Unknown command: ");
        console_print(cmd);
        console_println("");
        console_println("Type 'help' for list of commands.");
    }
}


