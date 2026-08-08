# TVStreamer v86 — FTA auto-bypass

# TVStreamer5 — Release 2

TVStreamer5 is an IPTV stream router, monitor and transcoder with a built-in web control panel. **Current program version: Release 2.**

The application receives live streams, monitors their state and bitrate, can switch to a backup input, optionally transcodes video/audio with GStreamer, remaps MPEG-TS service metadata where supported, and publishes one or more output formats.

## Main features

- Web control panel with English/Russian interface.
- Start, stop, edit and delete streams without editing JSON manually.
- Primary/backup input switching and automatic return to the primary source.
- Input/output bitrate graphs, CC-error monitoring and interface load monitoring.
- Multiple outputs per stream.
- MPEG-TS PID/service remap for compatible TS paths.
- Optional H.264/AAC or H.264/MP3 transcoding through a clean external `gst-launch-1.0` pipeline.
- YADIF deinterlacing, scaling and fixed 25 fps progressive output for the transcoder.
- HTTP TS and HLS delivery from the built-in HTTP server.
- SRT listener/caller support.
- UDP unicast/multicast VBR and CBR output.
- RTMP/YouTube push and RTSP push.
- Subscriber/IP filtering for HTTP TS, HLS and SRT listener sessions.
- Telegram notifications.
- VLC playlist generation.
- Docker build/run scripts for host-network deployments.

## Release 2 architecture

Normal streams and transcoded streams use separate protocol modules. The normal pipeline remains in-process; transcoding is performed by an external GStreamer process.

```text
Input protocol
   |
   +-- passthrough/remap path --------------------> selected output protocol
   |
   +-- optional GStreamer transcoder
          |
          +-- decode
          +-- YADIF deinterlace
          +-- scale / 25 fps progressive
          +-- x264 H.264
          +-- AAC or MP3 audio
          +-- protocol-specific output module
```

Protocol code is split under `src/protocols/`:

```text
src/protocols/inputs/          transcoder input URI modules
src/protocols/outputs/         transcoder output modules
src/protocols/stream/inputs/   normal stream input modules
src/protocols/stream/outputs/  normal stream output modules
```

The legacy in-process transcoder implementation remains in the source tree for compatibility/fallback work, but the active clean transcoder path is `GstTranscoderProcess` using `gst-launch-1.0`.

## Supported inputs

Typical input URLs:

```text
http://server/live.ts
https://server/live.ts
http://server/live/playlist.m3u8
hls://server/live/playlist.m3u8
udp://@:1234
udp://239.1.1.1:1234
rtp://239.1.1.1:5004
srt://192.168.1.10:9000
rtsp://user:password@192.168.1.10:554/stream1
rtsps://server/stream
rtmp://server/live/camera1
test://bars
```

A local backup file is also supported by the normal stream path. The input interface can be selected separately from the output interface for protocols where GStreamer/Linux provides an explicit bind/interface option.

## Output formats

The web UI currently exposes:

```text
udp-vbr   MPEG-TS over UDP
udp-cbr   paced/CBR MPEG-TS over UDP
srt       MPEG-TS over SRT listener or caller
http      MPEG-TS over HTTP
hls       HLS playlist + MPEG-TS segments
rtsp      RTSP push to an external RTSP server
rtmp      RTMP push
youtube   RTMP push to YouTube Live
```

RTP protocol modules exist in the codebase for MPEG-TS/RTP processing, but Release 2 does not expose RTP output as a selectable item in the current web UI.

HTTP and HLS player URLs use the application HTTP server:

```text
http://SERVER:9000/stream/STREAM_ID.ts
http://SERVER:9000/hls/STREAM_ID/playlist.m3u8
```

RTSP output is **push**, not an embedded RTSP server. `output_host` must point to a server that accepts RTSP publishing.

## Multiple outputs

A stream can have one primary output plus `additional_outputs`. Example:

```json
{
  "output_type": "udp-cbr",
  "output_host": "239.1.1.1",
  "output_port": 1234,
  "additional_outputs": [
    {
      "output_type": "srt",
      "output_mode": "listener",
      "output_host": "0.0.0.0",
      "output_port": 9001
    },
    {
      "output_type": "hls",
      "output_mode": "listener",
      "output_host": "192.168.1.20",
      "output_port": 9000
    }
  ]
}
```

## MPEG-TS remap

Enable `remap_enabled` and set the required values:

```json
{
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel",
  "service_provider": "Provider"
}
```

For UDP/SRT/HTTP MPEG-TS paths the mux/remap code uses MPEG-TS request pads and service mapping where the selected pipeline supports it.

**Known Release 2 limitation:** exact configured elementary PIDs are not currently guaranteed after **transcoded HLS** segmentation. Verify the generated `.ts` segments with `ffprobe -show_programs -show_streams` when exact HLS PID values are required. Do not rely only on the `.m3u8` playlist to verify remap.

## HLS behavior

Transcoded HLS is generated under:

```text
/tmp/tvstreamer5-hls/<stream-id>/
```

Release 2 uses a three-segment live playlist:

```text
playlist-length = 3
target-duration = 5 seconds
max-files = 5
```

The HLS directory for a stream is cleared when a new transcoded HLS pipeline starts so stale segments from a previous run are not mixed into a new playlist.

