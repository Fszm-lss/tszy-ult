#pragma once

#include <cstdio>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <list>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "socket_poll.hpp"
#include "socket_utils.hpp"
#include "mem_alloc.hpp"
#include "heartbeat_helper.hpp"
#include "safequeue.hpp"
#include "tcp_message.hpp"
#include "ikcp.h"
#include "request_stat.hpp"


namespace zbf {

// forward declaration
class kcp_user;

struct KcpMsgPtr {
    std::unique_ptr<tcp_message> msg;
    std::weak_ptr<kcp_user>      user;

    KcpMsgPtr() = default;
    KcpMsgPtr(tcp_message* m, std::shared_ptr<kcp_user> u) : msg(m), user(u) {}

    tcp_message* get() const { return msg.get(); }
    explicit operator bool() const { return msg != nullptr; }
};

enum kcp_mode { KcpModeNormal = 1, KcpModeRapid = 2 };

class kcp_user : public std::enable_shared_from_this<kcp_user>, public object_tracker<kcp_user> {
public:
    kcp_user(uint32_t ident, int fd, addrinfo* peer, unsigned short peerPort, bool serverSide, tcp_message_protocol* protocol)
    : conv(ident), fd(fd), peerPort(peerPort), _protocol(protocol), _peer(peer),
      _serverSide(serverSide), _hbhelper(serverSide ? UserTimeout : HeartbeatTime), _startupTime(0) {
        _kcp = ikcp_create(conv, (void*)this);
        ikcp_setoutput(_kcp, kcp_user::udp_output);
        _hbhelper.update();

        char szDesc[32] = {0};
        snprintf(szDesc, sizeof(szDesc), "conv:%u|fd:%d", conv, fd);
        _desc = szDesc;
    }

    ~kcp_user() {
        ikcp_release(_kcp);
        if (fd != INVALID_SOCK) {
            CLOSE_SOCKET(fd);
        }
        if (_peer) {
            freeaddrinfo(_peer);
        }
    }

    void setMode(kcp_mode mode) {
        if (mode == KcpModeNormal) {
            ikcp_nodelay(_kcp, 0, 40, 0, 0);
        } else {
            ikcp_nodelay(_kcp, 1, 10, 2, 1);
            ikcp_wndsize(_kcp, 128, 128); // default 32
            _kcp->rx_minrto = 10;         // default 100
        }
    }

    int post(std::unique_ptr<tcp_message> msg) {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        if (_wrQueue.size() > MaxPendingMsg) {
            LOG_ERR_MSG("send fail, queue full(%d)", _wrQueue.size());
            return 1;
        }
        _wrQueue.push_back(std::move(msg));
        return 0;
    }

public:
    int onInput(const unsigned char* data, int len) {
        if (isClosed()) return -1;
        std::lock_guard<std::mutex> lock(_lockKcp);
        int rc = ikcp_input(_kcp, (const char*)data, len);
        if (rc >= 0) {
            _nextUpdateMs = 0;  // notify writer: new data needs flush
        } else {
            LOG_ERR_MSG("ikcp_input(%s) fail, rc=%d", desc().c_str(), rc);
        }
        return rc;
    }

    tcp_message* onRecv() {
        if (isClosed()) return nullptr;
        std::lock_guard<std::mutex> lock(_lockKcp);
        int size = ikcp_peeksize(_kcp);
        if (size < 0) return nullptr;

        tcp_message* msg = new tcp_message();
        msg->data.resize(size);
        int rc = ikcp_recv(_kcp, &msg->data[0], size);
        if (rc < 0) {
            LOG_MSG(LogLevel::Debug, "ikcp_recv(%s) return, rc=%d", desc().c_str(), rc);
            delete msg;
            return nullptr;
        }

        if (_serverSide) {
            _hbhelper.update();
            if (_protocol->isHeartbeat(msg)) {
                LOG_MSG(LogLevel::TraceMore, "ikcp_recv(%s) got heartbeat", desc().c_str());
                delete msg;
                return nullptr;
            }
        }
        return msg;
    }

