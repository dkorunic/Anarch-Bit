idf_component_register(SRCS "main.cpp"
                       EMBED_FILES "Splash.bmp"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_timer nvs_flash efuse esp_adc esp_partition app_update)

# Anarch from the submodule, patched with mods/*.diff into the build dir by tools/patch_anarch.sh,
# so the submodule stays clean. Re-runs when a source or mod changes; a new mod file is picked up too.
set(ANARCH_DIR ${CMAKE_CURRENT_BINARY_DIR}/anarch)
file(GLOB ANARCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../anarch/*.h ${CMAKE_CURRENT_LIST_DIR}/../anarch/*.c)
file(GLOB ANARCH_MODS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../mods/*.diff)
if(NOT ANARCH_SOURCES)
    message(FATAL_ERROR "anarch submodule is empty, run: git submodule update --init")
endif()

# Declare the patched files as outputs, so ninja knows main.cpp must wait for (and rebuild after) patching.
set(ANARCH_PATCHED)
foreach(source ${ANARCH_SOURCES})
    get_filename_component(name ${source} NAME)
    list(APPEND ANARCH_PATCHED ${ANARCH_DIR}/${name})
endforeach()

add_custom_command(OUTPUT ${ANARCH_PATCHED}
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_anarch.sh ${ANARCH_DIR}
    DEPENDS ${ANARCH_SOURCES} ${ANARCH_MODS} ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_anarch.sh
    COMMENT "Patching Anarch with mods/*.diff"
    VERBATIM)
add_custom_target(anarch_patched DEPENDS ${ANARCH_PATCHED})
add_dependencies(${COMPONENT_LIB} anarch_patched)
target_include_directories(${COMPONENT_LIB} PRIVATE ${ANARCH_DIR})

# anarch is C99 written to also compile as C++; silence its harmless C++ warnings.
# -fwrapv: raycastlib relies on signed overflow wrapping (UBSan finds it in upstream too).
target_compile_options(${COMPONENT_LIB} PRIVATE -fwrapv -Wno-narrowing -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers)
