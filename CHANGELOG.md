# Changelog

## [0.0.2] - 2026-03-23

### Added
- Web Configuration UI (port 8082) — configure renderer, LMS, audio options from a browser
- WebUI auto-installed by `install.sh`, runs as separate service (systemd + OpenRC)

### Fixed
- SSDP discovery: stopSignal logic was inverted, causing immediate exit
- DSF header: use non-zero sizes for FFmpeg demuxer compatibility
- DSD data format: convert planar to DSF block-interleaved (4096-byte blocks)
- Prebuffer race: renderer no longer connects before ring buffer has data
- SOAP calls (SetAVTransportURI/Play) now run in background thread to avoid blocking decode
- Elapsed time reset after prebuffer to prevent LMS confusion
- Seek handler: 500ms timeout + detach prevents blocking on stuck audio thread
- Startup reconnect loop: STMf only sent when audio was actually playing
- Stream generation counter prevents stale STAT events from detached threads
- Force exit after 3x Ctrl+C (no more unkillable process)
- Heartbeat logging: time-based throttle instead of flooding on ts=0
- install.sh: runtime deps installed before binary verification, visible errors
- Config file: all values quoted, clear examples with proper syntax

## [0.0.1] - 2026-03-23

Initial test release.

### Added
- Slimproto client (TCP port 3483) — native implementation from public documentation
- HTTP stream client for fetching audio from LMS
- Decoders: FLAC (libFLAC), PCM/WAV/AIFF (native), DSD/DSF/DFF (native)
- Optional decoders: MP3 (libmpg123), Ogg Vorbis (libvorbisfile), AAC (fdk-aac)
- Optional FFmpeg decoder backend (libavcodec)
- DSD support: DSF, DFF, raw DSD, DoP detection and passthrough
- UPnP control point: SSDP discovery + AVTransport/RenderingControl/ConnectionManager
- Direct renderer connection via `--renderer-url` (skip SSDP, useful for cross-subnet)
- Mini HTTP server (AudioHttpServer) serving decoded audio to UPnP renderer
- WAV streaming with unknown length (RIFF header, data size 0x7FFFFFFF)
- DSF streaming for DSD-capable renderers
- Gapless playback: same-format track chaining (PCM and DSD)
- Ring buffer with flow control between decode thread and HTTP server
- PCM bit depth conversion: S32_LE MSB-aligned to 16/24/32-bit WAV
- LMS autodiscovery via UDP broadcast
- Renderer listing (`--list-renderers`)
- Volume forced to 100% for bit-perfect playback
- Exponential backoff reconnection to LMS
- CMake build system with architecture auto-detection (x64-v2/v3/v4, aarch64)
- Static build option (`-DSTATIC_BUILD=ON`)
