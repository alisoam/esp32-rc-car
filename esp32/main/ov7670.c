#include "ov7670.h"
#include "driver/i2c_master.h"
#include "esp_private/esp_cam_dvp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ov7670";

#define OV7670_I2C_ADDR  0x21
#define I2C_FREQ_HZ      100000
#define I2C_TIMEOUT_MS   100

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_dev;

typedef struct {
    uint8_t reg;
    uint8_t val;
} ov7670_reg_t;

static esp_err_t ov7670_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(i2c_dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t ov7670_write_regs(const ov7670_reg_t *regs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (regs[i].reg == 0xFF && regs[i].val == 0xFF) break;
        esp_err_t ret = ov7670_write_reg(regs[i].reg, regs[i].val);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "write reg 0x%02X=0x%02X failed: %d",
                     regs[i].reg, regs[i].val, ret);
            return ret;
        }
    }
    return ESP_OK;
}

static const ov7670_reg_t ov7670_qcif_rgb565[] = {
    {0x12, 0x80}, // COM7: Reset all registers
    {0xFF, 0xFF}, // End marker
};

static const ov7670_reg_t ov7670_config_a[] = {
    // After reset, configure for QCIF RGB565 with internal clock
    {0x12, 0x24}, // COM7: QCIF, RGB565 mode
    {0x11, 0x01}, // CLKRC: internal PCLK, no prescale
    {0x0C, 0x00}, // COM3: default, disable scaling
    {0x3E, 0x00}, // COM14: PCLK divider = 0, manual scaling
    {0x40, 0xD0}, // COM15: RGB565, full output range 00-FF
    {0xFF, 0xFF},
};

static const ov7670_reg_t ov7670_config_b[] = {
    // HREF / windowing
    {0x17, 0x13}, // HSTART
    {0x18, 0x01}, // HSTOP
    {0x19, 0x02}, // VSTART
    {0x1A, 0x7A}, // VSTOP
    {0x03, 0x0A}, // VREF
    {0x32, 0x80}, // HREF: HREF edge offset (bits 7:6=10 => to rising edge)
    {0xFF, 0xFF},
};

static const ov7670_reg_t ov7670_config_c[] = {
    // Scaling
    {0x70, 0x3A}, // SCALING_XSC
    {0x71, 0x35}, // SCALING_YSC
    {0x72, 0x11}, // SCALING_DCWCTR
    {0x73, 0xF0}, // SCALING_PCLK_DIV: bypass
    {0xA2, 0x02}, // SCALING_PCLK_DELAY
    {0xFF, 0xFF},
};

static const ov7670_reg_t ov7670_config_d[] = {
    // AGC, AEC, AWB
    {0x13, 0xE0}, // COM8: Enable AGC, AEC, AWB
    {0x00, 0x00}, // GAIN (AGC default)
    {0x10, 0x00}, // AECH
    {0x14, 0x38}, // COM9: Max AGC gain 4x, freeze AGC/AEC
    {0xFF, 0xFF},
};

static const ov7670_reg_t ov7670_config_e[] = {
    // Color matrix and quality
    {0x4F, 0x80}, // MTX1
    {0x50, 0x80}, // MTX2
    {0x51, 0x00}, // MTX3
    {0x52, 0x22}, // MTX4
    {0x53, 0x5E}, // MTX5
    {0x54, 0x80}, // MTX6
    {0x55, 0x0B}, // BRIGHTNESS
    {0x56, 0x40}, // CONTRAS (contrast center)
    {0x58, 0x9E}, // Matrix coefficient sign
    {0xFF, 0xFF},
};

static const ov7670_reg_t ov7670_config_f[] = {
    // Gamma and final
    {0x3D, 0xC0}, // COM13: Gamma enable, UV saturation auto adjust
    {0xFF, 0xFF},
};

esp_err_t ov7670_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OV7670_SDA,
        .scl_io_num = OV7670_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV7670_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev),
                        TAG, "i2c device");

    ESP_LOGI(TAG, "starting XCLK 10 MHz on GPIO %d", OV7670_XCLK);
    ESP_RETURN_ON_ERROR(
        esp_cam_ctlr_dvp_start_clock(0, OV7670_XCLK, CAM_CLK_SRC_DEFAULT, 10000000),
        TAG, "xclk start");
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t probe = i2c_master_probe(i2c_bus, OV7670_I2C_ADDR, I2C_TIMEOUT_MS);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "OV7670 not found at 0x%02X", OV7670_I2C_ADDR);
        esp_cam_ctlr_dvp_deinit(0);
        return probe;
    }
    ESP_LOGI(TAG, "OV7670 found at 0x%02X", OV7670_I2C_ADDR);

    // Reset
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_qcif_rgb565,
                        sizeof(ov7670_qcif_rgb565) / sizeof(ov7670_qcif_rgb565[0])),
                        TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configuration sequence
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_a, 4), TAG, "config a");
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_b, 6), TAG, "config b");
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_c, 5), TAG, "config c");
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_d, 4), TAG, "config d");
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_e, 9), TAG, "config e");
    ESP_RETURN_ON_ERROR(ov7670_write_regs(ov7670_config_f, 1), TAG, "config f");

    esp_cam_ctlr_dvp_deinit(0);

    ESP_LOGI(TAG, "OV7670 initialized: QCIF %dx%d RGB565", OV7670_WIDTH, OV7670_HEIGHT);
    return ESP_OK;
}
