// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dominique COMET (cometdom)
// This file is part of slim2UPnP. See LICENSE for details.

/**
 * @file OggFlacUnwrapper.cpp
 * @brief Ogg FLAC → native FLAC byte stream (no decoding)
 *
 * First packet of a logical stream (Xiph FLAC-to-Ogg mapping):
 *   0x7F "FLAC" major minor numHeaders(16 BE) "fLaC" <STREAMINFO block>
 * then one metadata block per header packet, then one FLAC frame per packet.
 */

#include "OggFlacUnwrapper.h"
#include "LogLevel.h"

#include <cstring>

namespace {

constexpr size_t OGG_HEADER_SIZE = 27;
constexpr uint8_t OGG_FLAG_CONTINUED = 0x01;
constexpr uint8_t OGG_FLAG_BOS = 0x02;
constexpr size_t FLAC_MAPPING_HEADER_SIZE = 51;   // 9 + "fLaC" + 4 + 34
constexpr size_t COMPACT_THRESHOLD = 65536;

// Ogg CRC-32: polynomial 0x04C11DB7, not reflected, initial value 0
struct CrcTable {
    uint32_t v[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int b = 0; b < 8; b++) {
                r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
            }
            v[i] = r;
        }
    }
};

uint32_t oggCrc(const uint8_t* data, size_t len) {
    static const CrcTable crcTable;   // thread-safe one-time init
    const uint32_t* table = crcTable.v;
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Sample rate (20 bits), channels (3), bits per sample (5) from a STREAMINFO body
bool sameFormat(const uint8_t* a, const uint8_t* b) {
    return a[10] == b[10] && a[11] == b[11] && a[12] == b[12] &&
           (a[13] & 0xF0) == (b[13] & 0xF0);
}

} // namespace

void OggFlacUnwrapper::feed(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    if (m_mode == Mode::Plain) {
        out.insert(out.end(), data, data + len);
        return;
    }
    if (m_stopped) return;

    m_in.insert(m_in.end(), data, data + len);

    if (m_mode == Mode::Sniffing) {
        if (m_in.size() < 4) return;
        if (std::memcmp(m_in.data(), "OggS", 4) != 0) {
            m_mode = Mode::Plain;
            out.insert(out.end(), m_in.begin(), m_in.end());
            m_in.clear();
            return;
        }
        m_mode = Mode::Ogg;
        LOG_INFO("[OggFLAC] Ogg FLAC stream detected, serving it as native FLAC");
    }

    parsePages(out);
}

void OggFlacUnwrapper::finish(std::vector<uint8_t>& out) {
    if (m_mode == Mode::Sniffing && !m_in.empty()) {
        out.insert(out.end(), m_in.begin(), m_in.end());
        m_in.clear();
    }
}

