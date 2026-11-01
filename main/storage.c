/* storage.c - SD card (FAT over SPI), single-writer logger task, users/config persistence.
 *
 * Locking rule (prevents deadlock): G.mtx may be held while taking sd_mtx, never the reverse.
 * The logger task takes only sd_mtx/ring_mtx and never G.mtx.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "storage";

#define LOG_LINE 160
#define RING_N   20

typedef struct {
    log_kind_t kind;
    time_t ts;
    char tag[14];
    char uid[UID_LEN];
    char text[88];
} log_evt_t;

static SemaphoreHandle_t sd_mtx;     /* serialises all SD file access     */
static SemaphoreHandle_t ring_mtx;   /* protects in-RAM recent-log buffer */
static QueueHandle_t     log_q;
static char ring[2][RING_N][LOG_LINE];
static int  ring_next[2], ring_cnt[2];

static void format_line(const log_evt_t *e, char *line)
{
    struct tm tm;
    char ts[24];
    localtime_r(&e->ts, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(line, LOG_LINE, "%s,%s,%s,%s", ts, e->tag, e->uid[0] ? e->uid : "-", e->text);
}

void log_post(log_kind_t kind, const char *tag, const char *uid, const char *fmt, ...)
{
    log_evt_t e = { .kind = kind, .ts = time(NULL) };
    strlcpy(e.tag, tag, sizeof e.tag);
    if (uid) strlcpy(e.uid, uid, sizeof e.uid);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof e.text, fmt, ap);
    va_end(ap);
    for (char *p = e.text; *p; p++) if (*p == ',') *p = ';';   /* keep CSV valid */

    printf("\r\n[%s] %s %s\r\n", e.tag, e.uid[0] ? e.uid : "", e.text);
    if (log_q) xQueueSend(log_q, &e, 0);
}

static void logger_task(void *arg)
{
    log_evt_t e;
    char line[LOG_LINE];
    for (;;) {
        if (xQueueReceive(log_q, &e, portMAX_DELAY) != pdTRUE) continue;
        format_line(&e, line);

        xSemaphoreTake(ring_mtx, portMAX_DELAY);
        strlcpy(ring[e.kind][ring_next[e.kind]], line, LOG_LINE);
        ring_next[e.kind] = (ring_next[e.kind] + 1) % RING_N;
        if (ring_cnt[e.kind] < RING_N) ring_cnt[e.kind]++;
        xSemaphoreGive(ring_mtx);

        if (G.sd_ok) {
            xSemaphoreTake(sd_mtx, portMAX_DELAY);
            FILE *f = fopen(e.kind == LOG_ACCESS ? F_ACCESS : F_EVENTS, "a");
            if (f) { fprintf(f, "%s\n", line); fclose(f); }
            else   ESP_LOGW(TAG, "cannot append log line to SD");
            xSemaphoreGive(sd_mtx);
        }
    }
}

void storage_print_ring(log_kind_t kind, int n)
{
    if (n < 1) n = 1;
    if (n > RING_N) n = RING_N;
    xSemaphoreTake(ring_mtx, portMAX_DELAY);
    int cnt = ring_cnt[kind];
    if (n > cnt) n = cnt;
    int start = (ring_next[kind] - n + RING_N) % RING_N;
    for (int i = 0; i < n; i++) printf("%s\r\n", ring[kind][(start + i) % RING_N]);
    if (n == 0) printf("(no records)\r\n");
    xSemaphoreGive(ring_mtx);
}

void storage_cat(const char *path)
{
    if (!G.sd_ok) { printf("SD card not available.\r\n"); return; }
    xSemaphoreTake(sd_mtx, portMAX_DELAY);
    FILE *f = fopen(path, "r");
    if (!f) printf("(file not found)\r\n");
    else {
        char line[LOG_LINE];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            printf("%s\r\n", line);
        }
        fclose(f);
    }
    xSemaphoreGive(sd_mtx);
}

