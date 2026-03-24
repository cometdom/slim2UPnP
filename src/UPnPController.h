/**
 * @file UPnPController.h
 * @brief UPnP control point for discovering and controlling MediaRenderers
 *
 * Uses libupnp (pupnp) for SSDP discovery, SOAP action sending, and XML parsing.
 * Thread-safe: can be called from audio thread and main thread.
 */

#ifndef SLIM2UPNP_UPNPCONTROLLER_H
#define SLIM2UPNP_UPNPCONTROLLER_H

#include <upnp/upnp.h>
#include <upnp/ixml.h>

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

class UPnPController {
public:
    struct RendererInfo {
        std::string friendlyName;
        std::string uuid;                   // UDN (e.g., "uuid:xxxx-yyyy")
        std::string location;               // Device description URL
        std::string avTransportControlURL;
        std::string avTransportEventURL;
        std::string renderingControlURL;
        std::string connectionManagerURL;
        std::string baseURL;                // Base URL for relative control URLs
        std::string protocolInfo;           // Cached from GetProtocolInfo (Sink)
    };

    UPnPController();
    ~UPnPController();

    // --- Lifecycle ---

    /// Initialize libupnp and register control point.
    /// networkInterface: empty for auto-detect, or e.g. "eth0".
    bool init(const std::string& networkInterface = "");

    /// Shutdown libupnp, unregister client.
    void shutdown();

    // --- Discovery ---

    /// Discover a renderer matching name/UUID. Retries indefinitely until
    /// found or *stopSignal becomes true. Returns true if found.
    bool discoverRenderer(const std::string& rendererMatch,
                          std::atomic<bool>* stopSignal = nullptr);

    /// Connect to a renderer directly via its description URL (skip SSDP).
    /// Useful when multicast doesn't work (e.g., WSL2, cross-subnet).
    bool connectDirect(const std::string& descriptionURL);

    /// Quick scan: discover renderers for timeoutSec seconds, return list.
    static std::vector<RendererInfo> scanRenderers(
        const std::string& networkInterface = "",
        int timeoutSec = 5);

    // --- AVTransport control ---

    bool setAVTransportURI(const std::string& uri,
                           const std::string& metadata = "");
    bool play(const std::string& speed = "1");
    bool stop();
    bool pause();

    // --- RenderingControl ---

    bool setVolume(int volume);
    int getVolume();

    // --- ConnectionManager ---

    /// Query and cache GetProtocolInfo (Sink). Call after discovery.
    bool queryProtocolInfo();

    /// Check if renderer supports a given MIME type.
    bool supportsFormat(const std::string& mimeType) const;

    /// Check if renderer supports DSD (DSF, DoP, etc.).
    bool supportsDSD() const;

    // --- Transport state ---

    /// Returns "STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING", etc.
    std::string getTransportState();

    /// Current position in seconds (-1 on error).
    int getPositionSeconds();

    // --- Accessors ---

    const RendererInfo& getRenderer() const { return m_renderer; }
    bool isReady() const { return m_ready.load(); }

    /// Server IP as seen by libupnp (useful for AudioHttpServer).
    std::string getServerIP() const;

private:
    // --- libupnp callback ---
    static int ctrlPointCallback(Upnp_EventType eventType,
                                 const void* event, void* cookie);
    int handleEvent(Upnp_EventType eventType, const void* event);

    // --- SOAP helpers ---
    /// Build an action document, send it, return response (caller must free).
    IXML_Document* sendAction(
        const std::string& controlURL,
        const std::string& serviceType,
        const std::string& actionName,
        const std::vector<std::pair<std::string, std::string>>& args = {});

    /// Extract a value from an action response document.
    static std::string getResponseValue(IXML_Document* response,
                                        const std::string& tagName);

    // --- XML parsing helpers ---
    bool parseDeviceDescription(const std::string& location);

    /// Resolve a potentially relative URL against the base URL.
    std::string resolveURL(const std::string& base,
                           const std::string& relative) const;

    /// Find a text element value in an XML document.
    static std::string getXmlElementValue(IXML_Document* doc,
                                          const std::string& tagName);
    static std::string getXmlElementValue(IXML_Element* element,
                                          const std::string& tagName);

    /// Find a service's control/event URL by serviceType.
    bool findServiceURLs(IXML_Document* doc,
                         const std::string& serviceType,
                         std::string& controlURL,
                         std::string& eventURL);

    // --- State ---
    UpnpClient_Handle m_clientHandle = -1;
    RendererInfo m_renderer;
    std::atomic<bool> m_ready{false};
    bool m_initialized = false;
    mutable std::mutex m_mutex;

    // --- Discovery synchronization ---
    std::mutex m_discoveryMutex;
    std::condition_variable m_discoveryCv;
    std::vector<RendererInfo> m_discovered;
    std::string m_discoveryMatch;       // What we're looking for
    bool m_discoveryFound = false;

    // --- Service type constants ---
    static constexpr const char* MEDIA_RENDERER_TYPE =
        "urn:schemas-upnp-org:device:MediaRenderer:1";
    static constexpr const char* AVTRANSPORT_TYPE =
        "urn:schemas-upnp-org:service:AVTransport:1";
    static constexpr const char* RENDERING_CONTROL_TYPE =
        "urn:schemas-upnp-org:service:RenderingControl:1";
    static constexpr const char* CONNECTION_MANAGER_TYPE =
        "urn:schemas-upnp-org:service:ConnectionManager:1";
};

#endif // SLIM2UPNP_UPNPCONTROLLER_H