void OggFlacUnwrapper::parsePages(std::vector<uint8_t>& out) {
    while (!m_stopped) {
        size_t avail = m_in.size() - m_inPos;
        if (avail < OGG_HEADER_SIZE) break;
        const uint8_t* p = m_in.data() + m_inPos;

        if (std::memcmp(p, "OggS", 4) != 0 || p[4] != 0) {
            // Lost sync: skip to the next capture pattern
            const uint8_t* end = m_in.data() + m_in.size();
            const uint8_t* next = p + 1;
            while (next + 4 <= end && std::memcmp(next, "OggS", 4) != 0) next++;
            m_inPos = static_cast<size_t>(next - m_in.data());
            if (next + 4 > end) break;
            continue;
        }

        size_t segments = p[26];
        if (avail < OGG_HEADER_SIZE + segments) break;
        size_t bodySize = 0;
        for (size_t i = 0; i < segments; i++) bodySize += p[OGG_HEADER_SIZE + i];
        size_t pageSize = OGG_HEADER_SIZE + segments + bodySize;
        if (avail < pageSize) break;

        // Verify the CRC (computed with the CRC field zeroed) before trusting it
        uint8_t crcField[4];
        std::memcpy(crcField, m_in.data() + m_inPos + 22, 4);
        std::memset(m_in.data() + m_inPos + 22, 0, 4);
        bool crcOk = oggCrc(m_in.data() + m_inPos, pageSize) == readLe32(crcField);
        std::memcpy(m_in.data() + m_inPos + 22, crcField, 4);
        if (!crcOk) {
            LOG_DEBUG("[OggFLAC] Page CRC mismatch, resyncing");
            m_inPos += 1;
            continue;
        }

        uint8_t flags = p[5];
        uint32_t serial = readLe32(p + 14);
        const uint8_t* segTable = p + OGG_HEADER_SIZE;
        const uint8_t* body = segTable + segments;

        if (flags & OGG_FLAG_BOS) {
            startLink(serial);
        }

        if ((m_linkHeaderPending || m_haveLink) && serial == m_serial) {
            if (!(flags & OGG_FLAG_CONTINUED) && !m_packet.empty()) {
                m_packet.clear();   // the rest of that packet was lost
            }
            size_t off = 0;
            for (size_t i = 0; i < segments && !m_stopped; i++) {
                size_t seg = segTable[i];
                m_packet.insert(m_packet.end(), body + off, body + off + seg);
                off += seg;
                if (seg < 255) {
                    handlePacket(serial, out);
                    m_packet.clear();
                }
            }
        }

        m_inPos += pageSize;
    }

    if (m_inPos >= COMPACT_THRESHOLD) {
        m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(m_inPos));
        m_inPos = 0;
    }
}

void OggFlacUnwrapper::startLink(uint32_t serial) {
    m_prevSerial = m_serial;
    m_serial = serial;
    m_linkHeaderPending = true;
    m_packet.clear();
}

void OggFlacUnwrapper::handlePacket(uint32_t serial, std::vector<uint8_t>& out) {
    if (m_linkHeaderPending) {
        m_linkHeaderPending = false;
        const uint8_t* h = m_packet.data();
        bool isFlac = m_packet.size() >= FLAC_MAPPING_HEADER_SIZE &&
                      h[0] == 0x7F && std::memcmp(h + 1, "FLAC", 4) == 0 &&
                      std::memcmp(h + 9, "fLaC", 4) == 0 && (h[13] & 0x7F) == 0;
        if (!isFlac) {
            LOG_DEBUG("[OggFLAC] Ignoring non-FLAC logical stream " << serial);
            m_serial = m_haveLink ? m_prevSerial : 0;
            return;
        }
        const uint8_t* info = h + 17;

        if (!m_haveLink) {
            // Native stream header: "fLaC" + STREAMINFO as the only (last)
            // metadata block. Total samples and MD5 describe this link only,
            // so they are cleared: the renderer must keep going into the next
            // links instead of stopping at the first song's length.
            std::memcpy(m_streamInfo, info, sizeof(m_streamInfo));
            uint8_t block[34];
            std::memcpy(block, info, sizeof(block));
            block[13] &= 0xF0;
            std::memset(block + 14, 0, sizeof(block) - 14);
            const uint8_t header[8] = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 34};
            out.insert(out.end(), header, header + sizeof(header));
            out.insert(out.end(), block, block + sizeof(block));
            m_haveLink = true;
            m_links = 1;
            return;
        }

        m_links++;
        if (!sameFormat(m_streamInfo, info)) {
            LOG_WARN("[OggFLAC] Format change in chained stream (link " << m_links
                     << ") is not supported — ending the stream");
            m_stopped = true;
            return;
        }
        LOG_INFO("[OggFLAC] New chained stream (link " << m_links << "), same format — continuing");
        return;
    }

    // Audio packets are FLAC frames (sync code 0xFFF8/0xFFF9); anything else
    // after the first packet is a metadata block, already covered by STREAMINFO.
    if (m_packet.size() >= 2 && m_packet[0] == 0xFF && (m_packet[1] & 0xFE) == 0xF8) {
        out.insert(out.end(), m_packet.begin(), m_packet.end());
    }
}
