cmake_minimum_required(VERSION 3.24)

project(ChromeMic VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_library(chromemic_core STATIC
    src/app_processes.cpp
    src/audio_devices.cpp
    src/audio_router.cpp
    src/dsp.cpp
    src/process_loopback.cpp
)

target_include_directories(chromemic_core PUBLIC src)
target_compile_definitions(chromemic_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000A)
target_compile_options(chromemic_core PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(chromemic_core PUBLIC ole32 uuid avrt propsys mmdevapi user32)

add_executable(ChromeMic WIN32
    src/main.cpp
    resources/ChromeMic.rc
)

target_compile_options(ChromeMic PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(ChromeMic PRIVATE chromemic_core advapi32 comctl32 dwmapi gdi32 shell32 user32 uxtheme)
# ChromeMic.rc already embeds the reviewed application manifest. Disable the
# generator's second manifest resource to avoid a duplicate RT_MANIFEST entry.
target_link_options(ChromeMic PRIVATE /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT /Brepro)

add_executable(chromemic_tests
    tests/chromemic_tests.cpp
    src/app_processes.cpp
    src/dsp.cpp
    src/audio_devices.cpp
    src/audio_router.cpp
    src/process_loopback.cpp
)

target_include_directories(chromemic_tests PRIVATE src)
target_compile_definitions(chromemic_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000A)
target_compile_options(chromemic_tests PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(chromemic_tests PRIVATE ole32 uuid avrt propsys mmdevapi user32)
target_link_options(chromemic_tests PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT /Brepro)

enable_testing()
add_test(NAME chromemic_tests COMMAND chromemic_tests)

add_executable(chromemic_smoke EXCLUDE_FROM_ALL tools/smoke_test.cpp)
target_compile_options(chromemic_smoke PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(chromemic_smoke PRIVATE chromemic_core)
target_link_options(chromemic_smoke PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT /Brepro)

add_executable(chromemic_tone_renderer EXCLUDE_FROM_ALL tools/tone_renderer.cpp)
target_compile_definitions(chromemic_tone_renderer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000A)
target_compile_options(chromemic_tone_renderer PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(chromemic_tone_renderer PRIVATE ole32 uuid avrt)
target_link_options(chromemic_tone_renderer PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT /Brepro)

add_executable(chromemic_process_isolation_test EXCLUDE_FROM_ALL tools/process_isolation_test.cpp)
target_compile_options(chromemic_process_isolation_test PRIVATE /W4 /sdl /guard:cf /permissive- /EHsc /utf-8)
target_link_libraries(chromemic_process_isolation_test PRIVATE chromemic_core)
target_link_options(chromemic_process_isolation_test PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /CETCOMPAT /Brepro)

install(TARGETS ChromeMic RUNTIME DESTINATION .)
install(FILES README.md START-HERE.txt AUDIT.md LICENSE DESTINATION .)
