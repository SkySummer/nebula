#include "nebula/common/codec/base64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::common {

namespace {

constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::uint8_t kInvalidBase64Value = 0xFFU;

using Base64DecodeTable = std::array<std::uint8_t, 256>;

consteval Base64DecodeTable make_decode_table(std::string_view alphabet) {
    Base64DecodeTable table{};
    table.fill(kInvalidBase64Value);

    for (std::size_t index = 0U; index < alphabet.size(); ++index) {
        table.at(alphabet.at(index)) = static_cast<std::uint8_t>(index);
    }

    return table;
}

constexpr Base64DecodeTable kBase64DecodeTable = make_decode_table(kBase64Alphabet);
constexpr Base64DecodeTable kBase64UrlDecodeTable = make_decode_table(kBase64UrlAlphabet);

struct DecodedChunk {
    std::array<std::uint8_t, 4> values{};
    std::size_t produced_bytes = 0U;
};

std::optional<std::uint8_t> base64_value(const Base64DecodeTable& decode_table, char ch) {
    const std::uint8_t value = decode_table.at(static_cast<unsigned char>(ch));
    if (value == kInvalidBase64Value) {
        return std::nullopt;
    }
    return value;
}

std::size_t encoded_size(std::size_t input_size, bool with_padding) {
    const std::size_t full_chunks = input_size / 3U;
    const std::size_t tail = input_size % 3U;

    std::size_t size = full_chunks * 4U;
    if (tail == 0U) {
        return size;
    }
    if (with_padding) {
        return size + 4U;
    }
    return size + (tail == 1U ? 2U : 3U);
}

std::string encode_impl(std::span<const std::byte> input, std::string_view alphabet, bool with_padding) {
    std::string out;
    out.reserve(encoded_size(input.size(), with_padding));

    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const auto b0 = std::to_integer<std::uint8_t>(input[cursor]);
        const bool has_b1 = (cursor + 1U) < input.size();
        const bool has_b2 = (cursor + 2U) < input.size();
        const std::uint8_t b1 = has_b1 ? std::to_integer<std::uint8_t>(input[cursor + 1U]) : 0U;
        const std::uint8_t b2 = has_b2 ? std::to_integer<std::uint8_t>(input[cursor + 2U]) : 0U;

        out.push_back(alphabet[(b0 >> 2U) & 0x3FU]);
        out.push_back(alphabet[((b0 & 0x03U) << 4U) | ((b1 >> 4U) & 0x0FU)]);
        if (has_b1) {
            out.push_back(alphabet[((b1 & 0x0FU) << 2U) | ((b2 >> 6U) & 0x03U)]);
        } else if (with_padding) {
            out.push_back('=');
        }
        if (has_b2) {
            out.push_back(alphabet[b2 & 0x3FU]);
        } else if (with_padding) {
            out.push_back('=');
        }

        cursor += 3U;
    }

    return out;
}

std::expected<DecodedChunk, Base64DecodeError> parse_chunk(std::string_view chunk_text,
                                                           const Base64DecodeTable& decode_table, bool allow_padding,
                                                           bool is_final_chunk) {
    if (chunk_text.size() == 1U) {
        return std::unexpected(Base64DecodeError::InvalidLength);
    }

    DecodedChunk chunk;
    bool seen_padding = false;
    std::size_t padding_count = 0U;

    for (std::size_t part = 0; part < chunk_text.size(); ++part) {
        const char ch = chunk_text[part];
        if (ch == '=') {
            if (!allow_padding || chunk_text.size() != 4U || part < 2U) {
                return std::unexpected(Base64DecodeError::InvalidPadding);
            }
            seen_padding = true;
            ++padding_count;
            continue;
        }
        if (seen_padding) {
            return std::unexpected(Base64DecodeError::InvalidPadding);
        }

        const std::optional<std::uint8_t> value = base64_value(decode_table, ch);
        if (!value.has_value()) {
            return std::unexpected(Base64DecodeError::InvalidCharacter);
        }
        chunk.values.at(part) = *value;
    }

    if (padding_count > 0U && !is_final_chunk) {
        return std::unexpected(Base64DecodeError::InvalidPadding);
    }
    if (padding_count == 1U && chunk_text[3U] != '=') {
        return std::unexpected(Base64DecodeError::InvalidPadding);
    }
    if (padding_count == 2U && (chunk_text[2U] != '=' || chunk_text[3U] != '=')) {
        return std::unexpected(Base64DecodeError::InvalidPadding);
    }
    if (padding_count > 2U) {
        return std::unexpected(Base64DecodeError::InvalidPadding);
    }

    if (chunk_text.size() == 4U) {
        chunk.produced_bytes = 3U - padding_count;
    } else if (padding_count == 0U) {
        chunk.produced_bytes = chunk_text.size() - 1U;
    } else {
        return std::unexpected(Base64DecodeError::InvalidPadding);
    }

    return chunk;
}

