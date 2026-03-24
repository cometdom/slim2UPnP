/**
 * @file UPnPController.cpp
 * @brief UPnP control point implementation using libupnp (pupnp)
 */

#include "UPnPController.h"
#include "LogLevel.h"

#include <upnp/upnptools.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>

// ============================================================================
// Construction / Destruction
// ============================================================================

UPnPController::UPnPController() = default;

UPnPController::~UPnPController() {
    shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool UPnPController::init(const std::string& networkInterface) {
    if (m_initialized) return true;

    const char* iface = networkInterface.empty() ? nullptr : networkInterface.c_str();
    int ret = UpnpInit2(iface, 0);
    if (ret != UPNP_E_SUCCESS) {
        LOG_ERROR("[UPnP] UpnpInit2 failed: " << UpnpGetErrorMessage(ret));
        return false;
    }

    ret = UpnpRegisterClient(reinterpret_cast<Upnp_FunPtr>(ctrlPointCallback), this, &m_clientHandle);
    if (ret != UPNP_E_SUCCESS) {
        LOG_ERROR("[UPnP] UpnpRegisterClient failed: " << UpnpGetErrorMessage(ret));
        UpnpFinish();
        return false;
    }

    m_initialized = true;
    LOG_INFO("[UPnP] Initialized, IP: " << getServerIP());
    return true;
}

void UPnPController::shutdown() {
    if (!m_initialized) return;

    m_ready.store(false);

    if (m_clientHandle >= 0) {
        UpnpUnRegisterClient(m_clientHandle);
        m_clientHandle = -1;
    }

    UpnpFinish();
    m_initialized = false;
    LOG_DEBUG("[UPnP] Shutdown complete");
}

std::string UPnPController::getServerIP() const {
    char* ip = UpnpGetServerIpAddress();
    return ip ? std::string(ip) : "";
}

// ============================================================================
// Discovery
// ============================================================================

bool UPnPController::discoverRenderer(const std::string& rendererMatch,
                                       std::atomic<bool>* stopSignal) {
    if (!m_initialized) return false;

    LOG_INFO("[UPnP] Searching for renderer"
             << (rendererMatch.empty() ? " (any)" : ": " + rendererMatch) << "...");

    m_discoveryMatch = rendererMatch;
    m_discoveryFound = false;

    while (!m_discoveryFound) {
        // stopSignal is g_running (true = keep running, false = stop)
        if (stopSignal && !stopSignal->load()) return false;

        // Clear previous results
        {
            std::lock_guard<std::mutex> lock(m_discoveryMutex);
            m_discovered.clear();
        }

        int ret = UpnpSearchAsync(m_clientHandle, 5, MEDIA_RENDERER_TYPE, this);
        if (ret != UPNP_E_SUCCESS) {
            LOG_WARN("[UPnP] Search failed: " << UpnpGetErrorMessage(ret));
        }

        // Wait for results (up to 6 seconds)
        {
            std::unique_lock<std::mutex> lock(m_discoveryMutex);
            m_discoveryCv.wait_for(lock, std::chrono::seconds(6), [this, stopSignal] {
                return m_discoveryFound || (stopSignal && !stopSignal->load());
            });
        }

        if (m_discoveryFound) break;

        if (stopSignal && !stopSignal->load()) return false;

        LOG_DEBUG("[UPnP] No matching renderer found, retrying...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // Query protocol info after discovery
    queryProtocolInfo();

    // Force volume to 100% for bit-perfect playback
    setVolume(100);

    LOG_INFO("[UPnP] Renderer found: " << m_renderer.friendlyName
             << " (" << m_renderer.uuid << ")");
    m_ready.store(true);
    return true;
}

bool UPnPController::connectDirect(const std::string& descriptionURL) {
    if (!m_initialized) return false;

    LOG_INFO("[UPnP] Direct connection to: " << descriptionURL);

    IXML_Document* xmlDoc = nullptr;
    int ret = UpnpDownloadXmlDoc(descriptionURL.c_str(), &xmlDoc);
    if (ret != UPNP_E_SUCCESS || !xmlDoc) {
        LOG_ERROR("[UPnP] Failed to download description: " << descriptionURL
                 << " (" << UpnpGetErrorMessage(ret) << ")");
        return false;
    }

    m_renderer.location = descriptionURL;
    m_renderer.friendlyName = getXmlElementValue(xmlDoc, "friendlyName");
    m_renderer.uuid = getXmlElementValue(xmlDoc, "UDN");

    std::string baseUrl = getXmlElementValue(xmlDoc, "URLBase");
    if (baseUrl.empty()) {
        auto pos = descriptionURL.find("://");
        if (pos != std::string::npos) {
            auto pathPos = descriptionURL.find('/', pos + 3);
            baseUrl = (pathPos != std::string::npos)
                ? descriptionURL.substr(0, pathPos)
                : descriptionURL;
        }
    }
    m_renderer.baseURL = baseUrl;

    std::string ctrlUrl, eventUrl;
    if (findServiceURLs(xmlDoc, AVTRANSPORT_TYPE, ctrlUrl, eventUrl)) {
        m_renderer.avTransportControlURL = resolveURL(baseUrl, ctrlUrl);
        m_renderer.avTransportEventURL = resolveURL(baseUrl, eventUrl);
    }
    if (findServiceURLs(xmlDoc, RENDERING_CONTROL_TYPE, ctrlUrl, eventUrl)) {
        m_renderer.renderingControlURL = resolveURL(baseUrl, ctrlUrl);
    }
    if (findServiceURLs(xmlDoc, CONNECTION_MANAGER_TYPE, ctrlUrl, eventUrl)) {
        m_renderer.connectionManagerURL = resolveURL(baseUrl, ctrlUrl);
    }

    ixmlDocument_free(xmlDoc);

    if (m_renderer.avTransportControlURL.empty()) {
        LOG_ERROR("[UPnP] No AVTransport service found at " << descriptionURL);
        return false;
    }

    queryProtocolInfo();
    setVolume(100);

    LOG_INFO("[UPnP] Connected to: " << m_renderer.friendlyName
             << " (" << m_renderer.uuid << ")");
    LOG_DEBUG("[UPnP] AVTransport: " << m_renderer.avTransportControlURL);

    m_ready.store(true);
    return true;
}

std::vector<UPnPController::RendererInfo> UPnPController::scanRenderers(
        const std::string& networkInterface, int timeoutSec) {
    UPnPController scanner;
    if (!scanner.init(networkInterface)) return {};

    scanner.m_discoveryMatch = "";  // Match all

    int ret = UpnpSearchAsync(scanner.m_clientHandle, timeoutSec,
                               MEDIA_RENDERER_TYPE, &scanner);
    if (ret != UPNP_E_SUCCESS) {
        LOG_ERROR("[UPnP] Scan search failed: " << UpnpGetErrorMessage(ret));
        return {};
    }

    // Wait for the search period
    std::this_thread::sleep_for(std::chrono::seconds(timeoutSec + 1));

    std::vector<RendererInfo> result;
    {
        std::lock_guard<std::mutex> lock(scanner.m_discoveryMutex);
        result = scanner.m_discovered;
    }

    scanner.shutdown();
    return result;
}

// ============================================================================
// libupnp callback
// ============================================================================

int UPnPController::ctrlPointCallback(Upnp_EventType eventType,
                                       const void* event, void* cookie) {
    auto* self = static_cast<UPnPController*>(cookie);
    return self->handleEvent(eventType, event);
}

int UPnPController::handleEvent(Upnp_EventType eventType, const void* event) {
    switch (eventType) {
    case UPNP_DISCOVERY_SEARCH_RESULT: {
        auto* discovery = static_cast<const UpnpDiscovery*>(event);
        const char* location = UpnpDiscovery_get_Location_cstr(discovery);
        const char* deviceId = UpnpDiscovery_get_DeviceID_cstr(discovery);

        if (!location || !deviceId) break;

        LOG_DEBUG("[UPnP] Discovery response: " << location);

        // Parse device description
        RendererInfo info;
        info.location = location;
        info.uuid = deviceId;

        IXML_Document* xmlDoc = nullptr;
        int ret = UpnpDownloadXmlDoc(location, &xmlDoc);
        if (ret != UPNP_E_SUCCESS || !xmlDoc) {
            LOG_DEBUG("[UPnP] Failed to download description: " << location);
            break;
        }

        // Extract friendly name
        info.friendlyName = getXmlElementValue(xmlDoc, "friendlyName");
        if (info.friendlyName.empty()) {
            ixmlDocument_free(xmlDoc);
            break;
        }

        // Determine base URL
        std::string baseUrl = getXmlElementValue(xmlDoc, "URLBase");
        if (baseUrl.empty()) {
            // Use location URL up to the path
            auto pos = std::string(location).find("://");
            if (pos != std::string::npos) {
                auto pathPos = std::string(location).find('/', pos + 3);
                baseUrl = (pathPos != std::string::npos)
                    ? std::string(location).substr(0, pathPos)
                    : std::string(location);
            }
        }
        info.baseURL = baseUrl;

        // Find service URLs
        std::string ctrlUrl, eventUrl;

        if (findServiceURLs(xmlDoc, AVTRANSPORT_TYPE, ctrlUrl, eventUrl)) {
            info.avTransportControlURL = resolveURL(baseUrl, ctrlUrl);
            info.avTransportEventURL = resolveURL(baseUrl, eventUrl);
        }

        if (findServiceURLs(xmlDoc, RENDERING_CONTROL_TYPE, ctrlUrl, eventUrl)) {
            info.renderingControlURL = resolveURL(baseUrl, ctrlUrl);
        }

        if (findServiceURLs(xmlDoc, CONNECTION_MANAGER_TYPE, ctrlUrl, eventUrl)) {
            info.connectionManagerURL = resolveURL(baseUrl, ctrlUrl);
        }

        ixmlDocument_free(xmlDoc);

        // Add to discovered list
        {
            std::lock_guard<std::mutex> lock(m_discoveryMutex);

            // Avoid duplicates
            bool duplicate = false;
            for (const auto& d : m_discovered) {
                if (d.uuid == info.uuid) { duplicate = true; break; }
            }
            if (!duplicate) {
                LOG_DEBUG("[UPnP] Found: " << info.friendlyName
                         << " [" << info.uuid << "]");
                m_discovered.push_back(info);
            }

            // Check if this matches what we're looking for
            if (!m_discoveryFound && !info.avTransportControlURL.empty()) {
                bool match = false;
                if (m_discoveryMatch.empty()) {
                    match = true;  // Accept first with AVTransport
                } else {
                    // Match by name (case-insensitive substring) or UUID
                    std::string nameLower = info.friendlyName;
                    std::string matchLower = m_discoveryMatch;
                    std::transform(nameLower.begin(), nameLower.end(),
                                   nameLower.begin(), ::tolower);
                    std::transform(matchLower.begin(), matchLower.end(),
                                   matchLower.begin(), ::tolower);

                    match = (nameLower.find(matchLower) != std::string::npos)
                         || (info.uuid.find(m_discoveryMatch) != std::string::npos);
                }

                if (match) {
                    m_renderer = info;
                    m_discoveryFound = true;
                    m_discoveryCv.notify_all();
                }
            }
        }
        break;
    }

    case UPNP_DISCOVERY_SEARCH_TIMEOUT:
        LOG_DEBUG("[UPnP] Search timeout");
        m_discoveryCv.notify_all();
        break;

    default:
        break;
    }

    return UPNP_E_SUCCESS;
}

// ============================================================================
// AVTransport control
// ============================================================================

bool UPnPController::setAVTransportURI(const std::string& uri,
                                        const std::string& metadata) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "SetAVTransportURI",
        {{"InstanceID", "0"},
         {"CurrentURI", uri},
         {"CurrentURIMetaData", metadata}});

    if (response) {
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] SetAVTransportURI: " << uri);
        return true;
    }
    LOG_ERROR("[UPnP] SetAVTransportURI failed for: " << uri);
    return false;
}

