/**
 * @file AudioHttpServer.cpp
 * @brief Mini HTTP server serving decoded audio to a UPnP renderer
 */

#include "AudioHttpServer.h"
#include "LogLevel.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <cstring>
#include <algorithm>
#include <sstream>

// ============================================================================
// Construction / Destruction
// ============================================================================

AudioHttpServer::AudioHttpServer() = default;

AudioHttpServer::~AudioHttpServer() {
    stop();
}

// ============================================================================
// Server lifecycle
// ============================================================================

bool AudioHttpServer::start(uint16_t port) {
    if (m_running.load()) {
        LOG_WARN("[AudioHttpServer] Already running");
        return true;
    }

    // Auto-detect local IP if not set
    if (m_localIP.empty()) {
        struct ifaddrs* ifas = nullptr;
        if (getifaddrs(&ifas) == 0) {
            for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                if (!(ifa->ifa_flags & IFF_UP))
                    continue;
                char buf[INET_ADDRSTRLEN];
                auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
                m_localIP = buf;
                break;
            }
            freeifaddrs(ifas);
        }
        if (m_localIP.empty()) {
            LOG_ERROR("[AudioHttpServer] Cannot detect local IP");
            return false;
        }
    }

    // Create listening socket
    m_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenSocket < 0) {
        LOG_ERROR("[AudioHttpServer] socket() failed: " << strerror(errno));
        return false;
    }

    int optval = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(m_listenSocket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("[AudioHttpServer] bind() failed: " << strerror(errno));
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    // Get actual port (if auto-selected)
    socklen_t addrLen = sizeof(addr);
    getsockname(m_listenSocket, reinterpret_cast<struct sockaddr*>(&addr), &addrLen);
    m_port = ntohs(addr.sin_port);

    if (listen(m_listenSocket, 1) < 0) {
        LOG_ERROR("[AudioHttpServer] listen() failed: " << strerror(errno));
        close(m_listenSocket);
        m_listenSocket = -1;
        return false;
    }

    m_running.store(true);
    m_serverThread = std::thread(&AudioHttpServer::serverLoop, this);

    LOG_INFO("[AudioHttpServer] Listening on http://" << m_localIP << ":" << m_port);
    return true;
}

void AudioHttpServer::stop() {
    m_running.store(false);
    m_endOfStream.store(true);

    // Wake up any blocked producers/consumers
    m_dataAvailable.notify_all();
    m_spaceAvailable.notify_all();

    // Close client socket to unblock handleClient
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSocket >= 0) {
            shutdown(m_clientSocket, SHUT_RDWR);
            close(m_clientSocket);
            m_clientSocket = -1;
        }
    }

    // Close listen socket to unblock accept
    if (m_listenSocket >= 0) {
        shutdown(m_listenSocket, SHUT_RDWR);
        close(m_listenSocket);
        m_listenSocket = -1;
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    m_clientConnected.store(false);
    LOG_DEBUG("[AudioHttpServer] Stopped");
}

// ============================================================================
// Stream URL
// ============================================================================

std::string AudioHttpServer::getStreamURL() const {
    std::string ext = getStreamExtension();
    return "http://" + m_localIP + ":" + std::to_string(m_port) + "/" + ext;
}

std::string AudioHttpServer::getStreamExtension() const {
    if (!m_passthroughMime.empty()) {
        // Map MIME type to file extension
        if (m_passthroughMime.find("flac") != std::string::npos) return "audio.flac";
        if (m_passthroughMime.find("dsf") != std::string::npos) return "audio.dsf";
        if (m_passthroughMime.find("dff") != std::string::npos) return "audio.dff";
        if (m_passthroughMime.find("wav") != std::string::npos) return "audio.wav";
        if (m_passthroughMime.find("aiff") != std::string::npos) return "audio.aiff";
        if (m_passthroughMime.find("mpeg") != std::string::npos) return "audio.mp3";
        if (m_passthroughMime.find("mp3") != std::string::npos) return "audio.mp3";
        if (m_passthroughMime.find("aac") != std::string::npos) return "audio.aac";
        if (m_passthroughMime.find("mp4") != std::string::npos) return "audio.m4a";
        if (m_passthroughMime.find("ogg") != std::string::npos) return "audio.ogg";
        return "audio.bin";
    }
    return m_format.isDSD ? "audio.dsf" : "audio.wav";
}

