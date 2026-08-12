FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# Build dependencies match the libraries requested by CMakeLists.txt.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    libboost-system-dev \
    libboost-thread-dev \
    libcurl4-openssl-dev \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libjsoncpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu 24.04 uses the time64 curl runtime package (libcurl4t64).
# gstreamer1.0-rtsp is required for rtspclientsink used by RTSP push output.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libboost-system1.83.0 \
    libboost-thread1.83.0 \
    libcurl4t64 \
    libjsoncpp25 \
    libgstreamer1.0-0 \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-rtsp \
    && rm -rf /var/lib/apt/lists/*

# Fail the image build immediately if a plugin required by the transcoder UI
# is not actually visible to the runtime GStreamer registry. This catches
# incomplete runtime images instead of showing every element as unavailable.
RUN set -eux; \
    for element in \
      uridecodebin decodebin queue videoconvert deinterlace videoscale videorate \
      capsfilter x264enc h264parse audioconvert audioresample audiorate aacparse \
      mpegtsmux udpsink dvbsrc tsparse tsdemux appsink; do \
        gst-inspect-1.0 "$element" >/dev/null; \
    done; \
    if gst-inspect-1.0 voaacenc >/dev/null 2>&1; then :; \
    elif gst-inspect-1.0 fdkaacenc >/dev/null 2>&1; then :; \
    elif gst-inspect-1.0 avenc_aac >/dev/null 2>&1; then :; \
    else echo "No supported AAC encoder was found in the runtime image" >&2; exit 1; fi

COPY --from=build /src/build/TVStreammerSAT5 /app/TVStreammerSAT5

WORKDIR /data
EXPOSE 9000/tcp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/app/TVStreammerSAT5"]