bool UPnPController::play(const std::string& speed) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "Play",
        {{"InstanceID", "0"},
         {"Speed", speed}});

    if (response) {
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] Play");
        return true;
    }
    LOG_ERROR("[UPnP] Play failed");
    return false;
}

bool UPnPController::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "Stop",
        {{"InstanceID", "0"}});

    if (response) {
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] Stop");
        return true;
    }
    LOG_ERROR("[UPnP] Stop failed");
    return false;
}

bool UPnPController::pause() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "Pause",
        {{"InstanceID", "0"}});

    if (response) {
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] Pause");
        return true;
    }
    LOG_WARN("[UPnP] Pause failed (renderer may not support pause)");
    return false;
}

// ============================================================================
// RenderingControl
// ============================================================================

bool UPnPController::setVolume(int volume) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_renderer.renderingControlURL.empty()) return false;

    auto* response = sendAction(
        m_renderer.renderingControlURL,
        RENDERING_CONTROL_TYPE,
        "SetVolume",
        {{"InstanceID", "0"},
         {"Channel", "Master"},
         {"DesiredVolume", std::to_string(volume)}});

    if (response) {
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] Volume set to " << volume);
        return true;
    }
    LOG_WARN("[UPnP] SetVolume failed");
    return false;
}

int UPnPController::getVolume() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_renderer.renderingControlURL.empty()) return -1;

    auto* response = sendAction(
        m_renderer.renderingControlURL,
        RENDERING_CONTROL_TYPE,
        "GetVolume",
        {{"InstanceID", "0"},
         {"Channel", "Master"}});

    if (response) {
        std::string val = getResponseValue(response, "CurrentVolume");
        ixmlDocument_free(response);
        try { return std::stoi(val); } catch (...) { return -1; }
    }
    return -1;
}

