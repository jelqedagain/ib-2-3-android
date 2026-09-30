cmake_minimum_required(VERSION 3.20)
project(ib3rt CXX C)
if (WIN32)
    enable_language(RC)
else()
    enable_language(ASM)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# fmt/dynarmic rely on headers leaking in transitively, which newer libc++ no longer does.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include stdlib.h -include vector -include string -include cstring -include algorithm")

if (NOT ANDROID)
# --- dynarmic (ARM64 -> x86-64 JIT) ---------------------------------------
set(Boost_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/boost-1.92.0)
set(BOOST_ROOT ${CMAKE_SOURCE_DIR}/external/boost-1.92.0)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

endif()

# --- bundled C libraries the guest links against ----------------------------
add_library(ib3_thirdparty STATIC external/miniz/miniz.c external/sqlite/sqlite3.c)
target_compile_options(ib3_thirdparty PRIVATE -w -O2)
target_compile_definitions(ib3_thirdparty PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

if (ANDROID)
    # --- Android (ARM64): the game's code runs natively; see src/cpu_native.* and src/port/android.
    file(GLOB_RECURSE IB3_ANDROID_SOURCES CONFIGURE_DEPENDS src/*.cpp)
    list(FILTER IB3_ANDROID_SOURCES EXCLUDE REGEX "src/(cpu\.cpp|libc/crypto\.cpp|uikit/boot\.cpp|uikit/window_win32\.cpp|game/pad_xinput\.cpp|audio/video_mf\.cpp)$")
    list(FILTER IB3_ANDROID_SOURCES EXCLUDE REGEX "src/(launcher|win)/|src/main\.cpp$|src/port/android/app\.cpp$")
    set_target_properties(ib3_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # ETC2 encoder: PVRTC textures are re-encoded for Android GPUs (src/gles/gl.cpp).
    add_library(etcpak STATIC external/etcpak/ProcessRGB.cpp external/etcpak/Tables.cpp external/etcpak/Dither.cpp)
    set_target_properties(etcpak PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(etcpak PRIVATE -w -O3)
    add_library(ib3core OBJECT ${IB3_ANDROID_SOURCES} src/cpu_native.S)
    set_target_properties(ib3core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ib3core PUBLIC src/port/android src external)
    target_compile_options(ib3core PUBLIC -Wno-unused-parameter -Wno-invalid-offsetof -fno-strict-aliasing)
    target_link_libraries(ib3core PUBLIC ib3_thirdparty etcpak aaudio mediandk android log dl m)

    # Command-line build (adb shell; renders off screen).
    add_executable(ib3android src/main.cpp)
    target_link_libraries(ib3android PRIVATE ib3core)
    target_link_options(ib3android PRIVATE -static-libstdc++)

    # The app (NativeActivity): libib3.so, packaged by scripts/build-apk.sh.
    set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
    add_library(ib3 SHARED src/main.cpp src/port/android/app.cpp ${GLUE}/android_native_app_glue.c)
    target_include_directories(ib3 PRIVATE ${GLUE})
    target_compile_definitions(ib3 PRIVATE IB3_ANDROID_APP)
    # port/android/app.cpp provides ANativeActivity_onCreate and calls the glue's under this name.
    set_source_files_properties(${GLUE}/android_native_app_glue.c PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(ib3 PRIVATE ib3core)
    target_link_options(ib3 PRIVATE -static-libstdc++ -u ANativeActivity_onCreate)
else()
    # --- the runtime ------------------------------------------------------------
    file(GLOB_RECURSE IB3RT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.c)
    list(FILTER IB3RT_SOURCES EXCLUDE REGEX "src/port/")
    add_executable(ib3rt ${IB3RT_SOURCES} src/app.rc)
    target_include_directories(ib3rt PRIVATE src external)
    target_link_libraries(ib3rt PRIVATE dynarmic ib3_thirdparty bcrypt winmm ole32 uuid mfuuid propsys windowscodecs comctl32 comdlg32 shell32)
    target_compile_options(ib3rt PRIVATE -Wno-unused-parameter -Wno-invalid-offsetof)
    # Keep our own image well away from the guest's preferred load address (0x100000000);
    # the main thread needs a deep host stack for nested guest <-> HLE calls.
    # A windowed program: no console window when started from Explorer or a shortcut.
    target_link_options(ib3rt PRIVATE -mwindows -Wl,--image-base=0x7ff600000000 -Wl,--stack,16777216 -static)

    # ANGLE (OpenGL ES on Direct3D 11) must sit next to the executable.
    add_custom_command(TARGET ib3rt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/external/angle/libEGL.dll ${CMAKE_SOURCE_DIR}/external/angle/libGLESv2.dll
            $<TARGET_FILE_DIR:ib3rt>)
endif()
