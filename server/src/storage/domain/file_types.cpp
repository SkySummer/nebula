#include "nebula/storage/domain/file_types.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nebula::storage {

namespace {

const std::unordered_map<std::string_view, std::string_view> kExtToType = {
    {"7z", "archive"},      {"apk", "archive"},     {"ar", "archive"},      {"bz", "archive"},
    {"bz2", "archive"},     {"cab", "archive"},     {"cpio", "archive"},    {"deb", "archive"},
    {"dmg", "archive"},     {"egg", "archive"},     {"gz", "archive"},      {"img", "archive"},
    {"iso", "archive"},     {"jar", "archive"},     {"lha", "archive"},     {"lz", "archive"},
    {"lz4", "archive"},     {"lzh", "archive"},     {"lzma", "archive"},    {"msi", "archive"},
    {"pkg", "archive"},     {"rar", "archive"},     {"rpm", "archive"},     {"tar", "archive"},
    {"tbz", "archive"},     {"tbz2", "archive"},    {"tgz", "archive"},     {"tlz", "archive"},
    {"txz", "archive"},     {"war", "archive"},     {"whl", "archive"},     {"xar", "archive"},
    {"xz", "archive"},      {"z", "archive"},       {"zip", "archive"},     {"zst", "archive"},
    {"3ga", "audio"},       {"8svx", "audio"},      {"aac", "audio"},       {"aif", "audio"},
    {"aiff", "audio"},      {"alac", "audio"},      {"amr", "audio"},       {"ape", "audio"},
    {"au", "audio"},        {"cda", "audio"},       {"dff", "audio"},       {"dsf", "audio"},
    {"flac", "audio"},      {"it", "audio"},        {"m4a", "audio"},       {"m4b", "audio"},
    {"mid", "audio"},       {"midi", "audio"},      {"mod", "audio"},       {"mp1", "audio"},
    {"mp2", "audio"},       {"mp3", "audio"},       {"mpa", "audio"},       {"oga", "audio"},
    {"ogg", "audio"},       {"opus", "audio"},      {"ra", "audio"},        {"ram", "audio"},
    {"s3m", "audio"},       {"snd", "audio"},       {"spx", "audio"},       {"tak", "audio"},
    {"tta", "audio"},       {"wav", "audio"},       {"weba", "audio"},      {"wma", "audio"},
    {"wv", "audio"},        {"xm", "audio"},        {"asm", "code"},        {"bat", "code"},
    {"c", "code"},          {"cc", "code"},         {"cjs", "code"},        {"clj", "code"},
    {"cls", "code"},        {"cmake", "code"},      {"conf", "code"},       {"cpp", "code"},
    {"cs", "code"},         {"css", "code"},        {"cts", "code"},        {"cxx", "code"},
    {"dart", "code"},       {"go", "code"},         {"h", "code"},          {"hh", "code"},
    {"hpp", "code"},        {"htm", "code"},        {"html", "code"},       {"hxx", "code"},
    {"ini", "code"},        {"java", "code"},       {"js", "code"},         {"json", "code"},
    {"json5", "code"},      {"jsonc", "code"},      {"jsx", "code"},        {"kt", "code"},
    {"kts", "code"},        {"less", "code"},       {"lua", "code"},        {"m", "code"},
    {"make", "code"},       {"mjs", "code"},        {"mk", "code"},         {"mts", "code"},
    {"php", "code"},        {"pl", "code"},         {"properties", "code"}, {"py", "code"},
    {"r", "code"},          {"rb", "code"},         {"rs", "code"},         {"sass", "code"},
    {"scss", "code"},       {"sh", "code"},         {"sql", "code"},        {"svelte", "code"},
    {"swift", "code"},      {"toml", "code"},       {"ts", "code"},         {"tsx", "code"},
    {"vue", "code"},        {"xml", "code"},        {"yaml", "code"},       {"yml", "code"},
    {"zsh", "code"},        {"csv", "csv"},         {"psv", "csv"},         {"tab", "csv"},
    {"tsv", "csv"},         {"et", "excel"},        {"ett", "excel"},       {"numbers", "excel"},
    {"ods", "excel"},       {"ots", "excel"},       {"xls", "excel"},       {"xlsb", "excel"},
    {"xlsm", "excel"},      {"xlsx", "excel"},      {"xlt", "excel"},       {"xltm", "excel"},
    {"xltx", "excel"},      {"apng", "image"},      {"arw", "image"},       {"avci", "image"},
    {"avcs", "image"},      {"avif", "image"},      {"bmp", "image"},       {"cr2", "image"},
    {"cr3", "image"},       {"dib", "image"},       {"djv", "image"},       {"djvu", "image"},
    {"dng", "image"},       {"emf", "image"},       {"gif", "image"},       {"heic", "image"},
    {"heif", "image"},      {"ico", "image"},       {"jfif", "image"},      {"jp2", "image"},
    {"jpe", "image"},       {"jpeg", "image"},      {"jpg", "image"},       {"jpm", "image"},
    {"jpx", "image"},       {"jxl", "image"},       {"nef", "image"},       {"nrw", "image"},
    {"orf", "image"},       {"pbm", "image"},       {"pcx", "image"},       {"pef", "image"},
    {"pgm", "image"},       {"png", "image"},       {"pnm", "image"},       {"ppm", "image"},
    {"psd", "image"},       {"raf", "image"},       {"raw", "image"},       {"rgb", "image"},
    {"rw2", "image"},       {"sr2", "image"},       {"srf", "image"},       {"srw", "image"},
    {"svg", "image"},       {"svgz", "image"},      {"tga", "image"},       {"tif", "image"},
    {"tiff", "image"},      {"wbmp", "image"},      {"webp", "image"},      {"wmf", "image"},
    {"xcf", "image"},       {"pdf", "pdf"},         {"dps", "powerpoint"},  {"dpt", "powerpoint"},
    {"key", "powerpoint"},  {"odp", "powerpoint"},  {"otp", "powerpoint"},  {"pot", "powerpoint"},
    {"potm", "powerpoint"}, {"potx", "powerpoint"}, {"pps", "powerpoint"},  {"ppsm", "powerpoint"},
    {"ppsx", "powerpoint"}, {"ppt", "powerpoint"},  {"pptm", "powerpoint"}, {"pptx", "powerpoint"},
    {"ans", "text"},        {"asc", "text"},        {"ascii", "text"},      {"docbook", "text"},
    {"log", "text"},        {"markdown", "text"},   {"md", "text"},         {"mdown", "text"},
    {"mkd", "text"},        {"mkdn", "text"},       {"nfo", "text"},        {"org", "text"},
    {"rst", "text"},        {"rtf", "text"},        {"tex", "text"},        {"text", "text"},
    {"txt", "text"},        {"3g2", "video"},       {"3gp", "video"},       {"asf", "video"},
    {"avi", "video"},       {"f4v", "video"},       {"flv", "video"},       {"m2ts", "video"},
    {"m2v", "video"},       {"m4v", "video"},       {"mjpeg", "video"},     {"mjpg", "video"},
    {"mkv", "video"},       {"mov", "video"},       {"mp2v", "video"},      {"mp4", "video"},
    {"mpe", "video"},       {"mpeg", "video"},      {"mpg", "video"},       {"ogm", "video"},
    {"ogv", "video"},       {"qt", "video"},        {"rm", "video"},        {"rmvb", "video"},
    {"vob", "video"},       {"webm", "video"},      {"wmv", "video"},       {"yuv", "video"},
    {"abw", "word"},        {"doc", "word"},        {"docm", "word"},       {"docx", "word"},
    {"dot", "word"},        {"dotm", "word"},       {"dotx", "word"},       {"odt", "word"},
    {"ott", "word"},        {"pages", "word"},      {"wps", "word"},        {"wpt", "word"},
};

[[nodiscard]] std::string file_name_from_path(std::string_view path) {
    const std::size_t slash_pos = path.rfind('/');
    if (slash_pos == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(slash_pos + 1U));
}

[[nodiscard]] std::string lowercase_ascii(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char ch : text) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

}  // namespace

std::string_view classify_file_type(std::string_view path) {
    const std::string normalized_name = lowercase_ascii(file_name_from_path(path));
    const std::size_t dot_pos = normalized_name.rfind('.');
    if (dot_pos == std::string::npos || dot_pos + 1U >= normalized_name.size()) {
        return "other";
    }

    const std::string_view ext = std::string_view(normalized_name).substr(dot_pos + 1U);
    const auto type_it = kExtToType.find(ext);
    if (type_it != kExtToType.end()) {
        return type_it->second;
    }
    return "other";
}

}  // namespace nebula::storage