## Transcoding

Transcoding is optional. When disabled, TVStreamer5 uses the normal passthrough/remap pipeline.

The current transcoder video path is approximately:

```text
uridecodebin
 -> raw video
 -> videoconvert
 -> deinterlace method=yadif
 -> videoscale method=lanczos (selected output resolution, preserve display aspect with borders when required)
 -> videorate
 -> 25 fps progressive I420, SAR/PAR 1:1 (square pixels)
 -> x264enc superfast / zerolatency with VUI enabled
 -> h264parse
```


The transcoder always normalizes the scaled video to **SAR/PAR 1:1** before H.264 encoding. The selected output width/height therefore use square pixels, and `x264enc` is instructed to emit VUI information. `videoscale add-borders=true` preserves the source display aspect ratio when the selected resolution has a different shape, adding borders instead of stretching the picture.

Audio is normalized to 48 kHz stereo before encoding. AAC is the stable/default transcoder path; MP3 is used when explicitly configured and an MP3 encoder is available. In the clean transcoder path, a UI/config value intended as audio `copy` is currently handled as AAC re-encode rather than bit-exact passthrough.

The external transcoder owns its own input socket. Therefore the transcoder input bitrate graph is an application-side estimate while this architecture is used; it is not a direct GStreamer pad-probe measurement of the external process input.

Check installed transcoder/protocol elements with:

```bash
./scripts/check_transcoder_plugins.sh
```

## Build on Ubuntu/Debian

Install only the current build/runtime dependencies:

```bash
./install_deps.sh
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Run from the directory that contains `tvstreamer5-config.json`:

```bash
./build/TVStreamer
```

Default web UI:

```text
http://localhost:9000
```

### Packages installed by `install_deps.sh`

Build libraries:

```text
build-essential
cmake
pkg-config
libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev
libgstreamer-plugins-bad1.0-dev
libcurl4-openssl-dev
libjsoncpp-dev
libboost-system-dev
libboost-thread-dev
```

Runtime GStreamer packages:

```text
gstreamer1.0-tools
gstreamer1.0-plugins-base
gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad
gstreamer1.0-plugins-ugly
gstreamer1.0-libav
gstreamer1.0-rtsp
ca-certificates
```

`gstreamer1.0-rtsp` is required for `rtspclientsink` used by the RTSP push output module. Development packages for Boost filesystem/program-options, OpenSSL, gst-rtsp-server, Git and wget are not required by the current CMake target and are no longer installed by the dependency script.

## Install as a systemd service

```bash
sudo mkdir -p /opt/tvstreamer5
sudo install -m 755 build/TVStreamer /opt/tvstreamer5/TVStreamer
sudo cp tvstreamer5-config.json /opt/tvstreamer5/
```

Example `/etc/systemd/system/tvstreamer5.service`:

```ini
[Unit]
Description=TVStreamer5 Release 2
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/tvstreamer5
ExecStart=/opt/tvstreamer5/TVStreamer
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tvstreamer5
sudo systemctl status tvstreamer5 --no-pager --full
```

## Docker

TVStreamer5 can be built and run entirely in Docker. This keeps compiler and
GStreamer development packages out of the host operating system. The runtime
container still uses the host network because IPTV multicast, RTP, SRT listener
mode and interface-specific bindings work most reliably with `--network host`.

### Install Docker Engine on Ubuntu

The recommended production installation uses Docker's official `apt`
repository. The commands below are suitable for supported Ubuntu releases such
as 22.04 LTS and 24.04 LTS.

Remove packages that can conflict with the official Docker Engine packages:

```bash
sudo apt remove -y docker.io docker-compose docker-compose-v2 docker-doc \
  docker-buildx podman-docker containerd runc || true
```

Add Docker's signing key and repository:

```bash
sudo apt update
sudo apt install -y ca-certificates curl

sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt update
```

Install Docker Engine, Buildx and the Compose plugin:

```bash
sudo apt install -y \
  docker-ce \
  docker-ce-cli \
  containerd.io \
  docker-buildx-plugin \
  docker-compose-plugin
```

Enable Docker at boot and verify the daemon:

```bash
sudo systemctl enable --now docker
sudo systemctl status docker --no-pager --full
sudo docker run --rm hello-world
```

Docker commands require `sudo` by default. To allow the current user to run
Docker commands without `sudo`:

```bash
sudo usermod -aG docker "$USER"
newgrp docker
docker version
```

Membership in the `docker` group effectively grants root-level access to the
host. Keep using `sudo docker ...` instead if that is preferable for the server.

### Build the TVStreamer5 Docker image

From the repository directory:

```bash
cd /home/monk/TVStreamer5
chmod +x scripts/build_container.sh scripts/run_container.sh
./scripts/build_container.sh
```

The default image name is:

```text
tvstreamer5:release2
```

Verify that the image exists:

```bash
docker image ls tvstreamer5:release2
docker image inspect tvstreamer5:release2 >/dev/null && echo "TVStreamer5 image OK"
```

The build script can be launched from any working directory because it resolves
the repository path from the script location itself. To use another image name:

```bash
IMAGE_NAME=my-tvstreamer:release2 ./scripts/build_container.sh
```

A direct equivalent build command is:

```bash
docker build --pull -t tvstreamer5:release2 .
```

### Prepare persistent TVStreamer5 data

For a production container, keep configuration and application data outside the
container. Example:

```bash
sudo mkdir -p /srv/tvstreamer5
sudo cp tvstreamer5-config.json /srv/tvstreamer5/tvstreamer5-config.json
sudo chown -R "$USER":"$USER" /srv/tvstreamer5
```

The `/srv/tvstreamer5` directory can then persist:

```text
tvstreamer5-config.json
tvstreamer5-subscribers.json
backup-files/
```

### Test the container interactively

The supplied run script starts an interactive temporary container:

```bash
CONFIG_FILE=/srv/tvstreamer5/tvstreamer5-config.json \
  ./scripts/run_container.sh