void append_decoded_chunk(const DecodedChunk& chunk, std::vector<std::byte>& out) {
    out.push_back(static_cast<std::byte>((chunk.values.at(0) << 2U) | ((chunk.values.at(1) >> 4U) & 0x03U)));
    if (chunk.produced_bytes >= 2U) {
        out.push_back(
            static_cast<std::byte>(((chunk.values.at(1) & 0x0FU) << 4U) | ((chunk.values.at(2) >> 2U) & 0x0FU)));
    }
    if (chunk.produced_bytes == 3U) {
        out.push_back(static_cast<std::byte>(((chunk.values.at(2) & 0x03U) << 6U) | (chunk.values.at(3) & 0x3FU)));
    }
}

std::expected<std::vector<std::byte>, Base64DecodeError> decode_impl(std::string_view input,
                                                                     const Base64DecodeTable& decode_table,
                                                                     bool allow_padding) {
    if (input.empty()) {
        return std::vector<std::byte>{};
    }
    if ((input.size() % 4U) == 1U) {
        return std::unexpected(Base64DecodeError::InvalidLength);
    }

    const bool has_padding_char = input.find('=') != std::string_view::npos;
    if (has_padding_char) {
        if (!allow_padding || ((input.size() % 4U) != 0U)) {
            return std::unexpected(Base64DecodeError::InvalidPadding);
        }
    }

    std::vector<std::byte> out;
    out.reserve((input.size() * 3U) / 4U);

    std::size_t idx = 0U;
    while (idx < input.size()) {
        const std::size_t remain = input.size() - idx;
        const std::size_t chunk_size = remain >= 4U ? 4U : remain;
        if (chunk_size < 4U && has_padding_char) {
            return std::unexpected(Base64DecodeError::InvalidPadding);
        }

        const std::string_view chunk_text = input.substr(idx, chunk_size);
        const bool is_final_chunk = (idx + chunk_size) == input.size();
        const auto chunk = parse_chunk(chunk_text, decode_table, allow_padding, is_final_chunk);
        if (!chunk.has_value()) {
            return std::unexpected(chunk.error());
        }

        append_decoded_chunk(*chunk, out);
        idx += chunk_size;
    }

    return out;
}

std::expected<std::string, Base64DecodeError> decode_to_string_impl(std::string_view input,
                                                                    const Base64DecodeTable& decode_table,
                                                                    bool allow_padding) {
    const auto decoded = decode_impl(input, decode_table, allow_padding);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }

    std::string text;
    text.reserve(decoded->size());
    for (const std::byte byte : *decoded) {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return text;
}

}  // namespace

std::string_view to_string(Base64DecodeError error) noexcept {
    switch (error) {
        case Base64DecodeError::InvalidLength:
            return "invalid_length";
        case Base64DecodeError::InvalidPadding:
            return "invalid_padding";
        case Base64DecodeError::InvalidCharacter:
            return "invalid_character";
    }
    std::unreachable();
}

std::string base64_encode(std::span<const std::byte> input) {
    return encode_impl(input, kBase64Alphabet, true);
}

std::string base64_encode(std::string_view input) {
    return encode_impl(std::as_bytes(std::span{input}), kBase64Alphabet, true);
}

std::expected<std::vector<std::byte>, Base64DecodeError> base64_decode_to_bytes(std::string_view input) {
    return decode_impl(input, kBase64DecodeTable, true);
}

std::expected<std::string, Base64DecodeError> base64_decode_to_string(std::string_view input) {
    return decode_to_string_impl(input, kBase64DecodeTable, true);
}

std::string base64url_encode(std::span<const std::byte> input) {
    return encode_impl(input, kBase64UrlAlphabet, false);
}

std::string base64url_encode(std::string_view input) {
    return encode_impl(std::as_bytes(std::span{input}), kBase64UrlAlphabet, false);
}

std::expected<std::vector<std::byte>, Base64DecodeError> base64url_decode_to_bytes(std::string_view input) {
    return decode_impl(input, kBase64UrlDecodeTable, false);
}

std::expected<std::string, Base64DecodeError> base64url_decode_to_string(std::string_view input) {
    return decode_to_string_impl(input, kBase64UrlDecodeTable, false);
}

}  // namespace nebula::common
