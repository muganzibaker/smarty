/* access.c - entrance terminal: schedule, RFID decisions, anti-passback, closing-time occupancy alert */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "app.h"
#include "esp_timer.h"
#include "freertos/task.h"

int now_minute(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

/* Supports windows that wrap past midnight (open > close). */
bool schedule_open_at(int m, const config_t *c)
{
    if (c->open_min < c->close_min) return m >= c->open_min && m < c->close_min;
    return m >= c->open_min || m < c->close_min;
}

void normalize_uid(char *uid)
{
    for (char *p = uid; *p; p++) *p = (char)toupper((unsigned char)*p);
}

static user_t *find_user_locked(const char *uid)
{
    for (int i = 0; i < G.nusers; i++)
        if (strcmp(G.users[i].uid, uid) == 0) return &G.users[i];
    return NULL;
}

void access_handle_tag(int dir, const char *uid_in)
{
    char uid[UID_LEN], name[NAME_LEN] = "unknown", reason[48] = "", why[LCD_COLS_MAX + 1] = "";   /* why = LCD-sized reason */
    strlcpy(uid, uid_in, sizeof uid);
    normalize_uid(uid);
    bool grant = false;
    int count;

    xSemaphoreTake(G.mtx, portMAX_DELAY);               /* one decision at a time */
    user_t *u = find_user_locked(uid);
    if (!u) {
        strlcpy(reason, "unregistered tag", sizeof reason);
        strlcpy(why, "Unknown tag", sizeof why);
    } else {
        strlcpy(name, u->name, sizeof name);
        if (!schedule_open_at(now_minute(), &G.cfg)) {
            strlcpy(reason, "outside permitted hours", sizeof reason);
            strlcpy(why, "Outside hours", sizeof why);
        } else if (dir == DIR_IN && u->inside) {
            strlcpy(reason, "anti-passback: already inside", sizeof reason);
            strlcpy(why, "Already inside", sizeof why);
        } else if (dir == DIR_OUT && !u->inside) {
            strlcpy(reason, "anti-passback: not inside", sizeof reason);
            strlcpy(why, "Not inside", sizeof why);
        } else {
            grant = true;
            u->inside = (dir == DIR_IN);
            G.inside_count += (dir == DIR_IN) ? 1 : -1;
            G.granted++;
            storage_save_users_locked();
        }
    }
    if (!grant) G.denied++;
    count = G.inside_count;
    xSemaphoreGive(G.mtx);

    if (grant) {
        io_door_pulse(dir);
        char l1[24], l2[24];
        snprintf(l1, sizeof l1, "%s %.8s", dir == DIR_IN ? "Welcome" : "Goodbye", name);
        snprintf(l2, sizeof l2, "Inside now: %d", count);
        display_msg(l1, l2, 3000);
        log_post(LOG_ACCESS, dir == DIR_IN ? "GRANT_IN" : "GRANT_OUT", uid, "%s; building count=%d", name, count);
    } else {
        io_deny();
        display_msg(dir == DIR_IN ? "DENIED (entry)" : "DENIED (exit)", why, 3000);
        log_post(LOG_ACCESS, dir == DIR_IN ? "DENY_IN" : "DENY_OUT", uid, "%s: %s", name, reason);
    }
}

/* Entrance terminal: polls the ENTRY and EXIT readers. */
void entrance_task(void *arg)
{
    static char last_uid[2][UID_LEN];
    static int64_t last_us[2];
    for (;;) {
        for (int r = 0; r < 2; r++) {
            char uid[UID_LEN];
            if (!rfid_poll(r, uid)) continue;
            int64_t now = esp_timer_get_time();
            if (strcmp(uid, last_uid[r]) == 0 && now - last_us[r] < 2000000) continue;  /* debounce */
            strlcpy(last_uid[r], uid, UID_LEN);
            last_us[r] = now;
            access_handle_tag(r, uid);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Closing-time monitor: alerts when people are still inside outside permitted hours. */
void monitor_task(void *arg)
{
    bool alerted = false;
    int64_t last_alert_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(G.mtx, portMAX_DELAY);
        bool open = schedule_open_at(now_minute(), &G.cfg);
        int inside = G.inside_count;
        xSemaphoreGive(G.mtx);

        int64_t now = esp_timer_get_time();
        if (open || inside == 0) { alerted = false; continue; }
        if (!alerted || now - last_alert_us > 600LL * 1000000) {      /* first alert, then every 10 min */
            alerted = true;
            last_alert_us = now;
            log_post(LOG_SYSTEM, "ALERT", NULL, "Building closed but %d person(s) still inside", inside);
            char l2[24];
            snprintf(l2, sizeof l2, "%d still inside", inside);
            display_msg("CLOSED! ALERT", l2, 5000);
            access_print_inside();
            for (int i = 0; i < 3; i++) { io_buzz(250); vTaskDelay(pdMS_TO_TICKS(500)); }
        }
    }
}

/* ---- administration helpers ---- */
const char *access_user_add(const char *uid_in, const char *name)
{
    char uid[UID_LEN];
    strlcpy(uid, uid_in, sizeof uid);
    normalize_uid(uid);
    size_t n = strlen(uid);
    if (n < 4 || n > 20 || n % 2) return "UID must be 4-20 hex digits (even length)";
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)uid[i])) return "UID must be hexadecimal";
    if (strlen(name) == 0 || strlen(name) >= NAME_LEN || strchr(name, ',')) return "invalid name";

    const char *err = NULL;
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    if (find_user_locked(uid)) err = "UID already registered";
    else if (G.nusers >= MAX_USERS) err = "user table full";
    else {
        user_t *u = &G.users[G.nusers++];
        strlcpy(u->uid, uid, sizeof u->uid);
        strlcpy(u->name, name, sizeof u->name);
        u->inside = false;
        storage_save_users_locked();
    }
    xSemaphoreGive(G.mtx);
    if (!err) log_post(LOG_SYSTEM, "USER_ADD", uid, "%s registered", name);
    return err;
}