```

Press `Ctrl+C` to stop it. Because this helper uses `--rm`, the temporary
container is automatically removed after exit; configuration and other data
remain on the host.

To increase GStreamer logging temporarily:

```bash
GST_DEBUG=2 \
CONFIG_FILE=/srv/tvstreamer5/tvstreamer5-config.json \
  ./scripts/run_container.sh
```

### Run TVStreamer5 as a background Docker service

Create the persistent production container:

```bash
docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreamer5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:release2
```

The web interface then uses the HTTP port configured by TVStreamer5, normally:

```text
http://SERVER_IP:9000
```

`--network host` is intentional. With host networking, Docker does not need
`-p 9000:9000`, `-p` mappings for UDP multicast, or separate SRT port
forwarding. TVStreamer5 binds directly to the host's interfaces and ports.

### Docker management from the console

The following commands manage the `tvstreamer5` container directly from a Linux
console. If the current user is not in the `docker` group, prefix each
`docker ...` command with `sudo`. Commands that stop or restart the Docker daemon
affect **all** containers on the host, not only TVStreamer5.

```bash
# Show Docker Engine status
systemctl status docker --no-pager --full

# Start / stop / restart the Docker daemon
sudo systemctl start docker
sudo systemctl stop docker
sudo systemctl restart docker

# Enable / disable Docker daemon autostart
sudo systemctl enable docker
sudo systemctl disable docker

# Show all running containers
docker ps

# Show running and stopped containers
docker ps -a

# Show only TVStreamer5
docker ps -a --filter name=tvstreamer5

# Start TVStreamer5
docker start tvstreamer5

# Stop TVStreamer5 gracefully
docker stop -t 15 tvstreamer5

# Restart TVStreamer5
docker restart -t 15 tvstreamer5

# Show container state, exit code and restart count
docker inspect tvstreamer5 --format \
  'status={{.State.Status}} running={{.State.Running}} exit={{.State.ExitCode}} restart={{.RestartCount}}'

# Follow logs in real time
docker logs -f tvstreamer5

# Show the last 200 log lines
docker logs --tail 200 tvstreamer5

# Show logs from the last 10 minutes
docker logs --since 10m tvstreamer5

# Show CPU, RAM and network usage
docker stats tvstreamer5

# Open a shell inside the running container
docker exec -it tvstreamer5 bash

# Show processes running in the TVStreamer5 container
docker top tvstreamer5

# Check GStreamer plugins inside the container
docker exec tvstreamer5 gst-inspect-1.0 x264enc
docker exec tvstreamer5 gst-inspect-1.0 srtsink
docker exec tvstreamer5 gst-inspect-1.0 rtspclientsink

# Check the config visible inside the container
docker exec tvstreamer5 ls -lah /data
docker exec tvstreamer5 test -f /data/tvstreamer5-config.json && echo "config OK"

# Inspect mounted host directories
docker inspect tvstreamer5 --format '{{json .Mounts}}'

# Inspect container network mode
docker inspect tvstreamer5 --format '{{.HostConfig.NetworkMode}}'

# Show the image used by the container
docker inspect tvstreamer5 --format '{{.Config.Image}}'

# Show installed TVStreamer5 images
docker image ls 'tvstreamer5*'

# Show Docker disk usage
docker system df
```

### Update TVStreamer5 in Docker

After pulling new source code, rebuild the image and recreate the container. A
running container does not automatically switch to a newly built image.

```bash
cd /home/monk/TVStreamer5
git pull origin main

./scripts/build_container.sh

docker stop -t 15 tvstreamer5
docker rm tvstreamer5

docker run -d \
  --name tvstreamer5 \
  --restart unless-stopped \
  --init \
  --network host \
  -v /srv/tvstreamer5:/data \
  -w /data \
  -e GST_DEBUG=1 \
  tvstreamer5:release2

