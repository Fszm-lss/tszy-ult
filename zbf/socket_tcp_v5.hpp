#ifndef socket_tcp_hpp
#define socket_tcp_hpp

#include <cstdio>
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <memory>
#include "memchunk.hpp"
#include "socket_poll.hpp"
#include "socket_utils.hpp"
#include "tcp_message.hpp"
#include "heartbeat_helper.hpp"
#include "request_stat.hpp"
#include "safequeue.hpp"

namespace zbf {

class tcpsock_user;
struct TcpMsgPtr {
    std::unique_ptr<tcp_message> msg;
    std::weak_ptr<tcpsock_user> user;

    TcpMsgPtr() = default;
    explicit TcpMsgPtr(tcp_message* p) : msg(p) {}
    TcpMsgPtr(tcp_message* p, std::weak_ptr<tcpsock_user> u) : msg(p), user(std::move(u)) {}

    tcp_message* get() const { return msg.get(); }
    explicit operator bool() const { return msg != nullptr; }

    tcp_message* release() {
        user.reset();
        return msg.release();
    }
};

enum { SEND_SUCCESS = 0, SEND_FAIL = 1, SOCK_CLOSED = 2, QUEUE_EMPTY = 3, RECV_FAIL = 4, SEND_TIMEOUT = 5, POOL_INVALID = 6 };
inline int _SendMsg(int fd, const tcp_message* msg);
inline int _SendData(int fd, std::list<TcpMsgPtr>& queue, std::mutex& lockQueue);
inline tcp_message* _PopMsg(memchunk* recvCache, tcp_message_protocol* protocol);

////////////////////////////////////////////////////////////////////////////////////////////////////
// server side
////////////////////////////////////////////////////////////////////////////////////////////////////

class tcpsock_server;
class tcpsock_user : public std::enable_shared_from_this<tcpsock_user>, object_tracker<tcpsock_user> {
    friend class tcpsock_server;
public:
    tcpsock_user() : _hbhelper(UserTimeout) {
        _recvCache = new memchunk(BufferSize);
    }

    virtual ~tcpsock_user() {
        uninit();
        delete _recvCache;
    }

    int init(const char* addr, unsigned short port, int fd, unsigned long long seq = 0ULL) {
        _peerAddr = addr;
        _peerPort = port;
        _fd = fd;
        _seq = seq;
        _hbhelper.update();
        LOG_MSG(LogLevel::Debug, "user(%s) init", desc(false).c_str());
        return 0;
    }    
    
    void setClose() {
        std::lock_guard<std::mutex> lock(_lockState);
        _closed = true;
    }

    bool isClosed() {
        std::lock_guard<std::mutex> lock(_lockState);
        return _closed;
    }

    void setStart() {
        std::lock_guard<std::mutex> lock(_lockState);
        _started = true;
    }

    bool isStarted() {
        std::lock_guard<std::mutex> lock(_lockState);
        return _started;
    }

    // if fail, caller need delete msg
    int post(tcp_message* msg) {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        if (_wrQueue.size() > MaxPendingMsg) {
            LOG_ERR_MSG("send fail, queue full(%d)", _wrQueue.size());
            return 1;
        }
        _wrQueue.emplace_back(TcpMsgPtr(msg));
        return 0;
    }
    
    bool isQueueEmpty() {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        return _wrQueue.empty();
    }

    std::string desc(bool brief = true) const {
        char szDesc[64] = {0};
        if (brief)
            snprintf(szDesc, sizeof(szDesc), "seq:%llu|fd:%d", _seq, _fd);
        else
            snprintf(szDesc, sizeof(szDesc), "seq:%llu|fd:%d|peer=%s|port:%u", _seq, _fd, _peerAddr.c_str(), _peerPort);
        return std::string(szDesc);
    }

private:
    int sendData() {
        if (isClosed()) return SOCK_CLOSED;
        return zbf::_SendData(_fd, _wrQueue, _lockWRQ);
    }

    int onRecv(memchunk* chunk) {
        if (isClosed()) return 1;
        std::lock_guard<std::mutex> lock(_lockRC);
        int rc = _recvCache->push(chunk->buffer, chunk->datasize);
        if (rc < 0) {
            LOG_ERR_MSG("onRecv(%s), push buffer fail, rc=%d", desc(false).c_str(), rc);
            return 2;
        }
        if (_recvCache->datasize > MaxPendingSize) {
            LOG_ERR_MSG("onRecv(%s), pending data is too long: %d", desc(false).c_str(), _recvCache->datasize);
            return 3;
        }
        return 0;
    }

    tcp_message* getRecvMsg() {
        if (isClosed()) return nullptr;
        std::lock_guard<std::mutex> lock(_lockRC);
        tcp_message* msg = zbf::_PopMsg(_recvCache, _protocol);
        if (msg) {
            _hbhelper.update();
            markMsgReceived(msg);
        }
        return msg;
    }

    void markMsgReceived(tcp_message* msg) {
        std::lock_guard<std::mutex> lock(_lockRDC);
        _rdColl.insert(msg);
    }

    void markMsgHandled(tcp_message* msg) {
        std::lock_guard<std::mutex> lock(_lockRDC);
        _rdColl.erase(msg);
    }

    bool checkAllMsgHandled() {
        std::lock_guard<std::mutex> lock(_lockRDC);
        return _rdColl.empty();
    }

    bool isExpired(TimeUnitSec now) {
        return _hbhelper.isExceed(now);
    }

    void enableWrite(int efd, bool enable) {
        std::lock_guard<std::mutex> lock(_lockEW);
        if (_enableWrite != enable) {
            _enableWrite = enable;
            socket_poll::sp_enable(efd, _fd, true, _enableWrite);
        }
    }

    void uninit() {
        if (_fd != INVALID_SOCK) {
            CLOSE_SOCKET(_fd);
            _fd = INVALID_SOCK;
        }
        std::lock_guard<std::mutex> lockQueue(_lockWRQ);
        LOG_MSG(LogLevel::Trace, "user(%s) uninit, clear rest msgs:%d", desc(false).c_str(), _wrQueue.size());
        _wrQueue.clear();
        LOG_MSG(LogLevel::Debug, "user(%s) uninit finish", desc(false).c_str());
    }

public:
    virtual void onConnect() = 0;
    virtual void onRecvMsg(const tcp_message* msg) = 0;
    virtual void onDisconnect() = 0;

    void setProtocol(tcp_message_protocol* protocol) { _protocol = protocol; }
    tcp_message_protocol* getProtocol() { return _protocol; }

private:
    int _fd{INVALID_SOCK};
    unsigned long long _seq{0ULL};
    std::string _peerAddr;
    unsigned short _peerPort{0};
    memchunk* _recvCache{nullptr};
    std::mutex _lockRC;
    std::list<TcpMsgPtr> _wrQueue;
    std::mutex _lockWRQ;
    std::unordered_set<tcp_message*> _rdColl;
    std::mutex _lockRDC;
    bool _closed{false};
    bool _started{false};
    std::mutex _lockState;
    heartbeat_helper _hbhelper;
    bool _enableWrite{false};
    std::mutex _lockEW;
    tcp_message_protocol* _protocol{nullptr}; // ref
    enum { BufferSize = 1024, MaxPendingSize = 10485760 /*10M*/, MaxPendingMsg = 256, UserTimeout = 90 };
};

struct message_swimlane {
    std::vector<uint32_t> msgTypes;
    size_t threads;
    message_swimlane() : threads(0) {}
};
typedef std::vector<message_swimlane> message_swimlanes;

struct WorkerIdGroup {
    std::vector<short> workerIds;
    size_t cursor{0};
};

class tcpsock_server {
public:
    enum { MaxPendingMsg = 256 };
    typedef std::unique_ptr<SafeQueue<TcpMsgPtr, MaxPendingMsg> > SafeQueuePtr;
public:
    // protocol: caller construct buf owner(this class) destruct
    tcpsock_server(tcp_message_protocol* protocol) : _statLogger(StatInterval, zbf::Minute) {
        _protocol = protocol;
        _efd = INVALID_SOCK;
        _shutdown = false;
        _maxConnections = DefaultMaxConns;
    }

