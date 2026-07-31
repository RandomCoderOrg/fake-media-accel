cmake_minimum_required(VERSION 3.20)
project(fake-media-accel VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(fma-common STATIC
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

if(ANDROID)
    add_executable(fake-media-acceld src/android/fake-media-acceld.c)
    target_link_libraries(fake-media-acceld PRIVATE fma-common mediandk android log)
else()
    add_executable(fake-media-acceld src/fake/fake-media-acceld.c)
    target_link_libraries(fake-media-acceld PRIVATE fma-common)

    include(GNUInstallDirs)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBVA QUIET IMPORTED_TARGET libva)
    endif()
    if(LIBVA_FOUND)
        add_library(fma-va-driver SHARED
            src/va/fma_drv_video.c
            src/va/h264_annexb.c
        )
        target_include_directories(fma-va-driver PRIVATE include src/va)
        target_link_libraries(fma-va-driver PRIVATE fma-client PkgConfig::LIBVA)
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
    endif()

    include(CTest)
    if(BUILD_TESTING)
        add_executable(fma-protocol-test tests/protocol-test.c)
        target_link_libraries(fma-protocol-test PRIVATE fma-common)
        add_test(NAME protocol COMMAND fma-protocol-test)
        add_test(NAME fake-roundtrip
            COMMAND sh ${CMAKE_SOURCE_DIR}/tests/fake-roundtrip.sh
                $<TARGET_FILE:fake-media-acceld>
                $<TARGET_FILE:fma-decode>
                ${CMAKE_BINARY_DIR}/fake-roundtrip.sock
                ${CMAKE_SOURCE_DIR}/LICENSE
                ${CMAKE_BINARY_DIR}/fake-roundtrip.nv12)
        if(LIBVA_FOUND)
            add_test(NAME va-roundtrip
                COMMAND sh ${CMAKE_SOURCE_DIR}/tests/va-roundtrip.sh
                    $<TARGET_FILE:fake-media-acceld>
                    $<TARGET_FILE:fma-va-driver-test>
                    ${CMAKE_BINARY_DIR}/va-roundtrip.sock)
        endif()
    endif()
endif()