# Verify the new container
docker ps --filter name=tvstreamer5
docker logs --tail 100 tvstreamer5
```

The bind-mounted `/srv/tvstreamer5` directory is not removed when the container
is recreated, so the configuration, subscriber database and backup files remain
persistent.

### Change the configuration and restart

Edit the host copy of the configuration:

```bash
sudoedit /srv/tvstreamer5/tvstreamer5-config.json
```

Then restart the container:

```bash
docker restart -t 15 tvstreamer5
docker logs --tail 100 tvstreamer5
```

### Remove or recreate the container

Removing the container does not remove `/srv/tvstreamer5` because that directory
is a host bind mount.

```bash
docker stop -t 15 tvstreamer5
docker rm tvstreamer5
```

Force-remove a stuck container only when a normal stop does not work:

```bash
docker rm -f tvstreamer5
```

Remove an old TVStreamer5 image after the container using it has been removed:

```bash
docker image rm tvstreamer5:release2
```

Remove only unused Docker objects:

```bash
docker container prune
docker image prune
docker builder prune
```

Do not use `docker system prune -a --volumes` on a production server unless you
explicitly intend to delete all unused images, networks, build cache and unused
Docker volumes.

### Docker troubleshooting

If the container immediately exits:

```bash
docker ps -a --filter name=tvstreamer5
docker inspect tvstreamer5 --format 'exit={{.State.ExitCode}} error={{.State.Error}}'
docker logs --tail 300 tvstreamer5
```

If port 9000 is already occupied:

```bash
sudo ss -lntp | grep ':9000'
```

If UDP multicast or SRT traffic is missing, first verify that host networking is
actually enabled and inspect the host interface directly:

```bash
docker inspect tvstreamer5 --format '{{.HostConfig.NetworkMode}}'
ip -br addr
ip route
sudo tcpdump -ni eth0 udp
```

Replace `eth0` with the real IPTV interface. Since TVStreamer5 uses host
networking, protocol diagnostics are performed on the host network namespace.

If the image needs to be rebuilt without Docker layer cache:

```bash
cd /home/monk/TVStreamer5
docker build --pull --no-cache -t tvstreamer5:release2 .
```

If Docker itself is unhealthy:

```bash
sudo systemctl status docker --no-pager --full
sudo journalctl -u docker -n 200 --no-pager
docker info
```

Docker's official Ubuntu installation documentation should be checked when
upgrading the host to a new Ubuntu release because supported distributions and
package names can change.

## Network tuning for high-bitrate UDP

Example Linux tuning:

```bash
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=8388608
sudo sysctl -w net.core.wmem_default=8388608
sudo sysctl -w net.ipv4.udp_rmem_min=131072
sudo sysctl -w net.ipv4.udp_wmem_min=131072
sudo sysctl -w net.core.netdev_max_backlog=50000
```

For multicast, verify routes and the selected interface instead of assuming the default route is correct:

```bash
ip -br addr
ip route get 239.1.1.1
sudo tcpdump -ni eth0 udp port 1234
```

`bad udp cksum` shown by `tcpdump` on the transmitting host can be a TX checksum-offload artifact; receiver-side capture is the better validation point.

## Backup failover

Set `backup_input_uri` to enable source failover. When the current input stops producing data, TVStreamer5 can switch to the backup source and later return to the primary source when it recovers.

Useful fields:

```text
backup_input_uri
backup_input_type
backup_file_loop
auto_start
```

The stream tile displays whether the primary or backup source is active.

## Subscriber access control

Subscriber settings are stored in:

```text
tvstreamer5-subscribers.json
```

When filtering is enabled, HTTP TS, HLS and SRT listener access can be restricted by subscriber IP and stream ID. The web UI can add/remove subscribers, assign streams and reset active sessions.

## Telegram notifications

Configure:

```text
telegram_token
telegram_chat_id
```

Notifications report stream start/stop, source failure, backup switching, recovery, pipeline errors and EOS events.

## Main configuration fields

Example:

```json
{
  "id": "channel-1",
  "name": "Channel 1",
  "input_uri": "udp://239.1.1.1:1234",
  "backup_input_uri": "",
  "input_mode": "auto",
  "input_interface_address": "",
  "output_type": "udp-cbr",
  "output_mode": "listener",
  "output_host": "239.2.2.2",
  "output_port": 1234,
  "interface_address": "",
  "target_bitrate": 7000000,
  "remap_enabled": true,
  "video_pid": 258,
  "audio_pid": 257,
  "service_id": 2,
  "service_name": "Channel 1",
  "service_provider": "TVStreamer5",
  "transcode_enabled": false,
  "transcode_resolution": "1280x720",
  "transcode_video_bitrate": 3500000,
  "transcode_audio_codec": "aac",
  "transcode_audio_bitrate": 192000,
  "additional_outputs": []
}
```

Global settings include `http_port`, `login`, `password`, Telegram settings and stream arrays. The web UI is the recommended configuration editor.

## Diagnostics

Service logs:

```bash
sudo journalctl -u tvstreamer5 -n 200 --no-pager
```

Processes:

```bash
ps aux | grep -Ei 'TVStreamer|gst-launch' | grep -v grep
```

GStreamer capability check:

```bash
./scripts/check_transcoder_plugins.sh
```

Inspect MPEG-TS:

```bash
ffprobe -hide_banner -show_programs -show_streams INPUT
```

Inspect current HLS segment:

```bash
SEG=$(ls -1t /tmp/tvstreamer5-hls/STREAM_ID/segment*.ts | head -1)
ffprobe -hide_banner -show_programs -show_streams "$SEG"
```

## Screenshots

Dashboard:

![TVStreamer5 dashboard](docs/screenshots/dashboard.png)

Stream settings:

![TVStreamer5 stream settings](docs/screenshots/stream-settings.png)

Network monitoring:

![TVStreamer5 network interface load](docs/screenshots/network.png)

## Notes

- Release 2 no longer uses FFmpeg as the active transcoder engine.
- Non-transcoded behavior should be treated as the stable baseline when diagnosing protocol-specific transcoder issues.
- Exact HLS PID preservation after transcoding remains a known limitation and must be verified on generated segments.
- For live IPTV, transport stability depends on source quality, kernel socket buffers, routing, multicast interface selection and available CPU for x264 encoding.

### CBR pacing for all transcoded outputs

For live IPTV inputs the external GStreamer transcoder disables `uridecodebin use-buffering` so a live UDP/SRT source is not repeatedly paused by generic URI buffering.

All **MPEG-TS based outputs after transcoding** now use one CBR transport policy: `mpegtsmux bitrate=<effective_mux_bitrate>` inserts NULL packets to maintain the selected multiplex bitrate, then `tsparse` normalizes TS timing and an `identity datarate=<bytes/s> sync=true` pacer releases the transport stream against the pipeline clock. This applies to UDP (including the old `udp-vbr` selector while transcoding), SRT, HTTP TS, HLS, RTP and FIFO relay outputs.

The effective TS CBR bitrate is `max(target_bitrate, video_bitrate + audio_bitrate + 1.2 Mbit/s)`. The extra headroom is reserved for MPEG-TS/SI overhead and prevents the mux target from being lower than the encoded elementary streams. The effective value is printed in the journal as `ts-cbr-bitrate=<bits_per_second>`.

RTMP/YouTube and RTSP do not carry MPEG-TS, so TS NULL-packet stuffing is not applicable to those protocols. Their transcoded H.264 branch still uses x264 CBR/HRD (`nal-hrd=cbr`, fixed encoder bitrate, VBV and CFR), and the audio encoder uses the configured fixed bitrate.

Current TS output pacing profile:

```text
UDP/RTP: mpegtsmux CBR -> tsparse -> identity CBR pacer -> network sink
HTTP:    mpegtsmux CBR -> tsparse -> identity CBR pacer -> tcpserversink
HLS:     mpegtsmux CBR -> tsparse -> identity CBR pacer -> hlssink
SRT:     mpegtsmux CBR -> tsparse -> identity CBR pacer -> loopback relay/public SRT
FIFO:    mpegtsmux CBR -> tsparse -> identity CBR pacer -> filesink/FIFO
```

For an SRT Listener with transcoding, the public `srtsink` is still owned by TVStreamer5 so subscriber monitoring continues to use the normal `caller-added`/`caller-removed` callbacks. The external transcoder only sends the already paced CBR MPEG-TS over `127.0.0.1:<relay>`. The in-process relay no longer rebuilds PCR timestamps a second time. Transcoded SRT also uses a 2500 ms SRT latency and disables `GstBaseSink max-bitrate`; the latter is important because a stale/default `target_bitrate` can otherwise throttle a 6+ Mbit/s transcoded stream to only about 2 Mbit/s and create periodic A/V stalls.

Recommended first receiver test for transcoded SRT is 2500-3000 ms latency.

### Проверка GStreamer внутри Docker

Release 2 инициализирует GStreamer до запуска HTTP-интерфейса. Это важно: страница настройки потоков проверяет наличие элементов транскодера сразу после запуска приложения. Если registry GStreamer еще не инициализирован, интерфейс ошибочно может показать все элементы как отсутствующие, даже если пакеты установлены в контейнере.

После пересборки контейнера можно проверить runtime напрямую:

```bash
docker exec tvstreamer5 gst-inspect-1.0 uridecodebin
docker exec tvstreamer5 gst-inspect-1.0 decodebin
docker exec tvstreamer5 gst-inspect-1.0 x264enc
docker exec tvstreamer5 gst-inspect-1.0 h264parse
docker exec tvstreamer5 gst-inspect-1.0 aacparse
docker exec tvstreamer5 gst-inspect-1.0 mpegtsmux
docker exec tvstreamer5 gst-inspect-1.0 identity
docker exec tvstreamer5 gst-inspect-1.0 udpsink
```

Для краткой проверки всех обязательных элементов из исходного дерева:

```bash
./scripts/check_transcoder_plugins.sh
```

Dockerfile также выполняет обязательную проверку этих элементов во время `docker build`. Если один из критических GStreamer-плагинов отсутствует, новый образ теперь не будет собран успешно.

### Web UI polling and external transcoder sockets

Release 2 updates live dashboard values without rebuilding every stream card on every poll. The `/api/state` and `/api/system-metrics` requests run as independent sequential polling loops: the next request is scheduled only after the previous request finishes. This avoids overlapping HTTP requests behind reverse proxies such as Nginx/HestiaCP and prevents visible dashboard flicker on busy servers.

A full stream-card render is now performed only when the stream configuration, stream order or UI language changes. Runtime values such as Online/Offline, backup state, active input, input/output bitrate and status are updated in place.

External `gst-launch-1.0` transcoders must not inherit TVStreamer5 HTTP, metrics or listener sockets. TVStreamer5 marks non-standard file descriptors `FD_CLOEXEC` before starting an external GStreamer process. Verify after starting a transcoded stream:

```bash
ss -lntp | grep -E ':9000|:9100'
```

The listening ports should belong to `TVStreamer` only; `gst-launch-1.0` should not appear as an owner of those sockets.

### SRT после транскодирования

В Release 2 внешний GStreamer-транскодер кодирует видео/звук и отдаёт готовый MPEG-TS во внутренний loopback UDP relay. Сам SRT Listener теперь снова принадлежит процессу `TVStreamer`, как и SRT без транскодинга. Поэтому события `caller-added` и `caller-removed` приходят напрямую из `srtsink`, а окно абонентов видит подключение сразу, без опроса `ss`.

Смысл полей SRT такой же, как у обычного SRT-потока:

- `Listener`: TVStreamer слушает `srt://:<port>` на `0.0.0.0` либо на адресе выбранного выходного интерфейса. Поле `Адрес выхода` используется только как адрес, показываемый в ссылке клиенту, и не используется как bind-адрес.
- `Caller`: `Адрес выхода` является адресом удалённого SRT-сервера, а выбранный выходной интерфейс применяется как локальный bind-адрес.
- Внешний `gst-launch-1.0` для SRT Listener больше не содержит `srtsink`; он содержит `udpsink host=127.0.0.1 port=<relay>`. SRT-сеть, фильтрация абонентов и активные сессии обслуживаются внутри TVStreamer.
- Для совместимости с GStreamer 1.20.x на Ubuntu 22.04 внешний `gst-launch-1.0` не использует свойство `auto-reconnect` у `srtsink`, потому что в этой версии элемента такого свойства нет.
- Если `gst-launch-1.0` завершается сразу из-за ошибки bind/URI/плагина, запуск потока завершается ошибкой вместо ложного статуса `running`.

