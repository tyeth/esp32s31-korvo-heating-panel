# The mDNS library is a managed component whose Kconfig lives in the component.
# The product only consumes its API; it does not compile any of it here.
idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES app_config app_driver)