std::string AudioHttpServer::getMimeType() const {
    if (!m_passthroughMime.empty()) {
        return m_passthroughMime;
    }
    if (m_format.isDSD) {
        return "application/octet-stream";
    }
    return "audio/wav";
}

void AudioHttpServer::setPassthroughMime(const std::string& contentType) {
    m_passthroughMime = contentType;
}

// ============================================================================
// Audio format
// ============================================================================

void AudioHttpServer::setFormat(const AudioFormat& format) {
    m_format = format;

    // Resize ring buffer based on format
    double seconds = format.isDSD ? DSD_BUFFER_SECONDS : PCM_BUFFER_SECONDS;
    size_t targetSize = static_cast<size_t>(format.bytesPerSecond() * seconds);
    targetSize = std::max(targetSize, MIN_BUFFER_SIZE);
    targetSize = std::min(targetSize, MAX_BUFFER_SIZE);

    // Align to frame boundary
    size_t frameSize = format.bytesPerFrame();
    if (frameSize > 0) {
        targetSize = (targetSize / frameSize) * frameSize;
    }

    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_ringBuffer.resize(targetSize);
        m_ringCapacity = targetSize;
        m_writePos = 0;
        m_readPos = 0;
    }

    m_formatReady.store(true);
    m_readyToServe.store(false);  // Wait for prebuffer before serving
    m_endOfStream.store(false);

    LOG_INFO("[AudioHttpServer] Format: " << format.sampleRate << " Hz, "
             << format.bitDepth << "-bit, " << format.channels << " ch"
             << (format.isDSD ? " (DSD)" : "")
             << " | Buffer: " << (targetSize / 1024) << " KB");
}

void AudioHttpServer::setReadyToServe() {
    m_readyToServe.store(true);
    LOG_DEBUG("[AudioHttpServer] Ready to serve (prebuffer done)");
}

// ============================================================================
// Audio data (producer — audio/decode thread)
// ============================================================================

size_t AudioHttpServer::writeAudio(const uint8_t* data, size_t bytes) {
    if (!m_running.load() || bytes == 0) return 0;

    size_t totalWritten = 0;

    while (totalWritten < bytes && m_running.load() && !m_endOfStream.load()) {
        std::unique_lock<std::mutex> lock(m_ringMutex);

        // Wait for space in ring buffer
        m_spaceAvailable.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return ringFree() > 0 || !m_running.load() || m_endOfStream.load();
        });

        if (!m_running.load() || m_endOfStream.load()) break;

        size_t toWrite = std::min(bytes - totalWritten, ringFree());
        if (toWrite > 0) {
            ringWrite(data + totalWritten, toWrite);
            totalWritten += toWrite;
            lock.unlock();
            m_dataAvailable.notify_one();
        }
    }

    return totalWritten;
}

void AudioHttpServer::signalEndOfStream() {
    m_endOfStream.store(true);
    m_dataAvailable.notify_all();
    LOG_DEBUG("[AudioHttpServer] End of stream signaled");
}

void AudioHttpServer::reset() {
    LOG_DEBUG("[AudioHttpServer] Reset");

    m_endOfStream.store(true);
    m_formatReady.store(false);
    m_readyToServe.store(false);
    m_passthroughMime.clear();

    // Disconnect current client to force re-fetch
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSocket >= 0) {
            shutdown(m_clientSocket, SHUT_RDWR);
        }
    }

    // Wake blocked threads
    m_dataAvailable.notify_all();
    m_spaceAvailable.notify_all();

    // Clear ring buffer
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        m_writePos = 0;
        m_readPos = 0;
    }

    m_bytesServed.store(0, std::memory_order_relaxed);
    m_endOfStream.store(false);
}

