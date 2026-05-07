// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dominique Comet (cometdom)
// This file is part of slim2UPnP. See LICENSE for details.

/**
 * @file main.cpp
 * @brief Main entry point for slim2UPnP
 *
 * Slimproto→UPnP bridge: passthrough mode.
 * Proxies audio streams from LMS to UPnP renderer without decoding.
 * Single process: SlimprotoClient + AudioHttpServer + UPnPController.
 */

#include "Config.h"
#include "SlimprotoClient.h"
#include "HttpStreamClient.h"
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

#define SLIM2UPNP_VERSION "0.1.23-beta"

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
        else if (arg == "--no-play-calibration") {
            config.calibrateElapsed = false;
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
                      << "                         Uses <port> and <port+1> for gapless slots A/B\n"
                      << "  --interface <iface>    Network interface (default: auto)\n"
                      << "  -l, --list-renderers   List available renderers and exit\n"
                      << "\n"
                      << "Audio:\n"
                      << "  --max-rate <hz>        Max sample rate (default: 1536000)\n"
                      << "  --no-dsd               Disable DSD support\n"
                      << "  --no-play-calibration  Disable renderer PLAYING-state elapsed calibration\n"
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

// ============================================
// Track info from stream header (passthrough)
// ============================================

struct TrackInfo {
    uint32_t durationSec = 0;   // 0 = unknown
    uint32_t sampleRate = 0;    // 0 = unknown
    uint8_t  bitDepth = 0;      // 0 = unknown
    uint8_t  channels = 0;      // 0 = unknown
};

/// Parse track info (duration + format params) from the first bytes of an audio stream.
/// Fields are 0 when not determinable.
static TrackInfo parseTrackInfo(const uint8_t* data, size_t len,
                                const std::string& contentType) {
    TrackInfo info;

    // FLAC STREAMINFO (offset 8 = after "fLaC" magic + 4-byte block header)
    //   bits 0-19   sample rate (20 bits)
    //   bits 20-22  channels - 1 (3 bits)
    //   bits 23-27  bits per sample - 1 (5 bits)
    //   bits 28-63  total samples (36 bits)
    if (contentType.find("flac") != std::string::npos && len >= 42) {
        if (data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
            const uint8_t* si = data + 8;
            info.sampleRate = (static_cast<uint32_t>(si[10]) << 12)
                            | (static_cast<uint32_t>(si[11]) << 4)
                            | (static_cast<uint32_t>(si[12]) >> 4);
            info.channels = static_cast<uint8_t>(((si[12] >> 1) & 0x07) + 1);
            info.bitDepth = static_cast<uint8_t>(
                (((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1);
            uint64_t totalSamples =
                (static_cast<uint64_t>(si[13] & 0x0F) << 32) |
                (static_cast<uint64_t>(si[14]) << 24) |
                (static_cast<uint64_t>(si[15]) << 16) |
                (static_cast<uint64_t>(si[16]) <<  8) |
                (static_cast<uint64_t>(si[17]));
            if (info.sampleRate > 0 && totalSamples > 0) {
                info.durationSec = static_cast<uint32_t>(totalSamples / info.sampleRate);
            }
            LOG_DEBUG("[Audio] FLAC track: " << info.sampleRate << "Hz, "
                      << static_cast<int>(info.bitDepth) << "-bit, "
                      << static_cast<int>(info.channels) << "ch, "
                      << info.durationSec << "s");
            return info;
        }
    }

    // DSF: parse header for sample count and rate
    if (contentType.find("dsf") != std::string::npos && len >= 60) {
        if (data[0] == 'D' && data[1] == 'S' && data[2] == 'D' && data[3] == ' ') {
            // fmt chunk at offset 28: sample rate at offset 32 (4 bytes LE)
            // sample count at offset 40 (8 bytes LE)
            info.sampleRate =
                data[32] | (data[33] << 8) | (data[34] << 16) | (data[35] << 24);
            uint64_t sampleCount =
                static_cast<uint64_t>(data[40]) |
                (static_cast<uint64_t>(data[41]) << 8) |
                (static_cast<uint64_t>(data[42]) << 16) |
                (static_cast<uint64_t>(data[43]) << 24) |
                (static_cast<uint64_t>(data[44]) << 32) |
                (static_cast<uint64_t>(data[45]) << 40) |
                (static_cast<uint64_t>(data[46]) << 48) |
                (static_cast<uint64_t>(data[47]) << 56);
            if (info.sampleRate > 0 && sampleCount > 0) {
                info.durationSec = static_cast<uint32_t>(sampleCount / info.sampleRate);
                LOG_DEBUG("[Audio] DSF duration: " << info.durationSec << "s");
            }
        }
    }

    return info;
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  slim2UPnP v" << SLIM2UPNP_VERSION << "\n"
              << "  Slimproto to UPnP bridge (passthrough)\n"
              << "═══════════════════════════════════════════════════════\n"
              << std::endl;

    std::cout << "Mode: passthrough (no decoding)" << std::endl;

    Config config = parseArguments(argc, argv);

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

    // Dual AudioHttpServer for ping-pong gapless (slot A and slot B)
    auto audioServerA = std::make_unique<AudioHttpServer>();
    auto audioServerB = std::make_unique<AudioHttpServer>();

    // Use UPnP's detected IP for both servers
    std::string serverIP = upnp->getServerIP();
    if (!serverIP.empty()) {
        audioServerA->setLocalIP(serverIP);
        audioServerB->setLocalIP(serverIP);
    }

    // If --http-port is specified, use port for slot A and port+1 for slot B.
    // Otherwise auto-select (port 0).
    uint16_t portA = config.httpServerPort;
    uint16_t portB = (config.httpServerPort != 0)
                     ? static_cast<uint16_t>(config.httpServerPort + 1)
                     : 0;

    if (!audioServerA->start(portA)) {
        std::cerr << "Failed to start audio HTTP server A"
                  << (portA ? " on port " + std::to_string(portA) : "")
                  << std::endl;
        return 1;
    }
    if (!audioServerB->start(portB)) {
        std::cerr << "Failed to start audio HTTP server B"
                  << (portB ? " on port " + std::to_string(portB) : "")
                  << std::endl;
        return 1;
    }

    // Ping-pong: servers[0] = slot A, servers[1] = slot B
    AudioHttpServer* servers[2] = { audioServerA.get(), audioServerB.get() };
    std::atomic<int> currentSlot{0};  // Index of active slot

    // Convenience alias for backward compatibility in cold start
    AudioHttpServer* audioServerPtr = servers[0];

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
            audioServerA->stop();
            audioServerB->stop();
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
    std::cout << "  HTTP A:     " << audioServerA->getStreamURL() << std::endl;
    std::cout << "  HTTP B:     " << audioServerB->getStreamURL() << std::endl;
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

                // Stop UPnP renderer if playing
                upnpPtr->stop();

                // Join any previous audio thread
                if (audioTestThread.joinable()) {
                    audioTestThread.join();
                }

                // Reset both audio servers for new stream
                servers[0]->reset();
                servers[1]->reset();
                currentSlot.store(0);
                audioServerPtr = servers[0];

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

                // Start passthrough audio thread
                char formatCode = cmd.format;
                char pcmRate = cmd.pcmSampleRate;
                char pcmSize = cmd.pcmSampleSize;
                char pcmChannels = cmd.pcmChannels;
                audioTestRunning.store(true);
                audioThreadDone.store(false, std::memory_order_release);
                uint32_t thisGeneration = streamGeneration.fetch_add(1) + 1;
                audioTestThread = std::thread([&httpStream, &slimproto,
                    &audioTestRunning, &audioThreadDone, &hasPendingTrack,
                    &pendingMutex, &pendingNextTrack, &streamGeneration,
                    thisGeneration, formatCode, pcmRate, pcmSize, pcmChannels,
                    servers, &currentSlot, upnpPtr, &config]() {

                    // Mutable copies of format params (updated on gapless chain)
                    char fmtCode = formatCode;
                    char pRate = pcmRate;
                    char pSize = pcmSize;
                    char pChannels = pcmChannels;

                    // Local pointer to active server
                    AudioHttpServer* audioServerPtr = servers[currentSlot.load()];

                    // ============================================================
                    // PASSTHROUGH: proxy LMS stream to renderer without decoding
                    // ============================================================
                    {
                    bool firstTrack = true;
                    std::string prevContentType;  // Track content type changes for gapless
                    TrackInfo prevTrackInfo;       // Track sample rate / bit depth changes

                    while (true) {  // === TRACK LOOP (passthrough) ===

                    // --- Extract Content-Type from LMS HTTP response ---
                    std::string contentType = httpStream->getContentType();
                    if (contentType.empty()) contentType = "application/octet-stream";
                    LOG_INFO("[Audio] Passthrough: " << contentType);

                    slimproto->sendStat(StatEvent::STMs);

                    // --- Setup AudioHttpServer for this track ---
                    if (!firstTrack) {
                        // Switch to next slot for gapless
                        int nextSlot = 1 - currentSlot.load();
                        servers[nextSlot]->reset();
                        audioServerPtr = servers[nextSlot];
                        LOG_INFO("[Gapless] Preparing slot " << nextSlot);
                    }

                    // Configure passthrough MIME type and ring buffer.
                    // For raw PCM (format=p), we know the exact format from strm-s,
                    // so use the real values upfront — avoids a second setFormat
                    // after prebuffer which would reset ring buffer positions and
                    // cause a byte misalignment (→ white noise on 24-bit).
                    AudioHttpServer::AudioFormat audioFmt{};
                    if (fmtCode == 'p') {
                        uint32_t sr = sampleRateFromCode(pRate);
                        uint32_t bd = sampleSizeFromCode(pSize);
                        uint32_t ch = (pChannels == '2') ? 2 : (pChannels == '1') ? 1 : 2;
                        audioFmt.sampleRate = sr ? sr : 44100;
                        audioFmt.bitDepth = bd ? bd : 16;
                        audioFmt.channels = ch;
                    } else {
                        audioFmt.sampleRate = 44100;  // Default for compressed formats
                        audioFmt.bitDepth = 24;
                        audioFmt.channels = 2;
                    }
                    audioServerPtr->setFormat(audioFmt);
                    audioServerPtr->setPassthroughMime(contentType);

                    // --- Prebuffer: read HTTP → write raw bytes ---
                    constexpr size_t PREBUFFER_BYTES = 256 * 1024;  // 256 KB
                    uint8_t httpBuf[65536];
                    uint8_t headerBuf[128];  // First bytes for duration parsing
                    size_t headerLen = 0;
                    uint64_t totalBytes = 0;
                    size_t prebuffered = 0;
                    bool httpEof = false;

                    while (prebuffered < PREBUFFER_BYTES &&
                           audioTestRunning.load(std::memory_order_acquire) && !httpEof) {
                        ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 100);
                        if (n > 0) {
                            // Capture first bytes for duration parsing
                            if (headerLen < sizeof(headerBuf)) {
                                size_t copy = std::min(static_cast<size_t>(n),
                                                       sizeof(headerBuf) - headerLen);
                                std::memcpy(headerBuf + headerLen, httpBuf, copy);
                                headerLen += copy;
                            }
                            audioServerPtr->writeAudio(httpBuf, static_cast<size_t>(n));
                            totalBytes += n;
                            prebuffered += n;
                        } else if (n < 0 || !httpStream->isConnected()) {
                            httpEof = true;
                        }
                    }

                    if (prebuffered == 0 || !audioTestRunning.load(std::memory_order_acquire)) {
                        break;
                    }

                    // Auto-detect format from magic bytes when Content-Type is generic
                    // (Roon sends no Content-Type, falling back to application/octet-stream)
                    // Skip magic bytes when format=p (raw PCM) — PCM samples can
                    // accidentally match MP3 sync (0xFF 0xE0+) and cause false detection
                    if (fmtCode != 'p' && headerLen >= 4 &&
                        (contentType == "application/octet-stream" ||
                         contentType.empty())) {
                        if (headerBuf[0] == 'f' && headerBuf[1] == 'L' &&
                            headerBuf[2] == 'a' && headerBuf[3] == 'C') {
                            contentType = "audio/flac";
                            LOG_INFO("[Audio] Detected FLAC from magic bytes");
                        } else if (headerBuf[0] == 'R' && headerBuf[1] == 'I' &&
                                   headerBuf[2] == 'F' && headerBuf[3] == 'F') {
                            contentType = "audio/wav";
                            LOG_INFO("[Audio] Detected WAV from magic bytes");
                        } else if (headerBuf[0] == 'D' && headerBuf[1] == 'S' &&
                                   headerBuf[2] == 'D' && headerBuf[3] == ' ') {
                            contentType = "audio/dsf";
                            LOG_INFO("[Audio] Detected DSF from magic bytes");
                        } else if (headerBuf[0] == 'F' && headerBuf[1] == 'R' &&
                                   headerBuf[2] == 'M' && headerBuf[3] == '8') {
                            contentType = "audio/dff";
                            LOG_INFO("[Audio] Detected DFF from magic bytes");
                        } else if (headerBuf[0] == 'F' && headerBuf[1] == 'O' &&
                                   headerBuf[2] == 'R' && headerBuf[3] == 'M') {
                            contentType = "audio/aiff";
                            LOG_INFO("[Audio] Detected AIFF from magic bytes");
                        } else if ((headerBuf[0] == 0xFF && (headerBuf[1] & 0xE0) == 0xE0) ||
                                   (headerBuf[0] == 'I' && headerBuf[1] == 'D' && headerBuf[2] == '3')) {
                            contentType = "audio/mpeg";
                            LOG_INFO("[Audio] Detected MP3 from magic bytes");
                        }
                        // Update AudioHttpServer MIME type with detected format
                        if (contentType != "application/octet-stream") {
                            audioServerPtr->setPassthroughMime(contentType);
                        }
                    }

                    // Raw PCM (format=p): the format was already set upfront
                    // before prebuffer, so we only need to switch MIME to WAV
                    // generation mode (clear passthrough MIME).
                    if (contentType == "application/octet-stream" && fmtCode == 'p') {
                        audioServerPtr->setPassthroughMime("");
                        contentType = "audio/wav";
                        LOG_INFO("[Audio] Raw PCM (format=p), serving as WAV ("
                                 << audioFmt.sampleRate << "Hz/"
                                 << audioFmt.bitDepth << "bit/"
                                 << audioFmt.channels << "ch)");
                    }

                    // Parse track duration + format params from stream header
                    TrackInfo trackInfo = parseTrackInfo(
                        headerBuf, headerLen, contentType);
                    uint32_t trackDurationSec = trackInfo.durationSec;

                    // --- Start UPnP playback ---
                    audioServerPtr->setReadyToServe();
                    slimproto->updateElapsed(0, 0);
                    slimproto->updateStreamBytes(0);

                    // Detect cross-format change. Two cases trigger a cold restart
                    // (Stop + SetAVTransportURI + Play) instead of SetNextAVTransportURI:
                    //  1. Container change (FLAC→WAV, etc.): MIME type differs.
                    //  2. Same container but different sample rate / bit depth / channels
                    //     (e.g., 96kHz/24bit FLAC → 48kHz/24bit FLAC). The renderer's
                    //     anticipated preload connection probes the next stream and
                    //     consumes ring buffer data, which gets overwritten before the
                    //     real playback connection arrives — final samples are lost,
                    //     causing the renderer to drain prematurely and miss the next
                    //     SetNextAVTransportURI window.
                    bool mimeChanged = !firstTrack &&
                                       !prevContentType.empty() &&
                                       prevContentType != contentType;
                    bool formatParamsChanged = !firstTrack &&
                        prevTrackInfo.sampleRate > 0 &&
                        trackInfo.sampleRate > 0 &&
                        (prevTrackInfo.sampleRate != trackInfo.sampleRate ||
                         prevTrackInfo.bitDepth   != trackInfo.bitDepth   ||
                         prevTrackInfo.channels   != trackInfo.channels);
                    bool crossFormat = mimeChanged || formatParamsChanged;

                    // Elapsed clock — atomic so the PLAYING-state calibration thread
                    // can recalibrate it once the renderer actually starts playing.
                    // Stored as nanoseconds since steady_clock epoch (lock-free on x86/aarch64).
                    std::atomic<int64_t> playStartNs{
                        std::chrono::steady_clock::now().time_since_epoch().count()
                    };

                    if (firstTrack || crossFormat) {
                        // Cold start (or cross-format restart):
                        // Stop + SetAVTransportURI + Play.
                        // Cross-format transitions can't use SetNextAVTransportURI
                        // because the renderer's anticipated preload consumes ring
                        // buffer data (including the header), which gets overwritten
                        // by the circular buffer before the real playback starts.
                        if (crossFormat) {
                            if (mimeChanged) {
                                LOG_INFO("[Gapless] Cross-format change ("
                                         << prevContentType << " → " << contentType
                                         << "), using cold restart");
                            } else {
                                LOG_INFO("[Gapless] Format params changed ("
                                         << prevTrackInfo.sampleRate << "Hz/"
                                         << static_cast<int>(prevTrackInfo.bitDepth) << "bit/"
                                         << static_cast<int>(prevTrackInfo.channels) << "ch → "
                                         << trackInfo.sampleRate << "Hz/"
                                         << static_cast<int>(trackInfo.bitDepth) << "bit/"
                                         << static_cast<int>(trackInfo.channels) << "ch), "
                                         << "using cold restart");
                            }
                            int oldSlot = currentSlot.load();
                            servers[oldSlot]->signalEndOfStream();
                            upnpPtr->stop();
                            currentSlot.store(1 - oldSlot);
                        }
                        bool doCalibrate = config.calibrateElapsed;
                        std::thread([upnpPtr, audioServerPtr, &slimproto,
                                     &streamGeneration, thisGeneration,
                                     &playStartNs, doCalibrate]() {
                            upnpPtr->setAVTransportURI(audioServerPtr->getStreamURL());
                            if (streamGeneration.load() != thisGeneration) return;
                            upnpPtr->play();
                            slimproto->sendStat(StatEvent::STMl);

                            // Poll GetTransportInfo until PLAYING, then recalibrate
                            // the elapsed clock. Corrects the 1-5s drift caused by
                            // renderer startup latency (SetAVTransportURI processing,
                            // HTTP connect, prebuffer, DAC ramp-up).
                            if (!doCalibrate) return;
                            auto pollStart = std::chrono::steady_clock::now();
                            constexpr int POLL_TIMEOUT_MS = 10000;
                            constexpr int POLL_INTERVAL_MS = 150;
                            while (streamGeneration.load() == thisGeneration) {
                                auto elapsedMs = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - pollStart).count();
                                if (elapsedMs >= POLL_TIMEOUT_MS) {
                                    LOG_DEBUG("[UPnP] PLAYING-state poll timed out");
                                    break;
                                }
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(POLL_INTERVAL_MS));
                                std::string state = upnpPtr->getTransportState();
                                if (state == "PLAYING") {
                                    playStartNs.store(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch().count(),
                                        std::memory_order_release);
                                    LOG_INFO("[UPnP] Renderer PLAYING after "
                                             << elapsedMs << "ms — elapsed clock recalibrated");
                                    break;
                                }
                            }
                        }).detach();
                    } else {
                        // Gapless same-format: SetNextAVTransportURI + signal end on old slot
                        int oldSlot = currentSlot.load();
                        int newSlot = 1 - oldSlot;
                        std::string nextURL = servers[newSlot]->getStreamURL();
                        LOG_INFO("[Gapless] SetNextAVTransportURI: " << nextURL);
                        upnpPtr->setNextAVTransportURI(nextURL);
                        servers[oldSlot]->signalEndOfStream();
                        currentSlot.store(newSlot);
                        slimproto->sendStat(StatEvent::STMl);
                        LOG_INFO("[Gapless] Active slot now " << newSlot);
                    }

                    prevContentType = contentType;
                    prevTrackInfo = trackInfo;

                    // --- Stream loop: read HTTP → write to ring buffer ---
                    uint32_t lastElapsedLog = 0;

                    // Lambda reads playStartNs so calibration updates take effect
                    auto computeElapsedMs = [&playStartNs]() -> uint32_t {
                        int64_t nowNs = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
                        int64_t startNs = playStartNs.load(std::memory_order_acquire);
                        int64_t diffNs = nowNs - startNs;
                        if (diffNs < 0) diffNs = 0;
                        return static_cast<uint32_t>(diffNs / 1'000'000);
                    };

                    while (audioTestRunning.load(std::memory_order_acquire) && !httpEof) {
                        ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 10);
                        if (n > 0) {
                            audioServerPtr->writeAudio(httpBuf, static_cast<size_t>(n));
                            totalBytes += n;
                            slimproto->updateStreamBytes(totalBytes);
                        } else if (n < 0 || !httpStream->isConnected()) {
                            httpEof = true;
                        }

                        // Report ring buffer state to LMS so it knows we're consuming data.
                        // Without this, streamBufSize/streamBufFull stay at 0 and LMS may
                        // abort the stream around 2 minutes (observed with Qobuz tracks).
                        slimproto->updateBufferState(
                            static_cast<uint32_t>(audioServerPtr->getBufferCapacity()),
                            static_cast<uint32_t>(audioServerPtr->getBufferUsed()),
                            0, 0);

                        // Update elapsed via wall clock since Play (recalibrated
                        // to the renderer's actual PLAYING state if calibration ran)
                        uint32_t elapsedMs = computeElapsedMs();
                        uint32_t elapsedSec = elapsedMs / 1000;
                        slimproto->updateElapsed(elapsedSec, elapsedMs);

                        if (elapsedSec >= lastElapsedLog + 10) {
                            lastElapsedLog = elapsedSec;
                            LOG_DEBUG("[Audio] Elapsed: " << elapsedSec << "s (wall clock)");
                        }
                    }

                    // --- HTTP EOF: signal end of stream so ring buffer can drain ---
                    if (httpEof) {
                        audioServerPtr->signalEndOfStream();
                    }

                    // --- Wait for track to nearly finish, then send STMd ---
                    if (httpEof && audioTestRunning.load(std::memory_order_acquire)) {
                        // Raw PCM (format=p): compute actual duration from bytes.
                        // Roon pre-buffers ahead of real-time, so HTTP EOF arrives
                        // 15-20s before the renderer has finished playing. Without
                        // a known duration, STMd would be sent immediately and Roon's
                        // strm-q would trigger UPnP Stop, truncating the tail.
                        if (trackDurationSec == 0 && fmtCode == 'p'
                            && audioFmt.sampleRate > 0 && audioFmt.bitDepth > 0
                            && audioFmt.channels > 0) {
                            uint64_t bytesPerSec = static_cast<uint64_t>(audioFmt.sampleRate)
                                                 * audioFmt.channels
                                                 * (audioFmt.bitDepth / 8);
                            if (bytesPerSec > 0) {
                                trackDurationSec = static_cast<uint32_t>(totalBytes / bytesPerSec);
                                LOG_INFO("[Audio] PCM duration computed: "
                                         << trackDurationSec << "s ("
                                         << totalBytes << " bytes / "
                                         << bytesPerSec << " B/s)");
                            }
                        }

                        LOG_INFO("[Audio] HTTP complete: " << totalBytes
                                 << " bytes, duration=" << trackDurationSec << "s");

                        // Duration unknown (WAV streaming, no Content-Length):
                        // Send STMd immediately so LMS queues next track ASAP.
                        // The renderer still has buffered audio to play while LMS prepares.
                        // Waiting for STOPPED first is too late — the renderer releases
                        // the Diretta target before the next track can be queued.
                        if (trackDurationSec == 0) {
                            LOG_INFO("[Audio] Unknown duration — sending STMd early "
                                     "to trigger next track from LMS");
                        } else {
                            // Known duration: wait until wall clock reaches near track end
                            constexpr uint32_t STMD_LEAD_SEC = 3;
                            uint32_t stmdTargetSec = (trackDurationSec > STMD_LEAD_SEC)
                                ? trackDurationSec - STMD_LEAD_SEC : 0;

                            while (audioTestRunning.load(std::memory_order_acquire)) {
                                uint32_t elapsedMs = computeElapsedMs();
                                uint32_t elapsedSec = elapsedMs / 1000;
                                slimproto->updateElapsed(elapsedSec, elapsedMs);

                                if (elapsedSec >= lastElapsedLog + 10) {
                                    lastElapsedLog = elapsedSec;
                                    LOG_DEBUG("[Audio] Elapsed: " << elapsedSec
                                              << "s (waiting for track end)");
                                }

                                if (elapsedSec >= stmdTargetSec) {
                                    break;
                                }

                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }

                        uint32_t finalElapsed = computeElapsedMs() / 1000;
                        LOG_INFO("[Audio] STMd at " << finalElapsed << "s (track="
                                 << trackDurationSec << "s)");
                        slimproto->sendStat(StatEvent::STMd);
                    }

                    // --- Gapless: wait for next track ---
                    if (!hasPendingTrack.load(std::memory_order_acquire) &&
                        audioTestRunning.load(std::memory_order_acquire)) {
                        LOG_DEBUG("[Gapless] Waiting for next track...");
                        auto waitStart = std::chrono::steady_clock::now();
                        constexpr int GAPLESS_WAIT_MS = 2000;
                        while (!hasPendingTrack.load(std::memory_order_acquire) &&
                               audioTestRunning.load(std::memory_order_acquire) &&
                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - waitStart).count() < GAPLESS_WAIT_MS) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                    }

                    // --- Chain to next track? ---
                    if (hasPendingTrack.load(std::memory_order_acquire)) {
                        std::shared_ptr<PendingTrack> next;
                        {
                            std::lock_guard<std::mutex> lock(pendingMutex);
                            next = std::move(pendingNextTrack);
                            pendingNextTrack.reset();
                            hasPendingTrack.store(false, std::memory_order_release);
                        }
                        if (next) {
                            LOG_INFO("[Gapless] Chaining to next track");
                            httpStream->disconnect();
                            httpStream = next->httpClient;

                            // Update format parameters from the new track's strm-s
                            fmtCode = next->formatCode;
                            pRate = next->pcmSampleRate;
                            pSize = next->pcmSampleSize;
                            pChannels = next->pcmChannels;

                            // Send RESP/STMh for new track
                            slimproto->sendStat(StatEvent::STMc);
                            slimproto->sendResp(next->responseHeaders);
                            slimproto->sendStat(StatEvent::STMh);

                            firstTrack = false;
                            continue;  // Loop back for next track
                        }
                    }

                    break;  // No more tracks
                    }  // end track loop

                    // Signal end of stream if still running
                    if (audioTestRunning.load(std::memory_order_acquire)) {
                        audioServerPtr->signalEndOfStream();
                        slimproto->sendStat(StatEvent::STMu);
                    }
                    }  // end passthrough scope
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
                    servers[0]->reset();
                    servers[1]->reset();
                    // Only send STMf if something was actually playing
                    // (avoids confusing LMS during initial registration strm-q)
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
                    servers[0]->reset();
                    servers[1]->reset();
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
        servers[0]->signalEndOfStream();
        servers[1]->signalEndOfStream();
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
    //
    // slim2UPnP only registers with LMS/Roon when the UPnP renderer is ready.
    // If the renderer disappears (BYEBYE, SOAP error), Slimproto is disconnected
    // so Roon sees the player go offline. When the renderer comes back (ALIVE),
    // the player reconnects and becomes visible in Roon again.
    //
    // UPnPController already manages isReady() via:
    //  - UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE → m_ready = false
    //  - UPNP_DISCOVERY_ADVERTISEMENT_ALIVE  → re-parse description, m_ready = true
    //  - sendAction() SOCKET_CONNECT/SOCKET_ERROR → m_ready = false
    //
    // This loop adds the Slimproto connect/disconnect side of that contract.

    constexpr int INITIAL_BACKOFF_S = 2;
    constexpr int MAX_BACKOFF_S = 30;
    int backoffS = INITIAL_BACKOFF_S;
    int connectionCount = 0;

    while (g_running.load(std::memory_order_acquire)) {

        // --- Gate: wait for renderer to be ready before registering with LMS ---
        if (!upnpPtr->isReady()) {
            if (connectionCount == 0) {
                // First startup: renderer not yet ready (should not happen — discoverRenderer
                // already waits, but guard against a race on connectDirect path)
                LOG_WARN("Renderer not ready at startup — waiting...");
            } else {
                // Renderer was lost: log once then poll silently
                LOG_WARN("Renderer unavailable — waiting before reconnecting to LMS...");
            }
            while (g_running.load(std::memory_order_acquire) && !upnpPtr->isReady()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (!g_running.load(std::memory_order_acquire)) break;
            LOG_INFO("Renderer ready — connecting to LMS");
        }

        // --- Reconnect backoff (skip on very first connection) ---
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

        // Monitor: stay connected while both LMS and renderer are up.
        // When the renderer goes away (isReady() → false), drop the Slimproto
        // connection so Roon sees the player go offline immediately.
        while (g_running.load(std::memory_order_acquire) && slimproto->isConnected()) {
            if (!upnpPtr->isReady()) {
                LOG_WARN("Renderer lost — disconnecting from LMS (player offline in Roon)");
                break;  // Fall through to stopAudioThread + disconnect below
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        stopAudioThread();
        slimproto->disconnect();
        if (slimprotoThread.joinable()) {
            slimprotoThread.join();
        }

        if (!g_running.load(std::memory_order_acquire)) break;

        if (!upnpPtr->isReady()) {
            // Renderer-triggered disconnect: loop back to the renderer gate above.
            // Do NOT print "Lost connection to LMS" — it was an intentional drop.
            LOG_INFO("Waiting for renderer to come back...");
            // Reset backoff — this is a renderer issue, not an LMS issue
            backoffS = INITIAL_BACKOFF_S;
        } else {
            LOG_WARN("Lost connection to LMS");
        }
    }

    // ============================================
    // Final shutdown
    // ============================================

    std::cout << "\nShutting down..." << std::endl;
    stopAudioThread();
    g_slimproto = nullptr;
    slimproto->disconnect();

    audioServerA->stop();
    audioServerB->stop();
    upnp->shutdown();

    return 0;
}
