# Keep this in step with the app_audio CMakeLists the engine emits
# (engine/src/generator.ts). The engine overwrites app_audio.cpp with the real
# implementation UNCONDITIONALLY but only rewrites this CMakeLists when the
# product has device types — so for a framework-only product THIS file is what
# the generated implementation is compiled against, and it cannot see esp-sr's
# WakeNet/MultiNet headers or esp_srmodel_init() without the dependency below.
#
# app_dsp_shim provides the generic dsps_dotprod_f32_arp4 the prebuilt voice
# libraries call (esp-dsp's assembly version faults on this target). It is
# linked WHOLE_ARCHIVE so that definition is in the image whatever order the
# archives are searched in, and app_audio.cpp references it below, so the
# dependency is real rather than a declaration only.
#
# app_logic is the product's vocabulary: app_audio.cpp calls its
# heating_voice_register() / heating_voice_apply_command() /
# heating_voice_on_event() hooks from the command-registration, recognised-
# phrase and listening-state slots the engine leaves empty.
# The voice-test sample is the developer's own recording and is NOT in the
# repository (.gitignore): a fresh checkout builds with an empty stand-in, and
# `voice-test` then reports that no sample is embedded. Drop a 16 kHz mono
# 16-bit little-endian PCM at components/app_audio/voice_test.pcm to use it.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/voice_test.pcm")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/voice_test.pcm" "")
endif()

idf_component_register(
    SRCS "app_audio.cpp"
    INCLUDE_DIRS "include"
    EMBED_FILES "voice_test.pcm"
    PRIV_REQUIRES app_dsp_shim espressif__esp-sr app_driver app_config app_logic esp_driver_i2s
                  espressif__esp_board_manager espressif__esp_codec_dev esp_timer
)
