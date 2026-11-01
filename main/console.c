/* console.c - serial management menu with authenticated configuration */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "app.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define SESSION_US   (120LL * 1000000)   /* admin session idle timeout */
#define MAX_FAILS    3
#define LOCKOUT_US   (60LL * 1000000)    /* login lockout after MAX_FAILS */

static bool    s_admin;
static int64_t s_last_us, s_lock_until_us;
static int     s_fails;

static void hash_pin(const char *user, const char *pin, char out[65])
{
    char buf[96];
    snprintf(buf, sizeof buf, "cocis:%s:%s", user, pin);
    sha256_hex(buf, out);
}

static bool need_admin(void)
{
    int64_t now = esp_timer_get_time();
    if (s_admin && now - s_last_us > SESSION_US) { s_admin = false; printf("Session expired.\r\n"); }
    if (!s_admin) { printf("Denied: administrator login required (login admin <pin>).\r\n"); return false; }
    s_last_us = now;
    return true;
}

static bool valid_pin(const char *p)
{
    size_t n = strlen(p);
    if (n < 4 || n > 8) return false;
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

static void print_help(void)
{
    printf(
    "\r\n=== CoCIS Smart Control - commands ===\r\n"
    " Monitoring (no login):\r\n"
    "  status                    system overview\r\n"
    "  who                       people currently inside the building\r\n"
    "  rooms                     room occupancy and lighting status\r\n"
    "  log [n]                   last n access records (max 20)\r\n"
    "  syslog [n]                last n system events (max 20)\r\n"
    "  logfile access|events|users|config   file contents from the SD card\r\n"
    "  schedule                  show building access hours\r\n"
    "  time                      show clock\r\n"
    "  scan in|out <uid>         simulate a tag tap at ENTRY/EXIT reader\r\n"
    " Authentication:\r\n"
    "  login admin <pin>   logout\r\n"
    " Administrator only:\r\n"
    "  schedule set HH:MM HH:MM  set access window (open close)\r\n"
    "  time set [YYYY-MM-DD] HH:MM\r\n"
    "  user list                 list registered users\r\n"
    "  user add <uid> <name>     register a tag, e.g. user add 01020304 Baker\r\n"
    "                            (uid = hex digits, no colons; name = one word)\r\n"
    "  user del <uid>            remove a tag, e.g. user del 01020304\r\n"
    "  idle <seconds>            room vacancy timeout (5-3600)\r\n"
    "  passwd <newpin>           change administrator PIN (4-8 digits)\r\n\r\n");
}

static void cmd_status(void)
{
    int open_m, close_m, idle, inside, nusers;
    uint32_t gr, de, on_ms[NUM_ROOMS];
    bool occ[NUM_ROOMS], lit[NUM_ROOMS], sd, is_open;
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    open_m = G.cfg.open_min; close_m = G.cfg.close_min; idle = G.cfg.idle_s;
    inside = G.inside_count; nusers = G.nusers; gr = G.granted; de = G.denied; sd = G.sd_ok;
    is_open = schedule_open_at(now_minute(), &G.cfg);
    for (int i = 0; i < NUM_ROOMS; i++) { occ[i] = G.rooms[i].occupied; lit[i] = G.rooms[i].light; on_ms[i] = G.rooms[i].light_on_ms; }
    xSemaphoreGive(G.mtx);

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    printf("Time            : %04d-%02d-%02d %02d:%02d:%02d\r\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    printf("Access window   : %02d:%02d - %02d:%02d (%s)\r\n", open_m / 60, open_m % 60, close_m / 60, close_m % 60, is_open ? "OPEN" : "CLOSED");
    printf("Building count  : %d inside, %d registered users\r\n", inside, nusers);
    printf("Access totals   : %lu granted, %lu denied\r\n", (unsigned long)gr, (unsigned long)de);
    for (int i = 0; i < NUM_ROOMS; i++)
        printf("Room %d          : %s, light %s, light-on time %lu s\r\n", i + 1, occ[i] ? "occupied" : "vacant", lit[i] ? "ON" : "OFF", (unsigned long)(on_ms[i] / 1000));
    printf("Vacancy timeout : %d s\r\n", idle);
    printf("SD card         : %s\r\n", sd ? "mounted" : "NOT available (RAM-only mode)");
    printf("LCD             : %s\r\n", display_ok() ? "online (I2C 0x27)" : "not detected");
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until_us) printf("Admin login     : LOCKED for %d more s\r\n", (int)((s_lock_until_us - now) / 1000000));
    else printf("Admin session   : %s\r\n", s_admin ? "logged in" : "not logged in");
}

static void cmd_rooms(void)
{
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    bool o[NUM_ROOMS], l[NUM_ROOMS];
    for (int i = 0; i < NUM_ROOMS; i++) { o[i] = G.rooms[i].occupied; l[i] = G.rooms[i].light; }
    xSemaphoreGive(G.mtx);
    for (int i = 0; i < NUM_ROOMS; i++)
        printf("Room %d: %s, light %s\r\n", i + 1, o[i] ? "OCCUPIED" : "vacant", l[i] ? "ON" : "OFF");
}

static bool parse_hm(const char *s, int *minutes)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

static void cmd_login(int argc, char **argv)
{
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until_us) { printf("Login locked. Try again in %d s.\r\n", (int)((s_lock_until_us - now) / 1000000) + 1); return; }
    if (argc < 3) { printf("Usage: login admin <pin>\r\n"); return; }

    char h[65];
    hash_pin(argv[1], argv[2], h);
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    int diff = 0;
    for (int i = 0; i < 64; i++) diff |= h[i] ^ G.cfg.admin_hash[i];     /* constant-time compare */
    xSemaphoreGive(G.mtx);

    if (diff == 0 && strcmp(argv[1], "admin") == 0) {
        s_admin = true; s_fails = 0; s_last_us = now;
        printf("Login successful.\r\n");
        log_post(LOG_SYSTEM, "AUTH_OK", NULL, "administrator logged in");
    } else {
        s_fails++;
        log_post(LOG_SYSTEM, "AUTH_FAIL", NULL, "failed login attempt %d/%d", s_fails, MAX_FAILS);
        printf("Invalid credentials.\r\n");
        if (s_fails >= MAX_FAILS) {
            s_lock_until_us = now + LOCKOUT_US;
            s_fails = 0;
            log_post(LOG_SYSTEM, "LOCKOUT", NULL, "admin login locked for %d s", (int)(LOCKOUT_US / 1000000));
            io_buzz(600);
            printf("Too many failures: login locked for %d s.\r\n", (int)(LOCKOUT_US / 1000000));
        }
    }
}