    void onUpdate(TimeUnitMillSec now) {
        if (isClosed()) return;
        std::lock_guard<std::mutex> lock(_lockKcp);
        if (_startupTime == 0) _startupTime = now;
        IUINT32 tick = (IUINT32)(now - _startupTime);
        ikcp_update(_kcp, tick);
        IUINT32 nextTick = ikcp_check(_kcp, tick);
        if (nextTick > tick) {
            _nextUpdateMs = now + (TimeUnitMillSec)(nextTick - tick);
        } else {
            _nextUpdateMs = 0;
        }
    }

    bool needsKcpUpdate(TimeUnitMillSec now) {
        return _nextUpdateMs == 0 || now >= _nextUpdateMs;
    }

    bool hasPendingSend() {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        return !_wrQueue.empty();
    }

    // 0 success | < 0 fail
    int send() {
        if (isClosed()) return -3;
        std::unique_ptr<tcp_message> msg;
        {
            std::lock_guard<std::mutex> lock(_lockWRQ);
            if (!_wrQueue.empty()) {
                msg = std::move(_wrQueue.front());
                _wrQueue.pop_front();
            }
        }
        if (!msg) return -1;

        int rc = 0;
        {
            std::lock_guard<std::mutex> lock(_lockKcp);
            rc = ikcp_send(_kcp, msg->data.data(), (int)msg->data.size());
        }

        if (rc < 0) {
            LOG_ERR_MSG("ikcp_send(%s) fail, rc=%d", desc().c_str(), rc);
            rc = -2;
        } else {
            if (!_serverSide) {
                _hbhelper.update();
            }
        }
        return rc;
    }

    static int udp_output(const char* buf, int len, ikcpcb* kcp, void* user) {
        kcp_user* kuser = (kcp_user*) user;
        // when callback, caller does not check rc
        return socket_utils::sendto(kuser->fd, buf, len, kuser->_peer);
    }

    std::string desc() const {
        return _desc;
    }

    // for server side
    bool isClosed() {
        return _closed.load(std::memory_order_relaxed);
    }
    void setClose() {
        _closed.store(true, std::memory_order_relaxed);
    }

    // for server side
    bool isExpired(TimeUnitSec now) {
        return _hbhelper.isExceed(now);
    }

    // for client side
    bool needHeartbeat(TimeUnitSec now) {
        return _hbhelper.isExceed(now);
    }
    void sendHeartbeat() {
        auto hb = std::unique_ptr<tcp_message>(_protocol->genHeartbeat());
        if (!hb) return;
        int rc = post(std::move(hb));
        // if rc != 0, hb already moved and will be cleaned up
    }

    tcp_message_protocol* getProtocol() const { return _protocol; }

public:
    IUINT32        conv;
    int            fd;
    unsigned short peerPort;
    enum { UserTimeout = 90, HeartbeatTime = 30, MaxPendingMsg = 256 };
private:
    tcp_message_protocol* _protocol{nullptr};  // non-owning ref
    ikcpcb*                          _kcp;
    std::mutex                       _lockKcp;
    addrinfo*                        _peer;
    std::list<std::unique_ptr<tcp_message>> _wrQueue;
    std::mutex                       _lockWRQ;
    std::atomic<bool>                _closed{false};
    std::string                      _desc;
    bool                             _serverSide;
    heartbeat_helper                 _hbhelper;
    TimeUnitMillSec                  _startupTime;
    // data race on reader/writer: benign because aligned 64-bit write is atomic on x86;
    // worst case is one-cycle misjudgment, self-corrects next cycle.
    // TODO: change to std::atomic if tsan warns.
    TimeUnitMillSec                  _nextUpdateMs{0};
};


class kcpsock_listener {
public:
	virtual void onRecvMsg(std::shared_ptr<kcp_user> user, const tcp_message* msg) = 0;
    virtual void onUserDisconnect(std::shared_ptr<kcp_user> user) {};
};

class kcpsock_server {
public:
    kcpsock_server(kcpsock_listener* listener, tcp_message_protocol* protocol, kcp_mode mode = KcpModeNormal)
    : _shutdown(false), _listener(listener), _protocol(protocol), _mode(mode) {
        _efd = socket_poll::sp_create();
        if (socket_poll::sp_invalid(_efd)) {
            LOG_LAST_ERR("sp_create fail");
        }
    }

