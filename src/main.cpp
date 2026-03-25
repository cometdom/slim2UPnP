/**
 * @file main.cpp
 * @brief Main entry point for slim2UPnP
 *
 * Slimproto→UPnP bridge with native DSD support.
 * Single process: SlimprotoClient + Decoder + AudioHttpServer + UPnPController.
 */

#include "Config.h"
#include "SlimprotoClient.h"
#include "HttpStreamClient.h"
#include "Decoder.h"
#include "DsdStreamReader.h"
#include "DsdProcessor.h"
#include "AudioHttpServer.h"
#include "UPnPController.h"
#include "LogLevel.h"

#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <cstring>
#include <vector>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>

#define SLIM2UPNP_VERSION "0.0.1"

// ============================================
// Globals
// ============================================

LogLevel g_logLevel = LogLevel::INFO;
std::atomic<bool> g_running{true};
SlimprotoClient* g_slimproto = nullptr;

void signalHandler(int signal) {
    static int signalCount = 0;
    signalCount++;

    if (signalCount >= 3) {
        // Force exit after 3 signals (stuck process)
        std::cerr << "\nForced exit." << std::endl;
        _exit(1);
    }

    std::cout << "\nSignal " << signal << " received, shutting down..." << std::endl;
    g_running.store(false, std::memory_order_release);
    if (g_slimproto) {
        g_slimproto->stop();
    }
}

// ============================================
// LMS Autodiscovery
// ============================================

std::string discoverLMS(int timeoutSec = 5, int retries = 3) {
    for (int attempt = 0; attempt < retries; attempt++) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return "";

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(3483);
        bcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        const char msg = 'e';
        sendto(sock, &msg, 1, 0,
               reinterpret_cast<struct sockaddr*>(&bcastAddr), sizeof(bcastAddr));

        struct pollfd pfd = {sock, POLLIN, 0};
        if (poll(&pfd, 1, timeoutSec * 1000) > 0) {
            char buf[32];
            struct sockaddr_in serverAddr{};
            socklen_t slen = sizeof(serverAddr);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                                 reinterpret_cast<struct sockaddr*>(&serverAddr), &slen);
            ::close(sock);
            if (n > 0) {
                std::string ip = inet_ntoa(serverAddr.sin_addr);
                LOG_INFO("Discovered LMS at " << ip
                         << " (attempt " << (attempt + 1) << ")");
                return ip;
            }
        }
        ::close(sock);

        if (attempt < retries - 1) {
            LOG_DEBUG("Discovery attempt " << (attempt + 1) << " timed out, retrying...");
        }
    }
    return "";
}

// ============================================
// Renderer Listing
// ============================================

void listRenderers(const std::string& networkInterface) {
    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  Scanning for UPnP Renderers...\n"
              << "═══════════════════════════════════════════════════════\n" << std::endl;

    auto renderers = UPnPController::scanRenderers(networkInterface, 5);

    if (renderers.empty()) {
        std::cout << "  No renderers found.\n" << std::endl;
        return;
    }

    int idx = 1;
    for (const auto& r : renderers) {
        std::cout << "  #" << idx++ << "  " << r.friendlyName << "\n"
                  << "       UUID: " << r.uuid << "\n"
                  << "       AVTransport: "
                  << (r.avTransportControlURL.empty() ? "(none)" : "yes") << "\n"
                  << std::endl;
    }

    std::cout << "Usage:\n"
              << "  ./slim2upnp -r \"" << renderers[0].friendlyName << "\"\n"
              << "  ./slim2upnp -r \"" << renderers[0].friendlyName << "\" -s <LMS_IP>\n"
              << std::endl;
}

// ============================================
// CLI Parsing
// ============================================

Config parseArguments(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if ((arg == "--server" || arg == "-s") && i + 1 < argc) {
            config.lmsServer = argv[++i];
        }
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            config.lmsPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if ((arg == "--name" || arg == "-n") && i + 1 < argc) {
            config.playerName = argv[++i];
        }
        else if ((arg == "--mac" || arg == "-m") && i + 1 < argc) {
            config.macAddress = argv[++i];
        }
        else if ((arg == "--renderer" || arg == "-r") && i + 1 < argc) {
            config.rendererName = argv[++i];
        }
        else if (arg == "--renderer-uuid" && i + 1 < argc) {
            config.rendererUUID = argv[++i];
        }
        else if (arg == "--renderer-url" && i + 1 < argc) {
            config.rendererURL = argv[++i];
        }
        else if (arg == "--http-port" && i + 1 < argc) {
            config.httpServerPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (arg == "--interface" && i + 1 < argc) {
            config.networkInterface = argv[++i];
        }
        else if (arg == "--max-rate" && i + 1 < argc) {
            config.maxSampleRate = std::atoi(argv[++i]);
        }
        else if (arg == "--no-dsd") {
            config.dsdEnabled = false;
        }
        else if (arg == "--decoder" && i + 1 < argc) {
            config.decoderBackend = argv[++i];
            if (config.decoderBackend != "native" && config.decoderBackend != "ffmpeg") {
                std::cerr << "Invalid decoder backend. Use: native, ffmpeg" << std::endl;
                exit(1);
            }
        }
        else if (arg == "--list-renderers" || arg == "-l") {
            config.listRenderers = true;
        }
        else if (arg == "--version" || arg == "-V") {
            config.showVersion = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            config.verbose = true;
        }
        else if (arg == "--quiet" || arg == "-q") {
            config.quiet = true;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "slim2UPnP - Slimproto to UPnP bridge with DSD support\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "LMS Connection:\n"
                      << "  -s, --server <ip>      LMS server address (auto-discover if omitted)\n"
                      << "  -p, --port <port>      Slimproto port (default: 3483)\n"
                      << "  -n, --name <name>      Player name (default: slim2UPnP)\n"
                      << "  -m, --mac <addr>       MAC address (default: auto-generate)\n"
                      << "\n"
                      << "UPnP Renderer:\n"
                      << "  -r, --renderer <name>  Renderer name (substring match)\n"
                      << "  --renderer-uuid <uuid> Renderer UUID (exact match)\n"
                      << "  --renderer-url <url>   Direct description URL (skip SSDP discovery)\n"
                      << "  --http-port <port>     HTTP server port (default: auto)\n"
                      << "  --interface <iface>    Network interface (default: auto)\n"
                      << "  -l, --list-renderers   List available renderers and exit\n"
                      << "\n"
                      << "Audio:\n"
                      << "  --max-rate <hz>        Max sample rate (default: 1536000)\n"
                      << "  --no-dsd               Disable DSD support\n"
                      << "  --decoder <backend>    Decoder backend: native (default), ffmpeg\n"
                      << "\n"
                      << "Logging:\n"
                      << "  -v, --verbose          Debug output (log level: DEBUG)\n"
                      << "  -q, --quiet            Errors and warnings only (log level: WARN)\n"
                      << "\n"
                      << "Other:\n"
                      << "  -V, --version          Show version information\n"
                      << "  -h, --help             Show this help\n"
                      << "\n"
                      << "Examples:\n"
                      << "  " << argv[0] << " -r \"DirettaRenderer\"\n"
                      << "  " << argv[0] << " -r \"DirettaRenderer\" -s 192.168.1.10\n"
                      << "  " << argv[0] << " -r \"DirettaRenderer\" -s 192.168.1.10 -n \"Living Room\" -v\n"
                      << std::endl;
            exit(0);
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            exit(1);
        }
    }

    return config;
}