static void set_time(int argc, char **argv)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int hm, y, mo, d;
    const char *ts = argv[argc - 1];
    if (!parse_hm(ts, &hm)) { printf("Usage: time set [YYYY-MM-DD] HH:MM\r\n"); return; }
    if (argc == 4) {
        if (sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 || y < 2024 || mo < 1 || mo > 12 || d < 1 || d > 31) {
            printf("Invalid date.\r\n"); return;
        }
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    }
    tm.tm_hour = hm / 60; tm.tm_min = hm % 60; tm.tm_sec = 0; tm.tm_isdst = -1;
    struct timeval tv = { .tv_sec = mktime(&tm) };
    settimeofday(&tv, NULL);
    log_post(LOG_SYSTEM, "TIME_SET", NULL, "clock set to %s", ts);
}

static void exec(char *line)
{
    char *argv[6];
    int argc = 0;
    for (char *p = strtok(line, " \t"); p && argc < 6; p = strtok(NULL, " \t")) argv[argc++] = p;
    if (argc == 0) return;
    const char *c = argv[0];

    if (!strcmp(c, "help") || !strcmp(c, "?")) print_help();
    else if (!strcmp(c, "status")) cmd_status();
    else if (!strcmp(c, "who")) access_print_inside();
    else if (!strcmp(c, "rooms")) cmd_rooms();
    else if (!strcmp(c, "log")) storage_print_ring(LOG_ACCESS, argc > 1 ? atoi(argv[1]) : 10);
    else if (!strcmp(c, "syslog")) storage_print_ring(LOG_SYSTEM, argc > 1 ? atoi(argv[1]) : 10);
    else if (!strcmp(c, "logfile")) {
        if (argc > 1 && !strcmp(argv[1], "access")) storage_cat(F_ACCESS);
        else if (argc > 1 && !strcmp(argv[1], "events")) storage_cat(F_EVENTS);
        else if (argc > 1 && !strcmp(argv[1], "users")) storage_cat(F_USERS);
        else if (argc > 1 && !strcmp(argv[1], "config")) storage_cat(F_CONFIG);
        else printf("Usage: logfile access|events|users|config\r\n");
    }
    else if (!strcmp(c, "scan")) {
        if (argc == 3 && (!strcmp(argv[1], "in") || !strcmp(argv[1], "out"))) {
            normalize_uid(argv[2]);
            rfid_sim_push(!strcmp(argv[1], "in") ? DIR_IN : DIR_OUT, argv[2]);
        } else printf("Usage: scan in|out <uid>\r\n");
    }
    else if (!strcmp(c, "rfidtest")) {
        if (argc == 2 && (argv[1][0] == '0' || argv[1][0] == '1')) rfid_debug(argv[1][0] - '0');
        else printf("Usage: rfidtest 0|1   (0 = ENTRY reader, 1 = EXIT reader)\r\n");
    }
    else if (!strcmp(c, "login")) cmd_login(argc, argv);
    else if (!strcmp(c, "logout")) {
        if (s_admin) { s_admin = false; log_post(LOG_SYSTEM, "AUTH_OUT", NULL, "administrator logged out"); }
        printf("Logged out.\r\n");
    }
    else if (!strcmp(c, "schedule")) {
        if (argc == 1) {
            xSemaphoreTake(G.mtx, portMAX_DELAY);
            int o = G.cfg.open_min, cl = G.cfg.close_min;
            xSemaphoreGive(G.mtx);
            printf("Access window: %02d:%02d - %02d:%02d\r\n", o / 60, o % 60, cl / 60, cl % 60);
        } else if (argc == 4 && !strcmp(argv[1], "set")) {
            int o, cl;
            if (!need_admin()) return;
            if (!parse_hm(argv[2], &o) || !parse_hm(argv[3], &cl)) { printf("Usage: schedule set HH:MM HH:MM\r\n"); return; }
            const char *e = access_set_schedule(o, cl);
            if (e) printf("Error: %s\r\n", e); else printf("Schedule updated.\r\n");
        } else printf("Usage: schedule | schedule set HH:MM HH:MM\r\n");
    }
    else if (!strcmp(c, "time")) {
        if (argc == 1) {
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            printf("%04d-%02d-%02d %02d:%02d:%02d\r\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else if ((argc == 3 || argc == 4) && !strcmp(argv[1], "set")) {
            if (need_admin()) set_time(argc, argv);
        } else printf("Usage: time | time set [YYYY-MM-DD] HH:MM\r\n");
    }
    else if (!strcmp(c, "user")) {
        if (!need_admin()) return;
        if (argc == 2 && !strcmp(argv[1], "list")) access_print_users();
        else if (argc == 4 && !strcmp(argv[1], "add")) { const char *e = access_user_add(argv[2], argv[3]); printf("%s\r\n", e ? e : "User added."); }
        else if (argc == 3 && !strcmp(argv[1], "del")) { const char *e = access_user_del(argv[2]); printf("%s\r\n", e ? e : "User removed."); }
        else printf("Usage: user list | user add <uid> <name> | user del <uid>\r\n");
    }
    else if (!strcmp(c, "idle")) {
        if (!need_admin()) return;
        int v = argc > 1 ? atoi(argv[1]) : 0;
        if (v < 5 || v > 3600) { printf("Usage: idle <5-3600 seconds>\r\n"); return; }
        xSemaphoreTake(G.mtx, portMAX_DELAY);
        G.cfg.idle_s = v;
        storage_save_config_locked();
        xSemaphoreGive(G.mtx);
        log_post(LOG_SYSTEM, "CONFIG", NULL, "room vacancy timeout set to %d s", v);
        printf("Vacancy timeout updated.\r\n");
    }
    else if (!strcmp(c, "passwd")) {
        if (!need_admin()) return;
        if (argc != 2 || !valid_pin(argv[1])) { printf("PIN must be 4-8 digits.\r\n"); return; }
        char h[65];
        hash_pin("admin", argv[1], h);
        xSemaphoreTake(G.mtx, portMAX_DELAY);
        strlcpy(G.cfg.admin_hash, h, sizeof G.cfg.admin_hash);
        storage_save_config_locked();
        xSemaphoreGive(G.mtx);
        log_post(LOG_SYSTEM, "CONFIG", NULL, "administrator PIN changed");
        printf("PIN changed.\r\n");
    }
    else printf("Unknown command '%s'. Type 'help'.\r\n", c);
}

void console_task(void *arg)
{
    char line[96];
    int len = 0;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\nCoCIS Smart Control & Monitoring System - type 'help'\r\ncocis> ");
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {                       /* no byte available yet */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len == 0) continue;
            line[len] = 0;
            printf("\r\n");
            exec(line);
            len = 0;
            printf("cocis> ");
        } else if ((ch == 0x08 || ch == 0x7F) && len > 0) {
            len--;
            printf("\b \b");
        } else if (ch >= 0x20 && ch < 0x7F && len < (int)sizeof line - 1) {
            line[len++] = (char)ch;
            putchar(ch);
        }
    }
}