    virtual ~tcpsock_server() {
        if (_protocol) delete _protocol;
    }

    int open(std::vector<HOST_AND_PORT >& addr_and_ports) {
        int cnt = 0;
        for (const auto& addr_port : addr_and_ports) {
            std::string addr = addr_port.first;
            unsigned short port = addr_port.second;
            if (!open(addr.c_str(), port)) {
                cnt += 1;
            }
        }
        return cnt;
    }

    int open(const char* serverName, std::vector<unsigned short>& ports) {
        int cnt = 0;
        for (const auto& port : ports) {
            if (!open(serverName, port)) {
                ++cnt;
            }
        }
        return cnt;
    }

    // 1. open(); 2. start()
    int open(const char* serverName, unsigned short port) {
        int rc = 0;
        int fd = INVALID_SOCK;
        struct addrinfo* svr_addr = nullptr;
        std::string addrAndPort = socket_utils::mergeAddr(serverName, port);

        svr_addr = socket_utils::getAddrInfo(serverName, port, 1);
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

        socket_utils::setDualstack(fd, svr_addr->ai_family);

        if (::bind(fd, (struct sockaddr*)svr_addr->ai_addr, svr_addr->ai_addrlen)) {
            LOG_LAST_ERR("bind fail(%s)", addrAndPort.c_str());
            EXIT_RC(rc, -4);
        }
        socket_utils::set_nonblocking(fd);

        if (::listen(fd, BackLog)) {
            LOG_LAST_ERR("listen fail(%s)", addrAndPort.c_str());
            EXIT_RC(rc, -5);
        }

        if (_efd == INVALID_SOCK) {
            _efd = socket_poll::sp_create();
            if (socket_poll::sp_invalid(_efd)) {
                LOG_LAST_ERR("sp_create fail(%s)", addrAndPort.c_str());
                EXIT_RC(rc, -6);
            }
        }
        
		if (socket_poll::sp_add(_efd, fd)) {
            LOG_LAST_ERR("sp_add fail(%s)", addrAndPort.c_str());
			EXIT_RC(rc, -7);
		}

        _listenFds.insert(fd);
        LOG_MSG(LogLevel::Debug, "%s open success(%s), fd=%d, ipv%d", desc().c_str(), addrAndPort.c_str(), fd, (svr_addr->ai_family == AF_INET)?4:6);
exit:
        if (rc) {
            if (fd != INVALID_SOCK) CLOSE_SOCKET(fd);
            LOG_MSG(LogLevel::Warn, "%s open fail(%s), rc=%d", desc().c_str(), addrAndPort.c_str(), rc);
        }
        if (svr_addr)
            freeaddrinfo(svr_addr);
		return rc;
    }

    // 1. serveUtilStop() + stop(); 2. close(); 3. destructor
    virtual void close() {
        for (int fd : _listenFds) CLOSE_SOCKET(fd);
        _listenFds.clear();
        if (!socket_poll::sp_invalid(_efd)) {
			socket_poll::sp_release(_efd);
			_efd = INVALID_SOCK;
		}
        
        {
            std::lock_guard<std::mutex> lock(_lockF2U);
            _fd2user.clear();
        }
        {
            std::lock_guard<std::mutex> lock(_lockUS);
            LOG_MSG(LogLevel::Trace, "%s close, clear pending users, count=%d", desc().c_str(), _users.size());
            _users.clear();
        }
        {
            std::lock_guard<std::mutex> lock(_lockCU);
            LOG_MSG(LogLevel::Trace, "%s close, clear closed users, count=%d", desc().c_str(), _closedUsers.size());
            _closedUsers.clear();
        }
        LOG_MSG(LogLevel::Debug, "%s close", desc().c_str());
    }

    void start(int workerNum = 8, const message_swimlanes& lanes = message_swimlanes()) {
        _shutdown = false;
		_iothread = std::thread([&](){
			ioprocessor(ThdIdBase+1); // 01
		});
        _monitor = std::thread([&](){
            monitor(ThdIdBase+2);
        });

        workerNum = std::max(workerNum, 2);
        workerNum = std::min(workerNum, 64);
        for (int i = 1; i <= workerNum; ++i) {
            short thdId = ThdIdBase+10+i; // max:ThdIdBase+10+64
            _workerIds.push_back(thdId);
            _recvQueue[thdId] = std::make_unique<SafeQueue<TcpMsgPtr, MaxPendingMsg> >();
            std::thread* thd = new std::thread([&, thdId](){
                worker(thdId);
            });
            _workers.push_back(thd);
        }

        configSwimlanes(lanes); // after worker init
        _statLogger.start(ThdIdBase+3);
        LOG_MSG(LogLevel::Debug, "%s tcp threads start, workers=%d", desc().c_str(), workerNum);
	}

    virtual void serveUtilStop() {
        if (_iothread.joinable())
		    _iothread.join();
        if (_monitor.joinable())
            _monitor.join();
        _statLogger.stop();
        for (int i = 0; i < _workers.size(); ++i) {
            std::thread* thd = _workers[i];
            if (thd->joinable())
                thd->join();
            delete thd;
        }
        _workers.clear();
        for (auto& pair : _recvQueue) {
            auto& queue = pair.second;
            TcpMsgPtr ptr;
            while (queue->raw_pop(ptr)) {
                // TcpMsgPtr destructor deletes msg
            }
        }
        _recvQueue.clear();
        LOG_MSG(LogLevel::Debug, "%s tcp threads stopped", desc().c_str());
	}

    void stop(bool join = false) {
        {
            std::lock_guard<std::mutex> lock(_lockSD);
		    _shutdown = true;
        }
        for (auto& pair : _recvQueue) {
            auto& queue = pair.second;
            queue->stop();
        }
        if (join) serveUtilStop();
	}

    virtual tcpsock_user* createUser() = 0;

    int configSwimlanes(const message_swimlanes& lanes) {
        int totalNeed = 0;
        for (int i = 0; i < lanes.size(); ++i) {
            totalNeed += lanes[i].threads;
        }

        int pos = 0;
        std::unordered_map<uint32_t, WorkerIdGroup> tmpMsgTyp2WkIdGrp;
        if (totalNeed >= _workerIds.size()) {
            LOG_MSG(LogLevel::Warn, "%s fail, lanes need=%d, actually=%d, use default", __FUNCTION__, totalNeed, _workerIds.size());
        } else {
            // config lane
            for (int i = 0; i < lanes.size(); ++i) {
                message_swimlane lane = lanes[i];
                if (lane.msgTypes.empty() || lane.threads == 0) {
                    LOG_ERR_MSG("invalid lane, msgTypes.size=%d, threads=%d", lane.msgTypes.size(), lane.threads);
                    continue;
                }
                std::vector<short> workerIds(_workerIds.begin()+pos, _workerIds.begin()+pos+lane.threads);
                pos += lane.threads;

                WorkerIdGroup grp = { .workerIds = workerIds, .cursor = 0 };
                for (int j = 0; j < lane.msgTypes.size(); ++j) {
                    uint32_t type = lane.msgTypes[j];
                    tmpMsgTyp2WkIdGrp[type] = grp;
                    LOG_MSG(LogLevel::Info, "%s, type=%d, use threads=%d", __FUNCTION__, type, workerIds.size());
                }
            }
        }
        // config default
        std::vector<short> workerIds(_workerIds.begin()+pos, _workerIds.end());
        tmpMsgTyp2WkIdGrp[DefaultMsgType] = WorkerIdGroup{ .workerIds = workerIds, .cursor = 0 };
        LOG_MSG(LogLevel::Info, "%s, type=default(%d), use threads=%d", __FUNCTION__, DefaultMsgType, workerIds.size());

        std::lock_guard<std::mutex> lock(_lockMT2WI);
        _msgTyp2WkIdGrp = tmpMsgTyp2WkIdGrp;
        return 0;
    }

    void setMaxConnections(int max) { _maxConnections.store(max); }
    int getMaxConnections() const { return _maxConnections.load(); }

    int currentConnections() {
        std::lock_guard<std::mutex> lock(_lockF2U);
        return _fd2user.size();
    }