Проверка процесса после запуска SRT Listener с транскодингом:

```bash
ps -eo pid,ppid,args | grep gst-launch | grep -v grep
```

В команде внешнего транскодера должен быть виден внутренний relay:

```text
uridecodebin ... use-buffering=false
mpegtsmux ... bitrate=<effective_cbr_bitrate>
tsparse ... smoothing-latency=500000
identity ... datarate=<effective_cbr_bytes_per_second> sync=true
udpsink host=127.0.0.1 port=<relay>
```

А SRT-порт должен принадлежать процессу TVStreamer:

```bash
ss -lunp | grep '<SRT_PORT>'
journalctl -u tvstreamer5 -n 100 --no-pager | grep -Ei 'srt|caller|relay|GStreamer transcoder|error|failed|exit'
```

Проверка приёмником GStreamer:

```bash
gst-launch-1.0 -v srtsrc uri="srt://SERVER_IP:SRT_PORT?mode=caller" ! tsparse ! fakesink sync=false
```

### Active sessions for transcoded SRT

SRT без транскодинга и SRT Listener после транскодинга используют одинаковый механизм активных абонентских сессий: TVStreamer владеет `srtsink` и получает `caller-added`/`caller-removed` callbacks напрямую.

Когда приёмник подключён, его удалённый IP должен совпадать с основным или резервным IP абонента, а сам абонент должен быть привязан к этому потоку. После этого окно «Абоненты» показывает абонента как Online. Если приёмник подключается через NAT, указывайте тот IP, который видит сервер TVStreamer.


