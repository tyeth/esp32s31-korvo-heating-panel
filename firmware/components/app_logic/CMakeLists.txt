# ESP-IDF 6 emptied the legacy `driver` component: it now carries ONLY i2c,
# touch_sensor and twai. gpio, ledc, i2s, spi, pcnt, rmt, uart and gptimer all
# became separate `esp_driver_*` components that `driver` no longer re-exports.
#
# They are listed explicitly here, rather than left to each block's
# requires.cmake, to keep the AGENT-FACING contract identical to what it was on
# IDF 5: a generated tree where `#include <driver/ledc.h>` simply works. The
# agents write C at runtime against these headers, and making each one a CMake
# dependency puzzle would spend their budget debugging the build system instead
# of the product. `driver` stays for the legacy i2c the sensor blocks use.
#
# WHOLE_ARCHIVE: blocks define WEAK hooks in the generated control.cpp
# (board_button's <prefix>_on_double_click, …) that the product overrides with
# a strong definition in a file of its own. Linked as an ordinary archive, a
# member that holds ONLY such an override is never pulled in — control.o
# already satisfies the reference — so the override links cleanly and silently
# does nothing. Whole-archive links every file the product put here.
idf_component_register(
    SRCS "app_logic.cpp" "button_registry.cpp" "control.cpp" "diagnostics.cpp" "console.cpp" "zc_behaviors.cpp"
         "heating.cpp" "heating_voice.cpp" "heating_ui.cpp" "heating_web.cpp"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    PRIV_REQUIRES app_driver app_config app_utils app_mqtt app_webui app_display app_console
                  nvs_flash esp_timer espressif__button espressif__esp_board_manager
                  espressif__esp_lcd_touch espressif__esp-sr lvgl__lvgl espressif__esp_lvgl_port
                  esp_netif lwip console esp_hw_support spi_flash esp_psram esp_app_format
                  driver esp_driver_gpio esp_driver_ledc esp_driver_i2s esp_driver_spi
                  esp_driver_pcnt esp_driver_rmt esp_driver_uart esp_driver_gptimer
)
