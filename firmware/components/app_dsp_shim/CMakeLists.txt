# Generic dot product for the voice front end — see dsps_dotprod_arp4.c.
#
# The component name is deliberately unrelated to esp-dsp's, and components are
# processed in dependency order: requirers are processed before their
# requirements, so an application component (app_audio) that lists this one
# FIRST in PRIV_REQUIRES pulls this archive in before esp-dsp's. The linker then
# resolves `dsps_dotprod_f32_arp4` here, and esp-dsp's own assembly member for
# that symbol is never pulled in.
idf_component_register(
    SRCS "dsps_dotprod_arp4.c"
    PRIV_REQUIRES espressif__esp-dsp
)
