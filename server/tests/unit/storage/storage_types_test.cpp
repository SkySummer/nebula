#include "nebula/storage/storage_types.hpp"

#include <optional>
#include <string>

#include "nebula_tests/test_support.hpp"

namespace {

using nebula::storage::StorageNodeType;
using nebula::testsupport::expect_equal;
using nebula::testsupport::expect_true;

void test_storage_node_type_parse_contract() {
    expect_equal(nebula::storage::parse_storage_node_type("file"),
                 std::optional<StorageNodeType>(StorageNodeType::File), "file node_type should parse");
    expect_equal(nebula::storage::parse_storage_node_type("directory"),
                 std::optional<StorageNodeType>(StorageNodeType::Directory), "directory node_type should parse");
    expect_true(!nebula::storage::parse_storage_node_type("symlink").has_value(),
                "unknown node_type should be rejected");
    expect_equal(std::string(nebula::storage::to_string(StorageNodeType::File)), std::string("file"),
                 "file node_type should serialize");
    expect_equal(std::string(nebula::storage::to_string(StorageNodeType::Directory)), std::string("directory"),
                 "directory node_type should serialize");
}

int run_storage_types_tests() {
    const std::vector<nebula::testsupport::TestCase> tests = {
        {"storage node type parse contract", test_storage_node_type_parse_contract},
    };
    return nebula::testsupport::run_tests(tests);
}

}  // namespace

int main() {
    return nebula::testsupport::run_main(run_storage_types_tests);
}