// ============================================
// DoP Detection
// ============================================

static bool detectDoP(const int32_t* samples, size_t numFrames, int channels) {
    if (numFrames < 16) return false;
    size_t check = std::min(numFrames, size_t(32));
    int matches = 0;
    uint8_t expected = 0;
    for (size_t i = 0; i < check; i++) {
        uint8_t marker = static_cast<uint8_t>(
            (samples[i * channels] >> 24) & 0xFF);
        if (i == 0) {
            if (marker != 0x05 && marker != 0xFA) return false;
            expected = (marker == 0x05) ? 0xFA : 0x05;
            matches++;
        } else {
            if (marker == expected) matches++;
            expected = (expected == 0x05) ? 0xFA : 0x05;
        }
    }
    return matches >= static_cast<int>(check * 9 / 10);
}

// ============================================
// PCM bit depth conversion: S32_LE MSB-aligned → target depth
// ============================================

/**
 * Convert S32_LE MSB-aligned samples to target bit depth for WAV output.
 * Returns number of bytes written.
 */
static size_t convertPcmBitDepth(const int32_t* src, uint8_t* dst,
                                  size_t numSamples, uint32_t targetBitDepth) {
    switch (targetBitDepth) {
    case 16:
        for (size_t i = 0; i < numSamples; i++) {
            int16_t val = static_cast<int16_t>(src[i] >> 16);
            dst[i * 2]     = static_cast<uint8_t>(val & 0xFF);
            dst[i * 2 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        }
        return numSamples * 2;

    case 24:
        for (size_t i = 0; i < numSamples; i++) {
            int32_t val = src[i] >> 8;
            dst[i * 3]     = static_cast<uint8_t>(val & 0xFF);
            dst[i * 3 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            dst[i * 3 + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
        }
        return numSamples * 3;

    case 32:
        std::memcpy(dst, src, numSamples * 4);
        return numSamples * 4;

    default:
        return 0;
    }
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  slim2UPnP v" << SLIM2UPNP_VERSION << "\n"
              << "  Slimproto to UPnP bridge with DSD support\n"
              << "═══════════════════════════════════════════════════════\n"
              << std::endl;

    // Log build capabilities
    std::cout << "Codecs: FLAC PCM"
#ifdef ENABLE_MP3
              << " MP3"
#endif
#ifdef ENABLE_OGG
              << " OGG"
#endif
#ifdef ENABLE_AAC
              << " AAC"
#endif
#ifdef ENABLE_FFMPEG
              << " [FFmpeg available]"
#endif
              << " DSD" << std::endl;

    Config config = parseArguments(argc, argv);

    if (config.decoderBackend == "ffmpeg") {
        std::cout << "Decoder: FFmpeg backend" << std::endl;
    }

    // Apply log level
    if (config.verbose) {
        g_logLevel = LogLevel::DEBUG;
        LOG_INFO("Verbose mode enabled (log level: DEBUG)");
    } else if (config.quiet) {
        g_logLevel = LogLevel::WARN;
    }

    // Handle immediate actions
    if (config.showVersion) {
        std::cout << "Version:  " << SLIM2UPNP_VERSION << std::endl;
        std::cout << "Build:    " << __DATE__ << " " << __TIME__ << std::endl;
        return 0;
    }

    if (config.listRenderers) {
        listRenderers(config.networkInterface);
        return 0;
    }

    // ============================================
    // Initialize UPnP Controller
    // ============================================

    auto upnp = std::make_unique<UPnPController>();
    if (!upnp->init(config.networkInterface)) {
        std::cerr << "Failed to initialize UPnP" << std::endl;
        return 1;
    }

    // Connect to renderer: direct URL or SSDP discovery
    if (!config.rendererURL.empty()) {
        // Direct connection (skip SSDP — useful for cross-subnet, WSL2, etc.)
        if (!upnp->connectDirect(config.rendererURL)) {
            std::cerr << "Failed to connect to renderer at " << config.rendererURL << std::endl;
            return 1;
        }
    } else {
        // SSDP discovery (retry indefinitely)
        std::string rendererMatch = config.rendererUUID.empty()
            ? config.rendererName : config.rendererUUID;

        if (!upnp->discoverRenderer(rendererMatch, &g_running)) {
            if (!g_running.load(std::memory_order_acquire)) return 0;
            std::cerr << "Failed to discover renderer" << std::endl;
            return 1;
        }
    }

    UPnPController* upnpPtr = upnp.get();

    // ============================================
    // Start Audio HTTP Server
    // ============================================

    auto audioServer = std::make_unique<AudioHttpServer>();

    // Use UPnP's detected IP for the audio server
    std::string serverIP = upnp->getServerIP();
    if (!serverIP.empty()) {
        audioServer->setLocalIP(serverIP);
    }

    if (!audioServer->start(config.httpServerPort)) {
        std::cerr << "Failed to start audio HTTP server" << std::endl;
        return 1;
    }

    AudioHttpServer* audioServerPtr = audioServer.get();

    // ============================================
    // Autodiscover LMS if not specified
    // ============================================

    if (config.lmsServer.empty()) {
        std::cout << "No LMS server specified, searching..." << std::endl;
        int logCycle = 0;
        while (g_running.load(std::memory_order_acquire)) {
            config.lmsServer = discoverLMS(2, 1);
            if (!config.lmsServer.empty()) break;
            if (++logCycle % 5 == 0) {
                std::cout << "Still searching for LMS server..." << std::endl;
            }
        }
        if (config.lmsServer.empty()) {
            audioServer->stop();
            upnp->shutdown();
            return 0;
        }
        std::cout << "Found LMS server: " << config.lmsServer << std::endl;
    }

    // Print configuration
    std::cout << "\nConfiguration:" << std::endl;
    std::cout << "  LMS Server: " << config.lmsServer << ":" << config.lmsPort << std::endl;
    std::cout << "  Player:     " << config.playerName << std::endl;
    std::cout << "  Renderer:   " << upnp->getRenderer().friendlyName << std::endl;
    std::cout << "  HTTP:       " << audioServer->getStreamURL() << std::endl;
    std::cout << "  Max Rate:   " << config.maxSampleRate << " Hz" << std::endl;
    std::cout << "  DSD:        " << (config.dsdEnabled ? "enabled" : "disabled") << std::endl;
    if (!config.macAddress.empty()) {
        std::cout << "  MAC:        " << config.macAddress << std::endl;
    }
    std::cout << std::endl;

    // ============================================
    // Create Slimproto client
    // ============================================

    auto slimproto = std::make_unique<SlimprotoClient>();
    g_slimproto = slimproto.get();

    auto httpStream = std::make_shared<HttpStreamClient>();
    std::thread audioTestThread;
    std::atomic<bool> audioTestRunning{false};
    std::atomic<bool> audioThreadDone{true};
    std::atomic<uint32_t> streamGeneration{0};  // Incremented on each strm-s

    // Gapless: pending next track
    struct PendingTrack {
        std::shared_ptr<HttpStreamClient> httpClient;
        std::string responseHeaders;
        char formatCode;
        char pcmSampleRate;
        char pcmSampleSize;
        char pcmChannels;
        char pcmEndian;
    };
    std::mutex pendingMutex;
    std::shared_ptr<PendingTrack> pendingNextTrack;
    std::atomic<bool> hasPendingTrack{false};

    // ============================================
    // Stream callback
    // ============================================

    slimproto->onStream([&](const StrmCommand& cmd, const std::string& httpRequest) {
        switch (cmd.command) {
            case STRM_START: {
                LOG_INFO("Stream start requested (format=" << cmd.format << ")");

                std::string streamIp = slimproto->getServerIp();
                if (cmd.serverIp != 0) {
                    struct in_addr addr;
                    addr.s_addr = cmd.serverIp;
                    streamIp = inet_ntoa(addr);
                }
                uint16_t streamPort = cmd.getServerPort();
                if (streamPort == 0) streamPort = SLIMPROTO_HTTP_PORT;

                // === SEEK/RESTART: thread stopping but not done yet ===
                if (!audioThreadDone.load(std::memory_order_acquire) &&
                    !audioTestRunning.load(std::memory_order_acquire)) {
                    LOG_DEBUG("[Seek] Waiting for audio thread to finish...");
                    if (audioTestThread.joinable()) {
                        // Wait with timeout to avoid blocking the slimproto thread
                        auto deadline = std::chrono::steady_clock::now()
                                      + std::chrono::milliseconds(500);
                        while (!audioThreadDone.load(std::memory_order_acquire) &&
                               std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        if (audioThreadDone.load(std::memory_order_acquire)) {
                            audioTestThread.join();
                        } else {
                            LOG_WARN("[Seek] Audio thread still running, detaching");
                            audioTestThread.detach();
                        }
                    }
                    audioThreadDone.store(true, std::memory_order_release);
                }

                // === GAPLESS PATH: audio thread active, queue next track ===
                if (!audioThreadDone.load(std::memory_order_acquire)) {
                    LOG_INFO("[Gapless] Audio thread active, queuing next track");

                    auto nextHttp = std::make_shared<HttpStreamClient>();
                    if (!nextHttp->connect(streamIp, streamPort, httpRequest)) {
                        LOG_ERROR("[Gapless] Failed to pre-connect next track");
                        slimproto->sendStat(StatEvent::STMn);
                        break;
                    }

                    slimproto->sendStat(StatEvent::STMc);
                    std::string respHeaders = nextHttp->getResponseHeaders();
                    slimproto->sendResp(respHeaders);
                    slimproto->sendStat(StatEvent::STMh);

                    {
                        std::lock_guard<std::mutex> lock(pendingMutex);
                        pendingNextTrack = std::make_shared<PendingTrack>(PendingTrack{
                            nextHttp, respHeaders,
                            cmd.format, cmd.pcmSampleRate, cmd.pcmSampleSize,
                            cmd.pcmChannels, cmd.pcmEndian
                        });
                        hasPendingTrack.store(true, std::memory_order_release);
                    }
                    break;
                }

                // === COLD START PATH ===
                // Note: do NOT send UPnP Stop here — SetAVTransportURI
                // replaces the current stream without forcing the renderer
                // to tear down its output connection (avoids ~8s glitch).

                // Join any previous audio thread
                if (audioTestThread.joinable()) {
                    audioTestThread.join();
                }

                // Reset audio server for new stream
                audioServerPtr->reset();

                // Connect HTTP stream
                if (!httpStream->connect(streamIp, streamPort, httpRequest)) {
                    LOG_ERROR("Failed to connect to audio stream");
                    slimproto->sendStat(StatEvent::STMn);
                    break;
                }

                slimproto->sendStat(StatEvent::STMc);
                slimproto->sendResp(httpStream->getResponseHeaders());
                slimproto->sendStat(StatEvent::STMh);

                slimproto->updateElapsed(0, 0);
                slimproto->updateStreamBytes(0);

                // Start audio decode thread
                char formatCode = cmd.format;
                char pcmRate = cmd.pcmSampleRate;
                char pcmSize = cmd.pcmSampleSize;
                char pcmChannels = cmd.pcmChannels;
                char pcmEndian = cmd.pcmEndian;
                audioTestRunning.store(true);
                audioThreadDone.store(false, std::memory_order_release);
                uint32_t thisGeneration = streamGeneration.fetch_add(1) + 1;
                audioTestThread = std::thread([&httpStream, &slimproto,
                    &audioTestRunning, &audioThreadDone, &hasPendingTrack,
                    &pendingMutex, &pendingNextTrack, &streamGeneration,
                    thisGeneration,
                    formatCode, pcmRate, pcmSize, pcmChannels, pcmEndian,
                    audioServerPtr, upnpPtr, &config]() {

                    bool openFailedInGapless = false;

                    // ============================================================
                    // DSD PATH
                    // ============================================================
                    if (formatCode == FORMAT_DSD) {
                      char dsdPcmRate = pcmRate;
                      char dsdPcmChannels = pcmChannels;
                      bool dsdFirstTrack = true;
                      AudioHttpServer::AudioFormat prevDsdFmt{};

                      while (true) {  // === DSD CHAINING LOOP ===
                        auto dsdReader = std::make_unique<DsdStreamReader>();

                        uint32_t hintRate = sampleRateFromCode(dsdPcmRate);
                        uint32_t hintCh = (dsdPcmChannels == '2') ? 2
                                        : (dsdPcmChannels == '1') ? 1 : 2;
                        if (hintRate > 0) {
                            dsdReader->setRawDsdFormat(hintRate, hintCh);
                        }

                        slimproto->sendStat(StatEvent::STMs);

                        if (!dsdFirstTrack) {
                            slimproto->updateElapsed(0, 0);
                            slimproto->updateStreamBytes(0);
                        }

                        uint8_t httpBuf[65536];
                        uint64_t totalBytes = 0;
                        bool formatLogged = false;
                        uint64_t lastElapsedLog = 0;
                        uint64_t gaplessBytesOffset = 0;
                        std::atomic<bool> playStarted{false};

                        constexpr size_t DSD_PLANAR_BUF = 16384;
                        uint8_t planarBuf[DSD_PLANAR_BUF];

                        // DSF block interleave: convert planar [L...][R...] to
                        // DSF block format [4096 L][4096 R][4096 L][4096 R]...
                        constexpr size_t DSF_BLOCK_SIZE = 4096;
                        std::vector<uint8_t> dsfInterleaveBuf;

                        // Convert planar DSD to DSF block-interleaved and write to server
                        auto writeDsfBlockInterleaved = [&](const uint8_t* planar, size_t totalBytes,
                                                            uint32_t ch) -> size_t {
                            if (ch < 2 || totalBytes < 2) {
                                // Mono: write directly
                                return audioServerPtr->writeAudio(planar, totalBytes);
                            }
                            // Planar layout: [L0..Ln][R0..Rn], each half = totalBytes/2
                            size_t bytesPerCh = totalBytes / ch;
                            const uint8_t* left = planar;
                            const uint8_t* right = planar + bytesPerCh;

                            dsfInterleaveBuf.resize(totalBytes);
                            size_t outPos = 0;
                            size_t srcPos = 0;

                            while (srcPos < bytesPerCh) {
                                size_t blockBytes = std::min(DSF_BLOCK_SIZE, bytesPerCh - srcPos);
                                // L block
                                std::memcpy(dsfInterleaveBuf.data() + outPos, left + srcPos, blockBytes);
                                outPos += blockBytes;
                                // Pad to DSF_BLOCK_SIZE if needed
                                if (blockBytes < DSF_BLOCK_SIZE) {
                                    std::memset(dsfInterleaveBuf.data() + outPos, 0, DSF_BLOCK_SIZE - blockBytes);
                                    outPos += DSF_BLOCK_SIZE - blockBytes;
                                }
                                // R block
                                std::memcpy(dsfInterleaveBuf.data() + outPos, right + srcPos, blockBytes);
                                outPos += blockBytes;
                                if (blockBytes < DSF_BLOCK_SIZE) {
                                    std::memset(dsfInterleaveBuf.data() + outPos, 0, DSF_BLOCK_SIZE - blockBytes);
                                    outPos += DSF_BLOCK_SIZE - blockBytes;
                                }
                                srcPos += blockBytes;
                            }
                            return audioServerPtr->writeAudio(dsfInterleaveBuf.data(), outPos);
                        };

                        constexpr unsigned int PREBUFFER_MS = 3000;
                        uint64_t pushedDsdBytes = 0;
                        bool serverReady = false;
                        AudioHttpServer::AudioFormat audioFmt{};
                        uint32_t detectedChannels = 2;
                        uint32_t dsdBitRate = 0;
                        uint64_t byteRateTotal = 0;

                        bool httpEof = false;
                        bool stmdSent = false;
                        bool gaplessWaitDone = false;
                        auto gaplessWaitStart = std::chrono::steady_clock::now();
                        constexpr int GAPLESS_WAIT_MS = 2000;

                        while (audioTestRunning.load(std::memory_order_acquire) &&
                               (!httpEof || dsdReader->availableBytes() > 0 ||
                                !dsdReader->isFinished() ||
                                !stmdSent ||
                                (stmdSent && !gaplessWaitDone))) {

                            // === PHASE 1: HTTP read + feed ===
                            constexpr size_t DSD_BUF_MAX = 1048576;
                            bool gotData = false;
                            if (!httpEof && dsdReader->availableBytes() < DSD_BUF_MAX) {
                                if (httpStream->isConnected()) {
                                    ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 2);
                                    if (n > 0) {
                                        gotData = true;
                                        totalBytes += n;
                                        slimproto->updateStreamBytes(totalBytes);
                                        dsdReader->feed(httpBuf, static_cast<size_t>(n));
                                    } else if (n < 0 || !httpStream->isConnected()) {
                                        httpEof = true;
                                        dsdReader->setEof();
                                    }
                                } else {
                                    httpEof = true;
                                    dsdReader->setEof();
                                }
                            }

                            // === GAPLESS: send STMd when renderer approaches end of track ===
                            // Use RAW bytes served vs total data bytes (no offset adjustment)
                            if (httpEof && playStarted.load(std::memory_order_acquire) && !stmdSent) {
                                uint64_t rawServed = audioServerPtr->getBytesServed();
                                uint64_t totalDataBytes = totalBytes;  // Total DSD bytes from HTTP
                                uint64_t threshold = byteRateTotal * 3;  // 3s of audio

                                if (totalDataBytes == 0 ||
                                    rawServed + threshold >= totalDataBytes) {
                                    stmdSent = true;
                                    gaplessWaitStart = std::chrono::steady_clock::now();
                                    uint32_t servedSec = (byteRateTotal > 0)
                                        ? static_cast<uint32_t>(rawServed / byteRateTotal) : 0;
                                    uint32_t trackSec = (byteRateTotal > 0)
                                        ? static_cast<uint32_t>(totalDataBytes / byteRateTotal) : 0;
                                    LOG_INFO("[Audio] DSD stream complete: " << totalBytes
                                             << " bytes [STMd at served " << servedSec << "s"
                                             << " / " << trackSec << "s]");
                                    slimproto->sendStat(StatEvent::STMd);
                                }
                            }

                            // === GAPLESS: wait for next track ===
                            if (stmdSent && dsdReader->availableBytes() == 0) {
                                if (hasPendingTrack.load(std::memory_order_acquire)) {
                                    LOG_INFO("[Gapless] DSD pending detected, chaining");
                                    break;
                                }
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - gaplessWaitStart).count();
                                if (elapsed >= GAPLESS_WAIT_MS) {
                                    gaplessWaitDone = true;
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                            }

                            // === PHASE 2: Format detection ===
                            if (!formatLogged && dsdReader->isFormatReady()) {
                                formatLogged = true;
                                const auto& fmt = dsdReader->getFormat();
                                dsdBitRate = fmt.sampleRate;
                                detectedChannels = fmt.channels;
                                byteRateTotal = (static_cast<uint64_t>(dsdBitRate) / 8) * detectedChannels;

                                audioFmt.sampleRate = dsdBitRate;
                                audioFmt.bitDepth = 1;
                                audioFmt.channels = detectedChannels;
                                audioFmt.isDSD = true;
                                audioFmt.dsdRate = dsdBitRate;

                                LOG_INFO("[Audio] DSD: " << DsdProcessor::rateName(dsdBitRate)
                                         << ", " << detectedChannels << " ch");
                            }

                            // === PHASE 3: Prebuffer ===
                            if (formatLogged && !serverReady) {
                                // Same-format gapless: skip setup
                                if (!dsdFirstTrack &&
                                    audioFmt.dsdRate == prevDsdFmt.dsdRate &&
                                    audioFmt.channels == prevDsdFmt.channels) {
                                    gaplessBytesOffset = audioServerPtr->getBytesServed();
                                    LOG_INFO("[Gapless] DSD same format, continuing stream"
                                        " (bytesOffset: " << gaplessBytesOffset << ")");
                                    serverReady = true;
                                    slimproto->updateElapsed(0, 0);
                                    lastElapsedLog = 0;
                                    slimproto->sendStat(StatEvent::STMl);
                                    continue;
                                }

                                size_t targetBytes = static_cast<size_t>(byteRateTotal * PREBUFFER_MS / 1000);
                                if (targetBytes > DSD_BUF_MAX * 3 / 4) {
                                    targetBytes = DSD_BUF_MAX * 3 / 4;
                                }

                                if (dsdReader->availableBytes() >= targetBytes || httpEof) {
                                    if (dsdReader->availableBytes() == 0) continue;

                                    // Set format and prebuffer to AudioHttpServer
                                    audioServerPtr->setFormat(audioFmt);

                                    // Flush prebuffer to ring buffer
                                    while (audioTestRunning.load(std::memory_order_relaxed)) {
                                        if (audioServerPtr->getBufferLevel() > 0.90f) break;
                                        size_t bytes = dsdReader->readPlanar(planarBuf, DSD_PLANAR_BUF);
                                        if (bytes == 0) break;
                                        writeDsfBlockInterleaved(planarBuf, bytes, detectedChannels);
                                        pushedDsdBytes += bytes;
                                    }

                                    // Prebuffer done — allow renderer to connect
                                    audioServerPtr->setReadyToServe();

                                    pushedDsdBytes = 0;
                                    slimproto->updateElapsed(0, 0);

                                    // Start UPnP playback in background thread
                                    serverReady = true;
                                    std::thread([upnpPtr, audioServerPtr, &slimproto,
                                                 &streamGeneration, thisGeneration,
                                                 &gaplessBytesOffset, &playStarted]() {
                                        upnpPtr->setAVTransportURI(audioServerPtr->getStreamURL());
                                        // Only send Play+STMl if this stream is still current
                                        if (streamGeneration.load() == thisGeneration) {
                                            upnpPtr->play();
                                            gaplessBytesOffset = audioServerPtr->getBytesServed();
                                            slimproto->updateElapsed(0, 0);
                                            slimproto->updateStreamBytes(0);
                                            playStarted.store(true, std::memory_order_release);
                                            slimproto->sendStat(StatEvent::STMl);
                                        }
                                    }).detach();
                                }
                                continue;
                            }

                            // === PHASE 4: Push DSD data ===
                            if (serverReady && dsdReader->availableBytes() > 0) {
                                if (audioServerPtr->getBufferLevel() <= 0.95f) {
                                    size_t bytes = dsdReader->readPlanar(planarBuf, DSD_PLANAR_BUF);
                                    if (bytes > 0) {
                                        writeDsfBlockInterleaved(planarBuf, bytes, detectedChannels);
                                        pushedDsdBytes += bytes;
                                    }
                                } else {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                            }

                            // === PHASE 5: Update elapsed (based on bytes served to renderer) ===
                            if (playStarted.load(std::memory_order_acquire) && byteRateTotal > 0) {
                                uint64_t served = audioServerPtr->getBytesServed();
                                if (served > gaplessBytesOffset)
                                    served -= gaplessBytesOffset;
                                else
                                    served = 0;
                                uint64_t totalMs = (served * 1000) / byteRateTotal;
                                uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
                                uint32_t elapsedMs = static_cast<uint32_t>(totalMs);
                                slimproto->updateElapsed(elapsedSec, elapsedMs);

                                if (elapsedSec >= lastElapsedLog + 10) {
                                    lastElapsedLog = elapsedSec;
                                    LOG_DEBUG("[Audio] DSD elapsed: " << elapsedSec << "s");
                                }
                            }

                            // === Anti-busy-loop ===
                            if (!gotData && dsdReader->availableBytes() == 0 && !httpEof) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }

                            if (dsdReader->hasError()) {
                                LOG_ERROR("[Audio] DSD stream reader error");
                                break;
                            }
                        }

                        // === GAPLESS CHECK: chain to next DSD track? ===
                        if (hasPendingTrack.load(std::memory_order_acquire)) {
                            std::shared_ptr<PendingTrack> next;
                            {
                                std::lock_guard<std::mutex> lock(pendingMutex);
                                next = std::move(pendingNextTrack);
                                pendingNextTrack.reset();
                                hasPendingTrack.store(false, std::memory_order_release);
                            }
                            if (next && next->formatCode == FORMAT_DSD) {
                                LOG_INFO("[Gapless] Chaining to next DSD track");
                                httpStream->disconnect();
                                httpStream = next->httpClient;
                                dsdPcmRate = next->pcmSampleRate;
                                dsdPcmChannels = next->pcmChannels;
                                prevDsdFmt = audioFmt;
                                dsdFirstTrack = false;
                                continue;
                            }
                            LOG_INFO("[Gapless] Cross-format (DSD→PCM), ending chain");
                        }

                        break;  // Exit DSD chaining loop
                      }

                      if (audioTestRunning.load(std::memory_order_acquire)) {
                          audioServerPtr->signalEndOfStream();
                          slimproto->sendStat(StatEvent::STMu);
                      }
                      audioThreadDone.store(true, std::memory_order_release);
                      return;
                    }

                    // ============================================================
                    // PCM/FLAC PATH with gapless chaining
                    // ============================================================
                    {
                    char curFormatCode = formatCode;
                    char curPcmRate = pcmRate;
                    char curPcmSize = pcmSize;
                    char curPcmChannels = pcmChannels;
                    char curPcmEndian = pcmEndian;
                    bool pcmFirstTrack = true;
                    AudioHttpServer::AudioFormat prevAudioFmt{};

                    constexpr size_t DECODE_CACHE_MAX_SAMPLES = 9216000;
                    std::vector<int32_t> decodeCache;
                    size_t decodeCachePos = 0;
                    bool serverReady = false;
                    AudioHttpServer::AudioFormat audioFmt{};
                    int detectedChannels = 2;

                    // Conversion buffer: S32_LE → target bit depth
                    constexpr size_t MAX_DECODE_FRAMES = 1024;
                    std::vector<uint8_t> convBuf(MAX_DECODE_FRAMES * 2 * 4);  // Max: stereo 32-bit

                    auto cacheFrames = [&]() -> size_t {
                        return (decodeCache.size() - decodeCachePos) /
                               std::max(detectedChannels, 1);
                    };

                    while (true) {  // === PCM/FLAC CHAINING LOOP ===

                    auto decoder = Decoder::create(curFormatCode, config.decoderBackend);
                    if (!decoder) {
                        LOG_ERROR("[Audio] Unsupported format: " << curFormatCode);
                        slimproto->sendStat(StatEvent::STMn);
                        if (pcmFirstTrack) {
                            audioThreadDone.store(true, std::memory_order_release);
                            return;
                        }
                        break;
                    }

                    if (curFormatCode == FORMAT_PCM) {
                        uint32_t sr = sampleRateFromCode(curPcmRate);
                        uint32_t bd = sampleSizeFromCode(curPcmSize);
                        uint32_t ch = (curPcmChannels == '2') ? 2
                                    : (curPcmChannels == '1') ? 1 : 0;
                        bool be = (curPcmEndian == '0');
                        if (sr > 0 && bd > 0 && ch > 0) {
                            decoder->setRawPcmFormat(sr, bd, ch, be);
                        }
                    }

                    slimproto->sendStat(StatEvent::STMs);

                    if (!pcmFirstTrack) {
                        slimproto->updateElapsed(0, 0);
                        slimproto->updateStreamBytes(0);
                    }

                    uint8_t httpBuf[65536];
                    int32_t decodeBuf[MAX_DECODE_FRAMES * 2];
                    uint64_t totalBytes = 0;
                    bool formatLogged = false;
                    uint64_t lastElapsedLog = 0;

                    constexpr unsigned int PREBUFFER_MS_NORMAL = 3000;
                    constexpr unsigned int PREBUFFER_MS_HIGHRATE = 3000;
                    unsigned int prebufferMs = PREBUFFER_MS_NORMAL;
                    uint64_t pushedFrames = 0;
                    uint64_t gaplessBytesOffset = 0;  // bytesServed at gapless transition
                    std::atomic<bool> playStarted{false};  // Set after Play + offset capture

                    bool dopDetected = false;
                    bool httpEof = false;
                    bool stmdSent = false;

                    while (audioTestRunning.load(std::memory_order_acquire) &&
                           (!httpEof || cacheFrames() > 0 || !stmdSent)) {

                        // ========== PHASE 1a: HTTP read ==========
                        bool gotData = false;
                        size_t cacheSamples = decodeCache.size() - decodeCachePos;
                        if (cacheSamples < DECODE_CACHE_MAX_SAMPLES && !httpEof) {
                            if (httpStream->isConnected()) {
                                ssize_t n = httpStream->readWithTimeout(
                                    httpBuf, sizeof(httpBuf), 2);
                                if (n > 0) {
                                    gotData = true;
                                    totalBytes += n;
                                    slimproto->updateStreamBytes(totalBytes);
                                    decoder->feed(httpBuf, static_cast<size_t>(n));
                                } else if (n < 0 || !httpStream->isConnected()) {
                                    httpEof = true;
                                    decoder->setEof();
                                }
                            } else {
                                httpEof = true;
                                decoder->setEof();
                            }
                        }

                        // === GAPLESS: send STMd when renderer approaches end of track ===
                        // Use RAW bytes served (not offset-adjusted) vs total decoded bytes.
                        // The offset is only for elapsed display, not for STMd timing.
                        if (httpEof && playStarted.load(std::memory_order_acquire) && !stmdSent) {
                            uint64_t decoded = decoder->getDecodedSamples();
                            size_t bytesPerFrame = audioFmt.channels * (audioFmt.bitDepth / 8);
                            uint64_t totalDecodedBytes = decoded * bytesPerFrame;
                            uint64_t rawServed = audioServerPtr->getBytesServed();

                            // Send STMd when renderer has consumed most of the decoded data
                            // (within ~3s worth of audio remaining)
                            size_t bytesPerSec = audioFmt.sampleRate * bytesPerFrame;
                            uint64_t threshold = static_cast<uint64_t>(bytesPerSec) * 3;
                            if (totalDecodedBytes == 0 ||
                                rawServed + threshold >= totalDecodedBytes) {
                                stmdSent = true;
                                uint32_t trackSec = (audioFmt.sampleRate > 0)
                                    ? static_cast<uint32_t>(decoded / audioFmt.sampleRate) : 0;
                                uint32_t servedSec = (bytesPerSec > 0)
                                    ? static_cast<uint32_t>(rawServed / bytesPerSec) : 0;
                                LOG_INFO("[Audio] Stream complete: " << totalBytes
                                         << " bytes, " << decoded
                                         << " frames (" << trackSec << "s)"
                                         << " [STMd at served " << servedSec << "s]");
                                slimproto->sendStat(StatEvent::STMd);
                            }
                        }

                        // ========== PHASE 1b: Drain decoder into cache ==========
                        if (decodeCache.size() - decodeCachePos < DECODE_CACHE_MAX_SAMPLES) {
                            while (true) {
                                size_t frames = decoder->readDecoded(
                                    decodeBuf, MAX_DECODE_FRAMES);
                                if (frames == 0) break;
                                decodeCache.insert(decodeCache.end(), decodeBuf,
                                    decodeBuf + frames * detectedChannels);
                            }
                        }

                        // ========== PHASE 2: Format detection ==========
                        if (!formatLogged && decoder->isFormatReady()) {
                            formatLogged = true;
                            auto fmt = decoder->getFormat();
                            LOG_INFO("[Audio] Decoding: " << fmt.sampleRate << " Hz, "
                                     << fmt.bitDepth << "-bit, " << fmt.channels << " ch");

                            // Gapless continuation check
                            if (serverReady && !pcmFirstTrack) {
                                bool sameFormat =
                                    (fmt.sampleRate == audioFmt.sampleRate &&
                                     fmt.channels == audioFmt.channels);
                                if (sameFormat) {
                                    gaplessBytesOffset = audioServerPtr->getBytesServed();
                                    LOG_INFO("[Gapless] PCM same format, continuing stream"
                                        " (cache: " << cacheFrames() << " frames,"
                                        " bytesOffset: " << gaplessBytesOffset << ")");
                                    slimproto->updateElapsed(0, 0);
                                    lastElapsedLog = 0;
                                    slimproto->sendStat(StatEvent::STMl);
                                } else {
                                    // Format change — drain old cache, then reopen
                                    LOG_INFO("[Gapless] Format change, draining "
                                             << cacheFrames() << " old frames");
                                    while (cacheFrames() > 0 &&
                                           audioTestRunning.load(std::memory_order_acquire)) {
                                        if (audioServerPtr->getBufferLevel() > 0.95f) {
                                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                            continue;
                                        }
                                        size_t push = std::min(cacheFrames(), MAX_DECODE_FRAMES);
                                        const int32_t* ptr = decodeCache.data() + decodeCachePos;
                                        size_t convBytes = convertPcmBitDepth(
                                            ptr, convBuf.data(),
                                            push * detectedChannels, audioFmt.bitDepth);
                                        if (convBytes > 0) {
                                            audioServerPtr->writeAudio(convBuf.data(), convBytes);
                                        }
                                        decodeCachePos += push * detectedChannels;
                                    }
                                    // Wait for ring buffer to drain (renderer reads at 1x speed)
                                    // so the last seconds of audio are not lost
                                    LOG_INFO("[Gapless] Waiting for ring buffer to drain before format change...");
                                    auto drainStart = std::chrono::steady_clock::now();
                                    constexpr int DRAIN_TIMEOUT_MS = 15000;  // Max 15s wait
                                    while (audioServerPtr->getBufferLevel() > 0.05f &&
                                           audioTestRunning.load(std::memory_order_acquire)) {
                                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - drainStart).count();
                                        if (elapsed >= DRAIN_TIMEOUT_MS) {
                                            LOG_WARN("[Gapless] Drain timeout, proceeding with format change");
                                            break;
                                        }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    }
                                    LOG_INFO("[Gapless] Ring buffer drained, switching format");
                                    // Signal end of old stream
                                    audioServerPtr->signalEndOfStream();
                                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                    // Format change requires full Stop so renderer accepts new URI
                                    upnpPtr->stop();
                                    audioServerPtr->reset();
                                    serverReady = false;
                                }
                            }

                            detectedChannels = fmt.channels;
                            audioFmt.sampleRate = fmt.sampleRate;
                            audioFmt.bitDepth = (fmt.bitDepth <= 24) ? 24 : 32;
                            audioFmt.channels = fmt.channels;

                            if (fmt.sampleRate > 192000) {
                                prebufferMs = PREBUFFER_MS_HIGHRATE;
                            }

                            // Resize conversion buffer for this format
                            convBuf.resize(MAX_DECODE_FRAMES * detectedChannels *
                                           (audioFmt.bitDepth / 8));
                        }

                        // ========== PHASE 3: Prebuffer ==========
                        if (formatLogged && !serverReady) {
                            // Same-format gapless
                            if (!pcmFirstTrack &&
                                audioFmt.sampleRate == prevAudioFmt.sampleRate &&
                                audioFmt.bitDepth == prevAudioFmt.bitDepth &&
                                audioFmt.channels == prevAudioFmt.channels) {
                                LOG_INFO("[Gapless] PCM same format, continuing stream");
                                serverReady = true;
                                slimproto->sendStat(StatEvent::STMl);
                                continue;
                            }

                            auto fmt = decoder->getFormat();
                            size_t targetFrames = static_cast<size_t>(
                                fmt.sampleRate) * prebufferMs / 1000;
                            if (cacheFrames() >= targetFrames || httpEof) {
                                size_t prebufFrames = cacheFrames();
                                if (prebufFrames == 0) continue;

                                // Detect DoP
                                if (!dopDetected && cacheFrames() >= 32) {
                                    const int32_t* samples =
                                        decodeCache.data() + decodeCachePos;
                                    if (detectDoP(samples, cacheFrames(), detectedChannels)) {
                                        dopDetected = true;
                                        audioFmt.bitDepth = 24;
                                        LOG_INFO("[Audio] DoP detected — passthrough as 24-bit PCM");
                                    }
                                }

                                // Prebuffer into ring buffer FIRST (before making server available)
                                // This ensures the renderer gets data immediately on connect
                                audioServerPtr->setFormat(audioFmt);

                                const int32_t* ptr = decodeCache.data() + decodeCachePos;
                                size_t remaining = prebufFrames;
                                size_t actualPushed = 0;
                                while (remaining > 0 &&
                                       audioTestRunning.load(std::memory_order_relaxed)) {
                                    if (audioServerPtr->getBufferLevel() > 0.95f) break;
                                    size_t chunk = std::min(remaining, MAX_DECODE_FRAMES);
                                    size_t samples = chunk * detectedChannels;
                                    size_t convBytes = convertPcmBitDepth(
                                        ptr, convBuf.data(), samples, audioFmt.bitDepth);
                                    if (convBytes > 0) {
                                        audioServerPtr->writeAudio(convBuf.data(), convBytes);
                                    }
                                    ptr += samples;
                                    remaining -= chunk;
                                    actualPushed += chunk;
                                }
                                decodeCachePos += actualPushed * detectedChannels;
                                pushedFrames += actualPushed;

                                // Prebuffer done — now allow renderer to connect and read
                                audioServerPtr->setReadyToServe();

                                // Reset elapsed: prebuffered frames are not playback time
                                pushedFrames = 0;
                                slimproto->updateElapsed(0, 0);

                                // Start UPnP playback in background thread
                                // (SetAVTransportURI blocks for seconds while renderer connects)
                                serverReady = true;
                                std::thread([upnpPtr, audioServerPtr, &slimproto,
                                             &streamGeneration, thisGeneration,
                                             &gaplessBytesOffset, &playStarted]() {
                                    upnpPtr->setAVTransportURI(audioServerPtr->getStreamURL());
                                    // Only send Play+STMl if this stream is still current
                                    if (streamGeneration.load() == thisGeneration) {
                                        upnpPtr->play();
                                        // Capture bytes already served as baseline
                                        // (prebuffer data served before Play)
                                        gaplessBytesOffset = audioServerPtr->getBytesServed();
                                        slimproto->updateElapsed(0, 0);
                                        slimproto->updateStreamBytes(0);
                                        playStarted.store(true, std::memory_order_release);
                                        slimproto->sendStat(StatEvent::STMl);
                                    }
                                }).detach();
                            }
                            continue;
                        }

                        // ========== PHASE 4: Push from cache to AudioHttpServer ==========
                        if (serverReady && cacheFrames() > 0) {
                            if (audioServerPtr->getBufferLevel() <= 0.95f) {
                                size_t push = std::min(cacheFrames(), MAX_DECODE_FRAMES);
                                const int32_t* ptr = decodeCache.data() + decodeCachePos;
                                size_t samples = push * detectedChannels;
                                size_t convBytes = convertPcmBitDepth(
                                    ptr, convBuf.data(), samples, audioFmt.bitDepth);
                                if (convBytes > 0) {
                                    audioServerPtr->writeAudio(convBuf.data(), convBytes);
                                }
                                decodeCachePos += samples;
                                pushedFrames += push;
                            } else {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            }
                        }

                        // ========== PHASE 5: Update elapsed (based on bytes served to renderer) ==========
                        if (playStarted.load(std::memory_order_acquire) && audioFmt.sampleRate > 0) {
                            size_t bytesPerSec = audioFmt.sampleRate * audioFmt.channels * (audioFmt.bitDepth / 8);
                            if (bytesPerSec > 0) {
                                uint64_t served = audioServerPtr->getBytesServed();
                                // Subtract gapless offset to get per-track elapsed
                                if (served > gaplessBytesOffset)
                                    served -= gaplessBytesOffset;
                                else
                                    served = 0;
                                uint64_t totalMs = (served * 1000) / bytesPerSec;
                                uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
                                uint32_t elapsedMs = static_cast<uint32_t>(totalMs);
                                slimproto->updateElapsed(elapsedSec, elapsedMs);

                                if (elapsedSec >= lastElapsedLog + 10) {
                                    lastElapsedLog = elapsedSec;
                                    LOG_DEBUG("[Audio] Elapsed: " << elapsedSec << "s (served)"
                                              << " cache=" << cacheFrames() << "f");
                                }
                            }
                        }

                        // ========== PHASE 6: Compact cache ==========
                        if (decodeCachePos > 500000) {
                            decodeCache.erase(decodeCache.begin(),
                                decodeCache.begin() + decodeCachePos);
                            decodeCachePos = 0;
                        }

                        // ========== PHASE 7: Anti-busy-loop ==========
                        if (!gotData && cacheFrames() == 0) {
                            // Sleep when waiting for data OR waiting for STMd to fire
                            std::this_thread::sleep_for(std::chrono::milliseconds(
                                (httpEof && !stmdSent) ? 100 : 1));
                        }

                        if (decoder->hasError()) {
                            LOG_ERROR("[Audio] Decoder error");
                            break;
                        }
                    }

                    // Drain decoder after HTTP EOF
                    decoder->setEof();
                    while (!decoder->isFinished() && !decoder->hasError() &&
                           audioTestRunning.load(std::memory_order_acquire)) {
                        size_t frames = decoder->readDecoded(decodeBuf, MAX_DECODE_FRAMES);
                        if (frames == 0) break;
                        decodeCache.insert(decodeCache.end(), decodeBuf,
                            decodeBuf + frames * detectedChannels);
                    }

                    // === GAPLESS CHECK ===
                    bool gaplessPending = hasPendingTrack.load(std::memory_order_acquire);

                    if (!gaplessPending) {
                        // Drain remaining cache
                        while (serverReady && cacheFrames() > 0 &&
                               audioTestRunning.load(std::memory_order_acquire)) {
                            if (audioServerPtr->getBufferLevel() > 0.95f) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                continue;
                            }
                            size_t push = std::min(cacheFrames(), MAX_DECODE_FRAMES);
                            const int32_t* ptr = decodeCache.data() + decodeCachePos;
                            size_t samples = push * detectedChannels;
                            size_t convBytes = convertPcmBitDepth(
                                ptr, convBuf.data(), samples, audioFmt.bitDepth);
                            if (convBytes > 0) {
                                audioServerPtr->writeAudio(convBuf.data(), convBytes);
                            }
                            decodeCachePos += samples;
                            pushedFrames += push;

                            if (decoder->isFormatReady()) {
                                auto fmt = decoder->getFormat();
                                if (fmt.sampleRate > 0) {
                                    uint64_t totalMs = pushedFrames * 1000 / fmt.sampleRate;
                                    slimproto->updateElapsed(
                                        static_cast<uint32_t>(totalMs / 1000),
                                        static_cast<uint32_t>(totalMs));
                                }
                            }
                        }

                        // Wait for gapless next track
                        if (!hasPendingTrack.load(std::memory_order_acquire) &&
                            audioTestRunning.load(std::memory_order_acquire)) {
                            LOG_DEBUG("[Gapless] PCM: waiting for next track...");
                            auto waitStart = std::chrono::steady_clock::now();
                            constexpr int GAPLESS_WAIT_MS = 2000;
                            while (!hasPendingTrack.load(std::memory_order_acquire) &&
                                   audioTestRunning.load(std::memory_order_acquire) &&
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - waitStart).count() < GAPLESS_WAIT_MS) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                        }
                    } else {
                        LOG_INFO("[Gapless] Next track pending, keeping "
                                 << cacheFrames() << " frames in cache");
                    }

                    // === Chain to next PCM/FLAC track? ===
                    if (hasPendingTrack.load(std::memory_order_acquire)) {
                        std::shared_ptr<PendingTrack> next;
                        {
                            std::lock_guard<std::mutex> lock(pendingMutex);
                            next = std::move(pendingNextTrack);
                            pendingNextTrack.reset();
                            hasPendingTrack.store(false, std::memory_order_release);
                        }
                        if (next && next->formatCode != FORMAT_DSD) {
                            LOG_INFO("[Gapless] Chaining to next PCM/FLAC track");
                            httpStream->disconnect();
                            httpStream = next->httpClient;
                            curFormatCode = next->formatCode;
                            curPcmRate = next->pcmSampleRate;
                            curPcmSize = next->pcmSampleSize;
                            curPcmChannels = next->pcmChannels;
                            curPcmEndian = next->pcmEndian;
                            prevAudioFmt = audioFmt;
                            pcmFirstTrack = false;
                            if (decodeCachePos > 0) {
                                decodeCache.erase(decodeCache.begin(),
                                    decodeCache.begin() + decodeCachePos);
                                decodeCachePos = 0;
                            }
                            continue;
                        }
                        LOG_INFO("[Gapless] Cross-format (PCM→DSD), ending chain");
                    }

                    break;  // Exit PCM/FLAC chaining loop
                    }  // end chaining loop
                    }  // end PCM/FLAC scope

                    if (audioTestRunning.load(std::memory_order_acquire) && !openFailedInGapless) {
                        audioServerPtr->signalEndOfStream();
                        slimproto->sendStat(StatEvent::STMu);
                    }
                    audioThreadDone.store(true, std::memory_order_release);
                });
                break;
            }

            case STRM_STOP:
                LOG_INFO("Stream stop requested");
                {
                    std::lock_guard<std::mutex> lock(pendingMutex);
                    pendingNextTrack.reset();
                    hasPendingTrack.store(false, std::memory_order_release);
                }
                {
                    bool wasActive = !audioThreadDone.load(std::memory_order_acquire);
                    audioTestRunning.store(false);
                    httpStream->disconnect();
                    upnpPtr->stop();
                    audioServerPtr->reset();
                    if (wasActive) {
                        slimproto->sendStat(StatEvent::STMf);
                    }
                }
                break;

            case STRM_PAUSE:
                LOG_INFO("Pause requested");
                upnpPtr->pause();
                slimproto->sendStat(StatEvent::STMp);
                break;

            case STRM_UNPAUSE:
                LOG_INFO("Unpause requested");
                upnpPtr->play();
                slimproto->sendStat(StatEvent::STMr);
                break;

            case STRM_FLUSH:
                LOG_INFO("Flush requested");
                {
                    std::lock_guard<std::mutex> lock(pendingMutex);
                    pendingNextTrack.reset();
                    hasPendingTrack.store(false, std::memory_order_release);
                }
                {
                    bool wasActive = !audioThreadDone.load(std::memory_order_acquire);
                    audioTestRunning.store(false);
                    httpStream->disconnect();
                    upnpPtr->stop();
                    audioServerPtr->reset();
                    if (wasActive) {
                        slimproto->sendStat(StatEvent::STMf);
                    }
                }
                break;

            default:
                break;
        }
    });

    slimproto->onVolume([](uint32_t gainL, uint32_t gainR) {
        LOG_DEBUG("Volume: L=0x" << std::hex << gainL << " R=0x" << gainR
                  << std::dec << " (ignored - bit-perfect)");
    });

    // Helper: stop audio thread and wait
    auto stopAudioThread = [&]() {
        audioTestRunning.store(false);
        httpStream->disconnect();
        if (audioTestThread.joinable()) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!audioThreadDone.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (audioThreadDone.load(std::memory_order_acquire)) {
                audioTestThread.join();
            } else {
                audioTestThread.detach();
                LOG_WARN("Audio thread did not stop in time, detached");
            }
        }
        upnpPtr->stop();
    };

    // Helper: interruptible sleep
    auto interruptibleSleep = [](int seconds) -> bool {
        for (int i = 0; i < seconds * 10; i++) {
            if (!g_running.load(std::memory_order_acquire)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };

    // ============================================
    // Connection loop with exponential backoff
    // ============================================

    constexpr int INITIAL_BACKOFF_S = 2;
    constexpr int MAX_BACKOFF_S = 30;
    int backoffS = INITIAL_BACKOFF_S;
    int connectionCount = 0;

    while (g_running.load(std::memory_order_acquire)) {
        if (connectionCount > 0) {
            LOG_WARN("Reconnecting to LMS in " << backoffS << "s...");
            if (!interruptibleSleep(backoffS)) break;
            backoffS = std::min(backoffS * 2, MAX_BACKOFF_S);
        }

        if (!slimproto->connect(config.lmsServer, config.lmsPort, config)) {
            if (g_running.load(std::memory_order_acquire)) {
                LOG_WARN("Failed to connect to LMS");
                if (connectionCount == 0) connectionCount = 1;
            }
            continue;
        }

        backoffS = INITIAL_BACKOFF_S;
        connectionCount++;

        std::thread slimprotoThread([&slimproto]() {
            slimproto->run();
        });

        if (connectionCount == 1) {
            LOG_INFO("Player registered with LMS");
            std::cout << "(Press Ctrl+C to stop)" << std::endl;
        } else {
            LOG_INFO("Reconnected to LMS");
        }
        std::cout << std::endl;

        while (g_running.load(std::memory_order_acquire) && slimproto->isConnected()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        stopAudioThread();
        slimproto->disconnect();
        if (slimprotoThread.joinable()) {
            slimprotoThread.join();
        }

        if (!g_running.load(std::memory_order_acquire)) break;
        LOG_WARN("Lost connection to LMS");
    }

    // ============================================
    // Final shutdown
    // ============================================

    std::cout << "\nShutting down..." << std::endl;
    stopAudioThread();
    g_slimproto = nullptr;
    slimproto->disconnect();

    audioServer->stop();
    upnp->shutdown();

    return 0;
}
