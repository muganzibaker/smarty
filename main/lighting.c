/* lighting.c - one task instance per lecture room: PIR occupancy -> automatic lighting */
#include "app.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define POLL_MS 200

void room_task(void *arg)
{
    const int r = (int)(intptr_t)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool motion = io_pir(r) != 0;
        bool changed = false, occ = false;

        xSemaphoreTake(G.mtx, portMAX_DELAY);
        room_t *R = &G.rooms[r];
        if (motion) {
            R->last_motion_us = now;
            if (!R->occupied) { R->occupied = true; changed = true; }
        } else if (R->occupied && now - R->last_motion_us > (int64_t)G.cfg.idle_s * 1000000LL) {
            R->occupied = false;
            changed = true;
        }
        if (R->light != R->occupied) {
            R->light = R->occupied;
            io_light_set(r, R->light);
        }
        if (R->light) R->light_on_ms += POLL_MS;
        occ = R->occupied;
        xSemaphoreGive(G.mtx);

        if (changed)
            log_post(LOG_SYSTEM, occ ? "ROOM_OCC" : "ROOM_VAC", NULL, "Room %d %s, light %s",
                     r + 1, occ ? "occupied" : "vacant", occ ? "ON" : "OFF");
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