### Единый мониторинг SRT-подключений

SRT Listener без транскодинга и SRT Listener после транскодинга теперь создают `srtsink` через один и тот же код `StreamManager::createOutputSink()`. Поэтому для обоих режимов используются одинаковые GStreamer-события подключения:

- `caller-connecting` — проверка доступа абонента;
- `caller-added` — добавление активной SRT-сессии;
- `caller-removed` — удаление активной SRT-сессии;
- `caller-rejected` — журналирование отказа.

Отдельный опрос `ss` для транскодированного SRT удалён. Он больше не нужен и мог давать двойной или запаздывающий подсчёт сессий. Внешний `gst-launch-1.0` только передаёт MPEG-TS на `127.0.0.1:<relay>`, а сетевой SRT Listener и мониторинг клиентов находятся внутри TVStreamer.

Для проверки подключите SRT caller и смотрите журнал:

```bash
journalctl -u tvstreamer5 -f | grep -Ei 'SRT connection monitoring|SRT caller|Transcoded SRT output relay'
```

При подключении ожидается `SRT caller added ... from <CLIENT_IP>`, а при отключении — `SRT caller removed ...`. Тот же IP используется для статуса Online в таблице абонентов.


### SRT transcoding session path verification

For an SRT Listener with transcoding enabled, the external `gst-launch-1.0` process must terminate at an internal loopback `udpsink`; the public `srtsink` is owned by TVStreamer5 so that the normal `caller-added`/`caller-removed` subscriber monitoring callbacks are used. The startup description should therefore contain `srt-listener-relay@127.0.0.1:`. If the UI still shows `srt-listener@srt://...`, an older/partial SRT output module is deployed.

The external transcoder command is now logged before process startup, and early `gst-launch-1.0` stderr is appended to the persistent web startup error so an exit code such as 255 is accompanied by the actual GStreamer error text.

