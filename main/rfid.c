/* rfid.c - minimal MFRC522 SPI driver (two readers on one bus) + serial tag simulation */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/queue.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

#define SPI_HOST_ID SPI2_HOST

#define REG_COMMAND   0x01
#define REG_COMIRQ    0x04
#define REG_ERROR     0x06
#define REG_FIFODATA  0x09
#define REG_FIFOLEVEL 0x0A
#define REG_CONTROL   0x0C
#define REG_BITFRAME  0x0D
#define REG_COLL      0x0E
#define REG_MODE      0x11
#define REG_TXCONTROL 0x14
#define REG_TXASK     0x15
#define REG_TMODE     0x2A
#define REG_TPRESCALE 0x2B
#define REG_TRELOADH  0x2C
#define REG_TRELOADL  0x2D

static spi_device_handle_t dev[2];
static uint8_t last_irq, last_err;
static QueueHandle_t sim_q[2];

static void wr(int r, uint8_t reg, uint8_t v)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), v };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_transmit(dev[r], &t);
}

static uint8_t rd(int r, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(dev[r], &t);
    return rx[1];
}

/* Burst-read n bytes from the FIFO in ONE SPI transaction (as the standard MFRC522 libraries do). */
static void rd_fifo(int r, uint8_t *out, int n)
{
    if (n <= 0) return;
    uint8_t tx[17], rx[17];
    uint8_t addr = (uint8_t)(((REG_FIFODATA << 1) & 0x7E) | 0x80);
    memset(tx, 0, sizeof tx);
    memset(rx, 0, sizeof rx);
    for (int i = 0; i < n; i++) tx[i] = addr;      /* last tx byte stays 0 */
    spi_transaction_t t = { .length = (size_t)(n + 1) * 8, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(dev[r], &t);
    for (int i = 0; i < n; i++) out[i] = rx[i + 1];
}

static bool transceive(int r, const uint8_t *tx, int txlen, uint8_t valid_bits, uint8_t *rx, int *rxlen)
{
    wr(r, REG_COMMAND, 0x00);          /* idle            */
    wr(r, REG_COMIRQ, 0x7F);           /* clear IRQ flags */
    wr(r, REG_FIFOLEVEL, 0x80);        /* flush FIFO      */
    for (int i = 0; i < txlen; i++) wr(r, REG_FIFODATA, tx[i]);
    wr(r, REG_BITFRAME, valid_bits);
    wr(r, REG_COMMAND, 0x0C);          /* transceive      */
    wr(r, REG_BITFRAME, rd(r, REG_BITFRAME) | 0x80);   /* start send */

    bool done = false;
    for (int i = 0; i < 60; i++) {
        uint8_t irq = rd(r, REG_COMIRQ);
        if (irq & 0x30) { done = true; break; }        /* RxIRq | IdleIRq */
        if (irq & 0x01) break;                         /* TimerIRq: no card */
        esp_rom_delay_us(500);
    }
    wr(r, REG_BITFRAME, rd(r, REG_BITFRAME) & (uint8_t)~0x80);
    last_irq = rd(r, REG_COMIRQ);
    last_err = rd(r, REG_ERROR);
    if (!done) return false;
    if (rd(r, REG_ERROR) & 0x13) return false;        /* buffer ovf / parity / protocol */

    int n = rd(r, REG_FIFOLEVEL);
    if (n > *rxlen) n = *rxlen;
    if (n > 16) n = 16;
    rd_fifo(r, rx, n);
    *rxlen = n;
    return true;
}

static void pcd_init(int r)
{
    wr(r, REG_COMMAND, 0x0F);                          /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(50));
    wr(r, REG_TMODE, 0x8D);
    wr(r, REG_TPRESCALE, 0x3E);
    wr(r, REG_TRELOADH, 0);
    wr(r, REG_TRELOADL, 30);
    wr(r, REG_TXASK, 0x40);
    wr(r, REG_MODE, 0x3D);
    wr(r, REG_TXCONTROL, rd(r, REG_TXCONTROL) | 0x03); /* antenna on */
}

void rfid_init(void)
{
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_RFID_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_RFID_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RFID_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    const int cs[2] = { PIN_RFID_IN_CS, PIN_RFID_OUT_CS };
    for (int i = 0; i < 2; i++) {
        spi_device_interface_config_t d = {
            .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = cs[i], .queue_size = 2,
        };
        spi_bus_add_device(SPI_HOST_ID, &d, &dev[i]);
        pcd_init(i);
        printf("RFID %s reader: MFRC522 VersionReg=0x%02X (expect 0x91/0x92; 0x00 or 0xFF = wiring problem)\r\n",
               i == 0 ? "ENTRY" : "EXIT", rd(i, 0x37));
        sim_q[i] = xQueueCreate(4, UID_LEN);
    }
}

/* Reads one 4-byte UID if a card is in the field. */
static bool hw_read(int r, char *uid_out)
{
    uint8_t buf[16];
    int len = sizeof buf;
    uint8_t reqa = 0x26;

    wr(r, REG_COLL, rd(r, REG_COLL) & (uint8_t)~0x80);
    if (!transceive(r, &reqa, 1, 0x07, buf, &len) || len != 2) return false;   /* REQA, 7 bits */

    uint8_t anti[2] = { 0x93, 0x20 };
    len = sizeof buf;
    if (!transceive(r, anti, 2, 0x00, buf, &len) || len != 5) return false;     /* anti-collision CL1 */
    if ((buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4]) return false;            /* BCC check */

    snprintf(uid_out, UID_LEN, "%02X%02X%02X%02X", buf[0], buf[1], buf[2], buf[3]);
    return true;
}

bool rfid_poll(int reader, char *uid_out)
{
    if (xQueueReceive(sim_q[reader], uid_out, 0) == pdTRUE) return true;       /* serial-simulated tap */
    return hw_read(reader, uid_out);
}

void rfid_sim_push(int reader, const char *uid)
{
    char b[UID_LEN];
    strlcpy(b, uid, sizeof b);
    xQueueSend(sim_q[reader], b, 0);
}

/* Console diagnostic: 'rfidtest 0|1' - one step-by-step read attempt with raw results. */
void rfid_debug(int r)
{
    uint8_t buf[16];
    int len = sizeof buf;
    uint8_t reqa = 0x26;
    wr(r, REG_COLL, rd(r, REG_COLL) & (uint8_t)~0x80);
    bool ok = transceive(r, &reqa, 1, 0x07, buf, &len);
    printf("REQA   : ok=%d len=%d ComIrq=0x%02X Error=0x%02X", ok, len, last_irq, last_err);
    for (int i = 0; ok && i < len; i++) printf(" %02X", buf[i]);
    printf("\r\n");
    if (!ok) { printf("No answer to REQA: card not detected by the reader.\r\n"); return; }

    uint8_t anti[2] = { 0x93, 0x20 };
    len = sizeof buf;
    ok = transceive(r, anti, 2, 0x00, buf, &len);
    printf("ANTICOL: ok=%d len=%d ComIrq=0x%02X Error=0x%02X", ok, len, last_irq, last_err);
    for (int i = 0; ok && i < len; i++) printf(" %02X", buf[i]);
    printf("\r\n");
}