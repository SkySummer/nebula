cmake_minimum_required(VERSION 3.20)
project(Nebula LANGUAGES CXX)

option(NEBULA_BUILD_TESTING "Build Nebula tests" ${PROJECT_IS_TOP_LEVEL})

if (NEBULA_BUILD_TESTING)
    enable_testing()
endif ()

add_subdirectory(server)