// ============================================================================
// Flow control
// ============================================================================

float AudioHttpServer::getBufferLevel() const {
    // Approximate — no lock for performance
    if (m_ringCapacity == 0) return 0.0f;
    size_t wp = m_writePos;
    size_t rp = m_readPos;
    size_t used = (wp >= rp) ? (wp - rp) : (m_ringCapacity - rp + wp);
    return static_cast<float>(used) / static_cast<float>(m_ringCapacity);
}

// ============================================================================
// WAV header
// ============================================================================

std::vector<uint8_t> AudioHttpServer::buildWavHeader() const {
    // RIFF/WAV header for streaming (unknown length)
    uint32_t sampleRate = m_format.sampleRate;
    uint16_t channels = static_cast<uint16_t>(m_format.channels);
    uint16_t bitsPerSample = static_cast<uint16_t>(m_format.bitDepth);
    uint16_t blockAlign = channels * (bitsPerSample / 8);
    uint32_t byteRate = sampleRate * blockAlign;

    // Use max value for unknown streaming length
    uint32_t dataSize = 0x7FFFFFFF;
    uint32_t riffSize = dataSize + 36;  // Will overflow, that's OK for streaming

    std::vector<uint8_t> hdr(44);
    auto put16 = [&](size_t off, uint16_t v) {
        hdr[off]     = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&](size_t off, uint32_t v) {
        hdr[off]     = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        hdr[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        hdr[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };

    // RIFF header
    hdr[0] = 'R'; hdr[1] = 'I'; hdr[2] = 'F'; hdr[3] = 'F';
    put32(4, riffSize);
    hdr[8] = 'W'; hdr[9] = 'A'; hdr[10] = 'V'; hdr[11] = 'E';

    // fmt chunk
    hdr[12] = 'f'; hdr[13] = 'm'; hdr[14] = 't'; hdr[15] = ' ';
    put32(16, 16);              // Chunk size
    put16(20, 1);               // PCM format
    put16(22, channels);
    put32(24, sampleRate);
    put32(28, byteRate);
    put16(32, blockAlign);
    put16(34, bitsPerSample);

    // data chunk
    hdr[36] = 'd'; hdr[37] = 'a'; hdr[38] = 't'; hdr[39] = 'a';
    put32(40, dataSize);

    return hdr;
}

std::vector<uint8_t> AudioHttpServer::buildDsfHeader() const {
    // DSF file header for DSD streaming
    // Minimal DSF: DSD chunk (28) + fmt chunk (52) + data chunk header (12) = 92 bytes header
    uint32_t channels = m_format.channels;
    uint32_t dsdRate = m_format.dsdRate;

    // Use large placeholder sizes for streaming (like WAV uses 0x7FFFFFFF)
    // FFmpeg's DSF demuxer needs non-zero sizes to work correctly
    uint64_t dataChunkSize = 0x7FFFFFFFFFFFFFFFULL;  // Max int64 (data chunk header + data)
    uint64_t totalFileSize = 28 + 52 + dataChunkSize; // DSD + fmt + data chunks

    std::vector<uint8_t> hdr(92, 0);
    auto put32 = [&](size_t off, uint32_t v) {
        hdr[off]     = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        hdr[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        hdr[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto put64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; i++) {
            hdr[off + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
        }
    };

    // DSD chunk (28 bytes)
    hdr[0] = 'D'; hdr[1] = 'S'; hdr[2] = 'D'; hdr[3] = ' ';
    put64(4, 28);               // DSD chunk size
    put64(12, totalFileSize);   // Total file size
    put64(20, 0);               // Metadata offset (none)

    // fmt chunk (52 bytes)
    hdr[28] = 'f'; hdr[29] = 'm'; hdr[30] = 't'; hdr[31] = ' ';
    put64(32, 52);              // fmt chunk size
    put32(40, 1);               // Format version
    put32(44, 0);               // Format ID: DSD raw
    put32(48, channels == 1 ? 1 : 2);  // Channel type: mono or stereo
    put32(52, channels);        // Channel count
    put32(56, dsdRate);         // Sample rate (DSD bit rate)
    put32(60, 1);               // Bits per sample (1 for DSD)
    put64(64, 0x7FFFFFFFFFFFFFFFULL);  // Sample count (large value for streaming)
    put32(72, 4096);            // Block size per channel
    put32(76, 0);               // Reserved

    // data chunk header (12 bytes)
    hdr[80] = 'd'; hdr[81] = 'a'; hdr[82] = 't'; hdr[83] = 'a';
    put64(84, dataChunkSize);   // Data chunk size

    return hdr;
}

// ============================================================================
// Server thread
// ============================================================================

void AudioHttpServer::serverLoop() {
    LOG_DEBUG("[AudioHttpServer] Server thread started");

    while (m_running.load()) {
        // Poll for incoming connections with timeout (for clean shutdown)
        struct pollfd pfd{};
        pfd.fd = m_listenSocket;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 200);  // 200ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (m_running.load()) {
                LOG_ERROR("[AudioHttpServer] poll() error: " << strerror(errno));
            }
            break;
        }
        if (ret == 0) continue;  // Timeout, check m_running

        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_listenSocket,
                              reinterpret_cast<struct sockaddr*>(&clientAddr),
                              &clientLen);
        if (clientFd < 0) {
            if (m_running.load() && errno != EINVAL) {
                LOG_ERROR("[AudioHttpServer] accept() error: " << strerror(errno));
            }
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        LOG_INFO("[AudioHttpServer] Client connected from " << clientIP);

        // Set TCP_NODELAY for low latency
        int optval = 1;
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            m_clientSocket = clientFd;
        }
        m_clientConnected.store(true);

        handleClient(clientFd);

        m_clientConnected.store(false);
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            if (m_clientSocket == clientFd) {
                m_clientSocket = -1;
            }
        }
        close(clientFd);

        LOG_INFO("[AudioHttpServer] Client disconnected");
    }

    LOG_DEBUG("[AudioHttpServer] Server thread exiting");
}

