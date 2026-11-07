# Generic dot product for the voice front end — see dsps_dotprod_arp4.c.
#
# WHOLE_ARCHIVE is the point of this component: a static archive is only
# searched for symbols that are still unresolved when the linker gets to it, and
# the reference here comes from esp-sr's prebuilt archive, which in a plain link
# is only reached after esp-dsp's archive has already satisfied the call with the
# (faulting) assembly member. Linking this archive whole puts its object into the
# image unconditionally, so `dsps_dotprod_f32_arp4` is defined before esp-dsp is
# consulted and esp-dsp's assembly member for it is never loaded.
#
# INCLUDE_DIRS exposes app_dsp_shim.h so app_audio can log which kernel the image
# actually linked — the check that keeps a future link-order change from
# silently reintroducing the faulting assembly.
idf_component_register(
    SRCS "dsps_dotprod_arp4.c"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    PRIV_REQUIRES espressif__esp-dsp
)
