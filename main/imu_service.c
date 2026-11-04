#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2c_master.h"

#include "bsp/esp-bsp.h"

#include "imu_service.h"

static const char *TAG = "imu";

/* ------------------------------------------------------------------
   QMI8658 register map.

   Values taken from SensorLib's REG/QMI8658Constants.h, which ships in
   this board's own Arduino examples. The 2.06 AMOLED board straps the
   chip to the LOW address (0x6B) - see examples/arduino/04_LVGL_QMI8658_ui.
   ------------------------------------------------------------------ */

#define QMI8658_ADDR            0x6B

#define REG_WHOAMI              0x00
#define REG_WHOAMI_VALUE        0x05
#define REG_CTRL1               0x02   /* serial interface / addr auto-increment */
#define REG_CTRL2               0x03   /* accel range [6:4], ODR [3:0]           */
#define REG_CTRL3               0x04   /* gyro  range [6:4], ODR [3:0]           */
#define REG_CTRL7               0x08   /* sensor enable                          */
#define REG_CTRL8               0x09
#define REG_STATUS0             0x2E
#define REG_TEMPERATURE_L       0x33
#define REG_AX_L                0x35   /* 12 bytes: AX AY AZ GX GY GZ, LE int16  */
#define REG_RESET               0x60
#define REG_RESET_VALUE         0xB0
#define REG_RST_RESULT          0x4D
#define REG_RST_RESULT_VAL      0x80

#define STATUS0_ACCEL_AVAIL     0x01
#define STATUS0_GYRO_AVAIL      0x02

#define CTRL7_ACCEL_EN          0x01
#define CTRL7_GYRO_EN           0x02

/* Range codes occupy bits [6:4] of CTRL2 / CTRL3 */
#define ACC_RANGE_4G            1      /* +/- 4 g      */
#define GYR_RANGE_512DPS        5      /* +/- 512 dps  */

/* ODR codes occupy bits [3:0]. 125 Hz / 112 Hz is far more than a 2 Hz
   UI needs, and draws less than the 1 kHz the vendor demo uses. */
#define ACC_ODR_125HZ           6
#define GYR_ODR_112HZ           6

/* Full-scale / 32768 -> units per LSB */
#define ACCEL_SCALE_G           (4.0f   / 32768.0f)
#define GYRO_SCALE_DPS          (512.0f / 32768.0f)

#define IMU_I2C_TIMEOUT_MS      1000

static i2c_master_dev_handle_t imu_dev  = NULL;
static bool                    imu_ok   = false;

/* ------------------------------------------------------------------
   Register helpers
   ------------------------------------------------------------------ */

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };

    return i2c_master_transmit(imu_dev, buf, sizeof(buf), IMU_I2C_TIMEOUT_MS);
}

static esp_err_t imu_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(
        imu_dev, &reg, 1, data, len, IMU_I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------
   Init
   ------------------------------------------------------------------ */

esp_err_t imu_service_init(void)
{
    if(imu_ok)
    {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if(bus == NULL)
    {
        ESP_LOGE(TAG, "no I2C bus handle from BSP");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMI8658_ADDR,
        .scl_speed_hz    = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &imu_dev);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Identify the chip before writing anything to it. */

    uint8_t who = 0;

    err = imu_read_regs(REG_WHOAMI, &who, 1);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "no response at 0x%02X: %s", QMI8658_ADDR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    if(who != REG_WHOAMI_VALUE)
    {
        ESP_LOGE(TAG, "WHO_AM_I = 0x%02X, expected 0x%02X", who, REG_WHOAMI_VALUE);
        return ESP_FAIL;
    }

    /* Soft reset. Datasheet allows up to 15 ms; poll the result register. */

    imu_write_reg(REG_RESET, REG_RESET_VALUE);

    bool reset_done = false;

    for(int i = 0; i < 20; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t result = 0;

        if(imu_read_regs(REG_RST_RESULT, &result, 1) == ESP_OK &&
           result == REG_RST_RESULT_VAL)
        {
            reset_done = true;
            break;
        }
    }

    if(!reset_done)
    {
        ESP_LOGW(TAG, "reset result never asserted - continuing anyway");
    }

    /* CTRL1 bit6 = ADDR_AI: registers auto-increment on burst reads, which
       is what lets us pull all 12 data bytes in one transaction. */

    err = imu_write_reg(REG_CTRL1, 0x40);
    if(err != ESP_OK) return err;

    /* CTRL8 bit7: use STATUS_INT.bit7 as the CTRL9 handshake (vendor default). */
    imu_write_reg(REG_CTRL8, 0x80);

    err = imu_write_reg(REG_CTRL2, (ACC_RANGE_4G << 4) | ACC_ODR_125HZ);
    if(err != ESP_OK) return err;

    err = imu_write_reg(REG_CTRL3, (GYR_RANGE_512DPS << 4) | GYR_ODR_112HZ);
    if(err != ESP_OK) return err;

    err = imu_write_reg(REG_CTRL7, CTRL7_ACCEL_EN | CTRL7_GYRO_EN);
    if(err != ESP_OK) return err;

    /* First conversion needs a moment to land. */
    vTaskDelay(pdMS_TO_TICKS(20));

    imu_ok = true;

    ESP_LOGI(TAG, "QMI8658 ready at 0x%02X (accel +/-4g @125Hz, gyro +/-512dps @112Hz)",
             QMI8658_ADDR);

    return ESP_OK;
}

bool imu_service_ready(void)
{
    return imu_ok;
}

/* ------------------------------------------------------------------
   Read
   ------------------------------------------------------------------ */

esp_err_t imu_service_read(imu_sample_t *out)
{
    if(out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!imu_ok)
    {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }

    /* 12 bytes: AX_L..GZ_H, little-endian signed 16-bit per axis. */

    uint8_t raw[12];

    esp_err_t err = imu_read_regs(REG_AX_L, raw, sizeof(raw));

    if(err != ESP_OK)
    {
        return err;
    }

    int16_t v[6];

    for(int i = 0; i < 6; i++)
    {
        v[i] = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
    }

    out->accel_g[0]  = v[0] * ACCEL_SCALE_G;
    out->accel_g[1]  = v[1] * ACCEL_SCALE_G;
    out->accel_g[2]  = v[2] * ACCEL_SCALE_G;

    out->gyro_dps[0] = v[3] * GYRO_SCALE_DPS;
    out->gyro_dps[1] = v[4] * GYRO_SCALE_DPS;
    out->gyro_dps[2] = v[5] * GYRO_SCALE_DPS;

    /* Temperature: low byte is the fraction, high byte the integer part. */

    uint8_t t[2];

    if(imu_read_regs(REG_TEMPERATURE_L, t, 2) == ESP_OK)
    {
        out->temperature_c = (float)t[1] + ((float)t[0] / 256.0f);
    }
    else
    {
        out->temperature_c = 0.0f;
    }

    return ESP_OK;
}