    void status() {
        LOG_MSG(LogLevel::Debug, "%s: current users=%d", desc().c_str(), currentConnections());
    }

private:
    bool isShutDown() {
        std::lock_guard<std::mutex> lock(_lockSD);
        return _shutdown;
	}

    std::shared_ptr<tcpsock_user> createUser(int fd, void* peer, int peerlen) {
        std::string addr;
        unsigned short port = 0;
        if (socket_utils::parseAddr(peer, peerlen, addr, port) < 0) {
            LOG_ERR_MSG("createUser, parseAddr fail");
            return nullptr;
        }
        
        socket_utils::set_nonblocking(fd);
        std::shared_ptr<tcpsock_user> user(this->createUser());
        if (socket_poll::sp_add(_efd, fd, false)) {
            LOG_LAST_ERR("createUser, sp_add fail");
            return nullptr;
        }

        user->init(addr.c_str(), port, fd, _userSeq.fetch_add(1, std::memory_order_relaxed));
        user->setProtocol(_protocol);
        {
            std::lock_guard<std::mutex> lock(_lockF2U);
            _fd2user[fd] = user;
        }
        std::lock_guard<std::mutex> lock(_lockUS);
        _users.push_back(user);
        return user;
    }

    std::shared_ptr<tcpsock_user> findUser(int fd) {
        std::lock_guard<std::mutex> lock(_lockF2U);
        auto it = _fd2user.find(fd);
        if (it != _fd2user.end())
            return it->second;
        LOG_ERR_MSG("findUser, find user by cache fail, fd=%d", fd);
        return nullptr;
    }

    int onIOError(int fd, const char* tag) {
        return closeUser(fd, tag);
    }

    int closeUser(int fd, const char* tag) {
        int rc = -1;
        std::lock_guard<std::mutex> lock(_lockF2U);
        auto it = _fd2user.find(fd);
        if (it != _fd2user.end()) {
            auto user = it->second;
            if (!user->isClosed()) {
                user->setClose();
                socket_poll::sp_del(_efd, fd);
                _fd2user.erase(fd);
                rc = 0; // closed user
                LOG_MSG(LogLevel::Warn, "%s(%s), tag=(%s), rc=%d", __FUNCTION__, user->desc().c_str(), tag, rc);
            } else {
                rc = 1; // already closed
                LOG_MSG(LogLevel::Warn, "%s(%s), tag=(%s), rc=%d", __FUNCTION__, user->desc().c_str(), tag, rc);
            }
        } else {
            rc = -1; // not found user
            LOG_MSG(LogLevel::Warn, "%s(fd=%d), tag=(%s), rc=%d", __FUNCTION__, fd, tag, rc);
        }
        return rc; 
    }

    void clearClosedUsers(int max = 10) {
        std::lock_guard<std::mutex> lock(_lockCU);
        int m = _closedUsers.size();
        if (m > max) m = max;
        for (int i = 0; i < m; ++i) {
            auto user = _closedUsers.front();
            _closedUsers.pop_front();
            if (user->checkAllMsgHandled()) {
                LOG_MSG(LogLevel::Debug, "delete user(%s)", user->desc().c_str());
            } else {
                _closedUsers.push_back(user);
            }
        }
    }

