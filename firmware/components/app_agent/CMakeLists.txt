# The panel's voice agent — the ESP-Claw/Mosaico pattern scoped to this
# product: utterance -> Whisper (external STT) -> an OpenAI-compatible chat
# endpoint with TOOLS (set_temperature / set_all / get_status / run_lua) and a
# capability manifest built from the live room table -> tool execution here,
# Lua included. app_logic supplies the heating API; lua is the vendored 5.4.8
# in ../third_party/lua.
idf_component_register(
    SRCS "app_agent.cpp" "agent_lua.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_console esp_http_client esp-tls mbedtls espressif__cjson
                  nvs_flash lua esp_timer console lwip
)
