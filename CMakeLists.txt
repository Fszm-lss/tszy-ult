cmake_minimum_required(VERSION 3.15)
project(TSZY-ULT LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
message(STATUS "# Project Root: ${PROJECT_SOURCE_DIR}")

find_package(Threads       REQUIRED)
find_package(OpenSSL       REQUIRED)
find_package(CURL          REQUIRED)

if(WIN32)
    # vendor path (sibling to project root)
    set(VENDOR_DIR "${CMAKE_SOURCE_DIR}/../vendor" CACHE PATH "Vendor dependencies directory")

    # httplib from vendor (header-only)
    add_library(_httplib INTERFACE)
    target_include_directories(_httplib INTERFACE "${VENDOR_DIR}/httplib-0.45.0")
    add_library(httplib::httplib ALIAS _httplib)

    # nlohmann-json (header-only, no cmake config in pacman)
    find_path(NLJSON_INCLUDE_DIR nlohmann/json.hpp)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${NLJSON_INCLUDE_DIR}")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

    link_libraries(ws2_32)
else()
    find_package(httplib       CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

# examples
add_executable(tcpserver  ${PROJECT_SOURCE_DIR}/examples/tcpserver.cpp)
target_include_directories(tcpserver  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(tcpserver  Threads::Threads)
add_executable(tcpcltpool ${PROJECT_SOURCE_DIR}/examples/tcpcltpool.cpp)
target_include_directories(tcpcltpool PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(tcpcltpool Threads::Threads)

add_executable(lycentral ${PROJECT_SOURCE_DIR}/examples/lycentral.cpp)
target_include_directories(lycentral  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycentral Threads::Threads nlohmann_json::nlohmann_json httplib::httplib)
add_custom_command(
    TARGET lycentral POST_BUILD
    COMMAND cp ${PROJECT_SOURCE_DIR}/examples/lycentral-conf.json ./
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "copy lycentral depends(config)"
)
add_executable(lylogic   ${PROJECT_SOURCE_DIR}/examples/lylogic.cpp)
target_include_directories(lylogic       PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lylogic   Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl)

add_executable(lycommon      ${PROJECT_SOURCE_DIR}/examples/lycommon.cpp)
target_include_directories(lycommon      PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycommon      Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl)
add_executable(lycomasynclt  ${PROJECT_SOURCE_DIR}/examples/lycomasynclt.cpp)
target_include_directories(lycomasynclt  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycomasynclt  Threads::Threads nlohmann_json::nlohmann_json)

add_executable(lygate    ${PROJECT_SOURCE_DIR}/examples/lygate.cpp)
target_include_directories(lygate    PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lygate    Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json CURL::libcurl)
add_executable(lygateclt ${PROJECT_SOURCE_DIR}/examples/lygateclt.cpp)
target_include_directories(lygateclt PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lygateclt Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)

# test
add_executable(testsslv2 ${PROJECT_SOURCE_DIR}/test/test_opensslv2.cpp)
target_include_directories(testsslv2 PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(testsslv2 Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_custom_command(
    TARGET testsslv2 POST_BUILD
    COMMAND cp -r ${PROJECT_SOURCE_DIR}/test/key ./
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "copy testsslv2 depends(key)"
)

add_executable(testhandshake ${PROJECT_SOURCE_DIR}/test/test_handshake.cpp)
target_include_directories(testhandshake PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(testhandshake Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
