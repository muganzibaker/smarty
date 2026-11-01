/* display.c - 16x2 character LCD (HD44780 controller + PCF8574 I2C backpack, address 0x27).
 *
 * Design: the display task is the ONLY owner of the I2C bus. Every other task (entrance, monitor, ...)
 * just posts a short message to a queue with display_msg(), so no bus mutex is needed and a slow or
 * missing LCD can never block door control. If the LCD is not detected the system runs normally.
 *
 * Idle screen alternates every ~4 s between:
 *    page 0:  "09:00:12 OPEN"      "In:3   Users:4"
 *    page 1:  "R1 occ light ON"    "R2 vac light OFF"
 * Access decisions (welcome / goodbye / denied / closing alert) interrupt it for a few seconds.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LCD_COLS 16

/* PCF8574 -> HD44780 wiring used by the common backpack (and by Wokwi): P0=RS P1=RW P2=EN P3=BL P4..P7=D4..D7 */
#define BIT_RS 0x01
#define BIT_EN 0x04
#define BIT_BL 0x08

typedef struct {
    char     l1[LCD_COLS + 1];
    char     l2[LCD_COLS + 1];
    uint32_t hold_ms;
} disp_msg_t;

static i2c_master_dev_handle_t s_dev;
static QueueHandle_t           s_q;
static bool                    s_ok;
static char                    s_shown[2][LCD_COLS + 1];   /* what is on screen now: skip unchanged lines (no flicker) */

static void expander_write(const uint8_t *b, size_t n)
{
    i2c_master_transmit(s_dev, b, n, 50);
}

static void lcd_nibble(uint8_t n)                     /* init-time 4-bit write */
{
    uint8_t b = (uint8_t)((n << 4) | BIT_BL);
    uint8_t seq[2] = { (uint8_t)(b | BIT_EN), b };
    expander_write(seq, sizeof seq);
}

static void lcd_send(uint8_t v, bool rs)              /* one byte = two nibbles, each latched by an EN pulse */
{
    uint8_t flags = BIT_BL | (rs ? BIT_RS : 0);
    uint8_t hi = (uint8_t)((v & 0xF0) | flags);
    uint8_t lo = (uint8_t)(((v << 4) & 0xF0) | flags);
    uint8_t seq[4] = { (uint8_t)(hi | BIT_EN), hi, (uint8_t)(lo | BIT_EN), lo };
    expander_write(seq, sizeof seq);
}

static void lcd_hw_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));                    /* power-up settle */
    lcd_nibble(0x3); vTaskDelay(pdMS_TO_TICKS(5));    /* HD44780 reset-by-instruction sequence */
    lcd_nibble(0x3); vTaskDelay(pdMS_TO_TICKS(2));
    lcd_nibble(0x3); vTaskDelay(pdMS_TO_TICKS(2));
    lcd_nibble(0x2); vTaskDelay(pdMS_TO_TICKS(2));    /* switch to 4-bit mode */
    lcd_send(0x28, false);                            /* 4-bit, 2 lines, 5x8 font */
    lcd_send(0x0C, false);                            /* display on, cursor off   */
    lcd_send(0x06, false);                            /* cursor moves right       */
    lcd_send(0x01, false);                            /* clear                    */
    vTaskDelay(pdMS_TO_TICKS(5));
}

static void lcd_line(int row, const char *text)
{
    char pad[LCD_COLS + 1];
    snprintf(pad, sizeof pad, "%-16.16s", text);      /* pad / truncate to exactly 16 columns */
    if (strcmp(pad, s_shown[row]) == 0) return;
    lcd_send((uint8_t)(0x80 | (row ? 0x40 : 0x00)), false);
    for (int i = 0; i < LCD_COLS; i++) lcd_send((uint8_t)pad[i], true);
    memcpy(s_shown[row], pad, sizeof pad);
}

static void idle_screen(unsigned tick)
{
    int  inside, nusers;
    bool open, occ[NUM_ROOMS], lit[NUM_ROOMS];

    xSemaphoreTake(G.mtx, portMAX_DELAY);             /* short critical section: copy, then release */
    inside = G.inside_count;
    nusers = G.nusers;
    open   = schedule_open_at(now_minute(), &G.cfg);
    for (int i = 0; i < NUM_ROOMS; i++) { occ[i] = G.rooms[i].occupied; lit[i] = G.rooms[i].light; }
    xSemaphoreGive(G.mtx);

    char a[48], b[48];
    if ((tick / 8) % 2 == 0) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(a, sizeof a, "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, open ? "OPEN" : "CLOSED");
        snprintf(b, sizeof b, "In:%-3d Users:%-3d", inside, nusers);
    } else {
        snprintf(a, sizeof a, "R1 %-3s light %-3s", occ[0] ? "occ" : "vac", lit[0] ? "ON" : "OFF");
        snprintf(b, sizeof b, "R2 %-3s light %-3s", occ[1] ? "occ" : "vac", lit[1] ? "ON" : "OFF");
    }
    lcd_line(0, a);
    lcd_line(1, b);
}

static void display_task(void *arg)
{
    disp_msg_t m;
    unsigned tick = 0;
    int64_t hold_until = 0;
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(500)) == pdTRUE) {      /* event message wins */
            lcd_line(0, m.l1);
            lcd_line(1, m.l2);
            hold_until = esp_timer_get_time() + (int64_t)m.hold_ms * 1000;
            continue;
        }
        if (esp_timer_get_time() >= hold_until) idle_screen(tick++);     /* otherwise refresh idle page */
    }
}

/* Non-blocking: safe to call from any task, never while the caller must not wait. Drops the message if the queue is full. */
void display_msg(const char *l1, const char *l2, uint32_t hold_ms)
{
    if (!s_ok) return;
    disp_msg_t m = { .hold_ms = hold_ms };
    strlcpy(m.l1, l1, sizeof m.l1);
    strlcpy(m.l2, l2, sizeof m.l2);
    xQueueSend(s_q, &m, 0);
}

bool display_ok(void) { return s_ok; }

void display_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bc, &bus);
    if (err == ESP_OK) err = i2c_master_probe(bus, LCD_ADDR, 100);          /* is anything answering at 0x27? */
    if (err == ESP_OK) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = LCD_ADDR,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    }
    if (err != ESP_OK) {
        log_post(LOG_SYSTEM, "LCD_FAIL", NULL, "LCD not available (%s): continuing without display", esp_err_to_name(err));
        return;
    }
    s_q = xQueueCreate(4, sizeof(disp_msg_t));
    lcd_hw_init();
    s_ok = true;
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    display_msg("CoCIS Smart Ctrl", "Starting...", 2000);
    log_post(LOG_SYSTEM, "LCD_OK", NULL, "16x2 LCD ready at I2C 0x%02X", LCD_ADDR);
}
