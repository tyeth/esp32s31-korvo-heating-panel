idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_driver app_logic app_config app_console app_utils app_audio app_display app_mqtt app_webui espressif__esp_board_manager
)
# Protocol components (app_matter / app_rainmaker / app_zigbee / app_ble_mesh)
# are added per-product via cmake_priv_requires.main from each protocol block.
