# TSZY-ULT

A C++17 header-only networking framework for building game server clusters. Cross-platform (Linux/Windows) with epoll-based async I/O, encrypted communication, and service discovery.

## Features

- **Transport layer** (`zbf/`) — TCP server/client with async I/O, epoll (Linux) / poll (Windows), connection pooling, heartbeat, send queues, thread-safe FIFO, timer wheel, per-request latency tracking, and memory leak detection
- **Crypto & utilities** (`fszm/`) — OpenSSL wrapper (RSA, DH, ECDH, DSA, AES CBC/CFB/OFB/CTR, MD5, SHA, base64, hex), HTTP client (libcurl), binary serialization with DataTable support, and thread-local MT19937 random generation
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
vcpkg install cpp-httplib nlohmann-json openssl curl
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

Available build targets: `tcpserver`, `tcpcltpool`, `lycentral`, `lylogic`, `lycommon`, `lycomasynclt`, `lygate`, `lygateclt`, `testsslv2`, `testhandshake`.

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
./createkey.sh dh               # DH CA + server + client certs
./createkey.sh clean            # Remove generated keys
```

Keys are output to `test/key/` and copied to the build directory by CMake post-build steps.

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
│  TCP · poll/epoll · timer · queue · log         │
└─────────────────────────────────────────────────┘
```

### Message Protocol

The application-level protocol uses a 20-byte packed header:

```
origin:16 | type:32 | serial:64 | size:32 | param:16
```

- **origin** — Routing key for dispatching to registered handlers
- **type** — Message type with flag bits:
  - `LYMSG_TYPE_REQ` — Standard request
  - `LYMSG_TYPE_RESP` (0x80000000) — Response, ORed with the original request type
  - `LYMSG_TYPE_HANDSHAKE` — Gateway handshake message
- **serial** — 64-bit request ID for matching async responses
- **size** — Payload size in bytes
- **param** — Additional flags: `LYMSG_PARAM_ENC` (0x8000) marks AES-encrypted payloads

Network byte order is optional (`LYMSG_NETWORK_BYTE_ORDER` define), defaulting to host order for homogeneous x86_64 clusters. A lower-level `default_msg_proto` with 4-byte header (`type:8|size:24`) is also available for raw TCP demos.

### Message Flow

Client connects → `tcpsock_user::onRecvMsg` → `NetUser::onRecvMsg` unpacks via `lymsg_protocol` → `NetUser::onRequest` routes to `UserHandler` by `origin` → handler returns one of:

| Return Value | Behavior |
|-------------|----------|
| `SYNC_RESPONSE` | Response sent immediately to the caller |
| `NO_RESPONSE` | An empty ACK is sent |
| `> ASYNC_RESPONSE` | Request context saved; response delivered later via `NetServer::response(serial, data)` |

For inter-service communication, `NetClient::request` dispatches a message and registers a callback by serial number. When the response arrives, `NetClient::onResponse` matches the serial and invokes the handler.

### Gateway Encryption

`GateServer` enforces an encrypted channel between external clients and the server cluster:

1. **Handshake request:** Client generates DH parameters + a temporary AES key, encrypts them with the server's RSA public key, and sends to the gateway.
2. **Handshake response:** Server decrypts with its RSA private key, generates a DH keypair from the client's parameters, and responds with its DH public key encrypted by the temporary AES key.
3. **Session established:** Both sides compute a shared AES key from the DH exchange. All subsequent messages carry the `LYMSG_PARAM_ENC` flag and are encrypted/decrypted with AES-CFB using this session key.

`GateUser` handles per-connection handshake state, automatically decrypting inbound encrypted messages and encrypting outbound responses.

### Service Discovery

The central registry enables dynamic service lookup:

- **`CentralServer`** (cpp-httplib HTTP server) — Maintains a key-value store of server configurations. Servers register their JSON config on startup via `POST /register/api`. Clients fetch configs via `GET /api/:key`.
- **`CentralClient`** (aliased as `lygc::Central`) — Static libcurl-based client. Default endpoint is `http://localhost:8081`. Servers call `Central::get("config")` at startup to retrieve the cluster configuration.

### Server Roles

| Server | Binary | Role |
|--------|--------|------|
| **Central** | `lycentral` | Service registry — loads `lycentral-conf.json`, serves config via HTTP |
| **Gateway** | `lygate` | Entry point — RSA handshake + AES encryption, forwards to logic via `NetClient` |
| **Logic** | `lylogic` | Business logic — `NetServer` with handlers registered per origin |
| **Common** | `lycommon` | Shared service — `NetServer` with sync request handling, proxies to logic |
| **Gate Client** | `lygateclt` | Test client — performs handshake then sends encrypted requests |
| **Common Client** | `lycomasynclt` | Test client — async request/response test against common service |

Demo servers (`tcpserver`, `tcpcltpool`) demonstrate raw `tcpsock_server` and `tcpsock_cltpool` usage without the game server framework.

### Server Lifecycle

All servers follow the same lifecycle pattern:

```cpp
Server server(config);
server.open();          // Bind and listen
server.start(workers);  // Launch I/O worker threads + timer
server.serveUtilStop(); // Block until signal (SIGINT/SIGTERM)
server.close();         // Graceful shutdown
```

`start()` launches a configurable number of worker threads for I/O processing. `NetServer::start` also starts an optional async client manager (for outbound HA connections) and a 10-second timer wheel for periodic status reporting. Signal handlers call `stop()` which unblocks `serveUtilStop()`.

### Memory Tracking

Controlled by the `ZBF_TRACE_MEMORY` define (enabled by default). All allocations use `ZBF_MALLOC`/`ZBF_FREE` macros for per-file accounting. Key classes inherit from `object_tracker<T>` (CRTP) to count live instances. Call `logMemTrackStat()` to dump current allocation statistics — this is automatically called on `atexit` in all examples.

## Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL | RSA, DH, ECDH, DSA, AES, DES, Camellia, MD5, SHA, base64, hex |
| libcurl | HTTP client for service discovery |
| cpp-httplib | HTTP server for central registry |
| nlohmann-json | JSON config parsing |

## License

[MIT](LICENSE)
