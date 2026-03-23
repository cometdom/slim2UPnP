# slim2UPnP

Slimproto to UPnP bridge with native DSD support.

slim2UPnP connects a Logitech Media Server (LMS) to any UPnP/DLNA renderer, acting as a virtual player. Unlike squeeze2upnp, it provides full DSD playback (DSF, DFF, DoP) through a single, clean binary with no external dependencies on Squeezelite.

## Why slim2UPnP?

| | squeeze2upnp | slim2UPnP |
|---|---|---|
| **DSD** | No native DSD | Full DSD support (DSF/DFF/DoP) |
| **Architecture** | Squeezelite + bridge | Single binary |
| **Slimproto** | Delegated to Squeezelite | Native implementation |
| **Decoder** | External | Internal (libFLAC, FFmpeg) |
| **Distribution** | Complex build chain | Precompiled binaries possible |

## Quick Start

```bash
# Build
cmake -B build
cmake --build build

# List available UPnP renderers
./build/slim2upnp --list-renderers

# Play through a renderer (auto-discover LMS)
./build/slim2upnp -r "My Renderer"

# Specify LMS server explicitly
./build/slim2upnp -r "My Renderer" -s 192.168.1.10

# Direct connection (skip SSDP, useful for cross-subnet setups)
./build/slim2upnp --renderer-url http://192.168.1.104:4005/description.xml
```

## Architecture

```
LMS (network)
  -> slim2UPnP (single process)
       -> SlimprotoClient (TCP port 3483)    : control protocol
       -> HttpStreamClient (port 9000)       : encoded audio from LMS
       -> Decoder (FLAC/PCM/DSD/MP3/OGG/AAC) : decode to raw audio
       -> AudioHttpServer (auto port)        : serves decoded audio via HTTP
       -> UPnPController (SSDP + SOAP)       : controls the UPnP renderer
            -> UPnP Renderer (e.g., DirettaRendererUPnP)
                 -> DAC
```

**Audio flow** (pull model): the decode thread fills a ring buffer, the UPnP renderer pulls audio from the AudioHttpServer via HTTP GET.

**Threading**: main (init/signals) + slimproto (TCP) + audio (decode) + HTTP server (serve).

## Options

```
LMS Connection:
  -s, --server <ip>        LMS server address (auto-discover if omitted)
  -p, --port <port>        Slimproto port (default: 3483)
  -n, --name <name>        Player name (default: slim2UPnP)
  -m, --mac <addr>         MAC address (default: auto-generate)

UPnP Renderer:
  -r, --renderer <name>    Renderer name (substring match)
  --renderer-uuid <uuid>   Renderer UUID (exact match)
  --renderer-url <url>     Direct description URL (skip SSDP)
  --http-port <port>       HTTP server port (default: auto)
  --interface <iface>      Network interface (default: auto)
  -l, --list-renderers     List available renderers and exit

Audio:
  --max-rate <hz>          Max sample rate (default: 1536000)
  --no-dsd                 Disable DSD support
  --decoder <backend>      Decoder: native (default), ffmpeg

Logging:
  -v, --verbose            Debug output
  -q, --quiet              Errors and warnings only

Other:
  -V, --version            Show version
  -h, --help               Show help
```

## Building

### Dependencies

**Required:**
- C++17 compiler (GCC 7+, Clang 5+)
- CMake 3.10+
- libFLAC (`libflac-dev`)
- libupnp (`libupnp-dev`)
- pthreads

**Optional:**
- libmpg123 (`libmpg123-dev`) — MP3 decoding
- libvorbis (`libvorbis-dev`) — Ogg Vorbis decoding
- fdk-aac (`libfdk-aac-dev`) — AAC decoding
- FFmpeg (`libavcodec-dev`, `libavutil-dev`) — alternative decoder backend

### Install dependencies

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake libflac-dev libupnp-dev \
  libmpg123-dev libvorbis-dev libavcodec-dev libavutil-dev

# Fedora
sudo dnf install gcc-c++ cmake flac-devel libupnp-devel \
  mpg123-devel libvorbis-devel ffmpeg-free-devel

# Arch
sudo pacman -S base-devel cmake flac libupnp mpg123 libvorbis ffmpeg
```

### Precompiled binaries

Static binaries (no shared library dependencies) are available in [GitHub Releases](../../releases) for the most common architectures:

| Binary | Architecture | Target |
|--------|-------------|--------|
| `slim2upnp-x64-v2` | x86_64 baseline | Any Intel/AMD 64-bit PC |
| `slim2upnp-x64-v3` | x86_64 AVX2 | Modern PCs (2013+, Haswell and newer) |
| `slim2upnp-aarch64` | ARM64 | Raspberry Pi 4/5, ARM servers |

```bash
# Download, install and run (example for x64-v2)
chmod +x slim2upnp-x64-v2
sudo cp slim2upnp-x64-v2 /usr/local/bin/slim2upnp
slim2upnp --list-renderers
```

> **Note:** Each binary is compiled for a specific CPU architecture. A binary built on x86_64 will not run on ARM64 and vice versa. Runtime libraries (libFLAC, libupnp, etc.) must be installed on the target system — the installer handles this automatically.

### Build from source

```bash
# Standard build (auto-detect SIMD: AVX2, AVX-512, NEON...)
cmake -B build
cmake --build build

