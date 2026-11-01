#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ---------------- Pin map (single ESP32 DevKit) ---------------- */
#define PIN_SPI_SCK      18
#define PIN_SPI_MOSI     23
#define PIN_SPI_MISO     19
#define PIN_SD_CS         5
#define PIN_RFID_IN_CS   17
#define PIN_RFID_OUT_CS  16
#define PIN_RFID_RST      4      /* moved from 21: GPIO21 is now the I2C SDA line */
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define LCD_ADDR       0x27
#define LCD_COLS_MAX     16
#define PIN_PIR1         34
#define PIN_PIR2         35
#define PIN_LIGHT1       32
#define PIN_LIGHT2       33
#define PIN_DOOR_IN      25
#define PIN_DOOR_OUT     26
#define PIN_DENY_LED     27
#define PIN_BUZZER       13

#define DIR_IN   0
#define DIR_OUT  1

#define MAX_USERS  32
#define UID_LEN    21
#define NAME_LEN   24
#define NUM_ROOMS   2

#define SD_MOUNT   "/sdcard"
#define F_ACCESS   SD_MOUNT "/access.csv"
#define F_EVENTS   SD_MOUNT "/events.log"
#define F_USERS    SD_MOUNT "/users.csv"
#define F_CONFIG   SD_MOUNT "/config.txt"

typedef struct {
    char uid[UID_LEN];
    char name[NAME_LEN];
    bool inside;
} user_t;

typedef struct {
    int  open_min;        /* minutes after midnight when access opens  */
    int  close_min;       /* minutes after midnight when access closes */
    int  idle_s;          /* seconds without motion before light off   */
    char admin_hash[65];  /* SHA-256 hex of "cocis:<user>:<pin>"      */
} config_t;

typedef struct {
    bool     occupied;
    bool     light;
    int64_t  last_motion_us;
    uint32_t light_on_ms;
} room_t;

/* Shared system state. G.mtx protects everything in this struct. */
typedef struct {
    SemaphoreHandle_t mtx;
    user_t   users[MAX_USERS];
    int      nusers;
    int      inside_count;
    config_t cfg;
    room_t   rooms[NUM_ROOMS];
    bool     sd_ok;
    uint32_t granted, denied;
} sys_state_t;
extern sys_state_t G;

/* sha256.c */
void sha256_hex(const char *msg, char out[65]);

/* io.c */
void io_init(void);
void io_door_pulse(int dir);
void io_deny(void);
void io_buzz(int ms);
void io_light_set(int room, bool on);
int  io_pir(int room);

/* storage.c */
typedef enum { LOG_ACCESS = 0, LOG_SYSTEM = 1 } log_kind_t;
void storage_init(void);
void log_post(log_kind_t kind, const char *tag, const char *uid, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void storage_save_users_locked(void);
void storage_save_config_locked(void);
void storage_print_ring(log_kind_t kind, int n);
void storage_cat(const char *path);

/* access.c */
int         now_minute(void);
bool        schedule_open_at(int minute, const config_t *c);
void        access_handle_tag(int dir, const char *uid);
void        entrance_task(void *arg);
void        monitor_task(void *arg);
const char *access_user_add(const char *uid, const char *name);
const char *access_user_del(const char *uid);
const char *access_set_schedule(int open_min, int close_min);
void        access_print_inside(void);
void        access_print_users(void);
void        normalize_uid(char *uid);

/* lighting.c */
void room_task(void *arg);

/* rfid.c */
void rfid_init(void);
bool rfid_poll(int reader, char *uid_out);
void rfid_sim_push(int reader, const char *uid);
void rfid_debug(int reader);

/* display.c  (16x2 I2C LCD; display task owns the bus, others only post messages) */
void display_init(void);
bool display_ok(void);
void display_msg(const char *l1, const char *l2, uint32_t hold_ms);

/* console.c */
void console_task(void *arg);