# Changelog

## [0.1.23-beta] - 2026-04-30

### Fixed
- **Playback halts on multi-format playlist when sample rate changes** (e.g., 96kHz FLAC → 48kHz FLAC, both with same `audio/x-flac` MIME type): `SetNextAVTransportURI` was used because cross-format detection only checked MIME type. The renderer's anticipated preload (short HTTP probe of the next stream) consumed ~2MB from the ring buffer, the circular buffer overwrote those bytes before the real playback connection arrived, the current track ended prematurely with missing data, and the renderer drained its buffer before slim2UPnP queued the next URI. Now `parseTrackInfo` extracts sample rate / bit depth / channels from the FLAC STREAMINFO header, and any of those changing between successive tracks triggers the same cold restart (Stop + SetAVTransportURI + Play) that was already used for MIME-level changes since v0.1.15-beta. (Reported by Dominique)

### Changed
- Version updated to 0.1.23-beta

## [0.1.22-beta] - 2026-04-18

### Fixed
- **Clang ThinLTO still broken on Fedora** (regression fix for v0.1.21-beta): the previous attempt set `CMAKE_CXX_COMPILER_AR` in the parent CMake cache, but CMake's IPO validation runs in a **subproject** with its own cache, so the setting didn't propagate. Switched to manual `-flto=thin` flags for Clang (and `-flto` for GCC, which doesn't support ThinLTO). This bypasses CMake IPO entirely — no test static library is built, so `llvm-ar` is never needed. Works out of the box on Fedora, Arch, Debian, Alpine, regardless of whether the `llvm` tools package is installed. slim2UPnP only produces an executable (no static libraries), so `llvm-ar` was never actually required for our build.

### Changed
- Version updated to 0.1.22-beta

## [0.1.21-beta] - 2026-04-18

### Fixed
- **Clang LTO silently disabled on Fedora/Arch and Debian with versioned LLVM**: CMake's IPO check failed with `CMAKE_CXX_COMPILER_AR-NOTFOUND` because it defaulted to GNU binutils `ar`, which can't read LLVM bitcode. LTO was silently disabled even when `LLVM=1` was set. Now CMakeLists.txt asks Clang directly via `-print-prog-name` to locate `llvm-ar`/`llvm-ranlib` (handles both `/usr/bin/llvm-ar` and versioned paths like `/usr/lib/llvm-18/bin/llvm-ar`). Falls back to `find_program` with versioned candidates. (Reported by Dominique on Fedora 43)

### Changed
- Version updated to 0.1.21-beta

## [0.1.20-beta] - 2026-04-18

### Added
- **Web UI Stop button**: Added a Stop button alongside the existing Save & Restart and Restart Only buttons. Useful for users running slim2UPnP on their own Linux distributions to stop the service directly from the web UI. Supports both systemd and OpenRC. Includes a confirmation dialog.

### Fixed
- **Track tail truncated with Roon (raw PCM, format=p)**: Roon pre-buffers audio ahead of real-time, so HTTP EOF arrived 15-20s before the renderer had finished playing. With `trackDurationSec == 0`, STMd was sent immediately on EOF, Roon replied with `strm-q` (stop — Roon doesn't use Slimproto gapless), and the UPnP Stop call forced the renderer to drop its remaining buffer. Now the actual duration is computed from `totalBytes / (sampleRate × channels × bytesPerSample)` for raw PCM, so STMd waits until near real track end. (Reported by Progman)
- **Erratic Roon/LMS progress bar at track start**: The elapsed clock was started on the `Play()` SOAP call, but the renderer takes 1-5s to actually start playing (SOAP processing, HTTP connect, prebuffer, DAC ramp-up). Now slim2UPnP polls `GetTransportInfo` every 150ms after Play until the renderer reports `PLAYING`, then recalibrates the elapsed clock to that moment. Only `GetTransportInfo` is used (just the transport state string) — no `GetPositionInfo` polling, so no risk of flooding or erratic values with DirettaRendererUPnP. Can be disabled with `--no-play-calibration`. (Reported by Progman)

### Changed
- Version updated to 0.1.20-beta

## [0.1.19-beta] - 2026-04-16

### Fixed
- **Compilation error with libupnp 1.14.26+**: `UpnpRegisterClient` callback signature changed from `const void*` to `void*` in newer libupnp versions, causing build failure with Clang. Now uses `reinterpret_cast<Upnp_FunPtr>` to support both old and new libupnp signatures. (Reported by sheviks, issue #2)

### Changed
- **LTO options reworked**: replaced `ENABLE_LTO` (boolean) with `LTO_MODE` (OFF/thin/full). ThinLTO uses CMake IPO, Full LTO uses manual `-flto` flags to avoid the CMake flag conflict that made Full LTO impossible. (Reported by sheviks, issue #2)
- **Linker selection**: new `LINKER` CMake option (lld, mold, gold) to select linker via `-fuse-ld=`. Useful with Clang + LTO where `lld` is preferred over the default `ld`. (Reported by sheviks, issue #2)
- Version updated to 0.1.19-beta

## [0.1.18-beta] - 2026-04-13

### Added
- **Clang + LTO release binaries**: precompiled Clang + LTO variants now published on GitHub Releases alongside the GCC builds. Three new binaries: `slim2upnp-x64-v2-clang-lto`, `slim2upnp-x64-v3-clang-lto`, `slim2upnp-aarch64-clang-lto`. May offer better audio performance than GCC builds. (Requested by Hoorna/Alfred)

### Changed
- Version updated to 0.1.18-beta

## [0.1.17-beta] - 2026-04-12

### Fixed
- **Gapless broken on large WAV files with unknown duration (ultra hi-res 705.6/768 kHz)**: When the track duration was unknown (WAV streaming without Content-Length), slim2UPnP waited for the renderer's transport state to reach STOPPED before sending STMd. By that time, the renderer had already released the Diretta target, and the next track's `SetNextAVTransportURI` arrived too late for a clean gapless transition. Now sends STMd immediately after HTTP EOF when duration is unknown, so LMS queues the next track while the renderer still has buffered audio to play. (Reported by abase/Heinz)

---

## [0.1.16-beta] - 2026-04-12

### Added
- **Clang + LTO build support**: build with `sudo LLVM=1 ./install.sh --build` for Clang with Link-Time Optimization. CMakeLists.txt adds `ENABLE_LTO` option using CMake's IPO support. install.sh detects `LLVM=1`, checks for clang/clang++, and falls back to GCC with a warning if not installed. Same `LLVM=1` convention as DirettaRendererUPnP. (Inspired by shevick PR #64)

### Changed
- Version updated to 0.1.16-beta

## [0.1.15-beta] - 2026-04-12

### Fixed
- **Cross-format gapless transitions** (FLAC→WAV, Qobuz→local): `SetNextAVTransportURI` triggers an "anticipated preload" in DirettaRendererUPnP where the renderer opens a short-lived HTTP connection to probe the format. This consumes data from the ring buffer (including the WAV/FLAC header), and the circular buffer overwrites the original data before the real playback connection arrives. Now cross-format transitions use `Stop + SetAVTransportURI + Play` (cold restart) instead of `SetNextAVTransportURI`. Same-format gapless (FLAC→FLAC) still uses `SetNextAVTransportURI` as before.
- Reverted the ineffective ring buffer readPos restore from v0.1.14 (circular buffer overwrites made it useless)

### Changed
- Version updated to 0.1.15-beta

## [0.1.14-beta] - 2026-04-12

### Fixed
- **Gapless transition fails between Qobuz and local files**: DirettaRendererUPnP uses "anticipated preload" to probe the next track's format — it opens a short-lived HTTP connection, reads a few KB, then closes. This consumed data from the ring buffer (including the WAV/FLAC header). When the real playback connection arrived, the header was gone and the renderer rejected the stream ("Connection reset by peer"). Now the ring buffer read position is restored when a client disconnects prematurely, so the next client gets the full stream from the beginning. (Reported by Dominique)

### Changed
- Version updated to 0.1.14-beta

## [0.1.13-beta] - 2026-04-12

### Fixed
- **Gapless cross-format transitions broken** (Qobuz → local or vice versa): format parameters (`formatCode`, `pcmRate`, `pcmSize`, `pcmChannels`) were captured by value in the audio thread lambda and never updated on gapless chain. When transitioning from FLAC to WAV (or PCM), the audio thread still used the first track's format code, causing incorrect format handling for the second track. Now the format parameters are updated from the PendingTrack when chaining. (Reported by Dominique)

### Changed
- Version updated to 0.1.13-beta

## [0.1.12-beta] - 2026-04-11

### Fixed
- **2-minute stream timeout with LMS/Roon**: Streams (especially Qobuz FLAC via LMS or Roon) would stop around 1m59s–2m00s, while JPlay iOS streaming the same tracks worked fine. Two bugs combined:
  1. `updateBufferState()` was declared but never called — `streamBufSize`/`streamBufFull` reported as 0 in every STMt
  2. `bytesRecvHi/Lo` were hardcoded to 0 based on an incorrect assumption about LMS position tracking
  Fixed by calling `updateBufferState()` in the stream loop with the AudioHttpServer ring buffer size/fill, and reporting real `m_bytesReceived` like Squeezelite does. Roon uses these values to verify the player is actively consuming data and was aborting streams at ~2 minutes when all were 0. (Reported by Dominique)

### Changed
- Version updated to 0.1.12-beta

## [0.1.10-beta] - 2026-04-10

### Fixed
- **`--http-port` / `HTTP_PORT` ignored**: the value was parsed from the CLI and config file but never passed to the AudioHttpServer instances, which always started with auto-selected random ports. Now the specified port is used for slot A and `port+1` for slot B (two consecutive ports are needed for gapless ping-pong). This allows users to open specific ports in their firewall instead of the full ephemeral range. (Reported on GitHub by a user on EndeavourOS/Celeron J4005)

### Changed
- Version updated to 0.1.10-beta
- `HTTP_PORT` documentation updated to clarify that two consecutive ports are used

## [0.1.9-beta] - 2026-04-10

### Fixed
- **Roon raw PCM now fully working** (tested at 192kHz/24bit with DirettaRendererUPnP). Root cause: `setFormat()` was called twice — first with a default 44100Hz for ring buffer sizing, then with the real rate after prebuffer. The second call reset ring buffer positions, causing the 256KB of prebuffered data to be effectively lost. The producer continued writing at position 0, but the HTTP stream cursor had advanced 256KB, creating a byte misalignment. At 24-bit stereo (6 bytes/frame), 262144 bytes is not a multiple of 6 → samples were misaligned by 4 bytes → white noise. Now the format is set upfront from strm-s parameters for raw PCM, avoiding the second setFormat and the position reset. This also resolves the ~0.22s truncation at track start.

### Added
- `pcmEndian` field now logged in strm-s messages for diagnostics

### Changed
- Version updated to 0.1.9-beta
- Roon users no longer need to enable FLAC compression as a workaround — raw PCM mode now works properly

## [0.1.8-beta] - 2026-04-06

### Fixed
- **SIGILL crash on older x86 CPUs** (Ivy Bridge, Celeron J4125, etc.): CI was not passing `TARGET_MARCH` to cmake, so the auto-detection used the CI runner's AVX2 support, compiling the "v2" binary with v3 instructions. Now explicitly passes `-DTARGET_MARCH=v2` to override auto-detection. (Reported by suur13, Hoorna)

### Changed
- Version updated to 0.1.8-beta

## [0.1.7-beta] - 2026-04-05

### Fixed
- **Roon PCM: false MP3 detection causing renderer crash**: when Roon sends raw PCM (`format=p`), the first audio samples could accidentally match the MP3 sync pattern (`0xFF 0xE0+`). slim2UPnP would then serve the stream as `audio.mp3`, causing DirettaRendererUPnP to crash. Magic bytes detection is now skipped when `format=p` — the stream is always served as WAV with a generated header.

### Known Limitations
- Roon raw PCM passthrough still has known issues (endianness, truncation at start). **Recommended: enable FLAC codec in Roon settings** for stable playback.

### Changed
- Version updated to 0.1.7-beta

## [0.1.6-beta] - 2026-04-01

### Fixed
- **No audio after renderer restart via web UI**: When DirettaRendererUPnP was restarted (e.g., "Save & Restart" in web UI), slim2UPnP kept using stale control URLs causing all SOAP actions to fail silently. Now handles UPnP BYEBYE/ALIVE advertisements to automatically reconnect and refresh URLs when the renderer restarts. Also detects SOAP connection failures for faster recovery. (Reported by Pascal)

---

## [0.1.5-beta] - 2026-03-29

### Fixed
- **Track end detection**: replaced fixed 10-second drain delay with renderer transport state polling (`GetTransportInfo`). STMd is now sent only when the renderer has actually finished playing, preventing premature track cutoff on long buffer drains (reported by Pascal)

### Changed
- Version updated to 0.1.5-beta

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
