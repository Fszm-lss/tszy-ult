#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
// Windows socket API uses char* instead of void*
#define RECV_BUF(buf) (char*)(buf)
#define SEND_BUF(buf) (const char*)(buf)
#define SOCK_OPT_VAL(val) (char*)&(val)
#define SOCK_OPT_CVAL(val) (const char*)&(val)
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#define CLOSE_SOCKET(fd) ::close(fd)
#define RECV_BUF(buf) (buf)
#define SEND_BUF(buf) (buf)
#define SOCK_OPT_VAL(val) &(val)
#define SOCK_OPT_CVAL(val) &(val)
#endif

#include <fcntl.h>
#include <ctime>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "log_utils.hpp"

namespace zbf {

#ifdef _WIN32
struct socket_init {
    socket_init() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
            LOG_ERR_MSG("WSAStartup fail, error=%d", WSAGetLastError());
        }
    }
    ~socket_init() {
        WSACleanup();
    }
};
inline socket_init _socket_init_;
#endif

#define EXIT_RC(rc, code)  { rc = code; goto exit; }
#define INVALID_SOCK       (-1)
// IS_ERR_AGAIN cross-platform error check:
// Linux: errno, checks EAGAIN/EINTR
// Windows: getSockErr() returns WSAGetLastError() instead of errno, checks WSAEWOULDBLOCK, EINTR not present
#ifdef _WIN32
#define IS_ERR_AGAIN(err)  ((err) == WSAEWOULDBLOCK)
inline int getSockErr() { return WSAGetLastError(); }
#define LOG_SOCK_ERR(err, fmt, args...) LOG_ERR_MSG(fmt ", WSA=%d", ##args, err)
#else
#define IS_ERR_AGAIN(err)  ((err) == EAGAIN || (err) == EINTR)
inline int getSockErr() { return errno; }
#define LOG_SOCK_ERR(err, fmt, args...) LOG_ERR_MSG(fmt ", error=%s(%d)", ##args, strerror(err), err)
#endif
#define LOG_LAST_ERR(fmt, args...) LOG_SOCK_ERR(getSockErr(), fmt, ##args)

typedef std::pair<std::string, unsigned short> HOST_AND_PORT;
typedef int  TimeUnitSec;
typedef long TimeUnitMillSec;

class socket_utils {

public:
    static int getSockAddrLen() {
		return (int) sizeof(sockaddr_storage);
	}

    // MTU(1500) - IP Header(20) - UDP Header(8)
    // max packet size: 65535-20-8 = 65507
    // return 65500
    static int getPacketMax() {
        return 65500;
	}


    static addrinfo* getAddrInfo(const char* hostname, unsigned short port, int tcp) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_PASSIVE;
        if (tcp) {
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
        } else {
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_protocol = IPPROTO_UDP;
        }

        char szPort[8] = {0};
        snprintf(szPort, sizeof(szPort), "%d", port);