void AudioHttpServer::handleClient(int clientSocket) {
    // Read HTTP request (we only need to check it's a GET)
    char reqBuf[2048];
    ssize_t reqLen = recv(clientSocket, reqBuf, sizeof(reqBuf) - 1, 0);
    if (reqLen <= 0) return;
    reqBuf[reqLen] = '\0';

    // Basic HTTP request validation
    if (strncmp(reqBuf, "GET ", 4) != 0) {
        const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        send(clientSocket, resp, strlen(resp), MSG_NOSIGNAL);
        return;
    }

    LOG_DEBUG("[AudioHttpServer] Received request: " << std::string(reqBuf, std::min(reqLen, (ssize_t)80)));

    // Wait for prebuffer to be ready (not just format — data must be in the ring buffer)
    while (!m_readyToServe.load() && m_running.load() && !m_endOfStream.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!m_readyToServe.load() || !m_running.load()) return;

    // Build HTTP response headers
    std::string mimeType = getMimeType();
    std::string responseHeader =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: " + mimeType + "\r\n"
        "Accept-Ranges: none\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "transferMode.dlna.org: Streaming\r\n"
        "\r\n";

    // Send HTTP headers
    if (send(clientSocket, responseHeader.c_str(), responseHeader.size(), MSG_NOSIGNAL) < 0) {
        LOG_ERROR("[AudioHttpServer] Failed to send HTTP header: " << strerror(errno));
        return;
    }

    // Send audio container header (WAV or DSF) — skip in passthrough mode
    if (m_passthroughMime.empty()) {
        std::vector<uint8_t> containerHeader;
        if (m_format.isDSD) {
            containerHeader = buildDsfHeader();
        } else {
            containerHeader = buildWavHeader();
        }

        if (!containerHeader.empty()) {
            if (send(clientSocket, containerHeader.data(), containerHeader.size(), MSG_NOSIGNAL) < 0) {
                LOG_ERROR("[AudioHttpServer] Failed to send container header: " << strerror(errno));
                return;
            }
        }
    }

    // Stream audio data from ring buffer
    std::vector<uint8_t> sendBuf(32768);  // 32 KB send chunks

    while (m_running.load()) {
        size_t bytesRead = 0;

        {
            std::unique_lock<std::mutex> lock(m_ringMutex);

            // Wait for data or end-of-stream
            m_dataAvailable.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return ringAvailable() > 0 || m_endOfStream.load() || !m_running.load();
            });

            if (!m_running.load()) break;

            bytesRead = ringRead(sendBuf.data(), sendBuf.size());

            if (bytesRead > 0) {
                lock.unlock();
                m_spaceAvailable.notify_one();
            }
        }

        if (bytesRead > 0) {
            // Send data to client
            size_t sent = 0;
            while (sent < bytesRead) {
                ssize_t n = send(clientSocket, sendBuf.data() + sent,
                                 bytesRead - sent, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (n < 0) {
                        LOG_DEBUG("[AudioHttpServer] Client write error: " << strerror(errno));
                    }
                    return;  // Client disconnected
                }
                sent += static_cast<size_t>(n);
                m_bytesServed.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            }
        } else if (m_endOfStream.load()) {
            // No more data and end of stream — done
            LOG_DEBUG("[AudioHttpServer] Stream complete, closing client connection");
            return;
        }
        // else: timeout with no data, loop and check again
    }
}

