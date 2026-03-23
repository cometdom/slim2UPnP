/**
 * @file AudioHttpServer.h
 * @brief Mini HTTP server serving decoded audio to a UPnP renderer
 *
 * Replaces DirettaSync as the audio output. Instead of pushing packets
 * to a Diretta target, it serves audio via HTTP to the UPnP renderer
 * (pull model: renderer GETs audio from this server).
 *
 * Threading model:
 *   - Producer: audio/decode thread calls writeAudio()
 *   - Consumer: HTTP server thread reads ring buffer in handleClient()
 *   - Server: dedicated thread running accept loop
 */

#ifndef SLIM2UPNP_AUDIOHTTPSERVER_H
#define SLIM2UPNP_AUDIOHTTPSERVER_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

class AudioHttpServer {
public:
    struct AudioFormat {
        uint32_t sampleRate = 44100;
        uint32_t bitDepth = 16;         // Output bit depth (16, 24, 32)
        uint32_t channels = 2;
        bool isDSD = false;
        uint32_t dsdRate = 0;           // DSD bit rate when isDSD=true

        size_t bytesPerFrame() const {
            if (isDSD) return channels;  // 1 byte per channel per DSD frame
            return channels * (bitDepth / 8);
        }

        size_t bytesPerSecond() const {
            if (isDSD) return (dsdRate / 8) * channels;
            return sampleRate * bytesPerFrame();
        }
    };

    AudioHttpServer();
    ~AudioHttpServer();

    // --- Server lifecycle ---

    /// Start HTTP server. port=0 for auto-select. Returns true on success.
    bool start(uint16_t port = 0);

    /// Stop server and close all connections.
    void stop();

    bool isRunning() const { return m_running.load(); }

    // --- Stream URL ---

    /// URL for UPnP SetAVTransportURI (e.g., "http://192.168.1.50:8080/audio.wav")
    std::string getStreamURL() const;

    /// Actual port the server is listening on.
    uint16_t getPort() const { return m_port; }

    /// Local IP address (auto-detected or set).
    std::string getLocalIP() const { return m_localIP; }

    /// Set local IP explicitly (call before start() if needed).
    void setLocalIP(const std::string& ip) { m_localIP = ip; }

    // --- Audio format ---

    /// Configure format for next stream. Call before writeAudio().
    /// This creates the ring buffer but does NOT allow clients to read yet.
    void setFormat(const AudioFormat& format);

    /// Signal that prebuffering is done and clients can start reading.
    /// Call after filling the ring buffer with initial data.
    void setReadyToServe();

    const AudioFormat& getFormat() const { return m_format; }

    /// MIME type for current format (e.g., "audio/wav", "audio/dsf").
    std::string getMimeType() const;

    // --- Audio data (called from audio/decode thread) ---

    /// Write decoded audio into ring buffer.
    /// PCM: interleaved samples at target bit depth (already converted from S32).
    /// DSD: planar DSD bytes.
    /// Blocks if ring buffer is full. Returns bytes written.
    size_t writeAudio(const uint8_t* data, size_t bytes);

    /// Signal end of current stream (ring buffer will drain then close).
    void signalEndOfStream();

    /// Reset for new stream: clear ring buffer, ready for new format/data.
    void reset();

    // --- Flow control ---

    /// Ring buffer fill level (0.0 = empty, 1.0 = full).
    float getBufferLevel() const;

    /// True if a UPnP renderer is currently connected and reading.
    bool isClientConnected() const { return m_clientConnected.load(); }

    // --- WAV header ---

    /// Build a WAV/RIFF header for streaming (data size = 0x7FFFFFFF).
    std::vector<uint8_t> buildWavHeader() const;

    /// Build a DSF header for streaming DSD.
    std::vector<uint8_t> buildDsfHeader() const;

private:
    // --- Server thread ---
    void serverLoop();
    void handleClient(int clientSocket);

    // --- Ring buffer helpers ---
    size_t ringAvailable() const;   // Bytes available to read
    size_t ringFree() const;        // Bytes free for writing
    size_t ringRead(uint8_t* dst, size_t maxBytes);
    size_t ringWrite(const uint8_t* src, size_t bytes);
    void ringReset();

    // --- Ring buffer ---
    std::vector<uint8_t> m_ringBuffer;
    size_t m_ringCapacity = 0;
    size_t m_writePos = 0;
    size_t m_readPos = 0;
    std::mutex m_ringMutex;
    std::condition_variable m_dataAvailable;    // Consumer waits
    std::condition_variable m_spaceAvailable;   // Producer waits

    static constexpr size_t MIN_BUFFER_SIZE = 64 * 1024;       // 64 KB
    static constexpr size_t MAX_BUFFER_SIZE = 32 * 1024 * 1024; // 32 MB
    static constexpr double PCM_BUFFER_SECONDS = 2.0;
    static constexpr double DSD_BUFFER_SECONDS = 1.0;

    // --- Audio format ---
    AudioFormat m_format;
    std::atomic<bool> m_formatReady{false};
    std::atomic<bool> m_readyToServe{false};  // Set after prebuffer is done

    // --- Server state ---
    int m_listenSocket = -1;
    uint16_t m_port = 0;
    std::string m_localIP;
    std::thread m_serverThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_endOfStream{false};
    std::atomic<bool> m_clientConnected{false};
    int m_clientSocket = -1;            // Current client (for abort on reset)
    std::mutex m_clientMutex;
};

#endif // SLIM2UPNP_AUDIOHTTPSERVER_H