    int ioprocessor(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s ioprocessor thread(%d) start", desc().c_str(), thdId);
        struct event ev[MAX_EVENT];
        struct sockaddr_storage peer;
        memchunk chunk(BufferSize);
        
        for (;;) {
            if (isShutDown()) break;
            clearClosedUsers();

            int n = socket_poll::sp_wait(_efd, ev, MAX_EVENT, EpollWait); // ms
            if (n < 0) {
                LOG_LAST_ERR("sp_wait fail, efd=%d", _efd);
                break;
            }
            
            // handle io event
            for (int i = 0; i < n; ++i) {
                if (isShutDown()) break;

                int fd = ev[i].fd;
                if (_listenFds.find(fd) != _listenFds.end()) {
                    if (ev[i].read) {
                        memset(&peer, 0, sizeof(sockaddr_storage));
                        socklen_t peerlen = sizeof(sockaddr_storage);
                        int conn = ::accept(fd, (sockaddr*) &peer, &peerlen);
                        if (conn < 0) {
                            LOG_LAST_ERR("accept fail, fd=%d", fd);
                        } else {
                            int maxConn = _maxConnections.load(std::memory_order_relaxed);
                            int curConn = currentConnections();
                            if (curConn >= maxConn) {
                                LOG_MSG(LogLevel::Warn, "%s connection limit reached: %d/%d, reject new connection",
                                    desc().c_str(), curConn, maxConn);
                                CLOSE_SOCKET(conn);
                            } else {
                                auto user = createUser(conn, &peer, peerlen);
                                if (!user) {
                                    CLOSE_SOCKET(conn);
                                }
                            }
                        }
                    } else if (ev[i].error || ev[i].eof) {
                        LOG_MSG(LogLevel::Warn, "listener encounter error or eof, fd=%d", fd);
                    }
                } else {
                    if (ev[i].read) {
                        int recvLen = socket_utils::recv(fd, chunk.buffer, chunk.buffersize);
                        if (recvLen > 0) {
                            chunk.datasize = recvLen;
                            auto user = findUser(fd);
                            if (user) {
                                if (user->onRecv(&chunk)) {
                                    onIOError(fd, "cache data error");
                                }
                            }
                        } else if (recvLen == -2) {
                            // EAGAIN, ignore
                        } else { // -1 / 0
                            onIOError(fd, "read error");
                        }
                    } else if (ev[i].write) {
                        auto user = findUser(fd);
                        if (user) {
                            int rc = user->sendData();
                            if (rc == zbf::SEND_FAIL) {
                                onIOError(fd, "write error");
                            } else if (rc == zbf::QUEUE_EMPTY) {
                                user->enableWrite(_efd, false);
                                LOG_MSG(LogLevel::TraceMore, "ioprocessor: send suspend(%s), queue empty", user->desc().c_str());
                            }
                        }
                    } else if (ev[i].error || ev[i].eof) {
                        onIOError(fd, "error or eof");
                    }
                }
            }
        }
        LOG_MSG(LogLevel::Debug, "%s ioprocessor thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    int monitor(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s monitor thread(%d) start", desc().c_str(), thdId);
        std::shared_ptr<tcpsock_user> user;
        tcp_message* msg = nullptr;
        int rc = 0;
        TimeUnitSec now = socket_utils::currentTimeSecs();
        int counter = 0;
        const int update_time_interval = 1000/ThreadIdle; // almost 1s

        for (;;) {
            if (isShutDown()) {
				break;
			}
            if (++counter >= update_time_interval) {
                now = socket_utils::currentTimeSecs();
                counter = 0;
            }

            bool idle = true;
            user = popFrontUser();
            if (user != nullptr) {
                // check if user expired
                if (user->isExpired(now)) {
                    LOG_MSG(LogLevel::Debug, "user(%s) expired, close it", user->desc().c_str());
                    closeUser(user->_fd, "user expired");
                }
                // check if user closed
                if (user->isClosed()) {
                    user->onDisconnect();
                    std::lock_guard<std::mutex> lock(_lockCU);
                    _closedUsers.push_back(user);
                    idle = false;
                } else {
                    if (!user->isStarted()) {
                        user->setStart();
                        user->onConnect();
                        idle = false;
                    }
                    // check if user need write
                    if (!user->isQueueEmpty()) {
                        user->enableWrite(_efd, true);
                    }
                    // get message and dispatch to worker
                    tcp_message* msg = user->getRecvMsg();
                    if (msg != nullptr) {
                        dispatchMsg(msg, user);
                        idle = false;
                    }
                    pushUserBack(user);
                }
            }
            if (idle)
                std::this_thread::sleep_for(std::chrono::milliseconds(ThreadIdle));
            else
                std::this_thread::yield();
        }
        LOG_MSG(LogLevel::Debug, "%s monitor thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    int worker(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start", desc().c_str(), thdId);
        RequestStat reqStat;
        _statLogger.add(&reqStat);

        for (;;) {
            // get msg from worker queue
            TcpMsgPtr ptr;
            int rc = _recvQueue[thdId]->pop_timeout(ptr, 100);
            // handle msg
            if (rc == zbf::SQ_POP_SUCCESS) {
                tcp_message* msg = ptr.get();
                auto user_sp = ptr.user.lock();
                if (!user_sp) {
                    LOG_MSG(LogLevel::Warn, "worker: user gone, discard msg(%s)", msg->desc().c_str());
                    continue; // ptr destructor deletes msg
                }
                if (_protocol->isHeartbeat(msg)) {
                    LOG_MSG(LogLevel::TraceMore, "heartbeat msg from user(%s)", user_sp->desc().c_str());
                } else {
                    LOG_MSG(LogLevel::Trace, "recv msg(%s) from user(%s)", msg->desc().c_str(), user_sp->desc().c_str());
                    if (!user_sp->isClosed()) {
                        // handle msg
                        uint32_t msgType = _protocol->type(msg);
                        reqStat.onReqStart(msgType);
                        user_sp->onRecvMsg(msg);
                        reqStat.onReqFinish(msgType);
                    }
                }
                user_sp->markMsgHandled(msg);
                // ptr destructor deletes msg
                std::this_thread::yield();
            } else if (rc == zbf::SQ_EXIT) {
                break;
            }
        }
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    std::shared_ptr<tcpsock_user> popFrontUser() {
        std::lock_guard<std::mutex> lock(_lockUS);
        std::shared_ptr<tcpsock_user> user;
        if (!_users.empty()) {
            user = _users.front();
            _users.pop_front();
        }
        return user;
    }

    void pushUserBack(const std::shared_ptr<tcpsock_user>& user) {
        std::lock_guard<std::mutex> lock(_lockUS);
        _users.push_back(user);
    }

    void dispatchMsg(tcp_message* msg, std::shared_ptr<tcpsock_user> user) {
        uint32_t msgType = _protocol->type(msg);
        short wkId = 0;
        {
            std::lock_guard<std::mutex> lock(_lockMT2WI);
            auto it = _msgTyp2WkIdGrp.find(msgType);
            if (it == _msgTyp2WkIdGrp.end()) {
                msgType = DefaultMsgType;
            }
            WorkerIdGroup& wkIdGrp = _msgTyp2WkIdGrp[msgType];
            wkId = wkIdGrp.workerIds[wkIdGrp.cursor++];
            if (wkIdGrp.cursor == wkIdGrp.workerIds.size()) wkIdGrp.cursor = 0;
        }
        
        // push msg to worker queue
        TcpMsgPtr ptr(msg, user->weak_from_this());
        LOG_MSG(LogLevel::TraceMore, "dispatch msg(%s) to worker(%d)", msg->desc().c_str(), wkId);
        if (!_recvQueue[wkId]->push(std::move(ptr))) {
            LOG_ERR_MSG("dispatchMsg fail, queue is full, msg=%s", msg->desc().c_str());
            // TcpMsgPtr destructor deletes msg
        }
    }

    std::string desc() {
        return std::string("tcpsock_server");
    }

protected:
    enum { StatInterval = 5, ThreadIdle = 20, EpollWait = 50, BufferSize = 4096, BackLog = 128, ThdIdBase = 200, DefaultMsgType = 0, DefaultMaxConns = 6000 };

private:
    tcp_message_protocol* _protocol{nullptr}; // holder
    int _efd;
    std::unordered_set<int> _listenFds;
    std::atomic<unsigned long long> _userSeq{1ULL};
    bool _shutdown;
    std::mutex _lockSD;

    std::thread _iothread;
    std::thread _monitor;
    std::vector<std::thread*> _workers;
    std::unordered_map<short, SafeQueuePtr> _recvQueue;

    std::list<std::shared_ptr<tcpsock_user>> _users;
    std::mutex _lockUS;
    std::unordered_map<int, std::shared_ptr<tcpsock_user>> _fd2user;
    std::mutex _lockF2U;
    std::list<std::shared_ptr<tcpsock_user>> _closedUsers;
    std::mutex _lockCU;
    
    std::vector<short> _workerIds;
    std::unordered_map<uint32_t, WorkerIdGroup> _msgTyp2WkIdGrp;
    std::mutex _lockMT2WI;
    ReqStatLogger _statLogger;
    std::atomic<int> _maxConnections;
};


////////////////////////////////////////////////////////////////////////////////////////////////////
// client side
////////////////////////////////////////////////////////////////////////////////////////////////////

class tcpsock_client : object_tracker<tcpsock_client> {
public:
    tcpsock_client(const char* serverAddr, unsigned short serverPort, tcp_message_protocol* protocol, unsigned long long seq = 0ULL, const char* owner = "")
    : _hbhelper(HeartbeatTime) {
        _serverAddr = serverAddr;
        _serverPort = serverPort;
        _seq = seq;
        _fd = INVALID_SOCK;
        _closed = false;
        _owner = owner;
        _protocol = protocol;
    }

    virtual ~tcpsock_client() {
        disconnect();
    }

    int connect(int timeout = 2000, bool logErr = true) {
        _closed = false;
        disconnect(); // reset _fd
        
        int rc = 0;
        int fd = INVALID_SOCK;
        struct addrinfo* svr_addr = nullptr;
        int conn = 0;
        std::string addrAndPort = socket_utils::mergeAddr(_serverAddr.c_str(), _serverPort);

        svr_addr = socket_utils::getAddrInfo(_serverAddr.c_str(), _serverPort, 1);
        if (!svr_addr) {
            EXIT_RC(rc, -1);
        }

        fd = ::socket(svr_addr->ai_family, svr_addr->ai_socktype, svr_addr->ai_protocol);
		if (fd < 0) {
            if (logErr) LOG_LAST_ERR("socket fail(%s)", addrAndPort.c_str());
            EXIT_RC(rc, -2);
        }

        socket_utils::set_nonblocking(fd);
        conn = ::connect(fd, svr_addr->ai_addr, svr_addr->ai_addrlen);
        if (conn == 0) {
            rc = 0;
        } else {
            if (socket_utils::checkConnError(conn, logErr)) {
                if (logErr) LOG_ERR_MSG("connect fail(%s)", addrAndPort.c_str());
                EXIT_RC(rc, -3);
            }
            if (!socket_utils::checkSockStat(fd, 2, timeout, logErr)) {
                if (logErr) LOG_ERR_MSG("checkSockStat fail(%s)", addrAndPort.c_str());
                EXIT_RC(rc, -4);
            }
            int soerr = socket_utils::checkSoError(fd, logErr);
            if (soerr != 0) {
                if (logErr) LOG_ERR_MSG("checkSoError fail(%s)", addrAndPort.c_str());
                EXIT_RC(rc, -5);
            }
        }
        socket_utils::set_blocking(fd);
        _fd = fd;
        flushAlive();
        LOG_MSG(LogLevel::Debug, "client(%s) connect success", desc(false).c_str());
exit:
        if (rc) {
            if (fd != INVALID_SOCK) CLOSE_SOCKET(fd);
            if (logErr) LOG_ERR_MSG("client connect fail(%s), rc=%d", addrAndPort.c_str(), rc);
        }
        if (svr_addr) freeaddrinfo(svr_addr);
        return rc;
    }

    void disconnect() {
        if (_fd != INVALID_SOCK) {
            LOG_MSG(LogLevel::Debug, "client(%s) disconnect", desc(false).c_str());
            CLOSE_SOCKET(_fd);
            _fd = INVALID_SOCK;
        }
    }

    int send(const tcp_message* req, tcp_message** ack = nullptr) {
        return innerSend(req, ack);
    }

    std::string desc(bool brief = true) {
        char szDesc[64] = {0};
        if (brief)
            snprintf(szDesc, sizeof(szDesc), "%s|seq:%llu|fd:%d", _owner.c_str(), _seq, _fd);
        else
            snprintf(szDesc, sizeof(szDesc), "%s|seq:%llu|fd:%d|addr=%s|port:%u", _owner.c_str(), _seq, _fd, _serverAddr.c_str(), _serverPort);
        return std::string(szDesc);
    }