// ============================================================================
// Ring buffer helpers
// ============================================================================

size_t AudioHttpServer::ringAvailable() const {
    // Must be called with m_ringMutex held
    if (m_ringCapacity == 0) return 0;
    return (m_writePos >= m_readPos)
        ? (m_writePos - m_readPos)
        : (m_ringCapacity - m_readPos + m_writePos);
}

size_t AudioHttpServer::ringFree() const {
    // Must be called with m_ringMutex held
    if (m_ringCapacity == 0) return 0;
    return m_ringCapacity - ringAvailable() - 1;  // -1 to distinguish full from empty
}

size_t AudioHttpServer::ringRead(uint8_t* dst, size_t maxBytes) {
    // Must be called with m_ringMutex held
    size_t avail = ringAvailable();
    size_t toRead = std::min(maxBytes, avail);
    if (toRead == 0) return 0;

    size_t firstPart = std::min(toRead, m_ringCapacity - m_readPos);
    std::memcpy(dst, m_ringBuffer.data() + m_readPos, firstPart);

    if (toRead > firstPart) {
        std::memcpy(dst + firstPart, m_ringBuffer.data(), toRead - firstPart);
    }

    m_readPos = (m_readPos + toRead) % m_ringCapacity;
    return toRead;
}

size_t AudioHttpServer::ringWrite(const uint8_t* src, size_t bytes) {
    // Must be called with m_ringMutex held
    size_t free = ringFree();
    size_t toWrite = std::min(bytes, free);
    if (toWrite == 0) return 0;

    size_t firstPart = std::min(toWrite, m_ringCapacity - m_writePos);
    std::memcpy(m_ringBuffer.data() + m_writePos, src, firstPart);

    if (toWrite > firstPart) {
        std::memcpy(m_ringBuffer.data(), src + firstPart, toWrite - firstPart);
    }

    m_writePos = (m_writePos + toWrite) % m_ringCapacity;
    return toWrite;
}

void AudioHttpServer::ringReset() {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    m_writePos = 0;
    m_readPos = 0;
}
