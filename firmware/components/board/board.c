#include "board.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796u.h"
#include "esp_lcd_touch_cst826.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_sleep.h"
#include "exio_pcf8574.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define I2C_SCL GPIO_NUM_7
#define I2C_SDA GPIO_NUM_8
#define LCD_H_RES 320
#define LCD_V_RES 480
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_5
#define LCD_MOSI GPIO_NUM_1
#define LCD_DC GPIO_NUM_3
#define LCD_CS GPIO_NUM_2
#define LCD_BL GPIO_NUM_6
#define LCD_DRAW_BUFFER_LINES 40
#define LCD_DRAW_BUFFER_PIXELS (LCD_H_RES * LCD_DRAW_BUFFER_LINES)
#define LCD_DRAW_BUFFER_BYTES (LCD_DRAW_BUFFER_PIXELS * sizeof(uint16_t))
/* Vendor 08_battery_test: R31=1.0 Mohm, R32=2.2 Mohm. */
#define BATTERY_UPPER_OHM 1050000U
#define BATTERY_LOWER_OHM 2150000U
#define BATTERY_PRESENT_MIN_MV 3000U
/* With USB attached and no cell at BAT, IP4054 leaves VBAT at about 4.29 V.
 * A single-cell Li-ion pack is not valid above this margin over its 4.2 V
 * regulation voltage.  CHG_N is low both after charge termination and with
 * no pack, so use this only together with the voltage limit. */
#define BATTERY_FLOATING_USB_MIN_MV 4250U
#define BATTERY_SHUTDOWN_MV 3300U
#define BATTERY_PLAUSIBLE_MIN_MV 2000U
#define BATTERY_SHUTDOWN_SAMPLES 10U
#define BATTERY_RECHECK_SECONDS 30U
/* The battery divider has a high source impedance.  Match the vendor battery
 * service by averaging a small burst instead of trusting one ADC conversion. */
#define BATTERY_ADC_SAMPLES 12U

static const char *TAG = "board";
static i2c_master_bus_handle_t i2c_bus;
static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static esp_lcd_touch_handle_t touch;
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_cali;
static uint8_t backlight_percent = 100;
static volatile board_battery_status_t battery_status = { .charge_state = BOARD_BATTERY_UNAVAILABLE };

static uint8_t battery_percent(uint16_t mv)
{
    static const struct { uint16_t mv; uint8_t percent; } points[] = {
        {3300, 0}, {3600, 10}, {3700, 20}, {3800, 40}, {3900, 60}, {4000, 80}, {4100, 90}, {4200, 100},
    };
    if (mv <= points[0].mv) return 0;
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); i++) {
        if (mv <= points[i].mv) {
            uint32_t span = points[i].mv - points[i - 1].mv;
            return points[i - 1].percent + ((mv - points[i - 1].mv) *
                (points[i].percent - points[i - 1].percent) + span / 2) / span;
        }
    }
    return 100;
}

static esp_err_t sample_battery(board_battery_status_t *sample)
{
    int raw, adc_mv;
    int64_t adc_mv_sum = 0;
    exio_pcf8574_level chg_n = EXIO_PCF8574_LEVEL_L;
    for (unsigned i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(battery_adc, ADC_CHANNEL_3, &raw), TAG, "ADC read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(battery_cali, raw, &adc_mv), TAG, "ADC calibration");
        adc_mv_sum += adc_mv;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    adc_mv = (adc_mv_sum + BATTERY_ADC_SAMPLES / 2) / BATTERY_ADC_SAMPLES;
    /* A normal ~2.8 V ADC reading multiplied by the 3.2 Mohm divider sum
     * exceeds UINT32_MAX.  Keep the intermediate in 64 bits. */
    uint64_t scaled_mv = (uint64_t)adc_mv * (BATTERY_UPPER_OHM + BATTERY_LOWER_OHM);
    uint32_t voltage = (scaled_mv + BATTERY_LOWER_OHM / 2) / BATTERY_LOWER_OHM;
    sample->voltage_mv = voltage > UINT16_MAX ? UINT16_MAX : voltage;
    sample->percent = battery_percent(sample->voltage_mv);
    /* Charge detection is useful, but an I2C read failure must not hide a valid
     * battery voltage/percentage from the header. */
    esp_err_t chg_err = exio_pcf8574_pin_read(EXIO_PCF8574_PIN_CHG_N, &chg_n);
    if (chg_err == ESP_OK) {
        /* The vendor battery example documents CHG_N high as charging. */
        sample->charge_state = chg_n == EXIO_PCF8574_LEVEL_H ?
            BOARD_BATTERY_CHARGING : BOARD_BATTERY_DISCHARGING;
    } else {
        sample->charge_state = BOARD_BATTERY_UNAVAILABLE;
        ESP_LOGW(TAG, "CHG_N read failed: %s", esp_err_to_name(chg_err));
    }
    /* A protected single-cell Li-ion pack cannot normally remain below 3.0 V.
     * Do this after CHG_N is read: the USB-powered, floating BAT node must not
     * be rendered as a 100% battery when no cell is connected. */
    sample->present = sample->voltage_mv >= BATTERY_PRESENT_MIN_MV &&
                      !(sample->charge_state == BOARD_BATTERY_DISCHARGING &&
                        sample->voltage_mv >= BATTERY_FLOATING_USB_MIN_MV);
    sample->valid = true;
    return ESP_OK;
}

static void battery_task(void *argument)
{
    (void)argument;
    unsigned critical_samples = 0;
    while (true) {
        board_battery_status_t sample = { .charge_state = BOARD_BATTERY_UNAVAILABLE };
        if (sample_battery(&sample) == ESP_OK) {
            battery_status = sample;
            const char *charge = sample.charge_state == BOARD_BATTERY_CHARGING ? "charging" :
                                 sample.charge_state == BOARD_BATTERY_DISCHARGING ? "discharging" : "charge state unavailable";
            ESP_LOGI(TAG, "battery: %u mV, %s, %s", sample.voltage_mv,
                     sample.present ? "present" : "not present", charge);

            /* Require several consecutive low readings so a short load spike
             * cannot shut the controller down.  Never sleep while the charger
             * reports that USB power is replenishing the cell. */
            bool critical = sample.charge_state == BOARD_BATTERY_DISCHARGING &&
                            sample.voltage_mv >= BATTERY_PLAUSIBLE_MIN_MV &&
                            sample.voltage_mv <= BATTERY_SHUTDOWN_MV;
            critical_samples = critical ? critical_samples + 1 : 0;
            if (critical_samples >= BATTERY_SHUTDOWN_SAMPLES) {
                ESP_LOGW(TAG, "battery critically low (%u mV); entering deep sleep", sample.voltage_mv);
                board_set_backlight(0);
                esp_lcd_panel_disp_on_off(lcd_panel, false);
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(BATTERY_RECHECK_SECONDS * 1000000ULL));
                esp_deep_sleep_start();
            }
        } else battery_status = sample;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t config = {.clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_SCL, .sda_io_num = I2C_SDA, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &i2c_bus), TAG, "I2C init");
    ESP_RETURN_ON_ERROR(exio_pcf8574_init(), TAG, "PCF8574 init");
    return exio_pcf8574_pin_write(EXIO_PCF8574_PIN_CHG_N, EXIO_PCF8574_LEVEL_H);
}

