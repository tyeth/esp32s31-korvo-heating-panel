#include "app_display.h"
#include "app_driver.h"
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>
// The emitted display_setup_task uses the FreeRTOS task API directly, so this
// file must include it itself. It used to arrive transitively via
// esp_lvgl_port.h, which is exactly the include-by-luck that broke seven
// sensor blocks when driver/i2c.h went away — a panel whose own includes do
// not pull FreeRTOS (the MIPI EK79007 does not) then fails on xTaskCreate.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>
#include <string.h>
#include "esp_board_manager.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"

static const char *TAG = "app_display";

static app_driver_handle_t s_handle = 0;

/* Assigned by the panel driver block's display_node_init slot. */
static lv_display_t *s_disp = NULL;



/* Hardware/param bus -> widgets. Runs OFF the LVGL task, so the whole
 * dispatch holds the LVGL mutex (0 = wait forever). Slot cases must NOT
 * lock again. */
static void display_driver_cb(
    app_driver_param_id_t param_id,
    app_driver_param_val_t val,
    app_driver_handle_t source,
    void *ctx)
{
    (void)source;
    (void)ctx;
    (void)val;
    if (!lvgl_port_lock(0)) return;
    switch (param_id) {

    default:
        break;
    }
    lvgl_port_unlock();
}



/* Panel/touch bring-up + widget tree, in a task so it can NEVER block boot:
 * SPI panel init hangs forever on the emulator (and can stall on real
 * miswired hardware) — boot must still reach "ZeroCode AI firmware started"
 * or functional_testing times out on healthy firmware. Same degraded-state
 * philosophy as the camera driver. */
static void display_setup_task(void *arg)
{
    (void)arg;
    /* LVGL port init lives HERE too — under the emulator it never returns
     * (no SPI/LCD to drive), and it must not take app_main down with it. */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed — display disabled");
        vTaskDelete(NULL);
        return;
    }

    /* Panel (and optional touch) hardware — instance blocks, in product.yml
     * order. The panel's slot assigns s_disp. */
    {
        /* The 'esp32_s31_korvo_1' board defines this panel ('display_lcd'), so
         * esp_board_manager_init() has already brought it up — bus, panel IO,
         * reset, mirror/swap and disp_on. All that is left is to hand LVGL the
         * handles. No pins appear here on purpose: they are the board's. */
        void *board_lcd = NULL;
        dev_display_lcd_config_t *board_lcd_cfg = NULL;
        if (esp_board_manager_get_device_handle("display_lcd", &board_lcd) != ESP_OK ||
            esp_board_manager_get_device_config("display_lcd", (void **)&board_lcd_cfg) != ESP_OK ||
            board_lcd == NULL || board_lcd_cfg == NULL) {
            ESP_LOGE(TAG, "board 'esp32_s31_korvo_1' declares 'display_lcd' but it has no handle");
        } else {
            dev_display_lcd_handles_t *board_lcd_h = (dev_display_lcd_handles_t *)board_lcd;
            /* Serial-ish panels (SPI/QSPI, i80, parlio) are pushed pixel by pixel
             * out of an LVGL buffer, so they need a DMA-capable partial buffer and
             * the RGB565 byte swap. RGB and DSI panels own their frame buffer and
             * need neither. */
            const bool board_lcd_serial =
                strcmp(board_lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0 ||
                strcmp(board_lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_I80) == 0 ||
                strcmp(board_lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_PARLIO) == 0;
            lvgl_port_display_cfg_t board_disp_cfg = {};
            board_disp_cfg.io_handle = board_lcd_h->io_handle;
            board_disp_cfg.panel_handle = board_lcd_h->panel_handle;
            board_disp_cfg.buffer_size = (uint32_t)board_lcd_cfg->lcd_width * 40;
            board_disp_cfg.double_buffer = true;
            board_disp_cfg.hres = board_lcd_cfg->lcd_width;
            board_disp_cfg.vres = board_lcd_cfg->lcd_height;
            board_disp_cfg.color_format =
                (board_lcd_cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_RGB888 ||
                 board_lcd_cfg->frame_format == DEV_DISPLAY_LCD_FRAME_FORMAT_BGR888)
                    ? LV_COLOR_FORMAT_RGB888
                    : LV_COLOR_FORMAT_RGB565;
            board_disp_cfg.flags.buff_dma = board_lcd_serial ? 1 : 0;
            board_disp_cfg.flags.swap_bytes = board_lcd_serial ? 1 : 0;
            /* An RGB panel owns its frame buffer in the LCD peripheral and has
             * no panel IO: handing it to the generic add-disp path trips its
             * assert(io_handle != NULL) and aborts the whole chip. The port has
             * a dedicated entry for it. */
            const bool board_lcd_rgb =
                strcmp(board_lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0;
            if (board_lcd_rgb) {
                lvgl_port_display_rgb_cfg_t board_rgb_cfg = {};
                board_rgb_cfg.flags.bb_mode = 0;
                board_rgb_cfg.flags.avoid_tearing = 0;
                s_disp = lvgl_port_add_disp_rgb(&board_disp_cfg, &board_rgb_cfg);
            } else {
                s_disp = lvgl_port_add_disp(&board_disp_cfg);
            }
            ESP_LOGI(TAG, "Panel 'display_lcd' from board 'esp32_s31_korvo_1': %s %s %dx%d%s",
                     board_lcd_cfg->chip, board_lcd_cfg->sub_type,
                     board_lcd_cfg->lcd_width, board_lcd_cfg->lcd_height,
                     s_disp ? "" : " (not registered)");
        }
    }

    {
        /* Touch from the same board definition ('lcd_touch'). Registered
         * against the panel above, so a panel that failed to bind takes the touch
         * with it rather than registering an input device pointing at nothing. */
        void *board_touch = NULL;
        if (s_disp == NULL) {
            ESP_LOGW(TAG, "no display — skipping board touch 'lcd_touch'");
        } else if (esp_board_manager_get_device_handle("lcd_touch", &board_touch) != ESP_OK ||
                   board_touch == NULL) {
            ESP_LOGE(TAG, "board 'esp32_s31_korvo_1' declares 'lcd_touch' but it has no handle");
        } else {
            dev_lcd_touch_handles_t *board_touch_h = (dev_lcd_touch_handles_t *)board_touch;
            lvgl_port_touch_cfg_t board_touch_cfg = {};
            board_touch_cfg.disp = s_disp;
            board_touch_cfg.handle = board_touch_h->touch_handle;
            if (lvgl_port_add_touch(&board_touch_cfg) == NULL) {
                ESP_LOGE(TAG, "board touch 'lcd_touch': registration failed");
            } else {
                ESP_LOGI(TAG, "Touch 'lcd_touch' from board 'esp32_s31_korvo_1'");
            }
        }
    }

    if (s_disp == NULL) {
        ESP_LOGE(TAG, "no display panel block in this product — add drivers/display_ili9341_spi");
        vTaskDelete(NULL);
        return;
    }

    /* No device cards: the screen is the product's own. A full-screen column
     * here (opaque under LVGL's light theme) would cover a product that draws
     * its own UI. */

    /* Register only once widgets exist — driver_cb cases null-check anyway. */
    s_handle = app_driver_register_solution("display", display_driver_cb, NULL);

    ESP_LOGI(TAG, "Display initialized");
    vTaskDelete(NULL);
}

esp_err_t app_display_init(void)
{
    xTaskCreate(display_setup_task, "display_setup", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Display setup started");
    return ESP_OK;
}