# Static binary (portable across distros of same architecture)
cmake -B build-static -DSTATIC_BUILD=ON
cmake --build build-static

# Production build (no verbose logging)
cmake -B build-prod -DNOLOG=ON
cmake --build build-prod

# Target a specific x86 level (for cross-compilation or CI)
cmake -B build -DTARGET_MARCH=v2       # x86-64 baseline (maximum compatibility)
cmake -B build -DTARGET_MARCH=v3       # AVX2 (better performance on modern CPUs)
cmake -B build -DTARGET_MARCH=v4       # AVX-512
```

## Installation

### Quick install (no compilation needed)

```bash
git clone https://github.com/cometdom/slim2UPnP.git
cd slim2UPnP
sudo ./install.sh
```

That's it. The installer automatically:
1. Detects your CPU architecture (x86_64, ARM64)
2. Downloads the right precompiled static binary from GitHub Releases
3. Verifies the SHA256 checksum
4. Detects your init system (systemd or OpenRC)
5. Installs the binary and sets up the service

Other install modes:
```bash
sudo ./install.sh --build        # Build from source instead of downloading
sudo ./install.sh /path/to/bin   # Install a specific binary
sudo ./install.sh --uninstall    # Remove everything
```

### systemd (Debian, Ubuntu, Fedora, Arch, AudioLinux)

```bash
sudo ./install.sh

# Edit configuration (set renderer and LMS)
sudo nano /etc/default/slim2upnp

# Start and enable at boot
sudo systemctl start slim2upnp
sudo systemctl enable slim2upnp

# Check status and logs
sudo systemctl status slim2upnp
sudo journalctl -u slim2upnp -f
```

### OpenRC (Gentoo, GentooPlayer)

```bash
sudo ./install.sh

# Edit configuration (set renderer and LMS)
sudo nano /etc/conf.d/slim2upnp

# Start and enable at boot
sudo rc-service slim2upnp start
sudo rc-update add slim2upnp default

# Check status and logs
sudo rc-service slim2upnp status
tail -f /var/log/slim2upnp.log
```

### Configuration

The config file (`/etc/default/slim2upnp` or `/etc/conf.d/slim2upnp`) contains `SLIM2UPNP_OPTS` with all command-line options. Examples:

```bash
# Minimal — auto-discover LMS, find renderer by name
SLIM2UPNP_OPTS="-r \"Diretta Renderer\""

# Explicit LMS + player name
SLIM2UPNP_OPTS="-r \"Diretta Renderer\" -s 192.168.1.10 -n \"Living Room\""

# Direct URL (skip SSDP, good for cross-subnet)
SLIM2UPNP_OPTS="--renderer-url http://192.168.1.104:4005/description.xml -s 192.168.1.10"

# Verbose logging for troubleshooting
SLIM2UPNP_OPTS="-r \"Diretta Renderer\" -s 192.168.1.10 -v"
```

Run `slim2upnp --list-renderers` to find available renderers on your network.

### Uninstall

```bash
sudo ./install.sh --uninstall
```

This stops the service, removes the binary, WebUI, and service files. Config files are kept (remove manually if desired).

## Web Configuration UI

slim2UPnP includes a web-based configuration interface — no need to edit config files manually.

After installation, open your browser at:

```
http://<your-machine-ip>:8082
```

The WebUI lets you:
- Set the UPnP renderer name or direct URL
- Configure the LMS server address and player name
- Toggle DSD, decoder backend, network options
- Save settings and restart the service with one click

The WebUI is installed automatically by `install.sh` and runs as a separate service (`slim2upnp-webui`). It requires Python 3.

## DSD Support

slim2UPnP handles DSD natively:
- **DSF** and **DFF** container parsing
- **DoP** (DSD over PCM) detection and passthrough
- Raw DSD streaming
- DSD64 through DSD512+

The renderer's capabilities are queried via UPnP `GetProtocolInfo`. DSD is served as DSF container or DoP-in-WAV depending on what the renderer supports.

## Gapless Playback

Same-format tracks are chained seamlessly: the HTTP stream continues without closing the connection between tracks. Cross-format transitions (e.g., FLAC to DSD) cause a clean stop/restart cycle.

## License

MIT License. See [LICENSE](LICENSE).

Slimproto protocol implemented from public documentation (wiki.lyrion.org). No code copied from GPL projects.
