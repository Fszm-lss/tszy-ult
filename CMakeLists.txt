cmake_minimum_required(VERSION 3.15)
project(TSZY-ULT LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
message(STATUS "# Project Root: ${PROJECT_SOURCE_DIR}")

find_package(Threads       REQUIRED)
find_package(OpenSSL       REQUIRED)
find_package(CURL          REQUIRED)

if(WIN32)
    # vendor path (sibling to project root)
    set(VENDOR_DIR "${CMAKE_SOURCE_DIR}/../vendor" CACHE PATH "Vendor dependencies directory")

    # httplib from vendor (header-only)
    file(GLOB HTTPLIB_INCLUDE_DIR LIST_DIRECTORIES true "${VENDOR_DIR}/httplib-*")
    list(GET HTTPLIB_INCLUDE_DIR 0 HTTPLIB_INCLUDE_DIR)
    add_library(_httplib INTERFACE)
    target_include_directories(_httplib INTERFACE "${HTTPLIB_INCLUDE_DIR}")
    add_library(httplib::httplib ALIAS _httplib)

    # asio (header-only, no cmake config in pacman)
    find_path(ASIO_INCLUDE_DIR asio.hpp)
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE "${ASIO_INCLUDE_DIR}")
    target_link_libraries(asio INTERFACE ws2_32 mswsock)
    add_library(asio::asio ALIAS asio)
    
    # nlohmann-json (header-only, no cmake config in pacman)
    find_path(NLJSON_INCLUDE_DIR nlohmann/json.hpp)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${NLJSON_INCLUDE_DIR}")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

    # hiredis (static lib, no cmake config in pacman)
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
    find_library(HIREDIS_LIB hiredis)
    add_library(hiredis STATIC IMPORTED)
    set_target_properties(hiredis PROPERTIES
        IMPORTED_LOCATION "${HIREDIS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}")
    add_library(hiredis::hiredis ALIAS hiredis)

    # MSYS2 mongoc cmake config bug: libm.a path is "D:/M/msys64/ucrt64/lib/libm.a",
    # fix with: sed -i 's|D:/M/msys64/ucrt64/lib/libm.a|C:/msys64/ucrt64/lib/libm.a|g' /ucrt64/lib/cmake/bson/00-mongo-platform-targets.cmake
    find_package(mongoc REQUIRED)

    link_libraries(ws2_32)
else()
    find_package(httplib       CONFIG REQUIRED)
    find_package(asio          CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(mongoc        CONFIG REQUIRED)
    find_package(hiredis       CONFIG REQUIRED)
endif()

# mysqlclient is a system library (no vcpkg port), locate it manually
find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient mariadb)
if(MYSQLCLIENT_LIBRARY)
    add_library(mysqlclient UNKNOWN IMPORTED)
    set_target_properties(mysqlclient PROPERTIES IMPORTED_LOCATION "${MYSQLCLIENT_LIBRARY}")
endif()

# jemalloc dynamic allocator (Linux only; set -DUSE_JEMALLOC=OFF to disable)
option(USE_JEMALLOC "Use jemalloc dynamic allocator" ON)
if(USE_JEMALLOC AND NOT WIN32)
    find_library(JEMALLOC_LIBRARY jemalloc REQUIRED)
    find_path(JEMALLOC_INCLUDE_DIR jemalloc/jemalloc.h REQUIRED)
    message(STATUS "USE_JEMALLOC: lib=${JEMALLOC_LIBRARY}, include=${JEMALLOC_INCLUDE_DIR}")

    # Global link and include for all subsequent targets
    link_libraries(${JEMALLOC_LIBRARY})
    include_directories(${JEMALLOC_INCLUDE_DIR})
    add_compile_definitions(USE_JEMALLOC)

    # Linux: binaries find libjemalloc.so in their own directory ($ORIGIN)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")

    # Resolve symlink: libjemalloc.so -> libjemalloc.so.2
    get_filename_component(JEMALLOC_RUNTIME_LIB "${JEMALLOC_LIBRARY}" REALPATH)
    get_filename_component(JEMALLOC_RUNTIME_NAME "${JEMALLOC_RUNTIME_LIB}" NAME)

    # Copy libjemalloc.so.N to build directory for development/testing
    add_custom_target(copy_jemalloc_so ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JEMALLOC_RUNTIME_LIB}" "${CMAKE_BINARY_DIR}/${JEMALLOC_RUNTIME_NAME}"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/${JEMALLOC_RUNTIME_NAME}"
        COMMENT "Copying ${JEMALLOC_RUNTIME_NAME} to binary output directory"
    )
endif()

