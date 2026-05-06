# Build dab_time_cli against welle.io submodule
# Usage: mkdir build-dab && cd build-dab && cmake -DBUILD_DAB_TIME=ON .. && make dab_time_cli

if(BUILD_DAB_TIME)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    find_package(Threads REQUIRED)

    # Collect welle.io sources (minimal set for FIC decode + rtl_tcp input)
    set(WELLE_DIR ${CMAKE_SOURCE_DIR}/lib/welle.io/src)

    file(GLOB WELLE_BACKEND_SRC ${WELLE_DIR}/backend/*.cpp)
    file(GLOB WELLE_VARIOUS_SRC ${WELLE_DIR}/various/*.cpp)
    file(GLOB WELLE_FEC_SRC ${WELLE_DIR}/libs/fec/*.c)
    set(WELLE_INPUT_SRC
        ${WELLE_DIR}/input/rtl_sdr.cpp
        ${WELLE_DIR}/input/rtl_tcp.cpp
        ${WELLE_DIR}/input/input_factory.cpp
        ${WELLE_DIR}/input/null_device.cpp
        ${WELLE_DIR}/input/raw_file.cpp
    )

    add_executable(dab_time_cli
        ${CMAKE_SOURCE_DIR}/src/dab_time_cli.cpp
        ${WELLE_BACKEND_SRC}
        ${WELLE_VARIOUS_SRC}
        ${WELLE_FEC_SRC}
        ${WELLE_INPUT_SRC}
    )

    target_include_directories(dab_time_cli PRIVATE
        ${WELLE_DIR}
        ${WELLE_DIR}/backend
        ${WELLE_DIR}/various
        ${WELLE_DIR}/input
        ${WELLE_DIR}/libs/fec
        ${FFTW3F_INCLUDE_DIRS}
    )

    target_link_libraries(dab_time_cli
        ${FFTW3F_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        rtlsdr
        m
    )

    # Optional: mpg123, faad, lame (for full welle.io, not needed for time-only)
    pkg_check_modules(MPG123 libmpg123)
    pkg_check_modules(FAAD faad2)
    find_library(LAME_LIB mp3lame)

    if(MPG123_FOUND)
        target_link_libraries(dab_time_cli ${MPG123_LIBRARIES})
        target_include_directories(dab_time_cli PRIVATE ${MPG123_INCLUDE_DIRS})
    endif()
    if(FAAD_FOUND)
        target_link_libraries(dab_time_cli ${FAAD_LIBRARIES})
    else()
        # Try direct link
        target_link_libraries(dab_time_cli faad)
    endif()
    if(LAME_LIB)
        target_link_libraries(dab_time_cli ${LAME_LIB})
    endif()

    target_compile_features(dab_time_cli PRIVATE cxx_std_14)

    install(TARGETS dab_time_cli DESTINATION bin)
endif()