    virtual ~kcpsock_server() {
        if (!socket_poll::sp_invalid(_efd)) {
			socket_poll::sp_release(_efd);
			_efd = INVALID_SOCK;
		}

        {
            std::lock_guard<std::mutex> lock(_lockUsers);
            LOG_MSG(LogLevel::Trace, "%s close, clear pending users, count=%d", desc().c_str(), _ident2User.size());
            _ident2User.clear();
            _fd2User.clear();
        }
        {
            KcpMsgPtr pair;
            while (_rdQueue.raw_pop(pair)) {
                // unique_ptr auto-releases
            }
        }
    }

    int start(int workerNum = 4) {
        _reader = std::thread([&](){
            reader(ThdIdBase+1);
		});
        _writer = std::thread([&](){
            writer(ThdIdBase+2);
		});

        workerNum = std::max(workerNum, 1);
        workerNum = std::min(workerNum, 32);
        for (int i = 1; i <= workerNum; ++i) {
            short thdId = ThdIdBase+10+i; // max:ThdIdBase+10+32
            std::thread* thd = new std::thread([&, thdId](){
                worker(thdId);
            });
            _workers.push_back(thd);
        }
        LOG_MSG(LogLevel::Debug, "%s kcp threads start, workers=%d", desc().c_str(), workerNum);
        _statLogger.start(ThdIdBase+3);
        return 0;
    }

    void serveUtilStop() {
        if (_reader.joinable())
            _reader.join();
        if (_writer.joinable())
            _writer.join();
        _statLogger.stop();
        for (int i = 0; i < _workers.size(); ++i) {
            std::thread* thd = _workers[i];
            if (thd->joinable())
                thd->join();
            delete thd;
        }
        _workers.clear();
        LOG_MSG(LogLevel::Debug, "%s udp threads stopped", desc().c_str());
    }

    void stop(bool join = false) {
        _shutdown.store(true, std::memory_order_release);
        _rdQueue.stop();
        if (join) serveUtilStop();
	}

    int addUser(uint32_t ident, const std::string& localAddr, unsigned short localPort, const std::string& peerAddr, unsigned short peerPort) {
        bool isV4 = false;
        int fd = socket_utils::create_udp_socket(localAddr.c_str(), localPort, isV4);
        if (fd < 0) return -1;

        addrinfo* peer = socket_utils::getAddrInfo(peerAddr.c_str(), peerPort, 0);
        if (!peer) {
            CLOSE_SOCKET(fd);
            return -2;
        }

        socket_utils::set_nonblocking(fd);
        auto user = std::make_shared<kcp_user>(ident, fd, peer, peerPort, true, _protocol.get());
        user->setMode(_mode);
        if (socket_poll::sp_add(_efd, fd)) {
            LOG_LAST_ERR("sp_add fail, fd=%d", fd);
            return -3;
        }

        std::lock_guard<std::mutex> lock(_lockUsers);
        _ident2User.insert(std::make_pair(ident, user));
        _fd2User.insert(std::make_pair(fd, user));
        LOG_MSG(LogLevel::Debug, "%s addUser(%s)", desc().c_str(), user->desc().c_str());
        return 0;
    }

    void kickUser(uint32_t ident) {
        auto user = findUserByIdent(ident);
        if (user) {
            closeUser(user->fd, "kick user");
        } else {
            LOG_ERR_MSG("kickUser findUserByIdent() fail, ident=%d", ident);
        }
    }

private:
    bool isShutDown() {
        return _shutdown.load(std::memory_order_acquire);
	}

