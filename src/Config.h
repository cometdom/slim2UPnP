// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dominique (cometdom)
// This file is part of slim2UPnP. See LICENSE for details.

/**
 * @file Config.h
 * @brief Configuration for slim2UPnP
 */

#ifndef SLIM2UPNP_CONFIG_H
#define SLIM2UPNP_CONFIG_H

#include <string>
#include <cstdint>

struct Config {
    // LMS connection
    std::string lmsServer;              // empty = autodiscovery
    uint16_t lmsPort = 3483;            // Slimproto TCP port
    std::string playerName = "slim2UPnP";
    std::string macAddress;             // empty = auto-generate

    // UPnP renderer
    std::string rendererName;           // empty = first discovered, or partial match
    std::string rendererUUID;           // empty = use name match
    std::string rendererURL;            // direct description URL (skip SSDP)
    uint16_t httpServerPort = 0;        // 0 = auto-select
    std::string networkInterface;       // empty = auto-detect

    // Audio
    int maxSampleRate = 1536000;
    bool dsdEnabled = true;

    // Logging
    bool verbose = false;
    bool quiet = false;

    // Actions
    bool listRenderers = false;
    bool showVersion = false;
};

#endif // SLIM2UPNP_CONFIG_H