// ============================================================================
// ConnectionManager
// ============================================================================

bool UPnPController::queryProtocolInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_renderer.connectionManagerURL.empty()) return false;

    auto* response = sendAction(
        m_renderer.connectionManagerURL,
        CONNECTION_MANAGER_TYPE,
        "GetProtocolInfo");

    if (response) {
        m_renderer.protocolInfo = getResponseValue(response, "Sink");
        ixmlDocument_free(response);
        LOG_DEBUG("[UPnP] ProtocolInfo Sink: "
                 << m_renderer.protocolInfo.substr(0, 200)
                 << (m_renderer.protocolInfo.size() > 200 ? "..." : ""));
        return true;
    }
    LOG_WARN("[UPnP] GetProtocolInfo failed");
    return false;
}

bool UPnPController::supportsFormat(const std::string& mimeType) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_renderer.protocolInfo.empty()) return true;  // Assume yes if unknown

    // Protocol info is CSV of "protocol:network:mime:info" entries
    std::string lower = m_renderer.protocolInfo;
    std::string mimeLower = mimeType;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::transform(mimeLower.begin(), mimeLower.end(), mimeLower.begin(), ::tolower);

    return lower.find(mimeLower) != std::string::npos;
}

bool UPnPController::supportsDSD() const {
    return supportsFormat("audio/dsf")
        || supportsFormat("audio/x-dsf")
        || supportsFormat("audio/dsd")
        || supportsFormat("application/dsf");
}

