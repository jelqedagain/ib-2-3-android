cmake_minimum_required(VERSION 3.20)
project(ib3rt CXX C RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# fmt/dynarmic rely on headers leaking in transitively, which newer libc++ no longer does.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include stdlib.h -include vector -include string -include cstring -include algorithm")

# --- dynarmic (ARM64 -> x86-64 JIT) ---------------------------------------
set(Boost_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/external/boost-1.92.0)
set(BOOST_ROOT ${CMAKE_SOURCE_DIR}/external/boost-1.92.0)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

# --- bundled C libraries the guest links against ----------------------------
add_library(ib3_thirdparty STATIC external/miniz/miniz.c external/sqlite/sqlite3.c)
target_compile_options(ib3_thirdparty PRIVATE -w -O2)
target_compile_definitions(ib3_thirdparty PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

# --- the runtime ------------------------------------------------------------
file(GLOB_RECURSE IB3RT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.c)
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
