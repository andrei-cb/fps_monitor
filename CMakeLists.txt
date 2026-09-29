cmake_minimum_required(VERSION 3.24)
project(fps_monitor LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC OR NOT WIN32)
    message(FATAL_ERROR "fps_monitor only builds on Windows with MSVC")
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Dear ImGui (no CMake support upstream, so we just fetch the sources)
# ---------------------------------------------------------------------------
set(IMGUI_VERSION "v1.92.9b")
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# PawnIO modules (signed binaries, loaded into the PawnIO driver at runtime).
# The AMD family 17h+ and Intel MSR modules are embedded into the exe as resources.
# ---------------------------------------------------------------------------
set(PAWNIO_MODULES_VERSION "0.2.11")
string(REPLACE "." "_" _pawnio_ver_us ${PAWNIO_MODULES_VERSION})
FetchContent_Declare(pawnio_modules
    URL https://github.com/namazso/PawnIO.Modules/releases/download/${PAWNIO_MODULES_VERSION}/release_${_pawnio_ver_us}.zip
    URL_HASH SHA256=43608CB89BC84247FEF1368A139013F7D043E17DB6D6C8DFC9B46BF0905A81F4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pawnio_modules)

# Sets <var> to the RC-escaped path of a module from the PawnIO modules archive.
function(pawnio_module_rc_path var file)
    set(path "${pawnio_modules_SOURCE_DIR}/${file}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${file} not found in PawnIO modules archive")
    endif()
    file(TO_NATIVE_PATH "${path}" native)
    string(REPLACE "\\" "\\\\" escaped "${native}")
    set(${var} "${escaped}" PARENT_SCOPE)
endfunction()
pawnio_module_rc_path(PAWNIO_AMD17_MODULE_RC AMDFamily17.bin)
pawnio_module_rc_path(PAWNIO_INTELMSR_MODULE_RC IntelMSR.bin)

# The official, signed PawnIO installer, embedded so users can install the driver
# from inside the app (it runs "PawnIO_setup.exe -install -silent" after asking).
option(FPS_MONITOR_BUNDLE_PAWNIO "Embed the PawnIO driver installer in the executable" ON)
set(PAWNIO_SETUP_VERSION "2.2.0")
set(PAWNIO_SETUP_RC_LINE "")
if(FPS_MONITOR_BUNDLE_PAWNIO)
    set(PAWNIO_SETUP_EXE "${CMAKE_CURRENT_BINARY_DIR}/_deps/PawnIO_setup-${PAWNIO_SETUP_VERSION}.exe")
    if(NOT EXISTS "${PAWNIO_SETUP_EXE}")
        file(DOWNLOAD
            https://github.com/namazso/PawnIO.Setup/releases/download/${PAWNIO_SETUP_VERSION}/PawnIO_setup.exe
            "${PAWNIO_SETUP_EXE}"
            EXPECTED_HASH SHA256=1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032
            STATUS _pawnio_setup_status)
        list(GET _pawnio_setup_status 0 _pawnio_setup_code)
        if(NOT _pawnio_setup_code EQUAL 0)
            file(REMOVE "${PAWNIO_SETUP_EXE}")
            message(FATAL_ERROR "Failed to download PawnIO_setup.exe: ${_pawnio_setup_status}")
        endif()
    endif()
    file(TO_NATIVE_PATH "${PAWNIO_SETUP_EXE}" _pawnio_setup_native)
    string(REPLACE "\\" "\\\\" _pawnio_setup_rc "${_pawnio_setup_native}")
    set(PAWNIO_SETUP_RC_LINE "IDR_PAWNIO_SETUP RCDATA \"${_pawnio_setup_rc}\"")
endif()

set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
file(TO_NATIVE_PATH "${APP_ICON}" _app_icon_native)
string(REPLACE "\\" "\\\\" APP_ICON_RC "${_app_icon_native}")

configure_file(resources/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
# The generated .rc lives in the build dir; recompile it when the icon changes.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/app.rc PROPERTIES OBJECT_DEPENDS "${APP_ICON}")

# ---------------------------------------------------------------------------
# AMD Display Library (ADL) SDK headers (MIT), for AMD GPU sensors. Only the
# headers are needed: atiadlxx.dll ships with the AMD driver and is loaded at runtime.
# ---------------------------------------------------------------------------
set(ADL_SDK_COMMIT "84c881cb2136f04287eb3af12de00a9e2867aa9e")
set(ADL_SDK_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/adl_sdk-${ADL_SDK_COMMIT}/include")
foreach(entry
        "adl_sdk.h=1d0141f0ff0d8ce11c40b1e37174827f6e0536620365b20d24b55f19adf97d63"
        "adl_defines.h=d1c39f8523ecb224160c99ee3b043219bc8e2265c5993ec57548e382c22bd10b"
        "adl_structures.h=58843d1fdd39f8003a7ab6dfa424a2a3f12d8ba01568b70a1976db528b9db336")
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 header)
    list(GET parts 1 hash)
    if(NOT EXISTS "${ADL_SDK_INCLUDE_DIR}/${header}")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/display-library/${ADL_SDK_COMMIT}/include/${header}"
            "${ADL_SDK_INCLUDE_DIR}/${header}"
            EXPECTED_HASH SHA256=${hash}
            STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${ADL_SDK_INCLUDE_DIR}/${header}")
            message(FATAL_ERROR "Failed to download ${header}: ${status}")
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(fps_monitor WIN32
    src/main.cpp
    src/config.cpp
    src/overlay_window.cpp
    src/overlay_ui.cpp
    src/sensor_hub.cpp
    src/pawnio_setup.cpp
    src/sensors/fps_etw.cpp
    src/sensors/game_detect.cpp
    src/sensors/cpu_usage.cpp
    src/sensors/cpu_pawnio.cpp
    src/sensors/gpu_nvml.cpp
    src/sensors/gpu_adl.cpp
    src/sensors/gpu_d3dkmt.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/app.rc)

target_include_directories(fps_monitor PRIVATE src resources)
target_include_directories(fps_monitor SYSTEM PRIVATE "${ADL_SDK_INCLUDE_DIR}")
target_compile_definitions(fps_monitor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(fps_monitor PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(fps_monitor PRIVATE
    imgui d3d11 dxgi dcomp pdh advapi32 gdi32 shell32 user32)

# ETW kernel providers and the PawnIO driver both require elevation.
option(FPS_MONITOR_REQUIRE_ADMIN "Embed a requireAdministrator manifest (FPS and CPU temp need it)" ON)
if(FPS_MONITOR_REQUIRE_ADMIN)
    target_link_options(fps_monitor PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