// ============================================================================
// Transport state
// ============================================================================

std::string UPnPController::getTransportState() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "GetTransportInfo",
        {{"InstanceID", "0"}});

    if (response) {
        std::string state = getResponseValue(response, "CurrentTransportState");
        ixmlDocument_free(response);
        return state;
    }
    return "UNKNOWN";
}

int UPnPController::getPositionSeconds() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto* response = sendAction(
        m_renderer.avTransportControlURL,
        AVTRANSPORT_TYPE,
        "GetPositionInfo",
        {{"InstanceID", "0"}});

    if (response) {
        std::string relTime = getResponseValue(response, "RelTime");
        ixmlDocument_free(response);

        // Parse "H:MM:SS" or "H:MM:SS.xxx"
        int h = 0, m = 0, s = 0;
        if (sscanf(relTime.c_str(), "%d:%d:%d", &h, &m, &s) >= 3) {
            return h * 3600 + m * 60 + s;
        }
    }
    return -1;
}

// ============================================================================
// SOAP helpers
// ============================================================================

IXML_Document* UPnPController::sendAction(
        const std::string& controlURL,
        const std::string& serviceType,
        const std::string& actionName,
        const std::vector<std::pair<std::string, std::string>>& args) {

    if (controlURL.empty()) return nullptr;

    // Build action document
    IXML_Document* action = nullptr;
    for (const auto& [name, value] : args) {
        int ret = UpnpAddToAction(&action, actionName.c_str(),
                                   serviceType.c_str(),
                                   name.c_str(), value.c_str());
        if (ret != UPNP_E_SUCCESS) {
            LOG_ERROR("[UPnP] Failed to build action " << actionName
                     << ": " << UpnpGetErrorMessage(ret));
            if (action) ixmlDocument_free(action);
            return nullptr;
        }
    }

    // If no args, create empty action
    if (args.empty()) {
        UpnpAddToAction(&action, actionName.c_str(),
                         serviceType.c_str(), nullptr, nullptr);
    }

    IXML_Document* response = nullptr;
    int ret = UpnpSendAction(m_clientHandle,
                              controlURL.c_str(),
                              serviceType.c_str(),
                              nullptr,  // DevUDN (ignored)
                              action,
                              &response);

    ixmlDocument_free(action);

    if (ret != UPNP_E_SUCCESS) {
        LOG_DEBUG("[UPnP] SendAction " << actionName << " failed: "
                 << UpnpGetErrorMessage(ret));
        if (response) {
            ixmlDocument_free(response);
            response = nullptr;
        }
    }

    return response;
}

