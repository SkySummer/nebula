#include "nebula/storage/domain/file_types.hpp"

#include <string_view>
#include <utility>
#include <vector>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_classify_file_type_by_known_extensions() {
    test::expect_equal(storage::classify_file_type("/files/photo.PNG"), std::string("image"),
                       "png should classify as image regardless of case");
    test::expect_equal(storage::classify_file_type("/docs/report.pdf"), std::string("pdf"),
                       "pdf should classify as pdf");
    test::expect_equal(storage::classify_file_type("/sheets/budget.xlsx"), std::string("excel"),
                       "xlsx should classify as excel");
    test::expect_equal(storage::classify_file_type("/slides/demo.pptx"), std::string("powerpoint"),
                       "pptx should classify as powerpoint");
    test::expect_equal(storage::classify_file_type("/code/main.ts"), std::string("code"), "ts should classify as code");
}

void test_classify_file_type_falls_back_to_other() {
    test::expect_equal(storage::classify_file_type("/misc/archive.unknown"), std::string("other"),
                       "unknown extension should classify as other");
    test::expect_equal(storage::classify_file_type("/misc/no-extension"), std::string("other"),
                       "missing extension should classify as other");
}

void test_classify_file_type_uses_mapping_table_extensions() {
    const std::vector<std::pair<std::string_view, std::string_view>> cases = {
        {"/archives/package.zst", "archive"},    {"/music/live.flac", "audio"},
        {"/configs/app.yaml", "code"},           {"/data/table.tsv", "csv"},
        {"/spreadsheets/budget.xlsm", "excel"},  {"/images/raw.heic", "image"},
        {"/slides/template.potm", "powerpoint"}, {"/notes/spec.markdown", "text"},
        {"/videos/movie.rmvb", "video"},         {"/docs/template.dotx", "word"},
    };

    for (const auto& [path, expected_type] : cases) {
        test::expect_equal(storage::classify_file_type(path), std::string(expected_type),
                           "extension from kExtToType should classify correctly");
    }
}

int run_file_types_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"classify file type by known extensions", test_classify_file_type_by_known_extensions},
        {"classify file type falls back to other", test_classify_file_type_falls_back_to_other},
        {"classify file type by mapping table extensions", test_classify_file_type_uses_mapping_table_extensions},
    };

    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_file_types_tests);
}
