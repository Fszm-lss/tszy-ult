# TSZY-ULT

A C++17 header-only networking framework for building game server clusters. Cross-platform (Linux/Windows) with epoll-based async I/O, encrypted communication, and service discovery.

## Features

- **Transport layer** (`zbf/`) — TCP server/client with async I/O, epoll (Linux) / poll (Windows), connection pooling, heartbeat, send queues, thread-safe FIFO, timer wheel, per-request latency tracking, and memory leak detection. Alternative transports: KCP reliable-UDP and standalone ASIO-based TCP.
- **Crypto & utilities** (`fszm/`) — OpenSSL wrapper (RSA, DH, ECDH, DSA, AES ECB/CBC/CFB/OFB/CTR, DES, Camellia, MD5, SHA, base64, hex), HTTP client (libcurl), binary serialization with DataTable support, and thread-local MT19937 random generation
- **Server framework** (`lygc/`) — Request/response routing by origin, sync/async handlers, gateway with DH+RSA handshake + AES-CFB encryption, HTTP-based service discovery, and JSON config management

## Quick Start

### Prerequisites

- C++17 compiler (GCC 9+, MSVC 2019+, Clang 10+)
- CMake 3.15+
- OpenSSL, libcurl
- Linux: [vcpkg](https://vcpkg.io/) with cpp-httplib, nlohmann-json
- Windows: MSYS2/UCRT64 with vendor dependencies

Install vcpkg dependencies (Linux):
```bash
vcpkg install cpp-httplib nlohmann-json openssl curl kcp asio
```

### Build

```bash
# Configure (generates build/debug/)
cmake --preset debug

# Build all targets
cmake --build --preset debug

# Build a single target
cmake --build --preset debug --target lygate
```

Available build targets: `tcpserver`, `tcpcltpool`, `lycentral`, `lylogic`, `lycommon`, `lycomasynclt`, `lygate`, `lygateclt`, `testsslv2`, `testhandshake`, `kcpserverv2`, `kcpclientv2`, `asio_tcpserver`, `asio_tcpclient`.

| Preset | Build Type | Platform |
|--------|-----------|----------|
| `debug` | Debug (symbols, no opt, assertions) | Linux (vcpkg) |
| `release` | Release (optimized, NDEBUG) | Linux (vcpkg) |
| `relwithdebinfo` | Optimized + debug symbols | Linux (vcpkg) |
| `debug-mingw` | Debug | Windows/MSYS2 UCRT64 |
| `release-mingw` | Release | Windows/MSYS2 UCRT64 |
| `relwithdebinfo-mingw` | Optimized + debug symbols | Windows/MSYS2 UCRT64 |

Build artifacts go to `build/<preset-name>/`.

### Run Tests

```bash
./build/debug/testsslv2      # OpenSSL wrappers: RSA, DH, ECDH, DSA, AES, hash
./build/debug/testhandshake  # DH+RSA handshake protocol
```

Generate crypto keys for tests (required before first run):

```bash
cd test && ./createkey.sh rsa   # RSA 4096-bit key pair
./createkey.sh dh               # DH CA + server + client certs (default if no args)
./createkey.sh ecdh             # ECDH certs (secp384r1 curve)
./createkey.sh dsa              # DSA 2048-bit key pair
./createkey.sh clean            # Remove generated keys
```

Keys are output to `test/key/`. Only `testsslv2` has a CMake post-build step to copy keys — if building only `testhandshake`, copy manually:
```bash
cp -r test/key build/debug/
```
`testhandshake` tests both DH+RSA and ECDH+RSA handshake variants.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  lygc/  — Game server framework                 │
│  GateServer · NetServer · NetClient · Central   │
│  lymsg_protocol · handshake · config            │
├─────────────────────────────────────────────────┤
│  fszm/  — Crypto & utilities                    │
│  OpenSSL · curl · serialize · random            │
├─────────────────────────────────────────────────┤
│  zbf/   — Transport primitives                  │
│  TCP · KCP · ASIO · poll/epoll · timer · queue  │
└─────────────────────────────────────────────────┘
```

Three transport stacks are available:

| Transport | Header | Protocol | Key Classes |
|-----------|--------|----------|-------------|
| TCP (epoll/poll) | `socket_tcp_v5.hpp` | TCP on epoll(Linux)/poll(Windows) | `tcpsock_server`, `tcpsock_user`, `tcpsock_ha_asynclt` |
| KCP (reliable UDP) | `socket_kcp_v2.hpp` | Reliable UDP via KCP | `kcpsock_server`, `kcp_user` |
| ASIO TCP | `asio_socket_tcp.hpp` | Standalone ASIO TCP | `tcpsock_server`, `tcpsock_client` (different interface) |

### Message Flow

Client connects → `tcpsock_user::onRecvMsg` → `NetUser::onRecvMsg` unpacks via `lymsg_protocol` → `NetUser::onRequest` routes to `UserHandler` by `origin` → handler returns one of:

| Return Value | Behavior |
|-------------|----------|
| `0` (`SYNC_RESPONSE`) | Response sent immediately to the caller |
| `1` (`NO_RESPONSE`) | An empty ACK is sent |
| Other (`>=2`, including `ASYNC_RESPONSE = 101`) | Request context saved; response delivered later via `NetServer::response(serial, data)` |

For inter-service communication, `NetClient::request` dispatches a message and registers a callback by serial number. When the response arrives, `NetClient::onResponse` matches the serial and invokes the handler.

### Gateway Encryption

`GateServer` enforces an encrypted channel between external clients and the server cluster:

1. **Handshake request:** Client generates DH/ECDH parameters + a temporary AES key, encrypts them with the server's RSA public key, and sends to the gateway.
2. **Handshake response:** Server decrypts with its RSA private key, generates a DH/ECDH keypair from the client's parameters, and responds with its public key encrypted by the temporary AES key.
3. **Session established:** Both sides compute a shared key from the DH/ECDH exchange (first 16 bytes = AES session key, next 16 bytes = IV). All subsequent messages carry the `LYMSG_TYPE_ENC` flag and are encrypted/decrypted with AES-CFB using this session key.

`GateUser` handles per-connection handshake state, automatically decrypting inbound encrypted messages and encrypting outbound responses.

### Service Discovery

The central registry enables dynamic service lookup:

- **`CentralServer`** (cpp-httplib HTTP server) — Maintains a key-value store of server configurations. In `lycentral`, the config JSON is loaded from file and stored via `set("config", data)`. Other servers then fetch it at startup.
- **`CentralServClt`** (aliased as `lygc::Central`) — Static libcurl-based client. Default endpoint is `http://localhost:8081`. Servers call `Central::get("config")` at startup to retrieve the cluster configuration.

### Server Roles

| Server | Binary | Role |
|--------|--------|------|
| **Central** | `lycentral` | Service registry — loads `lycentral-conf.json`, serves config via HTTP |
| **Gateway** | `lygate` | Entry point — RSA handshake + AES encryption, forwards to logic via `NetClient` |
| **Logic** | `lylogic` | Business logic — `NetServer` with handlers registered per origin |
| **Common** | `lycommon` | Shared service — `NetServer`, proxies to logic via async `NetClient::request` |
| **Gate Client** | `lygateclt` | Test client — performs handshake then sends encrypted requests |
| **Common Client** | `lycomasynclt` | Test client — async request/response test against common service |
| **KCP Server** | `kcpserverv2` | Demo — KCP reliable-UDP server with HTTP control plane |
| **KCP Client** | `kcpclientv2` | Demo — KCP client with HTTP-based connection setup |
| **ASIO Server** | `asio_tcpserver` | Demo — ASIO-based TCP echo (argv: host port) |
| **ASIO Client** | `asio_tcpclient` | Demo — ASIO-based TCP client with sync & async modes |

Demo servers (`tcpserver`, `tcpcltpool`) demonstrate raw `tcpsock_server` and `tcpsock_cltpool` usage without the game server framework.

### Server Lifecycle

All servers (except `lycentral`) follow the same lifecycle pattern, including `std::atexit(onExit)` for memory tracking dump on exit:

```cpp
Server server(config);
server.open();          // Bind and listen
server.start(workers);  // Launch I/O worker threads + timer
server.serveUtilStop(); // Block until signal (SIGINT/SIGTERM)
server.close();         // Graceful shutdown
```

`start()` launches a configurable number of worker threads for I/O processing. `NetServer::start(serverWorkers, cltMgrWorkers)` also starts an optional async client manager (for outbound HA connections) when `cltMgrWorkers > 0`, and a 10-second timer wheel for periodic status reporting. Signal handlers call `stop()` which unblocks `serveUtilStop()`. In Debug builds, ASIO timeouts are shortened (UserTimeout=9s, HeartbeatTime=3s) for faster iteration; Release uses 90s/30s.

### Memory Tracking

Controlled by the `ZBF_TRACE_MEMORY` define (enabled by default). All allocations use `ZBF_MALLOC`/`ZBF_FREE` macros for per-file accounting. Key classes inherit from `object_tracker<T>` (CRTP) to count live instances. Call `logMemTrackStat()` to dump current allocation statistics — this is automatically called via `std::atexit` in most examples.

## Cluster Startup

To run the full server cluster (in order):

```bash
# 1. Start the service registry (HTTP on port 8081)
./lycentral
# 2. Start the logic server (listens on port 35101)
./lylogic
# 3. Start the common proxy (port 35102, proxies to logic)
./lycommon
# 4. Start the gateway (port 35105, needs RSA private key)
./lygate
# 5. Test with encrypted client or direct async client
./lygateclt
./lycomasynclt
```

## Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL | RSA, DH, ECDH, DSA, AES, DES, Camellia, MD5, SHA, base64, hex |
| libcurl | HTTP client for service discovery |
| cpp-httplib | HTTP server for central registry |
| nlohmann-json | JSON config parsing |
| kcp | Reliable UDP transport |
| asio | Standalone ASIO TCP transport |

## License

[MIT](LICENSE)
