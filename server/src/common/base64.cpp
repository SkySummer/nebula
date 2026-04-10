#include "nebula/common/base64.hpp"

#include <cstddef>

namespace nebula::common {

namespace {

constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct DecodedChunk {
    std::uint8_t v0 = 0U;
    std::uint8_t v1 = 0U;
    std::uint8_t v2 = 0U;
    std::uint8_t v3 = 0U;
    std::size_t produced_bytes = 0U;
};

std::optional<std::uint8_t> base64_value(std::string_view alphabet, char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<std::uint8_t>(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<std::uint8_t>(26 + (ch - 'a'));
    }
    if (ch >= '0' && ch <= '9') {
        return static_cast<std::uint8_t>(52 + (ch - '0'));
    }
    if (ch == alphabet[62]) {
        return static_cast<std::uint8_t>(62);
    }
    if (ch == alphabet[63]) {
        return static_cast<std::uint8_t>(63);
    }
    return std::nullopt;
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

bool assign_decoded_value(std::size_t part, std::uint8_t value, DecodedChunk& chunk) {
    if (part == 0U) {
        chunk.v0 = value;
        return true;
    }
    if (part == 1U) {
        chunk.v1 = value;
        return true;
    }
    if (part == 2U) {
        chunk.v2 = value;
        return true;
    }
    if (part == 3U) {
        chunk.v3 = value;
        return true;
    }
    return false;
}

std::optional<DecodedChunk> parse_chunk(std::string_view chunk_text, std::string_view alphabet, bool allow_padding,
                                        bool is_final_chunk) {
    if (chunk_text.size() == 1U) {
        return std::nullopt;
    }

    DecodedChunk chunk;
    bool seen_padding = false;
    std::size_t padding_count = 0U;

    for (std::size_t part = 0; part < chunk_text.size(); ++part) {
        const char ch = chunk_text[part];
        if (ch == '=') {
            if (!allow_padding || chunk_text.size() != 4U || part < 2U) {
                return std::nullopt;
            }
            seen_padding = true;
            ++padding_count;
            continue;
        }
        if (seen_padding) {
            return std::nullopt;
        }

        const std::optional<std::uint8_t> value = base64_value(alphabet, ch);
        if (!value.has_value()) {
            return std::nullopt;
        }
        if (!assign_decoded_value(part, *value, chunk)) {
            return std::nullopt;
        }
    }

    if (padding_count > 0U && !is_final_chunk) {
        return std::nullopt;
    }
    if (padding_count == 1U && chunk_text[3U] != '=') {
        return std::nullopt;
    }
    if (padding_count == 2U && (chunk_text[2U] != '=' || chunk_text[3U] != '=')) {
        return std::nullopt;
    }
    if (padding_count > 2U) {
        return std::nullopt;
    }

    if (chunk_text.size() == 4U) {
        chunk.produced_bytes = 3U - padding_count;
    } else if (padding_count == 0U) {
        chunk.produced_bytes = chunk_text.size() - 1U;
    } else {
        return std::nullopt;
    }

    return chunk;
}

void append_decoded_chunk(const DecodedChunk& chunk, std::vector<std::uint8_t>& out) {
    out.push_back(static_cast<std::uint8_t>((chunk.v0 << 2U) | ((chunk.v1 >> 4U) & 0x03U)));
    if (chunk.produced_bytes >= 2U) {
        out.push_back(static_cast<std::uint8_t>(((chunk.v1 & 0x0FU) << 4U) | ((chunk.v2 >> 2U) & 0x0FU)));
    }
    if (chunk.produced_bytes == 3U) {
        out.push_back(static_cast<std::uint8_t>(((chunk.v2 & 0x03U) << 6U) | (chunk.v3 & 0x3FU)));
    }
}

std::optional<std::vector<std::uint8_t>> decode_impl(std::string_view input, std::string_view alphabet,
                                                     bool allow_padding) {
    if (input.empty()) {
        return std::vector<std::uint8_t>{};
    }
    if ((input.size() % 4U) == 1U) {
        return std::nullopt;
    }

    const bool has_padding_char = input.find('=') != std::string_view::npos;
    if (has_padding_char) {
        if (!allow_padding || ((input.size() % 4U) != 0U)) {
            return std::nullopt;
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve((input.size() * 3U) / 4U);

    std::size_t idx = 0U;
    while (idx < input.size()) {
        const std::size_t remain = input.size() - idx;
        const std::size_t chunk_size = remain >= 4U ? 4U : remain;
        if (chunk_size < 4U && has_padding_char) {
            return std::nullopt;
        }

        const std::string_view chunk_text = input.substr(idx, chunk_size);
        const bool is_final_chunk = (idx + chunk_size) == input.size();
        const std::optional<DecodedChunk> chunk = parse_chunk(chunk_text, alphabet, allow_padding, is_final_chunk);
        if (!chunk.has_value()) {
            return std::nullopt;
        }

        append_decoded_chunk(*chunk, out);
        idx += chunk_size;
    }

    return out;
}

std::optional<std::string> decode_to_string_impl(std::string_view input, std::string_view alphabet,
                                                 bool allow_padding) {
    const std::optional<std::vector<std::uint8_t>> decoded = decode_impl(input, alphabet, allow_padding);
    if (!decoded.has_value()) {
        return std::nullopt;
    }

    std::string text;
    text.reserve(decoded->size());
    for (const std::uint8_t byte : *decoded) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

}  // namespace

std::string base64_encode(std::span<const std::uint8_t> input) {
    return encode_impl(std::as_bytes(input), kBase64Alphabet, true);
}

std::string base64_encode(std::string_view input) {
    const std::span<const char> chars(input.data(), input.size());
    return encode_impl(std::as_bytes(chars), kBase64Alphabet, true);
}

std::optional<std::vector<std::uint8_t>> base64_decode_to_bytes(std::string_view input) {
    return decode_impl(input, kBase64Alphabet, true);
}

std::optional<std::string> base64_decode_to_string(std::string_view input) {
    return decode_to_string_impl(input, kBase64Alphabet, true);
}

std::string base64url_encode(std::span<const std::uint8_t> input) {
    return encode_impl(std::as_bytes(input), kBase64UrlAlphabet, false);
}

std::string base64url_encode(std::string_view input) {
    const std::span<const char> chars(input.data(), input.size());
    return encode_impl(std::as_bytes(chars), kBase64UrlAlphabet, false);
}

std::optional<std::vector<std::uint8_t>> base64url_decode_to_bytes(std::string_view input) {
    return decode_impl(input, kBase64UrlAlphabet, false);
}

std::optional<std::string> base64url_decode_to_string(std::string_view input) {
    return decode_to_string_impl(input, kBase64UrlAlphabet, false);
}

}  // namespace nebula::common
