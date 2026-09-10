// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

/// \file
/// \brief The protobuf wire format, read-only.
///
/// OTLP is protobuf, and the station only ever decodes it: a producer posts an
/// ExportMetricsServiceRequest and the successful answer is an empty ExportMetricsServiceResponse,
/// which serialises to nothing. Decoding one small, stable message tree is not worth a protobuf
/// runtime and a code generator in everest-core, so this reads the wire format directly.
///
/// That is safe for the same reason protobuf itself is forwards compatible: every field is
/// preceded by a tag carrying its wire type, so a field this code has never heard of can be
/// stepped over without knowing what it means. A newer producer therefore cannot break a reader
/// that ignores what it does not use.
namespace ocpp_module_common::otlp {

/// \brief The four wire types still in use. Groups (3 and 4) were removed from the language and
/// appear in no OTLP message; encountering one is treated as a malformed buffer.
enum class WireType : std::uint8_t {
    Varint = 0,           ///< int32/64, uint32/64, sint, bool, enum
    Fixed64 = 1,          ///< fixed64, sfixed64, double
    LengthDelimited = 2,  ///< string, bytes, embedded messages, packed repeated
    Fixed32 = 5           ///< fixed32, sfixed32, float
};

/// \brief One field header: which field, and how to read it.
struct Tag {
    std::uint32_t field{0};
    WireType type{WireType::Varint};
};

/// \brief A cursor over one protobuf message.
///
/// Every accessor is fallible and none throws: a truncated or malformed buffer makes the reader
/// invalid, and an invalid reader answers nothing for the rest of its life. Callers therefore
/// check `ok()` once at the end of a message rather than after every field.
///
/// The reader borrows its buffer and copies nothing. A string field is returned as a view into the
/// request body, so anything kept beyond the decode has to be copied by the caller.
class Reader {
public:
    explicit Reader(std::string_view data) : m_data(data) {
    }

    /// \returns true while nothing malformed has been read
    bool ok() const {
        return m_ok;
    }

    /// \returns true when the whole message has been consumed
    bool done() const {
        return m_pos >= m_data.size();
    }

    /// \brief Reads the next field header.
    /// \returns the tag, or nothing at the end of the message or on a malformed one
    std::optional<Tag> next() {
        if (not m_ok or done()) {
            return std::nullopt;
        }
        const auto key = read_varint();
        if (not key.has_value()) {
            return std::nullopt;
        }
        const auto wire = static_cast<std::uint8_t>(*key & 0x07u);
        const auto field = static_cast<std::uint32_t>(*key >> 3);
        if (field == 0) {
            return fail();
        }
        switch (wire) {
        case 0:
        case 1:
        case 2:
        case 5:
            break;
        default:
            // groups, which no OTLP message uses and which cannot be skipped by length
            return fail();
        }
        Tag tag;
        tag.field = field;
        tag.type = static_cast<WireType>(wire);
        return tag;
    }

    /// \brief Reads a varint field: int32, int64, uint32, uint64, bool or an enum.
    std::optional<std::uint64_t> varint() {
        return read_varint();
    }

    /// \brief Reads a fixed64 field, as it lies. Also the wire form of sfixed64 and double.
    std::optional<std::uint64_t> fixed64() {
        if (not m_ok or m_data.size() - m_pos < 8) {
            return fail();
        }
        std::uint64_t value = 0;
        std::memcpy(&value, m_data.data() + m_pos, 8);
        m_pos += 8;
        return le64(value);
    }

    /// \brief Reads a fixed32 field, as it lies. Also the wire form of sfixed32 and float.
    std::optional<std::uint32_t> fixed32() {
        if (not m_ok or m_data.size() - m_pos < 4) {
            return fail32();
        }
        std::uint32_t value = 0;
        std::memcpy(&value, m_data.data() + m_pos, 4);
        m_pos += 4;
        return le32(value);
    }

    /// \brief Reads a fixed64 field as the double it encodes.
    std::optional<double> real() {
        const auto bits = fixed64();
        if (not bits.has_value()) {
            return std::nullopt;
        }
        double value = 0.0;
        std::memcpy(&value, &*bits, 8);
        return value;
    }

    /// \brief Reads a length-delimited field: a string, a byte blob or an embedded message.
    /// \returns a view into the buffer this reader borrows, valid as long as that buffer is
    std::optional<std::string_view> bytes() {
        const auto length = read_varint();
        if (not length.has_value()) {
            return std::nullopt;
        }
        if (*length > m_data.size() - m_pos) {
            return fail_view();
        }
        const auto view = m_data.substr(m_pos, static_cast<std::size_t>(*length));
        m_pos += static_cast<std::size_t>(*length);
        return view;
    }

    /// \brief Reads an embedded message as a reader of its own.
    std::optional<Reader> message() {
        const auto view = bytes();
        if (not view.has_value()) {
            return std::nullopt;
        }
        return Reader(*view);
    }

    /// \brief Steps over a field of \p type without interpreting it.
    ///
    /// This is what makes the decoder forwards compatible: everything OTLP grows that this code
    /// does not read goes through here.
    /// \returns false when the buffer is malformed
    bool skip(WireType type) {
        switch (type) {
        case WireType::Varint:
            return read_varint().has_value();
        case WireType::Fixed64:
            return fixed64().has_value();
        case WireType::Fixed32:
            return fixed32().has_value();
        case WireType::LengthDelimited:
            return bytes().has_value();
        }
        return static_cast<bool>(fail_flag());
    }

private:
    std::optional<std::uint64_t> read_varint() {
        if (not m_ok) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        // a varint is at most ten groups of seven bits; anything longer is malformed rather than
        // merely large, and stopping here is what keeps a hostile body from walking the buffer
        for (unsigned shift = 0; shift < 70u; shift += 7u) {
            if (m_pos >= m_data.size()) {
                return fail();
            }
            const auto byte = static_cast<std::uint8_t>(m_data[m_pos]);
            m_pos += 1;
            value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0) {
                return value;
            }
        }
        return fail();
    }

    static std::uint64_t le64(std::uint64_t value) {
        return host_is_little_endian() ? value : byteswap64(value);
    }

    static std::uint32_t le32(std::uint32_t value) {
        return host_is_little_endian() ? value : byteswap32(value);
    }

    static bool host_is_little_endian() {
        const std::uint16_t probe = 1;
        std::uint8_t first = 0;
        std::memcpy(&first, &probe, 1);
        return first == 1;
    }

    static std::uint64_t byteswap64(std::uint64_t v) {
        return ((v & 0x00000000000000FFull) << 56) | ((v & 0x000000000000FF00ull) << 40) |
               ((v & 0x0000000000FF0000ull) << 24) | ((v & 0x00000000FF000000ull) << 8) |
               ((v & 0x000000FF00000000ull) >> 8) | ((v & 0x0000FF0000000000ull) >> 24) |
               ((v & 0x00FF000000000000ull) >> 40) | ((v & 0xFF00000000000000ull) >> 56);
    }

    static std::uint32_t byteswap32(std::uint32_t v) {
        return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
               ((v & 0xFF000000u) >> 24);
    }

    std::nullopt_t fail() {
        m_ok = false;
        return std::nullopt;
    }

    std::optional<std::uint32_t> fail32() {
        m_ok = false;
        return std::nullopt;
    }

    std::optional<std::string_view> fail_view() {
        m_ok = false;
        return std::nullopt;
    }

    bool fail_flag() {
        m_ok = false;
        return false;
    }

    std::string_view m_data;
    std::size_t m_pos{0};
    bool m_ok{true};
};

} // namespace ocpp_module_common::otlp