### MD5 password storage

The web password is no longer written to `tvstreamer5-config.json` in clear text. TVStreamer5 stores only a lowercase MD5 digest in the `password_md5` field. Existing configurations that still contain a legacy plaintext `password` are migrated automatically on the next successful startup and rewritten without the clear-text field.

HTTP Basic authentication itself is unchanged for clients: the browser/player still sends the normal password, while TVStreamer5 computes its MD5 digest and compares it with the stored digest. Changing the password from the web settings also writes only `password_md5`.

MD5 is provided here for compatibility with the requested configuration format. It is a one-way hash, not encryption, and it is not suitable as a modern password-hardening algorithm against offline attacks. Protect the configuration file and use HTTPS/reverse-proxy TLS for the web interface.

### Satellite DVB-S / DVB-S2 primary input

A stream can use a Linux DVB satellite tuner instead of the normal primary URL. Enable **«Принимать канал со спутника DVB-S / DVB-S2 вместо основного URL»** in the stream editor. The normal primary URL/interface/mode controls are hidden and the satellite tuner panel becomes available.

The panel supports adapter/frontend selection, frequency, symbol rate, DVB-S or DVB-S2, H/V polarization, modulation, FEC, pilot, rolloff, DiSEqC source, DVB-S2 MIS/Stream ID, service/program SID and universal-LNB LOF1/LOF2/switch frequencies. TVStreamer5 uses GStreamer `dvbbasebin` so a configured `satellite_service_id` is passed through `program-numbers` and the selected service is exposed as MPEG-TS to the normal output/remap/transcoding path.

For operator convenience, the **Add channel** form accepts satellite transponder frequency in **MHz** (for example `11531`). TVStreamer5 converts it to the existing internal **kHz** representation (`11531000`) before saving the configuration or passing it to GStreamer, so existing configs and DVB tuning behavior remain compatible. Symbol rate remains in **kBd**. Universal-LNB LOF1/LOF2/switch values remain technical **kHz** values: `9750000`, `10600000`, `11700000`. `DiSEqC source = -1` disables DiSEqC and `Stream ID = -1` disables MIS selection.

Check tuner devices and the GStreamer DVB plugin on Linux:

```bash
ls -l /dev/dvb/adapter*/frontend*
gst-inspect-1.0 dvbbasebin
gst-inspect-1.0 dvbsrc
./scripts/check_transcoder_plugins.sh
```

For Docker, the DVB devices must be passed into the container explicitly, for example:

```bash
docker run --device /dev/dvb/adapter0/frontend0 \
           --device /dev/dvb/adapter0/demux0 \
           --device /dev/dvb/adapter0/dvr0 \
           ...
```

The satellite source works both for normal MPEG-TS forwarding/remap and for the external GStreamer transcoder. With transcoding enabled, `dvbbasebin` feeds the selected program to `decodebin`, then the existing H.264/AAC and CBR output pipeline is used.

### DVB hardware discovery and transponder service scan

The satellite editor no longer asks for adapter/frontend numbers blindly. TVStreamer5 enumerates `/dev/dvb/adapter*` and each `frontend*` at runtime through `/api/dvb-devices`. The web form uses those results to build **Adapter** and **Frontend** drop-down lists. When the frontend can be opened, the list also shows its kernel-reported name and delivery systems such as DVB-S/DVB-S2.

The CA Provider/Card Manager also inventories serial readers from `/dev/serial/by-id/*` and resolves the current `/dev/ttyUSB*` or `/dev/ttyACM*` target plus udev vendor/model/serial metadata. Providers store the stable by-id path rather than a volatile tty number, so a reader remains associated with the same `ca-card-N` after reboot or USB renumbering.

A new **«Сканировать транспондер»** action tunes the exact DVB-S/S2 parameters currently entered in the form through `dvbbasebin`, reads the resulting MPEG-TS and parses PAT, SDT and PMT tables. The result includes:

- service/program SID;
- service and provider names;
- PMT PID;
- first video and audio PIDs;
- detected video/audio codec names;
- FTA/CA indication from SDT/PMT CA descriptors;
- frontend lock/signal/SNR information when exposed by the driver.

Selecting a discovered service copies its SID into the satellite service selector and also fills the output service name/provider and VPID/APID fields when they were discovered. Scanning is refused while another active TVStreamer stream is already using the same adapter/frontend, because retuning it would interrupt that channel.

The scanner uses the same GStreamer DVB source and the same frequency/symbol-rate/LNB/DiSEqC/MIS properties as normal satellite streaming, so it does not require `dvbv5-scan` or a separate channel database.

Conditional-access note: the Reader/Card Manager discovers and tracks Phoenix-style serial readers, associates them with per-card providers and enforces per-provider session capacity. It does **not** implement ECM/CW extraction, a software descrambler or a CW cache. Encrypted services are shown with the `CA` marker; descrambling still requires an authorized card/CAM integration.

### DVB tile signal meters

For satellite streams the main stream tile now shows two live percentage meters read from the selected Linux DVB frontend: **Signal level** (`FE_READ_SIGNAL_STRENGTH`) and **Signal quality** / SNR (`FE_READ_SNR`). The bars refresh with the normal `/api/state` polling and are colour-coded red / amber / green. A LOCK / NO LOCK indicator is shown beside the signal level. When the stream is stopped or running on a backup source the DVB meters fall back to 0%.

