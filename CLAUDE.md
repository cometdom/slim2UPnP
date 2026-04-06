# CLAUDE.md - slim2UPnP

This file provides guidance to Claude Code when working with this repository.

## Project Overview

**slim2UPnP** is a Slimproto→UPnP bridge in passthrough mode. It proxies audio from LMS directly to a UPnP renderer without decoding — the renderer handles all format decoding. This replaces squeeze2upnp (by Philippe/Ralphy) for users who want to use LMS with a UPnP renderer (e.g., DirettaRendererUPnP).

**Version**: 0.1.8-beta
**License**: GPL v3 (dual licensing: commercial license available). Slimproto protocol implemented from public documentation.

## Architecture

```
LMS (network)
  → slim2UPnP (single process, passthrough)
    → SlimprotoClient (TCP port 3483) : control protocol
    → HttpStreamClient (port 9000) : encoded audio stream from LMS
    → AudioHttpServer (auto port) : proxies audio to UPnP renderer (no decoding)
    → UPnPController (SSDP + AVTransport SOAP) : controls the UPnP renderer
      → UPnP Renderer (e.g., DirettaRendererUPnP)
        → DAC
```

**Audio flow** (passthrough/pull model): audio thread fetches encoded stream from LMS, writes it to ring buffer → UPnP renderer pulls via HTTP GET from AudioHttpServer. No decoding — the renderer handles all format decoding.

**Gapless architecture** (v0.1.0): Dual AudioHttpServer (slots A/B) with ping-pong. Each track is a separate HTTP stream. Gapless transitions use `SetNextAVTransportURI` — the renderer pre-connects to the next slot's URL while finishing the current track. This is the standard UPnP gapless mechanism.

**Threading**: main (init/signals) + slimproto (TCP LMS) + audio (HTTP fetch→ring buffer) + 2× HTTP server threads (serve to renderer, one per slot)

## File Structure

```
slim2UPnP/
  src/
    main.cpp                  — Entry point, CLI, orchestration, passthrough audio loop
    AudioHttpServer.h/cpp     — Mini HTTP server + ring buffer (proxies audio to renderer)
    UPnPController.h/cpp      — SSDP discovery + AVTransport/SOAP control (libupnp)
    SlimprotoClient.h/cpp     — Slimproto TCP protocol client
    SlimprotoMessages.h       — Binary protocol message structs
    HttpStreamClient.h/cpp    — HTTP audio stream fetcher
    Config.h                  — Configuration struct
    LogLevel.h                — Logging macros (LOG_ERROR/WARN/INFO/DEBUG)
  CMakeLists.txt              — Build system (only libupnp required)
  README.md                   — User documentation
  CHANGELOG.md                — Version history
  LICENSE                     — MIT license
```

Only 5 compiled source files: `main.cpp`, `AudioHttpServer.cpp`, `UPnPController.cpp`, `SlimprotoClient.cpp`, `HttpStreamClient.cpp`.

## Related Projects (same developer, same machine)

| Project | Path | Description |
|---------|------|-------------|
| slim2diretta | `/home/dominique/slim2Diretta/` | Native LMS player with Diretta output (mono-process) |
| DirettaRendererUPnP | `/home/dominique/DirettaRendererUPnP/` | UPnP renderer with Diretta output |
| squeeze2diretta | `/home/dominique/squeeze2diretta/` | Wrapper (Squeezelite + pipe → Diretta) |

### Code reuse from slim2diretta

The following components originated from slim2diretta (adapted for passthrough):
- `SlimprotoClient.cpp/h` — Slimproto TCP protocol client
- `SlimprotoMessages.h` — Binary protocol message structs
- `HttpStreamClient.cpp/h` — HTTP audio stream fetcher

All decoder components (FlacDecoder, PcmDecoder, DsdProcessor, DsdStreamReader, Mp3Decoder, OggDecoder, AacDecoder, FfmpegDecoder) were removed in v0.1.0.

### Components developed for slim2UPnP

- **UPnPController** (`src/UPnPController.h/cpp`) — SSDP discovery, direct URL connection, AVTransport/RenderingControl/ConnectionManager SOAP control via libupnp
- **AudioHttpServer** (`src/AudioHttpServer.h/cpp`) — Mini HTTP server with SPSC ring buffer, proxies encoded audio to renderer
- **main.cpp** (`src/main.cpp`) — Orchestration: passthrough audio loop, FLAC/DSF header parsing for duration, gapless chaining, MIME type mapping
- **Config.h** (`src/Config.h`) — UPnP-specific config (renderer name/UUID/URL, HTTP port, network interface)
- **LogLevel.h** (`src/LogLevel.h`) — Standalone logging (no DirettaSync dependency)

## Code Style (same as slim2diretta)

- **C++17** standard
- **Classes**: `PascalCase`
- **Functions**: `camelCase`
- **Members**: `m_camelCase`
- **Constants**: `UPPER_SNAKE_CASE`
- **Globals**: `g_camelCase`
- **Indentation**: 4 spaces

## Key Design Decisions

### Passthrough model (v0.1.0)
slim2UPnP proxies encoded audio from LMS directly to the UPnP renderer without decoding. The renderer handles all format decoding. This means:
- No decoder dependencies (libFLAC, FFmpeg, etc.) — dramatically smaller binary
- AudioHttpServer has a ring buffer (producer: HTTP fetch thread, consumer: HTTP server thread)
- Content-Type from LMS HTTP response is mapped to MIME type for SetAVTransportURI
- FLAC/DSF headers are parsed minimally for track duration (STMd timing), not for decoding

