cmake_minimum_required(VERSION 3.13)
project(Nebula LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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
target_include_directories(nebula_core PUBLIC ${INCLUDE_DIR})
target_compile_options(nebula_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nebula_core PUBLIC Threads::Threads OpenSSL::Crypto PkgConfig::LIBPQXX)

add_executable(nebula "${SRC_DIR}/main.cpp")
target_compile_options(nebula PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nebula PRIVATE nebula_core)

include(CTest)
if (BUILD_TESTING)
    target_compile_definitions(nebula_core PRIVATE NEBULA_ENABLE_TESTING_API=1)

    function(add_nebula_test target_name source_file)
        add_executable(${target_name} "${TEST_DIR}/${source_file}")
        set_target_properties(${target_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
        target_include_directories(${target_name} PRIVATE ${INCLUDE_DIR} ${TEST_DIR}/include)
        target_compile_definitions(${target_name} PRIVATE NEBULA_ENABLE_TESTING_API=1)
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${target_name} PRIVATE nebula_core)
        add_test(NAME ${target_name} COMMAND ${target_name})
        set_tests_properties(${target_name} PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()

    add_nebula_test(http_server_integration_test integration/http_server_integration_test.cpp)
    add_nebula_test(http_server_auth_db_integration_test integration/http_server_auth_db_integration_test.cpp)
    add_nebula_test(http_server_storage_db_integration_test integration/http_server_storage_db_integration_test.cpp)

    add_nebula_test(auth_service_test unit/auth/auth_service_test.cpp)
    add_nebula_test(jwt_secret_store_test unit/auth/jwt_secret_store_test.cpp)
    add_nebula_test(jwt_service_test unit/auth/jwt_service_test.cpp)
    add_nebula_test(password_hasher_test unit/auth/password_hasher_test.cpp)
    add_nebula_test(user_repository_test unit/auth/user_repository_test.cpp)

    add_nebula_test(base64_test unit/common/base64_test.cpp)
    add_nebula_test(database_utils_test unit/common/database_utils_test.cpp)
    add_nebula_test(json_test unit/common/json_test.cpp)
    add_nebula_test(logger_test unit/common/logger_test.cpp)
    add_nebula_test(postgres_connection_pool_test unit/common/postgres_connection_pool_test.cpp)
    add_nebula_test(string_utils_test unit/common/string_utils_test.cpp)

    add_nebula_test(toml_parser_test unit/config/toml_parser_test.cpp)

    add_nebula_test(api_response_contract_test unit/http/api_response_contract_test.cpp)
    add_nebula_test(http_parser_test unit/http/http_parser_test.cpp)
    add_nebula_test(http_response_writer_test unit/http/http_response_writer_test.cpp)
    add_nebula_test(router_test unit/http/router_test.cpp)

    add_nebula_test(eventfd_utils_test unit/net/eventfd_utils_test.cpp)

    add_nebula_test(main_options_test unit/server/main_options_test.cpp)
    add_nebula_test(server_app_test unit/server/server_app_test.cpp)
    add_nebula_test(server_config_loader_test unit/server/server_config_loader_test.cpp)
    add_nebula_test(startup_test unit/server/startup_test.cpp)

    add_nebula_test(storage_types_test unit/storage/storage_types_test.cpp)
    add_nebula_test(storage_utils_test unit/storage/storage_utils_test.cpp)

endif ()
