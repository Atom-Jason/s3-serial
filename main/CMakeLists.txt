idf_build_get_property(python PYTHON)
set(web_dir "${CMAKE_CURRENT_LIST_DIR}/../web")
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_inputs
    "${web_dir}/index.html"
    "${web_dir}/style.css"
    "${web_dir}/core.mjs"
    "${web_dir}/app.mjs"
    "${web_dir}/i18n.mjs"
    "${web_dir}/messages.mjs"
    "${web_dir}/demo.mjs")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(MAKE_DIRECTORY "${generated_dir}")
    set_source_files_properties("${generated_dir}/web_assets.c" PROPERTIES GENERATED TRUE)
endif()

idf_component_register(
    SRCS "app_main.c" "network_service.c" "web_service.c" "serial_core.c" "serial_service.c"
        "${generated_dir}/web_assets.c"
    INCLUDE_DIRS "." "${generated_dir}"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_driver_uart
        nvs_flash esp_timer json mbedtls esp_system esp_app_format)

add_custom_command(
    OUTPUT "${generated_dir}/web_assets.c" "${generated_dir}/web_assets.h" "${generated_dir}/web_assets.json"
    COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py"
        --web-dir "${web_dir}" --output-dir "${generated_dir}"
    DEPENDS ${web_inputs} "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py"
    VERBATIM)
add_custom_target(generate_web_assets DEPENDS
    "${generated_dir}/web_assets.c" "${generated_dir}/web_assets.h" "${generated_dir}/web_assets.json")
add_dependencies(${COMPONENT_LIB} generate_web_assets)
