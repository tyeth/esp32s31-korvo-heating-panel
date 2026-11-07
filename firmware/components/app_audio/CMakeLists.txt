# Keep this in step with the app_audio CMakeLists the engine emits
# (engine/src/generator.ts). The engine overwrites app_audio.cpp with the real
# implementation UNCONDITIONALLY but only rewrites this CMakeLists when the
# product has device types — so for a framework-only product THIS file is what
# the generated implementation is compiled against, and it cannot see esp-sr's
# WakeNet/MultiNet headers or esp_srmodel_init() without the dependency below.
#
# app_dsp_shim comes FIRST: it provides the generic dsps_dotprod_f32_arp4 the
# prebuilt voice libraries call (esp-dsp's assembly version faults on this
# target), and being listed first is what makes the linker prefer it.
#
# app_logic is the product's vocabulary: app_audio.cpp calls its
# heating_voice_register() / heating_voice_apply_command() /
# heating_voice_on_event() hooks from the command-registration, recognised-
# phrase and listening-state slots the engine leaves empty.
idf_component_register(
    SRCS "app_audio.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_dsp_shim espressif__esp-sr app_driver app_config app_logic esp_driver_i2s
)