        struct addrinfo *ai_list = nullptr;
        int rc = getaddrinfo(hostname, szPort, &hints, &ai_list);
        if (rc) {
            LOG_LAST_ERR("getaddrinfo fail(%s:%s)", hostname, szPort);
            return nullptr;
        }
        return ai_list;
    }

    static int parseAddr(const void* addrbuf, int addrlen, std::string& strIp, unsigned short& port) {
		if (addrbuf == nullptr || addrlen == 0) {
            LOG_ERR_MSG("parseAddr fail, param invalid");
			return -1;
		}

        const struct sockaddr* addr = (const sockaddr*) addrbuf;
        if (addr->sa_family == AF_INET) {
            char ipv4[INET_ADDRSTRLEN];
            const sockaddr_in* addr = (const sockaddr_in*) addrbuf;
            const char* p = inet_ntop(AF_INET, &addr->sin_addr, ipv4, sizeof(ipv4));
            if (p != nullptr) {
                port = ntohs(addr->sin_port);
                strIp = std::string(ipv4);
                return 0;
            }

        } else if (addr->sa_family == AF_INET6) {
            char ipv6[INET6_ADDRSTRLEN];
            const sockaddr_in6* addr = (const sockaddr_in6*) addrbuf;
            const char* p = inet_ntop(AF_INET6, &addr->sin6_addr, ipv6, sizeof(ipv6));
            if (p != nullptr) {
                port = ntohs(addr->sin6_port);
                strIp = std::string(ipv6);
                return 0;
            }
        }
        LOG_ERR_MSG("parseAddr fail, family not support");
		return -2;
	}

    static std::string mergeAddr(const char* addr, unsigned short port) {
        char szBuf[32] = {0};
        snprintf(szBuf, sizeof(szBuf), "%s:%d", addr, port);
        return std::string(szBuf);
    }

    static void set_nonblocking(int fd) {
#ifdef _WIN32
		unsigned long mode = 1;
		ioctlsocket(fd, FIONBIO, &mode);
#else
		int flag = fcntl(fd, F_GETFL, 0);
		if (-1 == flag) {
			return;
		}
		fcntl(fd, F_SETFL, flag | O_NONBLOCK);
#endif
	}

    static void set_blocking(int fd) {
#ifdef _WIN32
		unsigned long mode = 0;
		ioctlsocket(fd, FIONBIO, &mode);
#else
		int flag = fcntl(fd, F_GETFL, 0);
		if (-1 == flag) {
			return;
		}
		fcntl(fd, F_SETFL, flag & ~O_NONBLOCK);
#endif
	}

    // return -2 / -1 / 0 / > 0
    static int recvfrom(int fd, void* data, int size, void* addr, int* addrlen, bool logEAgain = true) {
		socklen_t socklen = *addrlen;
#ifdef _WIN32
		int rc = ::recvfrom(fd, RECV_BUF(data), size, 0, (sockaddr*)addr, &socklen);
#else
		int rc = ::recvfrom(fd, RECV_BUF(data), size, MSG_DONTWAIT, (sockaddr*)addr, &socklen);
#endif
		if (rc < 0) {
			int err = getSockErr();
			if (IS_ERR_AGAIN(err)) {
                if (logEAgain) LOG_SOCK_ERR(err, "recvfrom again, fd=%d, size=%d", fd, size);
                rc = -2;
			} else {
                LOG_SOCK_ERR(err, "recvfrom fail, fd=%d, size=%d", fd, size);
            }
		}
		*addrlen = socklen;
		return rc;
	}

    // return -2 / -1 / 0 / > 0
    static int sendto(int fd, const void* data, int size, const addrinfo* addr) {
		int rc = ::sendto(fd, SEND_BUF(data), size, 0, (const sockaddr*)addr->ai_addr, addr->ai_addrlen);
		if (rc < 0) {
			int err = getSockErr();
			if (IS_ERR_AGAIN(err)) {
                LOG_SOCK_ERR(err, "sendto again, fd=%d, size=%d", fd, size);
				return -2;
			} else {
                LOG_SOCK_ERR(err, "sendto fail, fd=%d, size=%d", fd, size);
            }
		}
		return rc;
	}

    // return -2 / -1 / 0 / > 0
    // recv no more than buf_szie
    static int recv(int fd, void* data, int size) {
		int rc = ::recv(fd, RECV_BUF(data), size, 0);
		if (rc < 0) { // -1
			int err = getSockErr();
			if (IS_ERR_AGAIN(err)) {
                LOG_SOCK_ERR(err, "recv again, fd=%d, size=%d", fd, size);
                rc = -2;
			} else {
                LOG_SOCK_ERR(err, "recv fail, fd=%d, size=%d", fd, size);
            }
		}
		return rc;
	}

    // return -2 / -1 / 0 / > 0
    // send no more than buf_szie
    static int send(int fd, const void* data, int size) {
		int rc = ::send(fd, SEND_BUF(data), size, 0);
		if (rc < 0) {
			int err = getSockErr();
			if (IS_ERR_AGAIN(err)) {
                LOG_SOCK_ERR(err, "send again, fd=%d, size=%d", fd, size);
                rc = -2;
			} else {
                LOG_SOCK_ERR(err, "send fail, fd=%d, size=%d", fd, size);
            }
		}
		return rc;
	}

    // return -1 / 0 / > 0
    // send data as mush as possible
    static int send_with_retry_(int fd, const void* data, int size, int max_retry) {
        int rc = size;
        int total_sent = 0;
        int rest = size;
        int offset = 0;
        unsigned char* buf = (unsigned char*) data;

        int retry = 0;
        do {
            int sent = ::send(fd, SEND_BUF(buf+offset), rest, 0);
            if (sent < 0) {
                int err = getSockErr();
                if (IS_ERR_AGAIN(err)) {
                    LOG_SOCK_ERR(err, "send again, fd=%d, total_sent=%d, rest=%d, retry=%d", fd, total_sent, rest, retry);
                    if (++retry >= max_retry) {
                        LOG_ERR_MSG("send unexpected, fd=%d, total_sent=%d, rest=%d, retry=%d", fd, total_sent, rest, retry);
                        rc = total_sent;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                } else {
                    LOG_SOCK_ERR(err, "send fail, fd=%d, total_sent=%d, rest=%d", fd, total_sent, rest);
                    rc = -1;
                    break;
                }
            } else if (sent == 0) {
                // peer close
                rc = 0;
                break;
            } else {
                total_sent += sent;
                rest -= sent;
                offset += sent;
                retry = 0; // reset retry
            }
        } while (total_sent < size);
		return rc;
	}

    // return -1 / 0 / > 0
    // recv data no more than buf_szie
    static int recv_with_retry_(int fd, void* buf, int buf_size, int max_retry) {
        int rc = 0;
        timespec tm_sleep;
        tm_sleep.tv_sec = 0;
        tm_sleep.tv_nsec = 1000000 * 20; // ms
        int retry = 0;

        do {
            int recvlen = ::recv(fd, RECV_BUF(buf), buf_size, 0);
            if (recvlen < 0) {
                int err = getSockErr();
                if (IS_ERR_AGAIN(err)) {
                    LOG_SOCK_ERR(err, "recv again, fd=%d, buf_size=%d, retry=%d", fd, buf_size, retry);
                    if (++retry >= max_retry) {
                        LOG_ERR_MSG("recv unexpected, fd=%d, buf_size=%d, retry=%d", fd, buf_size, retry);
                        rc = 0; // no data
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                } else {
                    LOG_SOCK_ERR(err, "recv fail, fd=%d, buf_size=%d", fd, buf_size);
                    rc = -1;
                    break;
                }
            } else if (recvlen == 0) {
                // peer close or no data
                rc = 0;
                break;
            } else {
                rc = recvlen;
                break; // success
            }
        } while (true);
		return rc;
	}

    // return -1 / 0 / > 0, block mode use
    static int send_with_retry(int fd, const void* data, int size, int max_retry = 10) {
        return send_with_retry_(fd, data, size, max_retry);
	}

    // return -1 / 0 / > 0, block mode use
    static int recv_with_retry(int fd, void* buf, int buf_size, int max_retry = 10) {
        return recv_with_retry_(fd, buf, buf_size, max_retry);
	}

    // Blocking read of size bytes until full, peer close, or error
    // return -2 peer close | -1 error | >= 0 success
    static int recv_all(int fd, void* buf, int size) {
        unsigned char* p = static_cast<unsigned char*>(buf);
        int left = size;

        timespec tm_sleep;
        tm_sleep.tv_sec = 0;
        tm_sleep.tv_nsec = 1000000 * 20; // ms
        int retry = 0;
        int max_retry = 50; // 1000 ms

        while (left > 0) {
            int n = ::recv(fd, RECV_BUF(p), left, 0);
            if (n == 0) {
                // Peer closed
                return -2;
            }
            if (n < 0) {
                int err = getSockErr();
                if (IS_ERR_AGAIN(err)) {
                    if (++retry >= max_retry) {
                        LOG_ERR_MSG("recv unexpected, fd=%d, buf_size=%d, retry=%d", fd, size, retry);
                        return size-left;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    // Interrupted or temporarily unreadable, retry
                    continue;
                }
                LOG_SOCK_ERR(err, "recv_all fail, fd=%d, expect=%d, left=%d", fd, size, left);
                return -1;
            }
            p    += n;
            left -= n;
        }
        return size-left;
    }

    static bool checkConnError(int rc, bool logErr = true) {
        if (rc == 0) return false;
        int err = getSockErr();
#ifdef _WIN32
        bool ret = (err != WSAEWOULDBLOCK);
#else
        bool ret = (err != EINPROGRESS && err != EINTR);
#endif
        if (logErr && ret) LOG_SOCK_ERR(err, "connect fail");
        return ret;
    }

    // status: 1 read, 2 write, 3 except
    static bool checkSockStat(int fd, int status, int timeout, bool logErr = true) {
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(fd, &fdset);

        timeval timer;
        timer.tv_sec = timeout/1000;
        timer.tv_usec = (timeout%1000)*1000;

        fd_set* rdSet = (status == 1) ? &fdset : 0;
        fd_set* wrSet = (status == 2) ? &fdset : 0;
        fd_set* exSet = (status == 3) ? &fdset : 0;

        int rc = select(fd+1, rdSet, wrSet, exSet, &timer);
        if (rc <= 0) {
            if (rc == 0) {
                if (logErr) LOG_ERR_MSG("select timeout(%d ms), fd=%d, status=%d", timeout, fd, status);
            } else {
                if (logErr) LOG_LAST_ERR("select fail, fd=%d, status=%d", fd, status);
            }
            return false;
        }

        if (FD_ISSET(fd, &fdset)) {
            return true;
        }
        return false;
    }

    static int checkSoError(int fd, bool logErr = true) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, SOCK_OPT_VAL(err), &len) < 0) {
            LOG_LAST_ERR("getsockopt(SO_ERROR) fail, fd=%d", fd);
            return -1;
        }
        if (err != 0) {
            if (logErr) LOG_SOCK_ERR(err, "checkSoError fail");
        }
        return err;
    }

    static int setReuseAddr(int fd) {
        int reuse = 1;
        socklen_t len = sizeof(reuse);
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, SOCK_OPT_CVAL(reuse), len) < 0) {
            LOG_LAST_ERR("setsockopt(SO_REUSEADDR) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Trace, "%s success, fd=%d, val=%d", __FUNCTION__, fd, reuse);
        return 0;
    }

    static int setReusePort(int fd) {
#ifdef _WIN32
        return 0;
#else
        int reuse = 1;
        socklen_t len = sizeof(reuse);
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, SOCK_OPT_CVAL(reuse), len) < 0) {
            LOG_LAST_ERR("setsockopt(SO_REUSEPORT) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Trace, "%s success, fd=%d, val=%d", __FUNCTION__, fd, reuse);
        return 0;
#endif
    }

    // allow IPv6 socket to accept v4 connections (dual-stack)
    // on Linux it's the default; on Windows IPV6_V6ONLY=1 by default, must opt in
    static int setDualstack(int fd, int family) {
        if (family == AF_INET6) {
            int no = 0;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, SOCK_OPT_CVAL(no), sizeof(no)) < 0) {
                LOG_LAST_ERR("setsockopt(IPV6_V6ONLY) fail, fd=%d", fd);
                return -1;
            }
        }
        return 0;
    }

    static int getRecvBufSize(int fd) {
        int recv_buf_size = 0;
        socklen_t len = sizeof(recv_buf_size);
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, SOCK_OPT_VAL(recv_buf_size), &len) < 0) {
            LOG_LAST_ERR("getsockopt(SO_RCVBUF) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Debug, "%s success, fd=%d, val=%d", __FUNCTION__, fd, recv_buf_size);
        return recv_buf_size;
    }

    static int setRecvBufSize(int fd, int recv_buf_size) {
        socklen_t len = sizeof(recv_buf_size);
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, SOCK_OPT_CVAL(recv_buf_size), len) < 0) {
            LOG_LAST_ERR("setsockopt(SO_RCVBUF) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Debug, "%s success, fd=%d, val=%d", __FUNCTION__, fd, recv_buf_size);
        return 0;
    }

    static int getSendBufSize(int fd) {
        int send_buf_size = 0;
        socklen_t len = sizeof(send_buf_size);
        if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, SOCK_OPT_VAL(send_buf_size), &len) < 0) {
            LOG_LAST_ERR("getsockopt(SO_SNDBUF) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Debug, "%s success, fd=%d, val=%d", __FUNCTION__, fd, send_buf_size);
        return send_buf_size;
    }

    static int setSendBufSize(int fd, int send_buf_size) {
        socklen_t len = sizeof(send_buf_size);
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, SOCK_OPT_CVAL(send_buf_size), len) < 0) {
            LOG_LAST_ERR("setsockopt(SO_SNDBUF) fail, fd=%d", fd);
            return -1;
        }
        LOG_MSG(LogLevel::Debug, "%s success, fd=%d, val=%d", __FUNCTION__, fd, send_buf_size);
        return 0;
    }

    // error return < 0  | success return > 0
    static int create_udp_socket(const char* serverName, unsigned short port, bool& isV4) {
        int rc = 0;
        int fd = INVALID_SOCK;
        isV4 = false;
        struct addrinfo* svr_addr = nullptr;
        std::string addrAndPort = socket_utils::mergeAddr(serverName, port);

        svr_addr = socket_utils::getAddrInfo(serverName, port, 0);
        if (!svr_addr) {
            EXIT_RC(rc, -1);
        }

        fd = ::socket(svr_addr->ai_family, svr_addr->ai_socktype, svr_addr->ai_protocol);
        if (fd < 0) {
            LOG_LAST_ERR("socket fail(%s)", addrAndPort.c_str());
            EXIT_RC(rc, -2);
        }

        if (socket_utils::setReuseAddr(fd)) {
            EXIT_RC(rc, -3);
        }

        if (socket_utils::setReusePort(fd)) {
            EXIT_RC(rc, -3);
        }

        socket_utils::setDualstack(fd, svr_addr->ai_family);

        if (::bind(fd, (struct sockaddr*)svr_addr->ai_addr, svr_addr->ai_addrlen)) {
            LOG_LAST_ERR("bind fail(%s)", addrAndPort.c_str());
            EXIT_RC(rc, -4);
        }
        
        rc = fd;
        isV4 = (svr_addr->ai_family == AF_INET);
        LOG_MSG(LogLevel::Debug, "%s success(%s), fd=%d, ipv%d", __FUNCTION__, addrAndPort.c_str(), fd, (isV4?4:6));
    exit:
        if (rc < 0) {
            if (fd != INVALID_SOCK) CLOSE_SOCKET(fd);
            LOG_MSG(LogLevel::Warn, "%s fail(%s), rc=%d", __FUNCTION__, addrAndPort.c_str(), rc);
        }
        if (svr_addr) freeaddrinfo(svr_addr);
        return rc;
    }
    
    static long currentTimeSecs() {
		struct timespec ts;
		int rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		if (rc) { // fail
			return TimeStampErr;
		}
		long ms = ts.tv_sec;
		return ms;
	}

	static long currentTimeMillis() {
		struct timespec ts;
		int rc = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		if (rc) { // fail
			return TimeStampErr;
		}
		long ms = ts.tv_sec*1000L + ts.tv_nsec/1000000L;
		return ms;
	}

    enum { TimeStampErr = 0, };
};

}