static esp_err_t init_lcd(void)
{
    const spi_bus_config_t bus = {.sclk_io_num = LCD_SCLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_DRAW_BUFFER_BYTES};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI init");
    const esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS,
        .pclk_hz = 80000000, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .trans_queue_depth = 5};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &lcd_io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796u(lcd_io, &panel_cfg, &lcd_panel), TAG, "LCD panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel, false), TAG, "LCD invert");
    return esp_lcd_panel_disp_on_off(lcd_panel, true);
}

static esp_err_t init_touch_lvgl(void)
{
    esp_lcd_panel_io_handle_t touch_io;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST826_ADDRESS,
        .control_phase_bytes = 1, .lcd_cmd_bits = 8, .flags.disable_control_phase = 1, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &touch_io), TAG, "touch IO");
    const esp_lcd_touch_config_t touch_cfg = {.x_max = LCD_H_RES, .y_max = LCD_V_RES, .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC};
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst826(touch_io, &touch_cfg, &touch), TAG, "touch init");
    /* esp_lvgl_port does not apply display_cfg.rotation to touch coordinates.
     * The display is rotated 180° from the former calibrated layout, so rotate
     * CST826 coordinates explicitly before its LVGL input device is registered. */
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_x(touch, true), TAG, "touch mirror X");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_y(touch, true), TAG, "touch mirror Y");
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL init");
    const lvgl_port_display_cfg_t display_cfg = {.io_handle = lcd_io, .panel_handle = lcd_panel,
        /* Two small internal-RAM DMA buffers keep every SPI transaction within
         * max_transfer_sz.  A full-screen PSRAM canvas made page transitions
         * issue 307200-byte transfers and could leave LVGL waiting forever
         * after the SPI driver rejected them. */
        .buffer_size = LCD_DRAW_BUFFER_PIXELS, .double_buffer = true,
        .hres = LCD_H_RES, .vres = LCD_V_RES,
        /* A 180-degree orientation is supported by the controller; doing it in
         * hardware avoids rotating every dirty region on the CPU. */
        .rotation = {.mirror_y = true},
        .flags = {.buff_dma = true, .sw_rotate = false}};
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (display == NULL) return ESP_FAIL;
    const lvgl_port_touch_cfg_t input_cfg = {.disp = display, .handle = touch};
    return lvgl_port_add_touch(&input_cfg) == NULL ? ESP_FAIL : ESP_OK;
}

static esp_err_t init_backlight(void)
{
    const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 10000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {.gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_1, .duty = 1023};
    return ledc_channel_config(&channel);
}

static esp_err_t init_battery(void)
{
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &battery_adc), TAG, "ADC unit");
    const adc_oneshot_chan_cfg_t channel = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(battery_adc, ADC_CHANNEL_3, &channel), TAG, "ADC channel");
    const adc_cali_curve_fitting_config_t cali = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali, &battery_cali), TAG, "ADC calibration");
    return xTaskCreate(battery_task, "battery", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C board setup");
    ESP_RETURN_ON_ERROR(init_lcd(), TAG, "LCD board setup");
    ESP_RETURN_ON_ERROR(init_touch_lvgl(), TAG, "touch/LVGL board setup");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight setup");
    return init_battery();
}

void board_battery_get_status(board_battery_status_t *status)
{
    if (status != NULL) *status = battery_status;
}

esp_err_t board_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    const uint32_t duty = (percent * 1023U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "backlight duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "backlight update");
    backlight_percent = percent;
    return ESP_OK;
}

uint8_t board_get_backlight(void)
{
    return backlight_percent;
}
