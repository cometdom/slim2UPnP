// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dominique COMET (cometdom)
// This file is part of slim2UPnP. See LICENSE for details.

/**
 * @file OggFlacUnwrapper.h
 * @brief Ogg FLAC → native FLAC byte stream (no decoding)
 *
 * LMS sends Ogg FLAC (internet radio) untouched only to players that
 * advertise "ogf"; otherwise it transcodes it with flac|sox, which stops
 * after the first song of a chained stream. We advertise "ogf" and turn the
 * Ogg pages back into a plain FLAC stream here, so the renderer receives the
 * same kind of streamed FLAC it got from the transcode, without the stop.
 *
 * Radio streams chain one Ogg logical stream per song. Links with the same
 * format just continue the FLAC stream (their header packets are dropped).
 * A link with a different format ends the stream (ended() becomes true).
 * Anything that does not start with "OggS" is passed through untouched.
 *
 * Written from the public specifications: Ogg encapsulation (RFC 3533) and
 * the Xiph "FLAC to Ogg mapping". Same logic as slim2diretta's OggFlacDecoder.
 */

#ifndef SLIM2UPNP_OGGFLACUNWRAPPER_H
#define SLIM2UPNP_OGGFLACUNWRAPPER_H

#include <cstddef>
#include <cstdint>
#include <vector>

class OggFlacUnwrapper {
public:
    /// Feed stream bytes; the bytes to forward are appended to out.
    void feed(const uint8_t* data, size_t len, std::vector<uint8_t>& out);
    /// End of input: flushes bytes held while sniffing (stream < 4 bytes).
    void finish(std::vector<uint8_t>& out);
    /// True once the input was recognised as Ogg FLAC.
    bool isOgg() const { return m_mode == Mode::Ogg; }
    /// True after an unsupported format change: stop reading the stream.
    bool ended() const { return m_stopped; }

private:
    enum class Mode { Sniffing, Plain, Ogg };

    void parsePages(std::vector<uint8_t>& out);
    void handlePacket(uint32_t serial, std::vector<uint8_t>& out);
    void startLink(uint32_t serial);

    Mode m_mode = Mode::Sniffing;
    std::vector<uint8_t> m_in;          // Unparsed Ogg bytes
    size_t m_inPos = 0;
    std::vector<uint8_t> m_packet;      // Packet being assembled across pages

    bool m_haveLink = false;            // A FLAC logical stream has started
    bool m_linkHeaderPending = false;   // Next packet is the link's first packet
    bool m_stopped = false;             // Stream ended (unsupported format change)
    uint32_t m_serial = 0;
    uint32_t m_prevSerial = 0;
    uint32_t m_links = 0;
    uint8_t m_streamInfo[34] = {};      // STREAMINFO body of the first link
};

#endif // SLIM2UPNP_OGGFLACUNWRAPPER_H