    std::shared_ptr<kcp_user> findUserByFd(int fd) {
        std::lock_guard<std::mutex> lock(_lockUsers);
        auto it = _fd2User.find(fd);
        if (it != _fd2User.end()) return it->second.lock();
        return nullptr;
    }

    std::shared_ptr<kcp_user> findUserByIdent(uint32_t ident) {
        std::lock_guard<std::mutex> lock(_lockUsers);
        auto it = _ident2User.find(ident);
        if (it != _ident2User.end()) return it->second;
        return nullptr;
    }

    // close user, remove it from _fd2User and _efd
    int closeUser(int fd, const char* tag) {
        int rc = -1;
        std::lock_guard<std::mutex> lock(_lockUsers);
        auto it = _fd2User.find(fd);
        if (it != _fd2User.end()) {
            auto user = it->second.lock();
            if (user && !user->isClosed()) {
                user->setClose();
                socket_poll::sp_del(_efd, user->fd);
                _fd2User.erase(it);
                rc = 0;
                LOG_MSG(LogLevel::Warn, "%s(%s), tag=(%s), rc=%d", __FUNCTION__, user->desc().c_str(), tag, rc);
            } else if (user) {
                rc = 1;
                LOG_MSG(LogLevel::Warn, "%s(%s), tag=(%s), rc=%d", __FUNCTION__, user->desc().c_str(), tag, rc);
            } else {
                _fd2User.erase(it);
                rc = 1;
            }
        } else {
            rc = -1;
            LOG_MSG(LogLevel::Warn, "%s(fd=%d), tag=(%s), rc=%d", __FUNCTION__, fd, tag, rc);
        }
        return rc;
    }

    int onReaderError(int fd, const char* tag) {
        return closeUser(fd, tag);
    }

