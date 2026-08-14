cmake_minimum_required(VERSION 3.10)
project(TVStreammerSAT5 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(CRYPT_LIBRARY crypt)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-mpegts-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(DVBCSA QUIET libdvbcsa)
if(NOT DVBCSA_FOUND)
    pkg_check_modules(DVBCSA QUIET dvbcsa)
endif()
if(NOT DVBCSA_FOUND)
    find_path(DVBCSA_INCLUDE_DIRS dvbcsa/dvbcsa.h)
    find_library(DVBCSA_LIBRARIES dvbcsa)
    if(NOT DVBCSA_INCLUDE_DIRS OR NOT DVBCSA_LIBRARIES)
        message(FATAL_ERROR "libdvbcsa development files were not found")
    endif()
endif()

find_package(Boost REQUIRED COMPONENTS thread)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${GSTREAMER_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${DVBCSA_INCLUDE_DIRS}
)

link_directories(
    ${GSTREAMER_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
    ${JSONCPP_LIBRARY_DIRS}
    ${Boost_LIBRARY_DIRS}
    ${DVBCSA_LIBRARY_DIRS}
)

add_executable(TVStreammerSAT5
    src/main.cpp
    src/utils.cpp
    src/ConfigManager.cpp
    src/DvbSatellite.cpp
    src/CardManager.cpp
    src/CaBackend.cpp
    src/TelegramNotifier.cpp
    src/UdpInput.cpp
    src/StableUdpOutput.cpp
    src/StreamManager.cpp
    src/TranscoderModule.cpp
    src/GstTranscoderProcess.cpp
    src/protocols/GstProtocolTypes.cpp
    src/protocols/GstInputProtocols.cpp
    src/protocols/GstOutputProtocols.cpp
    src/protocols/stream/StreamInputProtocol.cpp
    src/protocols/stream/StreamOutputProtocol.cpp
    src/protocols/stream/inputs/StreamTestInputProtocol.cpp
    src/protocols/stream/inputs/StreamHttpInputProtocol.cpp
    src/protocols/stream/inputs/StreamHlsInputProtocol.cpp
    src/protocols/stream/inputs/StreamUdpInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtpInputProtocol.cpp
    src/protocols/stream/inputs/StreamSrtInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtspInputProtocol.cpp
    src/protocols/stream/inputs/StreamRtmpInputProtocol.cpp
    src/protocols/stream/inputs/StreamFileInputProtocol.cpp
    src/protocols/stream/outputs/StreamUdpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamHttpOutputProtocol.cpp
    src/protocols/stream/outputs/StreamHlsOutputProtocol.cpp
    src/protocols/stream/outputs/StreamSrtOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtspOutputProtocol.cpp
    src/protocols/stream/outputs/StreamRtmpOutputProtocol.cpp
    src/protocols/inputs/GstHttpInputProtocol.cpp
    src/protocols/inputs/GstHlsInputProtocol.cpp
    src/protocols/inputs/GstUdpInputProtocol.cpp
    src/protocols/inputs/GstRtpInputProtocol.cpp
    src/protocols/inputs/GstSrtInputProtocol.cpp
    src/protocols/inputs/GstRtspInputProtocol.cpp
    src/protocols/inputs/GstRtmpInputProtocol.cpp
    src/protocols/outputs/GstOutputProtocolUtils.cpp
    src/protocols/outputs/GstFifoOutputProtocol.cpp
    src/protocols/outputs/GstUdpOutputProtocol.cpp
    src/protocols/outputs/GstRtpOutputProtocol.cpp
    src/protocols/outputs/GstSrtOutputProtocol.cpp
    src/protocols/outputs/GstHttpOutputProtocol.cpp
    src/protocols/outputs/GstHlsOutputProtocol.cpp
    src/protocols/outputs/GstRtspOutputProtocol.cpp
    src/protocols/outputs/GstRtmpOutputProtocol.cpp
    src/HttpServer.cpp
)

target_link_libraries(TVStreammerSAT5
    ${GSTREAMER_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
    dl
    glib-2.0
    gio-2.0
)


option(TVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE "Build non-decoding CaBackend ABI example plugin" OFF)
if(TVSTREAMMERSAT5_BUILD_CA_PLUGIN_EXAMPLE)
    add_library(tvstreammersat5_ca_passthrough_example SHARED
        examples/ca_backend_passthrough_plugin.cpp
    )
    target_include_directories(tvstreammersat5_ca_passthrough_example PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(tvstreammersat5_ca_passthrough_example PROPERTIES
        PREFIX ""
        OUTPUT_NAME "tvstreammersat5-ca-passthrough-example"
    )
endif()

add_library(tvstreammersat5_ca_newcamd SHARED
    src/ca/backends/newcamd/NewcamdPlugin.cpp
    src/ca/backends/newcamd/NewcamdClient.cpp
)
target_include_directories(tvstreammersat5_ca_newcamd PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(tvstreammersat5_ca_newcamd PRIVATE
    ${Boost_LIBRARIES}
    ${DVBCSA_LIBRARIES}
    OpenSSL::Crypto
    $<$<BOOL:${CRYPT_LIBRARY}>:${CRYPT_LIBRARY}>
)
set_target_properties(tvstreammersat5_ca_newcamd PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tvstreammersat5-ca-newcamd"
)
install(TARGETS TVStreammerSAT5 RUNTIME DESTINATION bin)
install(TARGETS tvstreammersat5_ca_newcamd LIBRARY DESTINATION /opt/tvstreammersat5/ca-plugins)
