idf_component_register(SRCS "main.cpp" "shaker.cpp" "five.cpp" "six.cpp" "analog_dsp.cpp" "power_control.cpp"
    INCLUDE_DIRS "."
    REQUIRES m5stack__m5unified usb_device_uac esp_timer nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