    int getFd() {
        return _fd;
    }

    bool isConnected() { // inner set
        return (_fd != INVALID_SOCK);
    }
    
    bool isClosed() { // outter set
        std::lock_guard<std::mutex> lock(_lockClose);
        return _closed;
    }
    void setClose() {
        std::lock_guard<std::mutex> lock(_lockClose);
        _closed = true;
    }

    bool needHeartbeat() {
        return _hbhelper.isExceed();
    }

    void flushAlive() {
        _hbhelper.update();
    }

private:
    // SEND_SUCCESS / SEND_FAIL / RECV_FAIL
    int innerSend(const tcp_message* req, tcp_message** ack) {
        int rc = 0;
        int recvlen = 0;
        tcp_message* res = nullptr;

        rc = socket_utils::send_with_retry(_fd, req->data.data(), req->data.size());
        if (rc < (int) req->data.size()) {
            EXIT_RC(rc, zbf::SEND_FAIL);
        }
        rc = zbf::SEND_SUCCESS;
        if (ack == nullptr) {
            goto exit;
        }
        
        {
            uint32_t headerSz = _protocol->headerSize();
            std::string hdrBuf(headerSz, '\0');
            recvlen = socket_utils::recv_all(_fd, &hdrBuf[0], (int) headerSz);
            if (recvlen != (int) headerSz) {
                LOG_ERR_MSG("client(%s) recv msg header fail, recvlen=%d, expect=%d", desc().c_str(), recvlen, headerSz);
                EXIT_RC(rc, zbf::RECV_FAIL);
            }

            tcp_message tmpMsg;
            tmpMsg.data = std::move(hdrBuf);
            uint32_t bodySz = _protocol->bodySize(&tmpMsg);
            if (bodySz > _protocol->MaxBodySize()) {
                LOG_ERR_MSG("client(%s) recv msg body size invalid: %d", desc().c_str(), bodySz);
                EXIT_RC(rc, zbf::RECV_FAIL);
            }

            res = new tcp_message();
            res->data = std::move(tmpMsg.data); // header
            if (bodySz > 0) {
                res->data.resize(headerSz + bodySz);
                recvlen = socket_utils::recv_all(_fd, &res->data[headerSz], (int) bodySz);
                if (recvlen < (int) bodySz) {
                    LOG_ERR_MSG("client(%s) recv msg body fail, recvlen=%d, expect=%d", desc().c_str(), recvlen, bodySz);
                    EXIT_RC(rc, zbf::RECV_FAIL);
                }
            }
        }

        *ack = res;
exit:
        if (rc != zbf::SEND_SUCCESS) {
            if (res) delete res;
            LOG_ERR_MSG("client(%s) innerSend fail, req=%s, rc=%d", desc().c_str(), req->desc().c_str(), rc);
            disconnect();
        } else {
            std::string resDesc = (res != nullptr) ? res->desc() : "nullptr";
            LOG_MSG(LogLevel::Trace, "client(%s) innerSend success, req=%s, ack=%s", desc().c_str(), req->desc().c_str(), resDesc.c_str());
            flushAlive();
        }
        return rc;
    }

public:
    enum { HeartbeatTime = 30 };

protected:
    std::string _serverAddr;
    unsigned short _serverPort;
    unsigned long long _seq;
    int _fd;
    std::string _owner;
    tcp_message_protocol* _protocol{nullptr}; // ref

private:
    bool _closed;
    std::mutex _lockClose;
    heartbeat_helper _hbhelper;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// client side pool
////////////////////////////////////////////////////////////////////////////////////////////////////

class tcpsock_cltpool : object_tracker<tcpsock_cltpool> {
public:
    // protocol: caller construct buf owner(this class) destruct
    tcpsock_cltpool(tcp_message_protocol* protocol, const char* name = "cltpool") {
        _protocol = protocol;
        _poolName = name;
        _poolSize = 0;
        _serverPort = 0;
        _heartbeat = _protocol->genHeartbeat();
    }

    // must call close() before destructor
    virtual ~tcpsock_cltpool() {
        delete _protocol;
        delete _heartbeat;
    }

    int open(const char* serverAddr, unsigned short serverPort, int poolSize = 8) {
        if (!serverAddr || serverPort == 0 || poolSize == 0) {
            return -1;
        }
        _serverAddr = serverAddr;
        _serverPort = serverPort;
        _poolSize = poolSize;

        tcpsock_client* clt = createClient();
        if (clt != nullptr) {
            std::lock_guard<std::mutex> lock(_lockClients);
            _clients.push_back(clt);
            _idleList.push_back(clt);
        }
        LOG_MSG(LogLevel::Debug, "tcpsock_cltpool(%s) open", desc().c_str());
        return 0;
    }

    void close() {
        std::lock_guard<std::mutex> lock(_lockClients);
        for (tcpsock_client* clt : _clients) {
            clt->disconnect();
            delete clt;
        }
        _clients.clear();
        _idleList.clear();
        LOG_MSG(LogLevel::Debug, "tcpsock_cltpool(%s) close", desc().c_str());
    }

    // SEND_SUCCESS / SEND_FAIL / SEND_TIMEOUT / SOCK_CLOSED
    int send(const tcp_message* req, tcp_message** ack, int wait_max = 2000 /*ms*/) {
        int rc = zbf::SEND_TIMEOUT;
        int wait = 50;
        int total_wait = 0;

        do {
            tcpsock_client* clt = popIdleClient();
            if (clt != nullptr) {
                rc = poolSend(clt, req, ack);
                pushClientBack(clt);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
            total_wait += wait;
        } while (total_wait < wait_max);

        if (rc) {
            LOG_MSG(LogLevel::Warn, "pool(%s) send fail, req=%s, rc=%d", desc().c_str(), req->desc().c_str(), rc);
        }
        return rc;
    }    

    bool isInvalid() {
        return _poolInvalid.load();
    }

    std::string desc() {
        char szDesc[64] = {0};
        snprintf(szDesc, sizeof(szDesc), "%s|sz:%d|addr=%s|port:%u", _poolName.c_str(), _poolSize, _serverAddr.c_str(), _serverPort);
        return std::string(szDesc);
    }

    void keepAlive() {
        std::list<tcpsock_client*> hbList;

        {
            std::lock_guard<std::mutex> lock(_lockClients);
            int size = _idleList.size();
            for (int i = 0; i < size; ++i) {
                tcpsock_client* clt = _idleList.front();
                _idleList.pop_front();
                if (clt->needHeartbeat()) {
                    hbList.push_back(clt);
                } else {
                    _idleList.push_back(clt);
                }
            }
        }
        
        for (tcpsock_client* clt : hbList) {
            poolSend(clt, _heartbeat, nullptr);
        }

        std::lock_guard<std::mutex> lock(_lockClients);
        _idleList.insert(_idleList.end(), hbList.begin(), hbList.end());
    }

    void status() {
        LOG_MSG(LogLevel::Debug, "%s[%s:%d]=%d/%d, %s", _poolName.c_str(), _serverAddr.c_str(), _serverPort, _clients.size(), _poolSize, isInvalid()?"invalid":"valid");
    }

private:
    tcpsock_client* createClient() {
        tcpsock_client* clt = new tcpsock_client(_serverAddr.c_str(), _serverPort, _protocol, clientSeq(), _poolName.c_str());
        int rc = clt->connect();
        LOG_MSG(LogLevel::Debug, "createClient(%s), connect=%d", clt->desc().c_str(), rc);
        updatePoolState(rc);
        return clt;
    }

    tcpsock_client* popIdleClient() {
        tcpsock_client* newClt = nullptr;
        {
            std::lock_guard<std::mutex> lock(_lockClients);
            if (_clients.size() < _poolSize && _idleList.empty()) {
                newClt = new tcpsock_client(_serverAddr.c_str(), _serverPort, _protocol, clientSeq(), _poolName.c_str());
                _clients.push_back(newClt);
            }
        }
        if (newClt) {
            int rc = newClt->connect();
            updatePoolState(rc);
            LOG_MSG(LogLevel::Debug, "createClient(%s), connect=%d", newClt->desc().c_str(), rc);
            std::lock_guard<std::mutex> lock(_lockClients);
            if (rc == 0) {
                _idleList.push_back(newClt);
            } else {
                _clients.erase(std::find(_clients.begin(), _clients.end(), newClt));
                delete newClt;
                newClt = nullptr;
            }
        }

        tcpsock_client* clt = nullptr;
        std::lock_guard<std::mutex> lock(_lockClients);
        if (!_idleList.empty()) {
            clt = _idleList.front();
            _idleList.pop_front();
        }
        return clt;
    }

    void pushClientBack(tcpsock_client* clt) {
        std::lock_guard<std::mutex> lock(_lockClients);
        _idleList.push_back(clt);
    }

    int poolSend(tcpsock_client* clt, const tcp_message* req, tcp_message** ack) {
        int rc = zbf::SOCK_CLOSED;
        if (clt->isConnected()) { // fd maybe broken, since it's client status
            rc = clt->send(req, ack);
        }
        if (rc) {
            clt->connect(1000, false); // maybe fail, while server is down
            if (clt->isConnected()) {
                rc = clt->send(req, ack);
            }
        }
        updatePoolState(rc);
        return rc;
    }

    void updatePoolState(int rc) {
        _poolInvalid.store(rc != zbf::SEND_SUCCESS);
    }

    unsigned long long clientSeq() {
        return _clientSeq.fetch_add(1, std::memory_order_relaxed);
    }

private:
    tcp_message_protocol* _protocol; // holder
    std::string _poolName;
    int _poolSize;
    tcp_message* _heartbeat;

    std::string _serverAddr;
    unsigned short _serverPort;
    std::atomic<unsigned long long> _clientSeq{1ULL};

    std::list<tcpsock_client*> _idleList;
    std::vector<tcpsock_client*> _clients;
    std::mutex _lockClients;
    std::atomic<bool> _poolInvalid{false};
};

class tcpsock_ha_cltpool : object_tracker<tcpsock_ha_cltpool> {
public:
    tcpsock_ha_cltpool() {
        _poolCursor.store(0);
    }

