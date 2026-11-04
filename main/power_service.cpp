#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "bsp/esp-bsp.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "power_service.h"

static const char *TAG = "power";

static XPowersPMU            PMU;
static i2c_master_dev_handle_t pmu_dev   = NULL;
static bool                    pmu_ready = false;

#define PMU_I2C_TIMEOUT_MS 1000

/* ------------------------------------------------------------------
   I2C glue: XPowersLib is transport-agnostic and calls these two
   callbacks. We ride on the same I2C bus the BSP already created for
   the touch controller, so no second driver instance is needed.
   ------------------------------------------------------------------ */

static int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    (void)devAddr;

    esp_err_t ret = i2c_master_transmit_receive(
        pmu_dev, &regAddr, 1, data, len, PMU_I2C_TIMEOUT_MS);

    return (ret == ESP_OK) ? 0 : -1;
}

static int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    (void)devAddr;

    uint8_t buffer[16];

    if (len + 1 > (uint8_t)sizeof(buffer))
    {
        return -1;
    }

    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(
        pmu_dev, buffer, len + 1, PMU_I2C_TIMEOUT_MS);

    return (ret == ESP_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------
   Public interface
   ------------------------------------------------------------------ */

extern "C" esp_err_t power_service_init(void)
{
    if (pmu_ready)
    {
        return ESP_OK;
    }

    /* bsp_i2c_get_handle() initialises the bus on first call and is
       idempotent, so this is safe whether or not the display started. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (bus == NULL)
    {
        ESP_LOGE(TAG, "no I2C bus handle from BSP");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = AXP2101_SLAVE_ADDRESS;   /* 0x34 */
    dev_cfg.scl_speed_hz    = 400000;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &pmu_dev);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte))
    {
        ESP_LOGE(TAG, "AXP2101 did not respond");
        return ESP_FAIL;
    }

    /* Turn on the ADC channels we actually read. */
    PMU.enableBattVoltageMeasure();
    PMU.enableVbusVoltageMeasure();
    PMU.enableSystemVoltageMeasure();

    /* This board has no battery thermistor on the TS pin. Leaving the
       measurement enabled makes the charger behave erratically, so the
       vendor port disables it — we do the same. */
    PMU.disableTSPinMeasure();

    PMU.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
    PMU.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    PMU.clearIrqStatus();

    pmu_ready = true;

    ESP_LOGI(TAG, "AXP2101 ready: %d%%, %u mV, battery %s",
             PMU.getBatteryPercent(),
             PMU.getBattVoltage(),
             PMU.isBatteryConnect() ? "present" : "absent");

    return ESP_OK;
}

extern "C" esp_err_t power_service_read(power_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!pmu_ready)
    {
        out->present       = false;
        out->percent       = -1;
        out->voltage_mv    = 0;
        out->charging      = false;
        out->usb_connected = false;
        return ESP_ERR_INVALID_STATE;
    }

    out->present       = PMU.isBatteryConnect();
    out->percent       = out->present ? PMU.getBatteryPercent() : -1;
    out->voltage_mv    = out->present ? PMU.getBattVoltage() : 0;
    out->charging      = PMU.isCharging();
    out->usb_connected = PMU.isVbusIn();

    return ESP_OK;
}