    int reader(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s reader thread(%d) start", desc().c_str(), thdId);
        struct event ev[MAX_EVENT];
        int recvBufSize = (socket_utils::getPacketMax()  + 7)/8 * 8;
        int addrBufSize = (socket_utils::getSockAddrLen() + 7)/8 * 8;
        unsigned char* recvBuf = (unsigned char*) ZBF_MALLOC(recvBufSize);
        unsigned char* addrBuf = (unsigned char*) ZBF_MALLOC(addrBufSize);
        LOG_MSG(LogLevel::Debug, "%s reader thread, recvBufSize=%d, addrBufSize=%d", desc().c_str(), recvBufSize, addrBufSize);

        for (;;) {
            if (isShutDown()) break;

            int n = socket_poll::sp_wait(_efd, ev, MAX_EVENT, EpollWait); // ms
            if (n < 0) {
                LOG_LAST_ERR("sp_wait fail, efd=%d", _efd);
                break;
            }

            for (int i = 0; i < n; ++i) {
                if (isShutDown()) break;
                
                int fd = ev[i].fd;
                auto user = findUserByFd(fd);
                if (!user || user->isClosed()) {
                    continue;
                }

                if (ev[i].read) {
                    int addrSize = addrBufSize;
                    // memset(addrBuf, 0, addrBufSize);
                    int recvLen = socket_utils::recvfrom(fd, recvBuf, recvBufSize, addrBuf, &addrSize);
                    if (recvLen > 0) {
                        user->onInput(recvBuf, recvLen);
                        tcp_message* msg = nullptr;
                        while ((msg = user->onRecv())) {
                            if (!_rdQueue.push(KcpMsgPtr(msg, user))) {
                                delete msg;
                                LOG_ERR_MSG("push fail, queue is full(%d)", _rdQueue.size());
                            }
                        }
                    } else if (recvLen == -1) {
                        onReaderError(fd, "recvfrom error");
                    } else {
                        // recvLen == 0/-2
                        // do nothing
                    }
                } else if (ev[i].error || ev[i].eof) {
                    onReaderError(fd, "err or eof");
                }
            }
        }

        ZBF_FREE(recvBuf);
        ZBF_FREE(addrBuf);
        LOG_MSG(LogLevel::Debug, "%s reader thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    int writer(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s writer thread(%d) start", desc().c_str(), thdId);
        TimeUnitMillSec ThreadWait = (_mode == KcpModeNormal) ? ThreadIdle : 10;
        std::vector<std::shared_ptr<kcp_user>> userList;

        for (;;) {
            if (isShutDown()) break;
            TimeUnitMillSec loopStart = socket_utils::currentTimeMillis();            

            userList.clear();
            {
                std::lock_guard<std::mutex> lock(_lockUsers);
                userList.reserve(_ident2User.size());
                for (const auto& [k, v] : _ident2User) userList.push_back(v);
            }

            TimeUnitMillSec loopNow = socket_utils::currentTimeMillis();
            for (auto& user : userList) {
                if (isShutDown()) break;

                if (user->isExpired((TimeUnitSec)(loopNow/1000))) {
                    LOG_MSG(LogLevel::Debug, "%s user(%s) expired, close it", desc().c_str(), user->desc().c_str());
                    closeUser(user->fd, "user expired");
                }
                if (user->isClosed()) {
                    _listener->onUserDisconnect(user);
                    {
                        std::lock_guard<std::mutex> lock(_lockUsers);
                        _ident2User.erase(user->conv);
                    }
                } else {
                    if (user->hasPendingSend() || user->needsKcpUpdate(loopNow)) {
                        user->send();
                        user->onUpdate(loopNow);
                    }
                }
            }

            TimeUnitMillSec loopCost = socket_utils::currentTimeMillis() - loopStart;
            if (loopCost < ThreadWait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ThreadWait-loopCost));
            } else {
                LOG_MSG(LogLevel::TraceMore, "%s worker thread(%d), loopCost=%ld", desc().c_str(), thdId, loopCost);
            }
        }
        LOG_MSG(LogLevel::Debug, "%s writer thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    int worker(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);
        RequestStat reqStat;
        _statLogger.add(&reqStat);

        for (;;) {
            KcpMsgPtr pair;
            int rc = _rdQueue.pop_timeout(pair, 100);
            if (rc == zbf::SQ_POP_SUCCESS) {
                auto user = pair.user.lock();
                tcp_message* msg = pair.get();
                if (!user) continue;
                if (!user->isClosed()) {
                    uint32_t msgType = _protocol->type(msg);
                    reqStat.onReqStart(msgType);
                    _listener->onRecvMsg(user, msg);
                    reqStat.onReqFinish(msgType);
                }
                std::this_thread::yield();
            } else if (rc == zbf::SQ_EXIT) {
                break;
            }
        }

        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    std::string desc() const {
        return std::string("kcpsock_server");
    }    

private:
    int _efd;

    std::unordered_map<uint32_t, std::shared_ptr<kcp_user>> _ident2User; // holder
    std::unordered_map<int, std::weak_ptr<kcp_user>>        _fd2User;    // ref
    std::mutex _lockUsers;

    std::atomic<bool> _shutdown;
    std::thread _reader;
    std::thread _writer;
    std::vector<std::thread*> _workers;

    SafeQueue<KcpMsgPtr, 256> _rdQueue; // holder
    std::unique_ptr<kcpsock_listener> _listener;
    std::unique_ptr<tcp_message_protocol> _protocol;
    kcp_mode _mode;
    ReqStatLogger _statLogger{5, zbf::Minute};

    enum { EpollWait = 50, ThreadIdle = 20, ThdIdBase = 150 };
};


class kcpsock_client {
public:
    kcpsock_client(kcpsock_listener* listener, tcp_message_protocol* protocol, kcp_mode mode = KcpModeNormal)
    : _shutdown(false), _listener(listener), _protocol(protocol), _mode(mode) {
    }

