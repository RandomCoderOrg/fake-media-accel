cmake_minimum_required(VERSION 3.20)
project(fake-media-accel VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)

add_library(fma-common STATIC
    src/common/av1_obu.c
    src/common/h264_stream.c
    src/common/ivf.c
    src/common/protocol.c
    src/common/transport.c
)
target_include_directories(fma-common PUBLIC include)

add_library(fma-client STATIC src/client/client.c)
target_include_directories(fma-client PUBLIC include)
target_link_libraries(fma-client PUBLIC fma-common)

add_executable(fma-info tools/fma-info.c)
target_link_libraries(fma-info PRIVATE fma-client)

add_executable(fma-decode tools/fma-decode.c)
target_link_libraries(fma-decode PRIVATE fma-client)

add_executable(fma-h264-inspect tools/fma-h264-inspect.c)
target_link_libraries(fma-h264-inspect PRIVATE fma-common)

add_executable(fma-ivf-inspect tools/fma-ivf-inspect.c)
target_link_libraries(fma-ivf-inspect PRIVATE fma-common)

if(ANDROID)
    add_executable(fake-media-acceld src/android/fake-media-acceld.c)
    target_link_libraries(fake-media-acceld PRIVATE
        fma-common mediandk android log Threads::Threads)
else()
    add_executable(fake-media-acceld src/fake/fake-media-acceld.c)
    target_link_libraries(fake-media-acceld PRIVATE fma-common Threads::Threads)

    include(GNUInstallDirs)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBVA QUIET IMPORTED_TARGET libva)
        pkg_check_modules(LIBVA_X11 QUIET IMPORTED_TARGET libva-x11)
        pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)
        pkg_check_modules(GLESV2 QUIET IMPORTED_TARGET glesv2)
        pkg_check_modules(OPENGL QUIET IMPORTED_TARGET gl)
        pkg_check_modules(X11 QUIET IMPORTED_TARGET x11)
    endif()
    if(LIBVA_FOUND)
        add_library(fma-va-driver SHARED
            src/va/fma_drv_video.c
            src/va/h264_annexb.c
            src/va/h264_timing.c
        )
        target_include_directories(fma-va-driver PRIVATE include src/va)
        target_link_libraries(fma-va-driver PRIVATE
            fma-client PkgConfig::LIBVA Threads::Threads)
        set_target_properties(fma-va-driver PROPERTIES
            OUTPUT_NAME fma_drv_video
            PREFIX ""
        )
        install(TARGETS fma-va-driver
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dri)
        add_executable(fma-va-driver-test tests/va-driver-test.c)
        target_link_libraries(fma-va-driver-test PRIVATE
            fma-va-driver PkgConfig::LIBVA
        )
        target_include_directories(fma-va-driver-test PRIVATE include src/va)
        add_executable(fma-va-surface-contract-test
            tests/va-surface-contract-test.c)
        target_link_libraries(fma-va-surface-contract-test PRIVATE
            fma-va-driver PkgConfig::LIBVA
        )
        if(LIBVA_X11_FOUND AND EGL_FOUND AND GLESV2_FOUND AND X11_FOUND)
            add_executable(fma-va-egl-probe tools/fma-va-egl-probe.c)
            target_link_libraries(fma-va-egl-probe PRIVATE
                PkgConfig::LIBVA PkgConfig::LIBVA_X11 PkgConfig::EGL
                PkgConfig::GLESV2 PkgConfig::X11
            )
        endif()
        if(EGL_FOUND AND OPENGL_FOUND)
            add_library(fma-egl-trace SHARED tools/fma-egl-trace.c)
            target_link_libraries(fma-egl-trace PRIVATE
                PkgConfig::EGL PkgConfig::OPENGL ${CMAKE_DL_LIBS}
            )
        endif()
        add_executable(fma-h264-timing-test
            tests/h264-timing-test.c
            src/va/h264_timing.c
        )
        target_include_directories(fma-h264-timing-test PRIVATE src/va)
    endif()

    include(CTest)
    if(BUILD_TESTING)
        add_executable(fma-protocol-test tests/protocol-test.c)
        target_link_libraries(fma-protocol-test PRIVATE fma-common)
        add_executable(fma-h264-stream-test tests/h264-stream-test.c)
        target_link_libraries(fma-h264-stream-test PRIVATE fma-common)
        add_executable(fma-ivf-test tests/ivf-test.c)
        target_link_libraries(fma-ivf-test PRIVATE fma-common)
        add_executable(fma-av1-obu-test tests/av1-obu-test.c)
        target_link_libraries(fma-av1-obu-test PRIVATE fma-common)
        add_executable(fma-client-concurrency-test
            tests/client-concurrency-test.c)
        target_link_libraries(fma-client-concurrency-test PRIVATE fma-client)
        add_test(NAME protocol COMMAND fma-protocol-test)
        add_test(NAME h264-stream COMMAND fma-h264-stream-test)
        add_test(NAME ivf COMMAND fma-ivf-test)
        add_test(NAME av1-obu COMMAND fma-av1-obu-test)
        add_test(NAME fake-concurrent-clients
            COMMAND sh ${CMAKE_SOURCE_DIR}/tests/fake-concurrent.sh
                $<TARGET_FILE:fake-media-acceld>
                $<TARGET_FILE:fma-client-concurrency-test>
                ${CMAKE_BINARY_DIR}/fake-concurrent.sock)
        set_tests_properties(fake-concurrent-clients PROPERTIES TIMEOUT 5)
        add_test(NAME fake-roundtrip
            COMMAND sh ${CMAKE_SOURCE_DIR}/tests/fake-roundtrip.sh
                $<TARGET_FILE:fake-media-acceld>
                $<TARGET_FILE:fma-decode>
                ${CMAKE_BINARY_DIR}/fake-roundtrip.sock
                ${CMAKE_SOURCE_DIR}/LICENSE
                ${CMAKE_BINARY_DIR}/fake-roundtrip.nv12)
        if(LIBVA_FOUND)
            add_test(NAME h264-timing COMMAND fma-h264-timing-test)
            add_test(NAME va-surface-contract
                COMMAND fma-va-surface-contract-test)
            add_test(NAME va-surface-contract-cpu
                COMMAND ${CMAKE_COMMAND} -E env FMA_VA_DMA_HEAP=none
                    $<TARGET_FILE:fma-va-surface-contract-test>)
            add_test(NAME va-roundtrip
                COMMAND sh ${CMAKE_SOURCE_DIR}/tests/va-roundtrip.sh
                    $<TARGET_FILE:fake-media-acceld>
                    $<TARGET_FILE:fma-va-driver-test>
                    ${CMAKE_BINARY_DIR}/va-roundtrip.sock)
            add_test(NAME va-roundtrip-fallback
                COMMAND ${CMAKE_COMMAND} -E env FMA_FAKE_DIRECT_OUTPUT=0
                    sh ${CMAKE_SOURCE_DIR}/tests/va-roundtrip.sh
                    $<TARGET_FILE:fake-media-acceld>
                    $<TARGET_FILE:fma-va-driver-test>
                    ${CMAKE_BINARY_DIR}/va-roundtrip-fallback.sock)
            add_test(NAME va-roundtrip-cpu-surface
                COMMAND ${CMAKE_COMMAND} -E env FMA_VA_DMA_HEAP=none
                    sh ${CMAKE_SOURCE_DIR}/tests/va-roundtrip.sh
                    $<TARGET_FILE:fake-media-acceld>
                    $<TARGET_FILE:fma-va-driver-test>
                    ${CMAKE_BINARY_DIR}/va-roundtrip-cpu-surface.sock)
        endif()
    endif()
endif()