# examples
add_executable(tcpserver  ${PROJECT_SOURCE_DIR}/examples/tcpserver.cpp)
target_include_directories(tcpserver  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(tcpserver  Threads::Threads)
add_executable(tcpcltpool ${PROJECT_SOURCE_DIR}/examples/tcpcltpool.cpp)
target_include_directories(tcpcltpool PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(tcpcltpool Threads::Threads)

add_executable(asio_tcpserver ${PROJECT_SOURCE_DIR}/examples/asio_tcpserver.cpp)
target_include_directories(asio_tcpserver PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(asio_tcpserver Threads::Threads asio::asio OpenSSL::SSL OpenSSL::Crypto)
add_dependencies(asio_tcpserver copy_key)
add_executable(asio_tcpclient ${PROJECT_SOURCE_DIR}/examples/asio_tcpclient.cpp)
target_include_directories(asio_tcpclient PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(asio_tcpclient Threads::Threads asio::asio OpenSSL::SSL OpenSSL::Crypto)
add_dependencies(asio_tcpclient copy_key)

add_custom_target(copy_lycentral_conf ALL
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/examples/lycentral-conf.json ${CMAKE_BINARY_DIR}/lycentral-conf.json
    COMMENT "copy lycentral config"
)
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "${CMAKE_BINARY_DIR}/lycentral-conf.json")

add_executable(lycentral ${PROJECT_SOURCE_DIR}/examples/lycentral.cpp)
target_include_directories(lycentral  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycentral Threads::Threads nlohmann_json::nlohmann_json httplib::httplib)
add_dependencies(lycentral copy_lycentral_conf)

add_executable(lylogic   ${PROJECT_SOURCE_DIR}/examples/lylogic.cpp)
target_include_directories(lylogic       PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lylogic   Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl)

add_executable(lycommon      ${PROJECT_SOURCE_DIR}/examples/lycommon.cpp)
target_include_directories(lycommon      PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycommon      Threads::Threads nlohmann_json::nlohmann_json CURL::libcurl)
add_executable(lycomasynclt  ${PROJECT_SOURCE_DIR}/examples/lycomasynclt.cpp)
target_include_directories(lycomasynclt  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lycomasynclt  Threads::Threads nlohmann_json::nlohmann_json)

add_custom_target(copy_key ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/test/key ${CMAKE_BINARY_DIR}/key
    COMMENT "copy test key files"
)
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES "${CMAKE_BINARY_DIR}/key")

add_executable(lygate    ${PROJECT_SOURCE_DIR}/examples/lygate.cpp)
target_include_directories(lygate    PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lygate    Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json CURL::libcurl)
add_dependencies(lygate copy_key)
add_executable(lygateclt ${PROJECT_SOURCE_DIR}/examples/lygateclt.cpp)
target_include_directories(lygateclt PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lygateclt Threads::Threads OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
add_dependencies(lygateclt copy_key)

add_executable(lydbproxy-mysql ${PROJECT_SOURCE_DIR}/examples/lydbproxy-mysql.cpp)
target_include_directories(lydbproxy-mysql PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lydbproxy-mysql Threads::Threads nlohmann_json::nlohmann_json mysqlclient CURL::libcurl)
add_executable(lydbproxy-mongo ${PROJECT_SOURCE_DIR}/examples/lydbproxy-mongo.cpp)
target_include_directories(lydbproxy-mongo PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lydbproxy-mongo Threads::Threads nlohmann_json::nlohmann_json mongoc::shared CURL::libcurl)
add_executable(lydbproxy-redis ${PROJECT_SOURCE_DIR}/examples/lydbproxy-redis.cpp)
target_include_directories(lydbproxy-redis PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lydbproxy-redis Threads::Threads nlohmann_json::nlohmann_json hiredis::hiredis CURL::libcurl)
add_executable(lydbagent    ${PROJECT_SOURCE_DIR}/examples/lydbagent.cpp)
target_include_directories(lydbagent    PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lydbagent    Threads::Threads nlohmann_json::nlohmann_json mongoc::shared mysqlclient hiredis::hiredis CURL::libcurl)
add_executable(lydbagentclt ${PROJECT_SOURCE_DIR}/examples/lydbagentclt.cpp)
target_include_directories(lydbagentclt PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(lydbagentclt Threads::Threads nlohmann_json::nlohmann_json)

# test
add_executable(testsslv2 ${PROJECT_SOURCE_DIR}/test/test_opensslv2.cpp)
target_include_directories(testsslv2 PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(testsslv2 Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_dependencies(testsslv2 copy_key)

add_executable(testhandshake ${PROJECT_SOURCE_DIR}/test/test_handshake.cpp)
target_include_directories(testhandshake PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(testhandshake Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_dependencies(testhandshake copy_key)
