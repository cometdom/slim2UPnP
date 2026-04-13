# slim2UPnP v0.1.18 beta

Slimproto to UPnP bridge with native DSD support.

slim2UPnP connects a Logitech Media Server (LMS) to any UPnP/DLNA renderer, acting as a virtual player. It operates in **passthrough mode** — audio is proxied directly from LMS to the renderer without decoding. The renderer handles all format decoding (FLAC, DSD, MP3, etc.). Single binary, minimal dependencies, ~191KB.

---

## Support This Project

If you find this tool valuable, you can support development:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cometdom)

**Important notes:**
- Donations are **optional** and appreciated
- Help cover test equipment and coffee
- **No guarantees** for features, support, or timelines
- The project remains free and open source for everyone

---

## Why slim2UPnP?

| | squeeze2upnp | slim2UPnP |
|---|---|---|
| **DSD** | No native DSD | Full DSD passthrough (DSF/DFF/DoP) |
| **Architecture** | Squeezelite + bridge | Single binary, passthrough |
| **Slimproto** | Delegated to Squeezelite | Native implementation |
| **Decoding** | Internal | Passthrough (renderer decodes) |
| **Dependencies** | Many libraries | libupnp only |
| **Distribution** | Complex build chain | Precompiled binaries (~191KB) |

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
  -> slim2UPnP (single process, passthrough)
       -> SlimprotoClient (TCP port 3483)    : control protocol
       -> HttpStreamClient (port 9000)       : encoded audio from LMS
       -> AudioHttpServer (auto port)        : proxies audio to renderer via HTTP
       -> UPnPController (SSDP + SOAP)       : controls the UPnP renderer
            -> UPnP Renderer (e.g., DirettaRendererUPnP)
                 -> DAC
```

**Audio flow** (passthrough): the audio thread fetches encoded audio from LMS and writes it to a ring buffer. The UPnP renderer pulls audio from the AudioHttpServer via HTTP GET. No decoding — the renderer handles all format decoding.

**Threading**: main (init/signals) + slimproto (TCP) + audio (fetch) + HTTP server (serve).

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
- libupnp (`libupnp-dev`)
- pthreads

No decoder libraries needed — slim2UPnP operates in passthrough mode.

### Install dependencies

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake libupnp-dev

# Fedora
sudo dnf install gcc-c++ cmake libupnp-devel

# Arch
sudo pacman -S base-devel cmake libupnp
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

> **Note:** Each binary is compiled for a specific CPU architecture. A binary built on x86_64 will not run on ARM64 and vice versa. Static binaries have no runtime library dependencies.

### Build from source

```bash
# Standard build with GCC (auto-detect SIMD: AVX2, AVX-512, NEON...)
cmake -B build
cmake --build build

# Clang + LTO (Link-Time Optimization — recommended for audio performance)
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DENABLE_LTO=ON
cmake --build build

# Or via install.sh:
sudo LLVM=1 ./install.sh --build

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

> **Clang + LTO:** Requires `clang` and `clang++` installed. On Debian/Ubuntu: `sudo apt install clang`, Fedora: `sudo dnf install clang`, Arch: `sudo pacman -S clang`, Gentoo: `sudo emerge clang`.

## Installation & Configuration

### Step 1 — Install

```bash
git clone https://github.com/cometdom/slim2UPnP.git
cd slim2UPnP
sudo ./install.sh
```

### Update to latest version

```bash
cd slim2UPnP
git pull
sudo ./install.sh
```

The installer will stop the running service, download/install the latest binary, and restart the service automatically.

That's it. The installer automatically:
1. Detects your CPU architecture (x86_64, ARM64)
2. Downloads the right precompiled binary from GitHub Releases
3. Verifies the SHA256 checksum
4. Detects your init system (systemd or OpenRC)
5. Installs the binary, the service, and the Web Configuration UI

### Step 2 — Configure via the Web UI

Open your browser and go to:

```
http://<your-machine-ip>:8082
```

From the Web UI you can:
- **Select your UPnP renderer** (enter its name, e.g. "Diretta Renderer")
- **Set the LMS server** IP address (or leave empty for auto-discovery)
- **Choose a player name** (shown in the LMS interface)
- **Toggle audio options** (DSD passthrough)
- **Save & Restart** the service with one click

No command line, no config file editing needed.

> **Tip:** To find the name of your renderer, check the LMS web interface or run `slim2upnp --list-renderers` from a terminal.

### Step 3 — Start playing

Once configured, select **slim2UPnP** as the player in LMS and press play.

The service starts automatically at boot. You can manage it from the Web UI or with these commands:

**systemd** (Debian, Ubuntu, Fedora, Arch, AudioLinux):
```bash
sudo systemctl status slim2upnp      # check status
sudo journalctl -u slim2upnp -f      # follow logs
sudo systemctl restart slim2upnp     # restart
```

**OpenRC** (Gentoo, GentooPlayer):
```bash
sudo rc-service slim2upnp status     # check status
tail -f /var/log/slim2upnp.log       # follow logs
sudo rc-service slim2upnp restart    # restart
```

### Uninstall

```bash
sudo ./install.sh --uninstall
```

### Advanced: manual configuration

<details>
<summary>Click to expand (for advanced users only)</summary>

The config file is located at `/etc/default/slim2upnp` (systemd) or `/etc/conf.d/slim2upnp` (OpenRC). It contains simple variables:

```bash
RENDERER="Diretta Renderer"
LMS_SERVER="192.168.1.10"
PLAYER_NAME="Living Room"
```

Other install modes:
```bash
sudo ./install.sh --build        # Build from source instead of downloading
sudo ./install.sh /path/to/bin   # Install a specific binary
```

</details>

## DSD Support

slim2UPnP passes DSD streams directly to the renderer:
- **DSF** and **DFF** passthrough
- **DoP** (DSD over PCM) passthrough
- DSD64 through DSD512+

The renderer handles all DSD decoding.

## Gapless Playback

Gapless playback uses the standard UPnP mechanism: dual AudioHttpServer slots (A/B) with `SetNextAVTransportURI`. Each track is a separate HTTP stream. The renderer pre-connects to the next slot while finishing the current track.

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).

Commercial licensing available — contact via [GitHub Issues](https://github.com/cometdom/slim2UPnP/issues).

Slimproto protocol implemented from public documentation (wiki.lyrion.org). No code copied from GPL projects.
