#include "bsp.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

/* I2C0: touch / PCA9557 / IMU share this bus */
#define BSP_I2C_NUM 0
#define BSP_I2C_SDA GPIO_NUM_1
#define BSP_I2C_SCL GPIO_NUM_2
#define BSP_I2C_FREQ_HZ 100000

/* PCA9557 IO expander */
#define PCA9557_ADDR 0x19
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_CONFIG 0x03
#define PCA9557_BIT_LCD_CS 0

/* LCD */
#define BSP_LCD_SPI_NUM SPI3_HOST
#define BSP_LCD_MOSI GPIO_NUM_40
#define BSP_LCD_CLK GPIO_NUM_41
#define BSP_LCD_DC GPIO_NUM_39
#define BSP_LCD_BACKLIGHT GPIO_NUM_42
#define BSP_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BSP_LCD_DRAW_BUF_LINES 40
#define BSP_LCD_LEDC_CH LEDC_CHANNEL_0

static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_disp_t *s_disp;
static int s_brightness;

static esp_err_t i2c_bus_init(void)
{
    const i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(BSP_I2C_NUM, &cfg), TAG, "i2c param");
    ESP_RETURN_ON_ERROR(i2c_driver_install(BSP_I2C_NUM, cfg.mode, 0, 0, 0), TAG, "i2c install");
    return ESP_OK;
}

static esp_err_t pca9557_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_write_to_device(BSP_I2C_NUM, PCA9557_ADDR, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t lcd_cs_set(int level)
{
    uint8_t out = 0;
    ESP_RETURN_ON_ERROR(i2c_master_write_read_device(BSP_I2C_NUM, PCA9557_ADDR,
                                                     (const uint8_t[]){PCA9557_REG_OUTPUT}, 1,
                                                     &out, 1, pdMS_TO_TICKS(100)),
                        TAG, "pca9557 read");
    if (level) {
        out |= (1u << PCA9557_BIT_LCD_CS);
    } else {
        out &= ~(1u << PCA9557_BIT_LCD_CS);
    }
    return pca9557_write(PCA9557_REG_OUTPUT, out);
}

static esp_err_t pca9557_init(void)
{
    /* DVP_PWDN=1, PA_EN=0, LCD_CS=1; low 3 pins output, rest input */
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, 0x05), TAG, "pca9557 output");
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, 0xF8), TAG, "pca9557 config");
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true, /* backlight is active-low */
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "ledc channel");
    s_brightness = 0;
    return ESP_OK;
}

esp_err_t bsp_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (1023u * (uint32_t)percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CH, duty),
                        TAG, "ledc duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CH),
                        TAG, "ledc update");
    s_brightness = percent;
    return ESP_OK;
}

int bsp_backlight_get(void)
{
    return s_brightness;
}

/* GPIO0 user key: active-low, polled with 3-sample debounce */
#define BSP_USER_BUTTON GPIO_NUM_0
#define BSP_USER_BUTTON_POLL_MS 20
#define BSP_USER_BUTTON_STABLE_POLLS 3
#define BSP_USER_BUTTON_LONG_MS 800

static void user_button_task(void *arg)
{
    bsp_button_cb_t cb = (bsp_button_cb_t)arg;
    bool pressed = false;
    bool long_sent = false;
    bool raw_last = false;
    int stable_cnt = 0;
    TickType_t pressed_at = 0;

    for (;;) {
        bool raw = gpio_get_level(BSP_USER_BUTTON) == 0;

        if (raw != raw_last) {
            raw_last = raw;
            stable_cnt = 0;
        } else if (stable_cnt < BSP_USER_BUTTON_STABLE_POLLS) {
            stable_cnt++;
        }

        if (stable_cnt >= BSP_USER_BUTTON_STABLE_POLLS) {
            if (raw && !pressed) {
                pressed = true;
                long_sent = false;
                pressed_at = xTaskGetTickCount();
            } else if (!raw && pressed) {
                pressed = false;
                if (!long_sent && cb) {
                    cb(BSP_BUTTON_SHORT);
                }
            } else if (raw && pressed && !long_sent &&
                       (xTaskGetTickCount() - pressed_at) * portTICK_PERIOD_MS >=
                           BSP_USER_BUTTON_LONG_MS) {
                long_sent = true;
                if (cb) {
                    cb(BSP_BUTTON_LONG);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BSP_USER_BUTTON_POLL_MS));
    }
}

esp_err_t bsp_user_button_init(bsp_button_cb_t cb)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_USER_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "user button gpio");

    BaseType_t ok = xTaskCreate(user_button_task, "user_btn", 4096, (void *)cb, 2, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "user button task");
    return ESP_OK;
}

static esp_err_t lcd_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = BSP_LCD_CLK,
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = GPIO_NUM_NC, /* CS is on PCA9557 */
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM,
                                                 &io_cfg, &s_lcd_io),
                        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_cfg, &s_panel),
                        TAG, "panel");

    /* order matters: reset -> CS low -> init */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(lcd_cs_set(0), TAG, "lcd cs");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return ESP_OK;
}

static esp_err_t lvgl_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8 * 1024;
    port_cfg.task_priority = 4;
    port_cfg.task_affinity = 1;
    port_cfg.task_max_sleep_ms = 100;
    port_cfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_panel,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,    /* internal DMA RAM */
            .buff_spiram = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "add display");
    return ESP_OK;
}

static esp_err_t touch_init(void)
{
    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    esp_lcd_panel_io_handle_t touch_io = NULL;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM,
                                                 &touch_io_cfg, &touch_io),
                        TAG, "touch io");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch_cfg, &s_touch),
                        TAG, "touch");

    const lvgl_port_touch_cfg_t lv_touch_cfg = {
        .disp = s_disp,
        .handle = s_touch,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&lv_touch_cfg), ESP_FAIL, TAG, "add touch");
    return ESP_OK;
}

esp_err_t bsp_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(pca9557_init(), TAG, "pca9557");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");

    ESP_LOGI(TAG, "display ready (320x240, FT6336)");
    return ESP_OK;
}