### Pull model (vs push in slim2diretta)
slim2diretta pushes audio packets to the Diretta target. slim2UPnP uses a **pull model**: the UPnP renderer connects to AudioHttpServer via HTTP GET and pulls audio data. This inversion means:
- AudioHttpServer has a ring buffer (producer: fetch thread, consumer: HTTP server thread)
- Flow control via ring buffer level (writeAudio blocks when full, handleClient blocks when empty)

### Direct renderer connection (`--renderer-url`)
SSDP multicast doesn't work in all environments (WSL2, cross-subnet). The `--renderer-url` option allows direct connection to a renderer's description XML, bypassing SSDP entirely.

## Slimproto Protocol

Implemented from public documentation (wiki.lyrion.org, SlimDevices wiki). Reference implementations consulted: Rust slimproto crate (MIT), Ada slimp (MIT). **No code copied from squeezelite (GPL v3).**

Key messages: HELO (registration), STAT (status), strm (stream control), audg (volume - forced 100%), setd (device name).

### Key STAT Events
- STMc: Connected
- STMd: Decoder ready (HTTP EOF)
- STMf: Flushed
- STMl: Buffer threshold reached (ready to play)
- STMp: Pause confirmed
- STMr: Resume confirmed
- STMs: Stream started (first bytes received)
- STMu: Track ended naturally

## UPnP Protocol

### Implemented UPnP services
- **AVTransport**: SetAVTransportURI, SetNextAVTransportURI, Play, Stop, Pause, GetPositionInfo, GetTransportInfo
- **RenderingControl**: SetVolume (force 100%), GetVolume
- **ConnectionManager**: GetProtocolInfo (check supported formats)

### Discovery
- SSDP via `UpnpSearchAsync` for `urn:schemas-upnp-org:device:MediaRenderer:1`
- Direct connection via `--renderer-url` (UpnpDownloadXmlDoc + XML parsing)

### Library
**libupnp (pupnp)** — same library as DirettaRendererUPnP (device side). slim2UPnP uses the control point side (`UpnpRegisterClient`, `UpnpSearchAsync`, `UpnpSendAction`).

## Lessons Learned (important pitfalls)

### Passthrough
- **Content-Type mapping is critical** — LMS sends `audio/flac`, `audio/x-wav`, `application/octet-stream` etc.; must map to correct MIME type for SetAVTransportURI
- **FLAC/DSF header parsing for duration only** — do NOT decode, just read metadata for STMd timing
- **Wall clock elapsed** — use `std::chrono::steady_clock` since there are no decoded samples to count

### Gapless (v0.1.0 architecture)
- **Dual AudioHttpServer** (slots A/B): each track is a separate HTTP stream with its own WAV/DSF header
- **SetNextAVTransportURI**: sent when cache drains, renderer pre-connects to next slot
- **signalEndOfStream** on old slot: renderer transitions to next track
- STMd delayed based on bytes-served vs total bytes (prevents early LMS UI switch)
- **Wall clock elapsed**: `steady_clock` based per-track position (no decoded samples in passthrough)
- 2-second gapless wait window before declaring track end
- Cross-format changes: renderer handles internally via SetNextAVTransportURI

### Gapless — failed approaches (DO NOT re-attempt)
- **Continuous WAV stream** (v0.0.1-v0.0.2): renderer sees one infinite track, `m_samplesPlayed` cumulates → erratic GetPositionInfo values (835s for a 3:45 track)
- **GetPositionInfo polling** (reverted): DirettaRendererUPnP returns erratic values; SOAP floods when called inline in decode loop
- **Delayed STMd based on pushedFrames** (reverted): loop exit condition deadlocked because elapsed never reached threshold
- **bytesReceived in STAT** (disabled): we download 10x faster than real-time → LMS position confusion

### Startup
- Retry renderer/server discovery indefinitely (important for systems without systemd restart like GentooPlayer/OpenRC)

## Build System

CMake with auto-detection (same pattern as slim2diretta):
- x64-v2 (baseline), x64-v3 (AVX2), x64-v4 (AVX-512)
- aarch64 (RPi4), aarch64-k16 (RPi5 16KB pages)
- Static build option: `-DSTATIC_BUILD=ON`
- Production build (no verbose logging): `-DNOLOG=ON`

## Dependencies

- **libupnp** (BSD) for UPnP control point (SSDP + SOAP + ixml)
- **POSIX threads** (pthreads)
- **C++17 runtime**

No decoder libraries required (passthrough mode). All codec dependencies (libFLAC, FFmpeg, libmpg123, libvorbis, fdk-aac) were removed in v0.1.0.

## Cross-Platform & Distribution

Unlike slim2diretta (which depends on the proprietary Diretta SDK), slim2UPnP has **zero proprietary dependencies**. This enables:

- **Precompiled static binaries** downloadable directly from GitHub Releases — no compilation needed for end users
- **Static builds** portable across distros of the same architecture (`-DSTATIC_BUILD=ON`)
- **Binaries are architecture-specific**: an x86_64 binary does not run on aarch64. Releases should provide separate binaries per architecture (x64-v2, x64-v3, aarch64)
- **All Linux platforms**: x64, aarch64 (RPi4/5) — single CI pipeline
- **Windows port** feasible: standard TCP + UPnP (only dependency) exist on Windows
- **macOS port** also possible with minimal effort

## Important Notes

- Volume forced to 100% for bit-perfect playback
- No automated tests — manual testing with LMS + UPnP renderer + DAC
- Primary target: Linux (cross-platform as stretch goal)
- Version is tracked in `src/main.cpp` (`SLIM2UPNP_VERSION`) and `CMakeLists.txt`