The redundant top **Output** information row was removed from stream tiles; output mode is still visible in the tile badge and URLs remain available through the URL button.

### DVB startup failure hardening

If a DVB frontend cannot start (for example because the selected adapter/frontend is already in use, inaccessible, or cannot lock with the requested parameters), TVStreamer5 now returns the complete GStreamer startup error without disposing `dvbbasebin`/`dvbsrc` while they are still in READY. The failed pipeline is explicitly driven to `GST_STATE_NULL` before the final reference is released. This prevents the GStreamer critical warning and heap-corruption/process-abort path that could otherwise turn a normal DVB startup failure into a web `Failed to fetch` error.

When a satellite stream reports `GstDvbSrc: Failed to start`, check the selected frontend for another owner before changing tuning parameters, for example with `sudo fuser -v /dev/dvb/adapter5/frontend0 /dev/dvb/adapter5/demux0 /dev/dvb/adapter5/dvr0` and `ps -fp <PID>`. An existing Astra/other receiver process using the same hardware must be stopped or a different free frontend selected.


### Satellite channel add workflow

Satellite tuner setup and transponder scanning now live in a dedicated **Add channel** modal instead of the normal stream editor. The modal discovers the Linux DVB adapters/frontends, accepts the DVB-S/S2 tuning parameters, scans PAT/SDT/PMT, and presents the discovered services as a searchable multi-select list.

After selecting one or more services, TVStreamer5 can create all corresponding stream tiles in one save operation. Each generated tile inherits the chosen adapter/frontend/transponder/LNB/MIS settings and gets its service SID, name/provider and discovered video/audio PIDs automatically. UDP output allocation can either advance the multicast IPv4 address while keeping one port or keep one address and advance the port. Generated tiles start stopped so the operator can review output/remap/transcode settings before starting them.

The regular stream editor no longer exposes satellite tuning or scanning controls. Editing an existing satellite tile preserves its DVB source configuration while allowing normal output, backup, remap and transcoder settings to be changed.

### Shared DVB frontend fan-out

Satellite channels that use the same Linux DVB `adapter/frontend` and exactly the same transponder tuning parameters now share one physical tuner session. TVStreamer5 starts one `dvbbasebin` per tuned frontend, forwards the complete transport stream through an internal loopback multicast relay, then creates a lightweight per-channel SID selector (`tsdemux program-number -> mpegtsmux`) for each active tile. Each selected service is exposed on its own localhost UDP relay, so the existing passthrough, remap, transcoding and output protocol code can consume it without opening the DVB frontend again.

This allows multiple services discovered on one transponder to run simultaneously from one frontend. Starting another tile on the same frontend and same transponder increments the shared frontend consumer count; stopping a tile removes only its service relay. The physical frontend is released only after the last channel using that transponder stops. Attempting to tune the same frontend to a different transponder while it still has active consumers is rejected with a clear startup error instead of retuning and interrupting the other channels.

Expected diagnostics include:

```text
Shared DVB frontend started: 5:0 ... relay=udp://@239.255.250.x:45xxx
Shared DVB frontend reused: 5:0 consumers=2 ...
Satellite service relay started: stream=... SID=230 ... service=udp://127.0.0.1:47xxx
```

The stream-tile CBR/VBR badge is positioned beside the delete button so the tile header no longer has the bitrate-mode badge floating in the center.

### Compact satellite channel wizard

The **Add channel** satellite wizard uses a denser four-column layout for the common tuner parameters (adapter, frontend, delivery system, polarization, frequency, symbol rate, modulation and FEC). Less frequently changed Pilot, Rolloff, DiSEqC, MIS, LNB and CA/CI settings are grouped in a collapsible **Additional DVB / LNB / CA settings** section. Signal meters, service search/multi-selection and bulk-output allocation remain visible without requiring a very tall modal.

On narrower screens the wizard automatically collapses to two columns and then one column. The discovered-service list is kept in a bounded scroll area so the scan controls and bulk-create actions stay close together.

## v85: Dynamic Reader/Card Manager

`System -> CA Providers` no longer assumes a fixed number of Phoenix readers or a global eight-channel limit. The UI discovers any number of `/dev/serial/by-id/*` serial readers, resolves their current tty device and udev metadata, and lets each `ca-card-N` provider bind to one stable reader identity. Readers can disappear and return without changing the configured provider association.

Each card/provider owns its own session capacity. `capacity_mode=manual` uses its configured `max_channels`; `capacity_mode=auto` is reserved for a documented card/provider capability interface and currently falls back to that same per-card value if no capability is reported. New providers start with a conservative fallback of 1 rather than a hard-coded 8. The start API rejects a stream when its selected card/provider is disabled, has no reader, the reader is offline, or that provider's effective capacity is already full.

The old `Authorized pre-decoded TS` endpoint is no longer part of the CA Provider UI in v85. Selecting a card/provider does not replace the DVB input transport. This release provides reader discovery, stable identity, hot-plug status and session accounting only; it does not implement ECM/CW extraction, software descrambling or key caching.