const char *access_user_del(const char *uid_in)
{
    char uid[UID_LEN];
    strlcpy(uid, uid_in, sizeof uid);
    normalize_uid(uid);
    const char *err = "UID not found";
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    for (int i = 0; i < G.nusers; i++) {
        if (strcmp(G.users[i].uid, uid) != 0) continue;
        if (G.users[i].inside) G.inside_count--;
        G.users[i] = G.users[--G.nusers];
        storage_save_users_locked();
        err = NULL;
        break;
    }
    xSemaphoreGive(G.mtx);
    if (!err) log_post(LOG_SYSTEM, "USER_DEL", uid, "user removed");
    return err;
}

const char *access_set_schedule(int open_min, int close_min)
{
    if (open_min < 0 || open_min > 1439 || close_min < 0 || close_min > 1439) return "time out of range";
    if (open_min == close_min) return "open and close times must differ";
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    G.cfg.open_min = open_min;
    G.cfg.close_min = close_min;
    storage_save_config_locked();
    xSemaphoreGive(G.mtx);
    log_post(LOG_SYSTEM, "SCHEDULE", NULL, "access window set to %02d:%02d-%02d:%02d",
             open_min / 60, open_min % 60, close_min / 60, close_min % 60);
    return NULL;
}

void access_print_inside(void)
{
    char names[MAX_USERS][NAME_LEN], uids[MAX_USERS][UID_LEN];
    int n = 0;
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    for (int i = 0; i < G.nusers; i++)
        if (G.users[i].inside) {
            strlcpy(names[n], G.users[i].name, NAME_LEN);
            strlcpy(uids[n], G.users[i].uid, UID_LEN);
            n++;
        }
    xSemaphoreGive(G.mtx);
    printf("People inside the building: %d\r\n", n);
    for (int i = 0; i < n; i++) printf("  %-10s %s\r\n", uids[i], names[i]);
}

void access_print_users(void)
{
    user_t copy[MAX_USERS];
    int n;
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    n = G.nusers;
    memcpy(copy, G.users, sizeof(user_t) * n);
    xSemaphoreGive(G.mtx);
    printf("Registered users: %d\r\n", n);
    for (int i = 0; i < n; i++)
        printf("  %-10s %-16s %s\r\n", copy[i].uid, copy[i].name, copy[i].inside ? "inside" : "outside");
}
