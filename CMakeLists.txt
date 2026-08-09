cmake_minimum_required(VERSION 3.10)
project(TVStreamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-mpegts-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

find_package(Boost REQUIRED COMPONENTS system thread)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${GSTREAMER_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

link_directories(
    ${GSTREAMER_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
    ${JSONCPP_LIBRARY_DIRS}
    ${Boost_LIBRARY_DIRS}
)

add_executable(TVStreamer
    src/main.cpp
    src/utils.cpp
    src/ConfigManager.cpp
    src/CaProviderManager.cpp
    src/DvbManager.cpp
    src/TelegramNotifier.cpp
    src/UdpInput.cpp
    src/UdpTsOutput.cpp
    src/UdpVbrOutput.cpp
    src/UdpCbrOutput.cpp
    src/UdpOutput.cpp
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

target_link_libraries(TVStreamer
    ${GSTREAMER_LIBRARIES}
    ${LIBCURL_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${Boost_LIBRARIES}
    pthread
    glib-2.0
    gio-2.0
)