    virtual ~kcpsock_client() {
    }

    int open(uint32_t ident, const std::string& localAddr, unsigned short localPort, const std::string& serverAddr, unsigned short serverPort) {
        bool isV4 = false;
        int fd = socket_utils::create_udp_socket(localAddr.c_str(), localPort, isV4);
        if (fd < 0) return -1;

        addrinfo* peer = socket_utils::getAddrInfo(serverAddr.c_str(), serverPort, 0);
        if (!peer) {
            CLOSE_SOCKET(fd);
            return -2;
        }

        socket_utils::set_nonblocking(fd);
        _user = std::make_shared<kcp_user>(ident, fd, peer, serverPort, false, _protocol.get());
        _user->setMode(_mode);
        return 0;
    }

    void close() {
        _user.reset();
    }

    int start(short thdId = 1001) {
        if (!_user) return -1;
        _worker = std::thread([&, thdId](){
            worker(thdId);
		});
        return 0;
    }

    void stop() {
        _shutdown.store(true, std::memory_order_release);
        if (_worker.joinable()) _worker.join();
    }

    int post(std::unique_ptr<tcp_message> msg) {
        if (!_user) return -1;
        return _user->post(std::move(msg));
    }

private:
    int worker(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);

        int recvBufSize = (socket_utils::getPacketMax() + 7)/8 * 8;
        int addrBufSize = (socket_utils::getSockAddrLen() + 7)/8 * 8;
        unsigned char* recvBuf = (unsigned char*) ZBF_MALLOC(recvBufSize);
        unsigned char* addrBuf = (unsigned char*) ZBF_MALLOC(addrBufSize);

        TimeUnitMillSec ThreadWait = (_mode == KcpModeNormal) ? ThreadIdle : 10;

        for (;;) {
            if (isShutDown()) break;
            
            TimeUnitMillSec loopStart = socket_utils::currentTimeMillis();

            // skip onUpdate + send when idle
            if (_user->hasPendingSend() || _user->needsKcpUpdate(loopStart)) {
                _user->onUpdate(loopStart);
                _user->send();
            } else if (_user->needHeartbeat((TimeUnitSec)(loopStart/1000))) {
                _user->sendHeartbeat();
            }
            
            // receive
            int addrSize = addrBufSize;
            int recvLen = socket_utils::recvfrom(_user->fd, recvBuf, recvBufSize, addrBuf, &addrSize, false);
            if (recvLen > 0) {
                _user->onInput(recvBuf, recvLen);
                tcp_message* msg = nullptr;
                while ((msg = _user->onRecv())) {
                    _listener->onRecvMsg(_user, msg);
                    delete msg;
                }
                // flush ACK immediately after receiving data
                _user->onUpdate(loopStart);
            } else if (recvLen == -1) {
                LOG_ERR_MSG("%s recvfrom fail, user=(%s), break worker", desc().c_str(), _user->desc().c_str());
                break;
            } else {
                // recvLen == 0 or -2 (EAGAIN)
                // do nothing
            }

            TimeUnitMillSec loopCost = socket_utils::currentTimeMillis() - loopStart;
            if (loopCost < ThreadWait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ThreadWait-loopCost));
            } else {
                LOG_MSG(LogLevel::TraceMore, "%s worker thread(%d), loopCost=%ld", desc().c_str(), thdId, loopCost);
            }
        }

        ZBF_FREE(recvBuf);
        ZBF_FREE(addrBuf);
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    bool isShutDown() {
        return _shutdown.load(std::memory_order_acquire);
	}

    std::string desc() const {
        return std::string("kcpsock_client");
    }

private:
    std::shared_ptr<kcp_user> _user;
    std::atomic<bool> _shutdown;
    std::thread _worker;
    std::unique_ptr<kcpsock_listener> _listener;
    std::unique_ptr<tcp_message_protocol> _protocol;
    kcp_mode _mode;

    enum { ThreadIdle = 20 };
};

}
