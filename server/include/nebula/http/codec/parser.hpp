#ifndef NEBULA_HTTP_CODEC_PARSER_HPP
#define NEBULA_HTTP_CODEC_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "nebula/http/protocol/request.hpp"
#include "nebula/http/protocol/status.hpp"

namespace nebula::http {

enum class ParseStatus : std::uint8_t {
    NeedMore,
    Complete,
    Error,
};

enum class ChunkedDecodePhase : std::uint8_t {
    ChunkSizeLine,
    ChunkPayload,
    ChunkTrailers,
};

struct HttpRequestParseContext {
    bool header_parsed = false;
    bool chunked_body = false;
    std::size_t header_bytes = 0;
    std::size_t content_length = 0;
    std::size_t chunk_cursor = 0;
    std::size_t chunk_size = 0;
    ChunkedDecodePhase chunk_phase = ChunkedDecodePhase::ChunkSizeLine;
    HttpRequest request;
    std::string decoded_chunked_body;

    void reset();
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    HttpStatus http_status = HttpStatus::OK;
    std::size_t consumed_bytes = 0;
    HttpRequest request;
    std::string error_message;
};

ParseResult parse_http_request(std::string_view buffer, std::size_t max_header_bytes, std::size_t max_body_bytes,
                               std::size_t max_request_target_bytes);
ParseResult parse_http_request(std::string_view buffer, std::size_t max_header_bytes, std::size_t max_body_bytes,
                               std::size_t max_request_target_bytes, HttpRequestParseContext& context);

}  // namespace nebula::http

#endif  // NEBULA_HTTP_CODEC_PARSER_HPP