std::string UPnPController::getResponseValue(IXML_Document* response,
                                               const std::string& tagName) {
    if (!response) return "";

    IXML_NodeList* nodeList = ixmlDocument_getElementsByTagName(
        response, tagName.c_str());
    if (!nodeList) return "";

    IXML_Node* node = ixmlNodeList_item(nodeList, 0);
    std::string value;
    if (node) {
        IXML_Node* textNode = ixmlNode_getFirstChild(node);
        if (textNode) {
            const char* val = ixmlNode_getNodeValue(textNode);
            if (val) value = val;
        }
    }

    ixmlNodeList_free(nodeList);
    return value;
}

// ============================================================================
// XML parsing helpers
// ============================================================================

std::string UPnPController::getXmlElementValue(IXML_Document* doc,
                                                 const std::string& tagName) {
    if (!doc) return "";

    IXML_NodeList* nodeList = ixmlDocument_getElementsByTagName(
        doc, tagName.c_str());
    if (!nodeList) return "";

    std::string value;
    IXML_Node* node = ixmlNodeList_item(nodeList, 0);
    if (node) {
        IXML_Node* textNode = ixmlNode_getFirstChild(node);
        if (textNode) {
            const char* val = ixmlNode_getNodeValue(textNode);
            if (val) value = val;
        }
    }

    ixmlNodeList_free(nodeList);
    return value;
}

std::string UPnPController::getXmlElementValue(IXML_Element* element,
                                                 const std::string& tagName) {
    if (!element) return "";

    IXML_NodeList* nodeList = ixmlElement_getElementsByTagName(
        element, tagName.c_str());
    if (!nodeList) return "";

    std::string value;
    IXML_Node* node = ixmlNodeList_item(nodeList, 0);
    if (node) {
        IXML_Node* textNode = ixmlNode_getFirstChild(node);
        if (textNode) {
            const char* val = ixmlNode_getNodeValue(textNode);
            if (val) value = val;
        }
    }

    ixmlNodeList_free(nodeList);
    return value;
}

bool UPnPController::findServiceURLs(IXML_Document* doc,
                                      const std::string& serviceType,
                                      std::string& controlURL,
                                      std::string& eventURL) {
    controlURL.clear();
    eventURL.clear();
    if (!doc) return false;

    IXML_NodeList* serviceList = ixmlDocument_getElementsByTagName(doc, "service");
    if (!serviceList) return false;

    unsigned long len = ixmlNodeList_length(serviceList);
    for (unsigned long i = 0; i < len; i++) {
        IXML_Node* serviceNode = ixmlNodeList_item(serviceList, i);
        if (!serviceNode) continue;

        auto* serviceElement = reinterpret_cast<IXML_Element*>(serviceNode);
        std::string type = getXmlElementValue(serviceElement, "serviceType");

        if (type == serviceType) {
            controlURL = getXmlElementValue(serviceElement, "controlURL");
            eventURL = getXmlElementValue(serviceElement, "eventSubURL");
            ixmlNodeList_free(serviceList);
            return !controlURL.empty();
        }
    }

    ixmlNodeList_free(serviceList);
    return false;
}

std::string UPnPController::resolveURL(const std::string& base,
                                        const std::string& relative) const {
    if (relative.empty()) return "";

    // Already absolute
    if (relative.find("://") != std::string::npos) return relative;

    // Relative path — prepend base
    std::string result = base;
    if (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    if (!relative.empty() && relative.front() != '/') {
        result += '/';
    }
    result += relative;
    return result;
}
