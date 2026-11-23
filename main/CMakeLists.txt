idf_component_register(
    SRCS
        main.c
        app_state.c
        sensor_protocol.c
        sensor_link.c
        calibration.c
        rtc_service.c
        data_logger.c
        ble_service.c
        ble_roles.c
        ble_central.c
        kneil_state.c
        kneil_ctrl_queue.c
        kneil_central.c
        haptics.c
        housekeeping.c
        usb_console.c
        imu_service.c
        power_service.cpp
        ui/ui.c
        ui/ui_face.c
        ui/ui_alert.c
        ui/ui_trend.c
        ui/ui_diag.c
        ui/ui_calib.c
        ui/ui_settings.c
        ui/ui_kneil.c
    INCLUDE_DIRS . ui)

# Vendor workarounds kept from the Waveshare example.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
if("chmorgan__esp-audio-player" IN_LIST build_components AND "esp_driver_i2s" IN_LIST build_components)
    idf_component_get_property(AUDIO_PLAYER_LIB chmorgan__esp-audio-player COMPONENT_LIB)
    if(TARGET ${AUDIO_PLAYER_LIB})
        target_link_libraries(${AUDIO_PLAYER_LIB} PUBLIC idf::esp_driver_i2s)
    endif()
endif()