    // must call clearPools() before destructor
    virtual ~tcpsock_ha_cltpool() {
    }

    void addPool(tcpsock_cltpool* pool) {
        std::lock_guard<std::mutex> lock(_lockCltPool);
        _cltPools.push_back(pool);
    }

    void addConnectPool(const char* serverAddr, unsigned short serverPort, int poolSize, tcp_message_protocol* protocol) {
        tcpsock_cltpool* pool = new tcpsock_cltpool(protocol, "ha_cltpool");
        if (!pool->open(serverAddr, serverPort, poolSize)) {
            addPool(pool);
        }
    }

    void clearPools() {
        std::lock_guard<std::mutex> lock(_lockCltPool);
        for (tcpsock_cltpool* pool : _cltPools) {
            pool->close();
            delete pool;
        }
        _cltPools.clear();
    }

    // SEND_SUCCESS / SEND_FAIL / SEND_TIMEOUT / POOL_INVALID
    int send(const tcp_message* req, tcp_message** ack, int wait_max = 2000 /*ms*/) {
        int rc = zbf::POOL_INVALID;
        tcpsock_cltpool* pool = getPool();
        if (pool) {
            rc = pool->send(req, ack, wait_max);
        } else {
            LOG_MSG(LogLevel::Warn, "send fail(no valid pool), req=%s", req->desc().c_str());
        }
        return rc;
    }

    // call every tcpsock_client::HeartbeatTime to keep connection alive
    void keepAlive() {
        for (tcpsock_cltpool* pool : _cltPools) {
            pool->keepAlive();
        }
    }

    void status() {
        for (tcpsock_cltpool* pool : _cltPools) {
            pool->status();
        }
    }

private:
    tcpsock_cltpool* getPool() {
        tcpsock_cltpool* pool = nullptr;
        std::lock_guard<std::mutex> lock(_lockCltPool);
        for (int i = 0; i < _cltPools.size(); ++i) {
            if (_poolCursor.load() == _cltPools.size())
                _poolCursor.store(0);
            tcpsock_cltpool* p = _cltPools[_poolCursor++];
            if (!p->isInvalid()) {
                pool = p;
                break;
            }
        }
        return pool;
    }

private:
    std::vector<tcpsock_cltpool*> _cltPools;
    std::atomic<int> _poolCursor{0};
    std::mutex _lockCltPool;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// client side async mode
////////////////////////////////////////////////////////////////////////////////////////////////////

class tcpsock_asynclt : public tcpsock_client {
public:
    tcpsock_asynclt(const char* addr, unsigned short port, tcp_message_protocol* protocol, unsigned long long seq = 0ULL, const char* owner = "") 
        : tcpsock_client(addr, port, protocol, seq, owner) {
        _recvCache = new memchunk(BufferSize);
        _enableWrite = false;
    }

    virtual ~tcpsock_asynclt() {
        delete _recvCache;
        _wrQueue.clear();
    }

    // if fail, caller need delete msg
    int post(tcp_message* msg) {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        if (_wrQueue.size() > MaxPendingMsg) {
            LOG_ERR_MSG("send fail, queue full(%d)", _wrQueue.size());
            return 1;
        }
        _wrQueue.emplace_back(TcpMsgPtr(msg));
        return 0;
    }

    bool isQueueEmpty() {
        std::lock_guard<std::mutex> lock(_lockWRQ);
        return _wrQueue.empty();
    }

    void enableWrite(int efd, bool enable) {
        std::lock_guard<std::mutex> lock(_lockEW);
        if (_enableWrite != enable) {
            _enableWrite = enable;
            socket_poll::sp_enable(efd, _fd, true, _enableWrite);
        }
    }

    // reinit write status when add to epoll
    void syncWriteStatus(bool enable) {
        _enableWrite = enable;
    }

public:
    int sendData() {
        if (isClosed()) return SOCK_CLOSED;
        int rc = zbf::_SendData(_fd, _wrQueue, _lockWRQ);
        if (rc == zbf::SEND_SUCCESS) {
            flushAlive();
        }
        return rc;
    }

    int onRecv(memchunk* chunk) {
        if (isClosed()) return 1;
        std::lock_guard<std::mutex> lock(_lockRC);
        int rc = _recvCache->push(chunk->buffer, chunk->datasize);
        if (rc < 0) {
            LOG_ERR_MSG("onRecv, push buffer fail, rc=%d", rc);
            return 2;
        }
        if (_recvCache->datasize > MaxPendingSize) {
            LOG_ERR_MSG("onRecv(%s), pending data is too long: %d", desc(false).c_str(), _recvCache->datasize);
            return 3;
        }
        return 0;
    }

    tcp_message* getRecvMsg() {
        if (isClosed()) return nullptr;
        std::lock_guard<std::mutex> lock(_lockRC);
        tcp_message* msg = zbf::_PopMsg(_recvCache, _protocol);
        return msg;
    }

private:
    memchunk* _recvCache;
    std::mutex _lockRC;
    std::list<TcpMsgPtr> _wrQueue;
    std::mutex _lockWRQ;
    bool _enableWrite;
    std::mutex _lockEW;
    
    enum { BufferSize = 1024, MaxPendingSize = 10485760, MaxPendingMsg = 128 };
};

enum { POST_SUCCESS = 0, POST_FAIL = 1 };

class tcpsock_ha_asynclt : object_tracker<tcpsock_ha_asynclt> {
public:
    tcpsock_ha_asynclt(const char* name = "ha_asynclt") {
        _name = name;
        _cltCursor = 0;
    }

    virtual ~tcpsock_ha_asynclt() {
        disconnect();
    }

    // add one client: addr:port
    void addConnect(const char* addr, unsigned short port, tcp_message_protocol* protocol) {
        tcpsock_asynclt* asynclt = new tcpsock_asynclt(addr, port, protocol, _cltSeq.fetch_add(1, std::memory_order_relaxed), _name.c_str());
        asynclt->connect();
        std::lock_guard<std::mutex> lock(_lockClients);
        _clients.push_back(asynclt);
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(_lockClients);
        for (tcpsock_asynclt* clt : _clients) {
            clt->disconnect();
            delete clt;
        }
        _clients.clear();
    }

