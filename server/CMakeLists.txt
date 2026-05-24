cmake_minimum_required(VERSION 3.20)
project(Nebula LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS true)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif ()

find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
if (CLANG_TIDY_EXE)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--format-style=file")
endif ()

set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")
set(INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
set(TEST_DIR "${PROJECT_SOURCE_DIR}/tests")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)

file(GLOB_RECURSE NEBULA_CORE_SRC CONFIGURE_DEPENDS "${SRC_DIR}/*.cpp")
list(REMOVE_ITEM NEBULA_CORE_SRC "${SRC_DIR}/main.cpp")

add_library(nebula_core ${NEBULA_CORE_SRC})

target_compile_features(nebula_core PUBLIC cxx_std_23)
set_target_properties(nebula_core PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(nebula_core PUBLIC ${INCLUDE_DIR})
target_compile_options(nebula_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nebula_core PUBLIC Threads::Threads OpenSSL::Crypto PkgConfig::LIBPQXX)

add_executable(nebula "${SRC_DIR}/main.cpp")
target_compile_options(nebula PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nebula PRIVATE nebula_core)

include(CTest)
if (BUILD_TESTING)
    function(add_nebula_test target_name source_file)
        add_executable(${target_name} "${TEST_DIR}/${source_file}")
        set_target_properties(${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests" CXX_EXTENSIONS OFF)
        target_include_directories(${target_name} PRIVATE ${INCLUDE_DIR} ${TEST_DIR}/include)
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${target_name} PRIVATE nebula_core)
        add_test(NAME ${target_name} COMMAND ${target_name})
        set_tests_properties(${target_name} PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()

    add_nebula_test(http_server_integration_test integration/http_server_integration_test.cpp)
    add_nebula_test(http_server_auth_db_integration_test integration/http_server_auth_db_integration_test.cpp)
    add_nebula_test(http_server_storage_db_integration_test integration/http_server_storage_db_integration_test.cpp)

    add_nebula_test(app_config_test unit/app/app_config_test.cpp)
    add_nebula_test(app_main_options_test unit/app/main_options_test.cpp)
    add_nebula_test(app_server_app_test unit/app/server_app_test.cpp)
    add_nebula_test(app_startup_test unit/app/startup_test.cpp)

    add_nebula_test(auth_jwt_secret_store_test unit/auth/jwt_secret_store_test.cpp)
    add_nebula_test(auth_jwt_service_test unit/auth/jwt_service_test.cpp)
    add_nebula_test(auth_module_test unit/auth/module_test.cpp)
    add_nebula_test(auth_password_hasher_test unit/auth/password_hasher_test.cpp)
    add_nebula_test(auth_permissions_test unit/auth/permissions_test.cpp)
    add_nebula_test(auth_repository_test unit/auth/repository_test.cpp)
    add_nebula_test(auth_service_test unit/auth/service_test.cpp)

    add_nebula_test(common_arithmetic_test unit/common/arithmetic_test.cpp)
    add_nebula_test(common_base64_test unit/common/base64_test.cpp)
    add_nebula_test(common_crypto_test unit/common/crypto_test.cpp)
    add_nebula_test(common_file_io_test unit/common/file_io_test.cpp)
    add_nebula_test(common_hex_test unit/common/hex_test.cpp)
    add_nebula_test(common_json_test unit/common/json_test.cpp)
    add_nebula_test(common_logger_test unit/common/logger_test.cpp)
    add_nebula_test(common_logger_types_test unit/common/logger_types_test.cpp)
    add_nebula_test(common_string_test unit/common/string_test.cpp)
    add_nebula_test(common_toml_test unit/common/toml_test.cpp)

    add_nebula_test(database_config_test unit/database/config_test.cpp)
    add_nebula_test(database_connection_info_test unit/database/connection_info_test.cpp)
    add_nebula_test(database_connection_pool_test unit/database/connection_pool_test.cpp)

    add_nebula_test(http_api_response_contract_test unit/http/api_response_contract_test.cpp)
    add_nebula_test(http_parser_test unit/http/parser_test.cpp)
    add_nebula_test(http_request_redaction_test unit/http/request_redaction_test.cpp)
    add_nebula_test(http_response_writer_test unit/http/response_writer_test.cpp)
    add_nebula_test(http_router_test unit/http/router_test.cpp)

    add_nebula_test(net_eventfd_test unit/net/eventfd_test.cpp)

    add_nebula_test(server_http_request_dispatcher_test unit/server/http_request_dispatcher_test.cpp)

    add_nebula_test(storage_download_ticket_test unit/storage/download_ticket_test.cpp)
    add_nebula_test(storage_file_types_test unit/storage/file_types_test.cpp)
    add_nebula_test(storage_module_test unit/storage/module_test.cpp)
    add_nebula_test(storage_object_store_test unit/storage/object_store_test.cpp)
    add_nebula_test(storage_path_test unit/storage/path_test.cpp)
    add_nebula_test(storage_permissions_test unit/storage/permissions_test.cpp)
    add_nebula_test(storage_types_test unit/storage/types_test.cpp)
endif ()
