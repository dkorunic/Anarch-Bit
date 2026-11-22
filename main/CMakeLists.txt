idf_component_register(SRCS "main.cpp"
                       EMBED_FILES "Splash.bmp"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_driver_gptimer hal esp_timer nvs_flash efuse esp_adc esp_partition app_update)

# Anarch from the submodule, patched with mods/*.diff into the build dir by tools/patch_anarch.sh,
# so the submodule stays clean. Re-runs when a source or mod changes; a new mod file is picked up too.
set(ANARCH_DIR ${CMAKE_CURRENT_BINARY_DIR}/anarch)
file(GLOB ANARCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../anarch/*.h ${CMAKE_CURRENT_LIST_DIR}/../anarch/*.c)
file(GLOB ANARCH_MODS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../mods/*.diff)
list(SORT ANARCH_MODS)
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
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ANARCH_DIR} # inside build/: also clears trees from older script versions
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_anarch.sh ${ANARCH_DIR} ${ANARCH_MODS}
    DEPENDS ${ANARCH_SOURCES} ${ANARCH_MODS} ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_anarch.sh
    COMMENT "Patching Anarch with mods/*.diff"
    VERBATIM)
add_custom_target(anarch_patched DEPENDS ${ANARCH_PATCHED})
add_dependencies(${COMPONENT_LIB} anarch_patched)
target_include_directories(${COMPONENT_LIB} PRIVATE ${ANARCH_DIR})

# anarch is C99 written to also compile as C++; silence its harmless C++ warnings.
# -fwrapv: raycastlib relies on signed overflow wrapping (UBSan finds it in upstream too).
target_compile_options(${COMPONENT_LIB} PRIVATE -fwrapv -Wno-narrowing -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers)

# The rendering's per-column and per-pixel functions (~3.5 KB) run from internal RAM, out of the
# flash cache both cores share (main.cpp's SFG_setPixel carries IRAM_ATTR). Names as -Os builds them.
set(ANARCH_IRAM
    _Z27RCL_castRaysMultiHitColumns10RCL_CameraPFlssES1_PFvP13RCL_HitResulttt7RCL_RayE18RCL_RayConstraintsss
    _Z19RCL_castRayMultiHit7RCL_RayPFlssES1_P13RCL_HitResultPt18RCL_RayConstraints
    _Z26_RCL_columnFunctionComplexP13RCL_HitResulttt7RCL_Ray
    _ZL25_RCL_drawHorizontalColumnlllllsaasP7RCL_RayP13RCL_PixelInfo$constprop$0
    _ZL13_RCL_drawWallllllllsP13RCL_PixelInfo
    _Z22_RCL_floorCeilFunctionss _Z28RCL_perspectiveScaleVerticalll
    _Z13SFG_pixelFuncP13RCL_PixelInfo _ZL16SFG_getTexelFullhll _Z20SFG_drawScaledSpritePKhssshlh
    _Z14SFG_texturesAtss _Z17SFG_floorHeightAtss _Z19SFG_ceilingHeightAtss _ZL14SFG_getMapTilePK9SFG_LevelssPh)
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/iram_hot.sh ${CMAKE_OBJCOPY} ${CMAKE_OBJDUMP}
        $<TARGET_FILE:${COMPONENT_LIB}> ${ANARCH_IRAM}
    VERBATIM)

# The game (main.cpp holds Anarch) built for size, as in CGB-Bit: there, fewer instruction cache
# misses beat -O2's faster code by 3 points of CPU load. Measured here: README, Performance.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS -Os)