    std::vector<tcpsock_asynclt*> allClients() {
        std::lock_guard<std::mutex> lock(_lockClients);
        std::vector<tcpsock_asynclt*> clts = _clients;
        return clts;
    }

public:
    // POST_SUCCESS / POST_FAIL
    // if fail, caller need delete msg
    int post(tcp_message* msg) {
        int rc = zbf::POST_FAIL;
        tcpsock_asynclt* asynclt = getClient();
        if (asynclt) {
            if (0 == asynclt->post(msg))
                rc = zbf::POST_SUCCESS;
        }
        return rc;
    }

    // one of response for _clients
    virtual void onResponse(const tcp_message* msg) = 0;

private:
    tcpsock_asynclt* getClient() {
        tcpsock_asynclt* asynclt = nullptr;
        std::lock_guard<std::mutex> lock(_lockClients);
        for (int i = 0; i < _clients.size(); ++i) {
            if (_cltCursor == _clients.size()) _cltCursor = 0;
            tcpsock_asynclt* clt = _clients[_cltCursor++];
            if (clt->isConnected() && !clt->isClosed()) {
                asynclt = clt;
                break;
            }
        }
        return asynclt;
    }

protected:
    std::string _name;
    std::vector<tcpsock_asynclt*> _clients;
    int _cltCursor;
    std::mutex _lockClients;
    std::atomic<unsigned long long> _cltSeq{1ULL};
};

class async_client_manager {
public:
    // protocol: caller construct buf owner(this class) destruct
    async_client_manager(tcp_message_protocol* protocol) {
        _protocol = protocol;
        _shutdown = false;
        _workerNum = 0;
        _efd = INVALID_SOCK;
    }

    virtual ~async_client_manager() {
        delete _protocol;
    }

    void start(int workerNum = 4) {
        _efd = socket_poll::sp_create();
        if (socket_poll::sp_invalid(_efd)) {
            LOG_LAST_ERR("%s sp_create fail", desc().c_str());
            return;
        }

        _iothread = std::thread([&]() {
			ioprocessor(ThdIdBase+1);  // 1
		});
        _monitor = std::thread([&]() {
			monitor(ThdIdBase+2); // 2
		});

        _workerNum = std::min(workerNum, 16); // 21 ~ 36
        for (int i = 0; i < _workerNum; ++i) {
            short thdId = ThdIdBase+21+i;
            int workerIndex = i;
            std::thread* thd = new std::thread([&, thdId, workerIndex](){
                worker(thdId, workerIndex);
            });
            _workers.push_back(thd);
        }
        LOG_MSG(LogLevel::Debug, "%s start, workers=%d", desc().c_str(), _workerNum);
    }

    void serveUtilStop() {
        if (_iothread.joinable())
		    _iothread.join();
        if (_monitor.joinable())
		    _monitor.join();
        for (int i = 0; i < _workers.size(); ++i) {
            std::thread* thd = _workers[i];
            if (thd->joinable())
                thd->join();
            delete thd;
        }
        _workers.clear();
        clearClients();
        if (!socket_poll::sp_invalid(_efd)) {
			socket_poll::sp_release(_efd);
			_efd = INVALID_SOCK;
		}
        LOG_MSG(LogLevel::Debug, "%s stopped", desc().c_str());
    }

    void stop(bool join = false) {
        setShutDown(true);
        if (join) serveUtilStop();
    }

    void manageClient(tcpsock_ha_asynclt* ha_clt) {
        std::lock_guard<std::mutex> lock(_lockHAClients);
        _haClients.push_back(ha_clt);
    }

    void status() {
        int total = 0;
        {
            std::lock_guard<std::mutex> lock(_lockHAClients);
            for (tcpsock_ha_asynclt* ha_clt : _haClients) {
                total += ha_clt->allClients().size();
            }
        }
        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        LOG_MSG(LogLevel::Debug, "%s: current clients=%d/%d", desc().c_str(), _fd2client.size(), total);
    }

    tcp_message_protocol* getProtocol() { return _protocol; }

private:
    void clearClients() {
        {
            std::lock_guard<std::mutex> lock(_lockHAClients);
            for (tcpsock_ha_asynclt* ha_clt : _haClients) {
                ha_clt->disconnect();
                delete ha_clt;
            }
            _haClients.clear();
        }

        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        _monitorColl.clear();
        _fd2client.clear();
    }

    bool isShutDown() {
        std::lock_guard<std::mutex> lock(_lockSD);
        return _shutdown;
	}

    void setShutDown(bool sd) {
        std::lock_guard<std::mutex> lock(_lockSD);
        _shutdown = sd;
	}

    bool isMonitored(tcpsock_asynclt* asynclt) {
        if (!asynclt) return false;
        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        return (_monitorColl.find(asynclt) != _monitorColl.end());
    }

    void addMonitored(tcpsock_asynclt* asynclt) {
        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        socket_utils::set_nonblocking(asynclt->getFd());
        if (socket_poll::sp_add(_efd, asynclt->getFd(), false)) {
            LOG_LAST_ERR("sp_add fail, efd=%d, fd=%d", _efd, asynclt->getFd());
        } else {
            asynclt->syncWriteStatus(false);
            _monitorColl.insert(asynclt);
            _fd2client.insert(std::make_pair(asynclt->getFd(), asynclt));
        }
    }

    void removeMonitored(tcpsock_asynclt* asynclt) {
        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        socket_poll::sp_del(_efd, asynclt->getFd());
        _monitorColl.erase(asynclt);
        _fd2client.erase(asynclt->getFd());
        asynclt->setClose(); // in fact, fd was broken
    }

    void onIOError(int fd, tcpsock_asynclt* asynclt, const char* tag) {
        if (isMonitored(asynclt)) {
            LOG_MSG(LogLevel::Warn, "IO error(%s), remove monitored client(%s)", tag, asynclt->desc().c_str());
            removeMonitored(asynclt);
        } else {
            LOG_MSG(LogLevel::Warn, "IO error(%s), no monitored client, fd=%d", tag, fd);
        }
    }

    tcpsock_asynclt* getClientByFd(int fd) {
        std::lock_guard<std::mutex> lock(_lockMonitorColl);
        auto it = _fd2client.find(fd);
        if (it != _fd2client.end()) {
            return it->second;
        }
        return nullptr;
    }

