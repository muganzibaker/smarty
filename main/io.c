/* io.c - GPIO, door/deny indicators, buzzer (LEDC), room lights, PIR inputs */
#include "app.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

enum { T_DOOR_IN, T_DOOR_OUT, T_DENY, T_COUNT };
static const int pulse_pin[T_COUNT] = { PIN_DOOR_IN, PIN_DOOR_OUT, PIN_DENY_LED };
static esp_timer_handle_t pulse_timer[T_COUNT];
static esp_timer_handle_t buzz_timer;

#define DOOR_OPEN_MS 3000
#define DENY_MS      1200

static void pulse_off_cb(void *arg) { gpio_set_level((int)(intptr_t)arg, 0); }

static void buzz_off_cb(void *arg)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void pulse(int idx, int ms)
{
    gpio_set_level(pulse_pin[idx], 1);
    esp_timer_stop(pulse_timer[idx]);               /* ignore error if idle */
    esp_timer_start_once(pulse_timer[idx], (uint64_t)ms * 1000ULL);
}

void io_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_DOOR_IN) | (1ULL << PIN_DOOR_OUT) | (1ULL << PIN_DENY_LED) |
                        (1ULL << PIN_LIGHT1) | (1ULL << PIN_LIGHT2),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_PIR1) | (1ULL << PIN_PIR2),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    for (int i = 0; i < T_COUNT; i++) {
        gpio_set_level(pulse_pin[i], 0);
        esp_timer_create_args_t a = { .callback = pulse_off_cb, .arg = (void *)(intptr_t)pulse_pin[i], .name = "pulse" };
        esp_timer_create(&a, &pulse_timer[i]);
    }
    gpio_set_level(PIN_LIGHT1, 0);
    gpio_set_level(PIN_LIGHT2, 0);

    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {
        .gpio_num = PIN_BUZZER, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&lc);
    esp_timer_create_args_t b = { .callback = buzz_off_cb, .name = "buzz" };
    esp_timer_create(&b, &buzz_timer);
}

void io_door_pulse(int dir) { pulse(dir == DIR_IN ? T_DOOR_IN : T_DOOR_OUT, DOOR_OPEN_MS); }

void io_deny(void)
{
    pulse(T_DENY, DENY_MS);
    io_buzz(150);
}

void io_buzz(int ms)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    esp_timer_stop(buzz_timer);
    esp_timer_start_once(buzz_timer, (uint64_t)ms * 1000ULL);
}

void io_light_set(int room, bool on) { gpio_set_level(room == 0 ? PIN_LIGHT1 : PIN_LIGHT2, on ? 1 : 0); }
int  io_pir(int room)                { return gpio_get_level(room == 0 ? PIN_PIR1 : PIN_PIR2); }
