# Changelog

## [0.1.4-beta] - 2026-03-27

### Fixed
- **Roon gapless**: STMd drain delay (10s after renderer consumes all data) enables proper track transitions when duration is unknown
- **Roon PCM passthrough**: raw PCM (format=p) now served with generated WAV header using Slimproto strm-s parameters (rate, size, channels). Renderer receives valid RIFF/WAV instead of unknown `/audio.bin`
- **Roon track truncation**: when track duration is unknown (FLAC with `total_samples=0`), STMd is deferred until renderer finishes, preventing premature stop
- **Stream drain**: `signalEndOfStream()` called after HTTP EOF so AudioHttpServer ring buffer drains properly and client disconnect is detected
- **WebUI OpenRC support** (GentooPlayer): service restart detects init system and uses `rc-service` for OpenRC

### Known Limitations
- Roon PCM at ≥192kHz/24bit produces white noise (endianness/padding mismatch — under investigation)
- Roon progress bar frozen (Roon uses its own timer, not Slimproto elapsed)

### Changed
- **License changed from MIT to GPL v3** (dual licensing: commercial license available)
- Version updated to 0.1.4-beta

## [0.1.3-beta] - 2026-03-27

### Fixed
- **Roon track truncation**: when track duration is unknown (Roon sends FLAC with `total_samples=0`), STMd is no longer sent prematurely based on bytes consumed. The wait loop now continues until LMS/Roon sends a natural stop, preventing tracks from being cut short before playback completes.
- **WebUI OpenRC support** (GentooPlayer): service restart now detects the init system and uses `rc-service` for OpenRC instead of `systemctl`

### Changed
- Version updated to 0.1.3-beta

## [0.1.2-beta] - 2026-03-27

### Fixed
- **Roon compatibility**: auto-detect audio format from magic bytes (fLaC, RIFF, DSD, FRM8, FORM, MP3 sync) when server sends no Content-Type header. URL extension and MIME type are set correctly for the renderer.
- **GitHub Releases**: CI no longer marks beta tags as prerelease — `install.sh` now finds the latest version correctly
- **Install script**: simplified dependencies (libupnp only), config merge on update preserves user settings (RENDERER, LMS_SERVER, etc.) while removing obsolete options (DECODER)
- **WebUI/config**: removed decoder backend option (Native/FFmpeg) — not applicable in passthrough mode

### Changed
- Version updated to 0.1.2-beta
- README: added update instructions (`git pull && sudo ./install.sh`)

## [0.1.0-beta] - 2026-03-26

### Major
- **Passthrough mode** — audio from LMS is proxied directly to the UPnP renderer without decoding
- All decoder dependencies removed (libFLAC, FFmpeg, libmpg123, libvorbis, fdk-aac)
- Binary reduced from ~500KB to 191KB, only 5 source files

### Added
- FLAC/DSF header parsing for track duration (STMd timing)
- Wall clock elapsed tracking
- Content-Type extraction from LMS HTTP response
- MIME type mapping for all formats (FLAC, WAV, DSF, DFF, MP3, AAC, OGG)

### Preserved
- SetNextAVTransportURI gapless playback (dual AudioHttpServer slots A/B)

## [0.0.3-beta] - 2026-03-25

### Added
- **Dual AudioHttpServer architecture** (ping-pong slots A/B) — each track is a separate HTTP stream
- **SetNextAVTransportURI** for proper UPnP gapless transitions — universal, works with any UPnP renderer
- UPnP renderer scan in WebUI ("Scan for renderers" button)
- Ko-fi donation link in README

### Changed
- Gapless mechanism: replaced continuous WAV stream with per-track streams via SetNextAVTransportURI
- Elapsed tracking: uses bytes-served from AudioHttpServer (per-track, starts at 0)
- STMd timing: delayed until renderer has consumed within 3s of track end
- Ring buffer doubled (PCM: 4s, DSD: 2s) for better buffering margin
- Prebuffer increased to 3s for all sample rates (eliminates startup glitch)
- bytesReceived not reported to LMS (was causing position confusion)
- Model/ModelName changed from slim2diretta to slim2upnp in HELO

### Fixed
- **Cross-format gapless**: tracks no longer truncated 10s before end
- **Same-format gapless**: Pink Floyd-style seamless transitions work perfectly
- **Startup glitch (~8s silence)**: eliminated by 3s prebuffer
- **Progress bar sync**: bytes-served approach with baseline subtraction
- Static binary SIGILL: libupnp built from source in CI (Alpine musl)
- libupnp API compatibility: supports both old and new callback signatures
- install.sh: robust AVX2 detection with x64-v2 fallback
- WebUI: renderer scan via --list-renderers integration

### Known Limitations
- LMS UI advances to next track ~10s early (prebuffer latency)
- DSD path not yet adapted to dual-server architecture
- First play glitch requires DirettaRendererUPnP v2.1.5 warmup fix

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
