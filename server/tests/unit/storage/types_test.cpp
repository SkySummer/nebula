#include "nebula/storage/domain/types.hpp"

#include <optional>
#include <string>

#include "nebula_tests/common.hpp"

namespace {

using namespace nebula;

void test_storage_node_type_parse_contract() {
    test::expect_equal(nebula::storage::parse_storage_node_type("file"),
                       std::optional<storage::StorageNodeType>(storage::StorageNodeType::File),
                       "file node_type should parse");
    test::expect_equal(nebula::storage::parse_storage_node_type("directory"),
                       std::optional<storage::StorageNodeType>(storage::StorageNodeType::Directory),
                       "directory node_type should parse");
    test::expect_true(!nebula::storage::parse_storage_node_type("symlink").has_value(),
                      "unknown node_type should be rejected");
    test::expect_equal(nebula::storage::to_string(storage::StorageNodeType::File), std::string("file"),
                       "file node_type should serialize");
    test::expect_equal(nebula::storage::to_string(storage::StorageNodeType::Directory), std::string("directory"),
                       "directory node_type should serialize");
}

int run_types_tests() {
    const std::vector<nebula::test::TestCase> tests = {
        {"storage node type parse contract", test_storage_node_type_parse_contract},
    };
    return nebula::test::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::test::run_main(run_types_tests);
}
