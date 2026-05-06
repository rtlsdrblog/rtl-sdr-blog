# Build dab_time_cli against welle.io submodule
# Audio processing excluded - only FIC/OFDM needed for time sync

if(BUILD_DAB_TIME)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    find_package(Threads REQUIRED)

    set(WELLE_DIR ${CMAKE_SOURCE_DIR}/lib/welle.io/src)

    file(GLOB WELLE_BACKEND_SRC "${WELLE_DIR}/backend/*.cpp")
    # Exclude audio processing - not needed for time sync
    list(FILTER WELLE_BACKEND_SRC EXCLUDE REGEX "dab_decoder|dabplus_decoder|dab-audio|decoder_adapter|msc-handler|mot_manager|pad_decoder")

    file(GLOB WELLE_VARIOUS_SRC "${WELLE_DIR}/various/*.cpp")
    set(WELLE_FEC_SRC
        ${WELLE_DIR}/libs/fec/decode_rs_char.c
        ${WELLE_DIR}/libs/fec/encode_rs_char.c
        ${WELLE_DIR}/libs/fec/init_rs_char.c
    )
    set(WELLE_INPUT_SRC
        ${WELLE_DIR}/input/rtl_sdr.cpp
        ${WELLE_DIR}/input/rtl_tcp.cpp
        ${WELLE_DIR}/input/input_factory.cpp
        ${WELLE_DIR}/input/null_device.cpp
        ${WELLE_DIR}/input/raw_file.cpp
    )

    add_executable(dab_time_cli
        ${CMAKE_SOURCE_DIR}/src/dab_time_cli.cpp
        ${CMAKE_SOURCE_DIR}/src/msc_stub.cpp
        ${WELLE_BACKEND_SRC}
        ${WELLE_VARIOUS_SRC}
        ${WELLE_FEC_SRC}
        ${WELLE_INPUT_SRC}
    )

    target_compile_definitions(dab_time_cli PRIVATE HAVE_RTLSDR)
    set_target_properties(dab_time_cli PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_features(dab_time_cli PRIVATE cxx_std_14)

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

    install(TARGETS dab_time_cli DESTINATION bin)
endif()