    int ioprocessor(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s ioprocessor thread(%d) start", desc().c_str(), thdId);
        struct event ev[MAX_EVENT];
        memchunk chunk(BufferSize);

        for (;;) {
            if (isShutDown()) break;

            int n = socket_poll::sp_wait(_efd, ev, MAX_EVENT, EpollWait);
            if (n < 0) {
                LOG_LAST_ERR("sp_wait fail, efd=%d", _efd);
                break;
            }
            for (int i = 0; i < n; ++i) {
                if (isShutDown()) break;
                
                int fd = ev[i].fd;
                tcpsock_asynclt* asynclt = getClientByFd(fd);
                if (ev[i].read) {
                    int recvLen = socket_utils::recv(fd, chunk.buffer, chunk.buffersize);
                    if (recvLen > 0) {
                        chunk.datasize = recvLen;
                        if (isMonitored(asynclt)) {
                            if (asynclt->onRecv(&chunk)) {
                                onIOError(fd, asynclt, "cache data");
                            }
                        } else {
                            LOG_MSG(LogLevel::Warn, "unidentified read, no monitored client(%p), recvLen=%d, fd=%d", asynclt, recvLen, fd);
                        }
                    } else if (recvLen == -2) {
                        // EAGAIN, ignore
                    } else {
                        onIOError(fd, asynclt, "read");
                    }
                } else if (ev[i].write) {
                    if (isMonitored(asynclt)) {
                        int rc = asynclt->sendData();
                        if (rc == zbf::SEND_FAIL) {
                            onIOError(fd, asynclt, "write");
                        } else if (rc == zbf::QUEUE_EMPTY) {
                            asynclt->enableWrite(_efd, false);
                            LOG_MSG(LogLevel::TraceMore, "ioprocessor send suspend(%s), queue empty", asynclt->desc().c_str());
                        }
                    }
                } else if (ev[i].error || ev[i].eof) {
                    onIOError(fd, asynclt, "error or eof");
                }
            }
        }
        LOG_MSG(LogLevel::Debug, "%s ioprocessor thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    int monitor(short thdId) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s monitor thread(%d) start", desc().c_str(), thdId);

        for (;;) {
            if (isShutDown()) {
				break;
			}
            std::vector<tcpsock_ha_asynclt*> haCltsCopy;
            {
                std::lock_guard<std::mutex> lock(_lockHAClients);
                haCltsCopy = _haClients;
            }
            
            for (tcpsock_ha_asynclt* ha_clt : haCltsCopy) {
                std::vector<tcpsock_asynclt*> clients = ha_clt->allClients();
                for (tcpsock_asynclt* asynclt : clients) {
                    if (!isMonitored(asynclt)) {
                        if (asynclt->isConnected() && !asynclt->isClosed()) { // connected & not close
                            addMonitored(asynclt);
                            LOG_MSG(LogLevel::Debug, "start monitor (%s)", asynclt->desc().c_str());
                        } else {
                            if (!asynclt->connect(1000, false)) { // not connected or closed
                                addMonitored(asynclt);
                                LOG_MSG(LogLevel::Debug, "start monitor (%s)", asynclt->desc().c_str());
                            }
                        }
                    } else {
                        if (!asynclt->isQueueEmpty()) {
                            asynclt->enableWrite(_efd, true);
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(ThreadIdle));
        }
        
        LOG_MSG(LogLevel::Debug, "%s monitor thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }
    
    // send & recive
    int worker(short thdId, int workerIndex) {
        log_utils::registerThreadId(thdId);
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) start, index=%d", desc().c_str(), thdId, workerIndex);
        int workerNum = _workerNum;
        int counter = 0;
        const int check_alive_interval = 3000/ThreadIdle; // almost 3s

        for (;;) {
            if (isShutDown()) {
				break;
			}
            bool check_alive = false;
            if (++counter >= check_alive_interval) {
                check_alive = true;
                counter = 0;
            }

            bool idle = true;
            // pickup matched part
            std::vector<tcpsock_ha_asynclt*> haCltsPart;
            {
                std::lock_guard<std::mutex> lock(_lockHAClients);
                for (int i = 0; i < _haClients.size(); ++i) {
                    if (i % workerNum == workerIndex) {
                        haCltsPart.push_back(_haClients[i]);
                    }
                }
            }

            // handle all clients in groups
            for (tcpsock_ha_asynclt* ha_clt : haCltsPart) {
                if (isShutDown()) {
                    break;
                }
                std::vector<tcpsock_asynclt*> clients = ha_clt->allClients();
                for (tcpsock_asynclt* asynclt : clients) {
                    if (!isMonitored(asynclt))
                        continue;
                    if (notifyRecv(asynclt, ha_clt))
                        idle = false;
                    if (check_alive && checkAlive(asynclt)) {
                        idle = false;
                    }
                }
            }
            if (idle) {
                // this thread is idle
                std::this_thread::sleep_for(std::chrono::milliseconds(ThreadIdle));
            } else {
                std::this_thread::yield();
            }
        }
        LOG_MSG(LogLevel::Debug, "%s worker thread(%d) exit", desc().c_str(), thdId);
        return 0;
    }

    bool notifyRecv(tcpsock_asynclt* asynclt, tcpsock_ha_asynclt* ha_clt) {
        tcp_message* msg = asynclt->getRecvMsg();
        if (msg != nullptr) {
            ha_clt->onResponse(msg);
            delete msg;
            return true;
        }
        return false;
    }

    bool checkAlive(tcpsock_asynclt* asynclt) {
        if (asynclt->isConnected() && !asynclt->isClosed()) {
            if (asynclt->needHeartbeat()) {
                tcp_message* hb = _protocol->genHeartbeat();
                if (0 != asynclt->post(hb)) {
                    delete hb;
                }
                return true;
            }
        }
        return false;
    }

    std::string desc() {
        return std::string("async_client_manager");
    }

private:
    int _efd;
    tcp_message_protocol* _protocol{nullptr}; // holder
    bool _shutdown;
    std::mutex _lockSD;
    std::thread _iothread;
    std::thread _monitor;
    std::vector<std::thread*> _workers;
    int _workerNum;

    std::vector<tcpsock_ha_asynclt*> _haClients;          // own, manager will delete
    std::mutex _lockHAClients;
    std::unordered_set<tcpsock_asynclt*> _monitorColl;    // ref, do not manage memory
    std::unordered_map<int, tcpsock_asynclt*> _fd2client; // ref
    std::mutex _lockMonitorColl;
    
    enum { BufferSize = 4096, ThdIdBase = 300, ThreadIdle = 20, EpollWait = 50 };
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// utils functions
////////////////////////////////////////////////////////////////////////////////////////////////////

// thread unsafe, return sent size
inline int _SendMsg(int fd, const tcp_message* msg) {
    int sendlen = socket_utils::send(fd, msg->data.data(), msg->data.size());
    if (sendlen <= 0) {
        if (sendlen != -2)
            LOG_ERR_MSG("%s fail, sendlen=%d, expect=%d, fd=%d", __FUNCTION__, sendlen, msg->data.size(), fd);
    } else {
        if (sendlen < (int) msg->data.size())
            LOG_MSG(LogLevel::Warn, "%s not finish, sendlen=%d, expect=%d, fd=%d", __FUNCTION__, sendlen, msg->data.size(), fd);
        else
            LOG_MSG(LogLevel::TraceMore, "%s success, msg=%s, fd=%d", __FUNCTION__, msg->desc().c_str(), fd);
    }
    return sendlen;
}

// SEND_SUCCESS / SEND_FAIL / QUEUE_EMPTY
inline int _SendData(int fd, std::list<TcpMsgPtr>& queue, std::mutex& lockQueue) {
    tcp_message* msg = nullptr;
    {
        std::lock_guard<std::mutex> lock(lockQueue);
        if (queue.empty()) return zbf::QUEUE_EMPTY;
        msg = queue.front().release();
        queue.pop_front();
    }

    int rc = zbf::SEND_SUCCESS;
    int sendlen = zbf::_SendMsg(fd, msg);
    if (sendlen <= 0) {
        if (sendlen != -2) {
            delete msg;
            rc = zbf::SEND_FAIL;
        } else {
            std::lock_guard<std::mutex> lock(lockQueue);
            queue.emplace_front(TcpMsgPtr(msg));
        }
    } else if (sendlen < (int) msg->data.size()) {
        tcp_message* rest = new tcp_message();
        rest->data.assign(msg->data.data() + sendlen, msg->data.size() - sendlen);
        std::lock_guard<std::mutex> lock(lockQueue);
        queue.emplace_front(TcpMsgPtr(rest));
        delete msg;
        // as success
    } else {
        delete msg;
        // success
    }
    return rc;
}

inline tcp_message* _PopMsg(memchunk* recvCache, tcp_message_protocol* protocol) {
    tcp_message* msg = nullptr;
    if (!protocol) return nullptr;

    uint32_t headerSz = protocol->headerSize();
    if (recvCache->datasize < headerSz) return nullptr;

    // peek header to get body size
    tcp_message tmpMsg;
    tmpMsg.data.assign((char*)recvCache->buffer, headerSz);
    uint32_t bodySz = protocol->bodySize(&tmpMsg);

    if (bodySz > protocol->MaxBodySize()) {
        LOG_ERR_MSG("Message size invalid: header=%d, body=%d, drop recv cache", headerSz, bodySz);
        recvCache->pop(recvCache->datasize);
        return nullptr;
    }

    uint32_t totalSz = headerSz + bodySz;
    if (recvCache->datasize >= totalSz) {
        msg = new tcp_message();
        msg->data.assign((char*)recvCache->buffer, totalSz);
        recvCache->pop(totalSz);
    }
    return msg;
}

}

#endif