/* ---- persistence (caller holds G.mtx) ---- */
void storage_save_users_locked(void)
{
    if (!G.sd_ok) return;
    xSemaphoreTake(sd_mtx, portMAX_DELAY);
    FILE *f = fopen(F_USERS, "w");
    if (f) {
        for (int i = 0; i < G.nusers; i++)
            fprintf(f, "%s,%s,%d\n", G.users[i].uid, G.users[i].name, G.users[i].inside ? 1 : 0);
        fclose(f);
    } else ESP_LOGW(TAG, "cannot write users file");
    xSemaphoreGive(sd_mtx);
}

void storage_save_config_locked(void)
{
    if (!G.sd_ok) return;
    xSemaphoreTake(sd_mtx, portMAX_DELAY);
    FILE *f = fopen(F_CONFIG, "w");
    if (f) {
        fprintf(f, "open=%d\nclose=%d\nidle=%d\nadmin=%s\n",
                G.cfg.open_min, G.cfg.close_min, G.cfg.idle_s, G.cfg.admin_hash);
        fclose(f);
    } else ESP_LOGW(TAG, "cannot write config file");
    xSemaphoreGive(sd_mtx);
}

static void load_config(void)
{
    FILE *f = fopen(F_CONFIG, "r");
    if (!f) return;
    char line[100];
    while (fgets(line, sizeof line, f)) {
        int v;
        char s[70];
        if (sscanf(line, "open=%d", &v) == 1)        G.cfg.open_min = v;
        else if (sscanf(line, "close=%d", &v) == 1)  G.cfg.close_min = v;
        else if (sscanf(line, "idle=%d", &v) == 1)   G.cfg.idle_s = v;
        else if (sscanf(line, "admin=%64s", s) == 1) strlcpy(G.cfg.admin_hash, s, sizeof G.cfg.admin_hash);
    }
    fclose(f);
}

static bool load_users(void)
{
    FILE *f = fopen(F_USERS, "r");
    if (!f) return false;
    char line[100];
    while (fgets(line, sizeof line, f) && G.nusers < MAX_USERS) {
        char uid[UID_LEN], name[NAME_LEN];
        int inside;
        if (sscanf(line, "%20[^,],%23[^,],%d", uid, name, &inside) == 3) {
            user_t *u = &G.users[G.nusers++];
            strlcpy(u->uid, uid, sizeof u->uid);
            strlcpy(u->name, name, sizeof u->name);
            u->inside = inside != 0;
            if (u->inside) G.inside_count++;
        }
    }
    fclose(f);
    return true;
}

/* Users registered on every fresh boot (the Wokwi SD card starts empty each run).
 * EDIT THE NAMES below to your group members' names (one word, no commas). */
static void seed_demo_users(void)
{
    const char *u[][2] = {
        { "01020304", "Benny" },
        { "11223344", "Oliver" },
        { "55667788", "Charlie" },
        { "AABBCCDD", "Derrick" },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) {
        strlcpy(G.users[G.nusers].uid, u[i][0], UID_LEN);
        strlcpy(G.users[G.nusers].name, u[i][1], NAME_LEN);
        G.users[G.nusers].inside = false;
        G.nusers++;
    }
}

void storage_init(void)
{
    sd_mtx   = xSemaphoreCreateMutex();
    ring_mtx = xSemaphoreCreateMutex();
    log_q    = xQueueCreate(32, sizeof(log_evt_t));
    xTaskCreate(logger_task, "logger", 4096, NULL, 4, NULL);

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = true, .max_files = 4, .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI2_HOST;

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mc, &card);
    G.sd_ok = (err == ESP_OK);

    bool users_loaded = false;
    xSemaphoreTake(G.mtx, portMAX_DELAY);
    if (G.sd_ok) {
        load_config();
        users_loaded = load_users();
        if (!users_loaded) seed_demo_users();
        storage_save_config_locked();
        storage_save_users_locked();
    } else {
        seed_demo_users();      /* degraded mode: RAM only */
    }
    xSemaphoreGive(G.mtx);

    if (G.sd_ok) log_post(LOG_SYSTEM, "SD_OK", NULL, "SD card mounted, %d users loaded", G.nusers);
    else         log_post(LOG_SYSTEM, "SD_FAIL", NULL, "SD mount failed (%s): running in RAM-only mode", esp_err_to_name(err));
}