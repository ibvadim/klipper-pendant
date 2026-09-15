
#include <stdio.h>
#include <stdlib.h>

#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_st7796u.h"

static const char *TAG = "st7796u";

static esp_err_t panel_st7796u_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796u_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796u_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7796u_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7796u_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7796u_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7796u_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7796u_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7796u_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const st7796u_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} st7796u_panel_t;

esp_err_t esp_lcd_new_panel_st7796u(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7796u_panel_t *st7796u = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7796u = (st7796u_panel_t *)calloc(1, sizeof(st7796u_panel_t));
    ESP_GOTO_ON_FALSE(st7796u, ESP_ERR_NO_MEM, err, TAG, "no mem for st7796u panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
        switch (panel_dev_config->color_space) {
        case ESP_LCD_COLOR_SPACE_RGB:
            st7796u->madctl_val = 0;
            break;
        case ESP_LCD_COLOR_SPACE_BGR:
            st7796u->madctl_val |= LCD_CMD_BGR_BIT;
            break;
        default:
            ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
            break;
    }
    #else
        switch (panel_dev_config->rgb_endian) {
        case LCD_RGB_ENDIAN_RGB:
            printf("LCD_RGB_ENDIAN_RGB\r\n");
            st7796u->madctl_val = 0;
            break;
        case LCD_RGB_ENDIAN_BGR:
            printf("LCD_RGB_ENDIAN_BGR\r\n");
            st7796u->madctl_val |= LCD_CMD_BGR_BIT;
            break;
        default:
            ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
            break;
        }
    #endif


    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7796u->colmod_val = 0x55;
        st7796u->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st7796u->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        st7796u->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7796u->io = io;
    st7796u->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7796u->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        st7796u->init_cmds = ((st7796u_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        st7796u->init_cmds_size = ((st7796u_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
    }
    st7796u->base.del = panel_st7796u_del;
    st7796u->base.reset = panel_st7796u_reset;
    st7796u->base.init = panel_st7796u_init;
    st7796u->base.draw_bitmap = panel_st7796u_draw_bitmap;
    st7796u->base.invert_color = panel_st7796u_invert_color;
    st7796u->base.set_gap = panel_st7796u_set_gap;
    st7796u->base.mirror = panel_st7796u_mirror;
    st7796u->base.swap_xy = panel_st7796u_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    st7796u->base.disp_off = panel_st7796u_disp_on_off;
#else
    st7796u->base.disp_on_off = panel_st7796u_disp_on_off;
#endif
    *ret_panel = &(st7796u->base);
    ESP_LOGD(TAG, "new st7796u panel @%p", st7796u);

    // ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_st7796u_VER_MAJOR, ESP_LCD_st7796u_VER_MINOR,
    //          ESP_LCD_st7796u_VER_PATCH);

    return ESP_OK;

err:
    if (st7796u) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7796u);
    }
    return ret;
}

static esp_err_t panel_st7796u_del(esp_lcd_panel_t *panel)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);

    if (st7796u->reset_gpio_num >= 0) {
        gpio_reset_pin(st7796u->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7796u panel @%p", st7796u);
    free(st7796u);
    return ESP_OK;
}

static esp_err_t panel_st7796u_reset(esp_lcd_panel_t *panel)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;

    // perform hardware reset
    if (st7796u->reset_gpio_num >= 0) {
        gpio_set_level(st7796u->reset_gpio_num, st7796u->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7796u->reset_gpio_num, !st7796u->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_LOGD(TAG, "RST \r\n");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5ms before sending new command
    }
    return ESP_OK;
}

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes; // Length of data in above data array; 0xFF = end of cmds.
} lcd_init_cmd_t;



static const st7796u_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    /* Power contorl B, power control = 0, DC_ENA = 1 */
    {0x11, (uint8_t []){ 0x00 }, 0, 120},

    // {0x36, (uint8_t []){ 0x08 }, 1, 0},

    {0x3A, (uint8_t []){ 0x05 }, 1, 0},
    {0xF0, (uint8_t []){ 0xC3 }, 1, 0},
    {0xF0, (uint8_t []){ 0x96 }, 1, 0},
    {0xB4, (uint8_t []){ 0x01 }, 1, 0},
    {0xB7, (uint8_t []){ 0xC6 }, 1, 0},
    {0xC0, (uint8_t []){ 0x80, 0x45 }, 2, 0},
    {0xC1, (uint8_t []){ 0x13 }, 1, 0},
    {0xC2, (uint8_t []){ 0xA7 }, 1, 0},
    {0xC5, (uint8_t []){ 0x0A }, 1, 0},
    {0xE8, (uint8_t []){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (uint8_t []){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, (uint8_t []){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32},14, 0},
    {0xF0, (uint8_t []){ 0x3C }, 1, 0},
    {0xF0, (uint8_t []){ 0x69 }, 1, 120},
    {0x21, (uint8_t []){ 0x00 }, 0, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},

};





static esp_err_t panel_st7796u_init(esp_lcd_panel_t *panel)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;
    printf("st7796u->madctl_val  = %d\r\n",st7796u->madctl_val);
    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7796u->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7796u->colmod_val,
    }, 1), TAG, "send command failed");

    const st7796u_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    if (st7796u->init_cmds) {
        init_cmds = st7796u->init_cmds;
        init_cmds_size = st7796u->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7796u_lcd_init_cmd_t);
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            st7796u->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            st7796u->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_st7796u_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st7796u->io;

    x_start += st7796u->x_gap;
    x_end += st7796u->x_gap;
    y_start += st7796u->y_gap;
    y_end += st7796u->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7796u->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_st7796u_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7796u_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;
    if (mirror_x) {
        st7796u->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7796u->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7796u->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7796u->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7796u->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7796u_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;
    if (swap_axes) {
        st7796u->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7796u->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7796u->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7796u_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    st7796u->x_gap = x_gap;
    st7796u->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7796u_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7796u_panel_t *st7796u = __containerof(panel, st7796u_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7796u->io;
    int command = 0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
