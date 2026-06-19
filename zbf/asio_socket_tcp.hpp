#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <atomic>
#include "asio.hpp"
#include "log_utils.hpp"
#include "tcp_message.hpp"
#include "asio_worker.hpp"
#include "heartbeat_helper.hpp"

namespace zbf {

using asio::ip::tcp;

class tcpsock_user;
class tcpsock_listener {
public:
    virtual void on_connect(std::shared_ptr<tcpsock_user> user) = 0;
    virtual void on_message(std::shared_ptr<tcpsock_user> user, std::unique_ptr<tcp_message> msg) = 0;
    virtual void on_disconnect(std::shared_ptr<tcpsock_user> user) = 0;
};


class tcpsock_user : public std::enable_shared_from_this<tcpsock_user>, object_tracker<tcpsock_user> {
public:
    tcpsock_user(asio_worker::work_strand& io_strand, asio_worker::work_strand& wk_strand, bool server_side, tcp_message_protocol* protocol, tcpsock_listener* listener) :
        _io_strand(io_strand), _worker_strand(wk_strand), _socket(io_strand.get_inner_executor()),
        _hbhelper(server_side? UserTimeout:HeartbeatTime), _stop(false), _server_side(server_side), _protocol(protocol), _listener(listener) {
        assert(_listener != nullptr);
    }

    virtual ~tcpsock_user() {
    }

    tcp::socket& sock() {
        return _socket;
    }

    void start() {
        LOG_MSG(LogLevel::Debug, "(%s) start", desc().c_str());
        asio::post(_worker_strand, [self = shared_from_this()]() mutable {
            self->_listener->on_connect(self);
            self->start_read();
        });
    }

    bool stop() {
        return try_stop(0);
    }

    void post(std::unique_ptr<tcp_message> msg) {
        write_data(std::move(msg));
    }

    void on_tick(TimeUnitSec now) {
        if (_stop.load()) return;
        if (_server_side) {
            if (_hbhelper.isExceed(now)) {
                LOG_MSG(LogLevel::Debug, "(%s) expired, stop it", desc().c_str());
                try_stop(2);
            }
        } else {
            if (_hbhelper.isExceed(now)) {
                LOG_MSG(LogLevel::Trace, "(%s) send heartbeat", desc().c_str());
                std::unique_ptr<tcp_message> hb(_protocol->genHeartbeat());
                post(std::move(hb));
            }
        }
    }

    std::string desc() {
        std::lock_guard<std::mutex> lock(_lockDesc);
        if (!_desc.empty()) return _desc;
        try {
            if (_socket.is_open()) {
                std::string addr = _socket.remote_endpoint().address().to_string();
                int port = _socket.remote_endpoint().port();
                char szDesc[128] = { 0 };
                snprintf(szDesc, sizeof(szDesc), "tcpsock_user:peer=%s|port:%d", addr.c_str(), port);
                _desc = szDesc;
            }
        } catch (asio::system_error& e) {
            LOG_ERR_MSG("cache desc() error: %s", e.what());
        }
        return _desc.empty() ? "invalid desc" : _desc;
    }

private:
    void start_read() {
        asio::post(_io_strand, [self = shared_from_this()]() mutable {
            self->_hbhelper.update();
            self->read_header();
        });
    }

    void read_header() {
        if (_stop.load()) return;
        uint32_t hdrSz = _protocol->headerSize();
        auto header = std::make_shared<std::string>(hdrSz, '\0');
        asio::mutable_buffer buf = asio::buffer(header->data(), hdrSz);
        asio::async_read(_socket, buf, asio::bind_executor(_io_strand,
            [self = shared_from_this(), header](const std::error_code ec, std::size_t bytes_trans) mutable {
                self->read_body(header, ec, bytes_trans);
            }));
    }

    void read_body(std::shared_ptr<std::string> header, const std::error_code ec, std::size_t bytes_trans) {
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                // normal close
                return;
            } else if (ec == asio::error::try_again || ec == asio::error::interrupted) {
                // never occur
                LOG_ERR_MSG("(%s) read_body error(unexpected): %s", desc().c_str(), ec.message().c_str());
                handle_error(nullptr, ec);
                return;
            } else {
                LOG_ERR_MSG("(%s) read_body error: %s", desc().c_str(), ec.message().c_str());
                handle_error(nullptr, ec);
                return;
            }
        }

        if (_stop.load()) return;
        
        tcp_message tmp;
        tmp.data = std::move(*header);
        uint32_t bodySz = _protocol->bodySize(&tmp);
        if (bodySz > _protocol->MaxBodySize()) {
            LOG_ERR_MSG("(%s) invalid body size: %u", desc().c_str(), bodySz);
            handle_error(nullptr, asio::error::message_size);
            return;
        }

        uint32_t hdrSz = tmp.data.size();
        auto msg = std::make_unique<tcp_message>();
        msg->data = std::move(tmp.data);
        msg->data.resize(hdrSz + bodySz);

        if (bodySz > 0) {
            asio::mutable_buffer buf = asio::buffer(msg->data.data() + hdrSz, bodySz);
            asio::async_read(_socket, buf, asio::bind_executor(_io_strand,
                [self = shared_from_this(), msg = std::move(msg)](const std::error_code ec, std::size_t bytes_trans) mutable {
                    self->handle_read(std::move(msg), ec, bytes_trans);
                }));
        } else {
            handle_read(std::move(msg), ec, 0);
        }
    }

    void handle_read(std::unique_ptr<tcp_message> msg, const std::error_code ec, std::size_t bytes_trans) {
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                // normal close
                return;
            } else if (ec == asio::error::try_again || ec == asio::error::interrupted) {
                // never occur
                LOG_ERR_MSG("(%s) handle_read error(unexpected): %s", desc().c_str(), ec.message().c_str());
                handle_error(nullptr, ec);
                return;
            } else {
                LOG_ERR_MSG("(%s) handle_read error: %s", desc().c_str(), ec.message().c_str());
                handle_error(std::move(msg), ec);
                return;
            }
        }

        if (_stop.load()) return;

        if (_server_side) _hbhelper.update();
        LOG_MSG(LogLevel::TraceMore, "(%s) handle_read, msg=%s", desc().c_str(), msg->desc().c_str());
        if (!is_heartbeat(msg.get())) {
            if (_readQueue.size() < MaxRdQueueSize) {
                _readQueue.push_back(std::move(msg));
                schedule_dispatch();
            } else {
                LOG_ERR_MSG("(%s) read queue overflow, dropping message(%s)", desc().c_str(), msg->desc().c_str());
            }
        } else {
            LOG_MSG(LogLevel::TraceMore, "(%s) Heartbeat received", desc().c_str());
        }
        read_header();
    }

    void schedule_dispatch() {
        if (_pending) return;
        _pending = true;
        asio::post(_io_strand, [self = shared_from_this()]() {
            auto batch = std::make_unique<std::deque<std::unique_ptr<tcp_message>>>();
            self->_readQueue.swap(*batch);

            while (!batch->empty()) {
                auto chunk = std::make_unique<std::deque<std::unique_ptr<tcp_message>>>();
                int count = 0;
                while (!batch->empty() && count < MaxBatchSize) {
                    chunk->push_back(std::move(batch->front()));
                    batch->pop_front();
                    ++count;
                }
                asio::post(self->_worker_strand, [self, chunk = std::move(chunk)]() mutable {
                    while (!chunk->empty()) {
                        auto msg = std::move(chunk->front());
                        chunk->pop_front();
                        self->_listener->on_message(self, std::move(msg));
                    }
                });
            }
            self->_pending = false;
        });
    }

    void handle_error(std::unique_ptr<tcp_message> msg, const std::error_code ec) {
        if (!try_stop(1)) return; // already stopped

        // if manually stop(), on_disconnect would not fire
        asio::post(_worker_strand, [self = shared_from_this()]() mutable {
            self->_listener->on_disconnect(self);
        });
    }

    bool try_stop(int source) {
        bool expected = false;
        if (!_stop.compare_exchange_strong(expected, true)) {
            LOG_MSG(LogLevel::Debug, "(%s) already stop, source=%d", desc().c_str(), source);
            return false;
        }
        try {
            _socket.shutdown(tcp::socket::shutdown_both);
            _socket.close();
        } catch (const asio::system_error& e) {
            LOG_ERR_MSG("(%s) try_stop error: %s, source=%d", desc().c_str(), e.what(), source);
        }
        LOG_MSG(LogLevel::Debug, "(%s) stopped, source=%d", desc().c_str(), source);
        return true;
    }

    void write_data(std::unique_ptr<tcp_message> msg) {
        asio::post(_io_strand, [self = shared_from_this(), message = std::move(msg)]() mutable {
            if (self->_writeQueue.size() < MaxWrQueueSize) {
                bool wasEmpty = self->_writeQueue.empty();
                self->_writeQueue.push_back(std::move(message));
                if (wasEmpty) self->do_write();
            } else {
                LOG_ERR_MSG("(%s) write queue overflow, dropping message(%s)", self->desc().c_str(), message->desc().c_str());
            }
        });
    }

    void do_write() {
        if (_writeQueue.empty() || _stop.load()) return;
        auto& msg = _writeQueue.front();
        asio::mutable_buffer buf = asio::buffer(msg->data.data(), msg->data.size());
        asio::async_write(_socket, buf, asio::bind_executor(_io_strand,
            [self = shared_from_this()](const std::error_code ec, std::size_t bytes_trans) mutable {
                self->handle_write(ec, bytes_trans);
            }));
    }

    void handle_write(const std::error_code ec, std::size_t bytes_trans) {
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                // normal close
                return;
            } else if (ec == asio::error::try_again || ec == asio::error::interrupted) {
                // never occur
                LOG_ERR_MSG("(%s) handle_write error(unexpected): %s", desc().c_str(), ec.message().c_str());
                handle_error(nullptr, ec);
                return;
            } else {
                LOG_ERR_MSG("(%s) handle_write error: %s", desc().c_str(), ec.message().c_str());
                handle_error(nullptr, ec);
                return;
            }
        }

        if (!_writeQueue.empty()) {
            auto msg = std::move(_writeQueue.front());
            LOG_MSG(LogLevel::TraceMore, "(%s) handle_write, msg=%s", desc().c_str(), msg->desc().c_str());
            _writeQueue.pop_front();
            if (!_server_side) _hbhelper.update();
        }

        if (!_writeQueue.empty() && !_stop.load()) {
            do_write();
        }
    }

    bool is_heartbeat(const tcp_message* msg) {
        return _protocol->isHeartbeat(msg);
    }

protected:
    asio_worker::work_strand  _io_strand;
    asio_worker::work_strand  _worker_strand;
    tcp::socket               _socket;
    heartbeat_helper          _hbhelper;
    std::deque<std::unique_ptr<tcp_message>> _writeQueue;
    std::deque<std::unique_ptr<tcp_message>> _readQueue;
    bool                      _pending{false};
    std::atomic<bool>         _stop;
    bool                      _server_side;
    tcpsock_listener*         _listener;
    tcp_message_protocol*     _protocol;
    std::string               _desc;
    std::mutex                _lockDesc;

#ifdef NDEBUG
    enum { UserTimeout = 90, HeartbeatTime = 30 }; // for release
#else
    enum { UserTimeout = 9,  HeartbeatTime = 3 }; // for debug
#endif
    enum { MaxRdQueueSize = 256, MaxWrQueueSize = 256, MaxBatchSize = 32 };
};

using user_queue = std::unordered_set<std::shared_ptr<tcpsock_user>>;
class tcpsock_server : public std::enable_shared_from_this<tcpsock_server>, public tcpsock_listener {
public:
    tcpsock_server(const std::string& address, unsigned short port, tcp_message_protocol* protocol) : _ioProcessor("IO"),
        _acceptor(_ioProcessor.get(0)), _timer(_ioProcessor.get(0)), _address(address), _port(port), _protocol(protocol) {
    }

    virtual ~tcpsock_server() = default;

    void start(int workerNum = std::thread::hardware_concurrency()) {
        workerNum = std::max(workerNum, 1);
        workerNum = std::min(workerNum, 32);
        LOG_MSG(LogLevel::Debug, "%s start, workers=%d", desc().c_str(), workerNum);
        _worker = std::make_unique<asio_worker>("BIZ", workerNum);
        _worker->start();
        if (!do_listen()) {
            do_accept();
            LOG_MSG(LogLevel::Trace, "%s start, do_accept start", desc().c_str());
        }
        _ioProcessor.start();
        fire_timer();
        LOG_MSG(LogLevel::Trace, "%s start, timer start", desc().c_str());
    }

    void serveUtilStop() {
        _ioProcessor.join();
        LOG_MSG(LogLevel::Trace, "%s IO stopped", desc().c_str());
        _worker->join();
        LOG_MSG(LogLevel::Debug, "%s BIZ stopped", desc().c_str());
    }

    void stop(bool join = false) {
        try {
            _acceptor.cancel();
            _timer.cancel();
        } catch (const asio::system_error& e) {
            LOG_ERR_MSG("%s timer cancel error: %s", desc().c_str(), e.what());
        }
        LOG_MSG(LogLevel::Trace, "%s timer stopped", desc().c_str());
        
        user_queue users;
        {
            std::lock_guard<std::mutex> lock(_lockUQ);
            users.swap(_userQueue);
        }
        for (auto& user : users) {
            try {
                user->stop();
            } catch (const std::exception& e) {
                LOG_ERR_MSG("%s user(%s) stop error: %s", desc().c_str(), user->desc().c_str(), e.what());
            }
        }
        LOG_MSG(LogLevel::Trace, "%s pending users stopped: %d", desc().c_str(), users.size());
        users.clear();

        try {
            _ioProcessor.stop();
        } catch (const asio::system_error& e) {
            LOG_ERR_MSG("%s io processor stop error: %s", desc().c_str(), e.what());
        }        
        try {
            _worker->stop();
        } catch (const std::exception& e) {
            LOG_ERR_MSG("%s worker stop error: %s", desc().c_str(), e.what());
        }

        if (join) serveUtilStop();
    }

public:
    virtual void on_connect(std::shared_ptr<tcpsock_user> user) {
    }

    virtual void on_message(std::shared_ptr<tcpsock_user> user, std::unique_ptr<tcp_message> msg) {
    }

    virtual void on_disconnect(std::shared_ptr<tcpsock_user> user) {
        std::lock_guard<std::mutex> lock(_lockUQ);
        LOG_MSG(LogLevel::Debug, "%s on_disconnect(%s)", desc().c_str(), user->desc().c_str());
        _userQueue.erase(user);
    }

private:
    int do_listen() {
		try {
            LOG_MSG(LogLevel::Debug, "%s do_listen start", desc().c_str());
            tcp::resolver resolver(_ioProcessor.get(0));
            tcp::resolver::results_type resolve_result = resolver.resolve(_address, std::to_string(_port));
            tcp::endpoint endpoint = *resolve_result.begin();

            _acceptor.open(endpoint.protocol());
            _acceptor.set_option(tcp::acceptor::reuse_address(true));
#ifndef _WIN32
            {
                int reuse = 1;
                ::setsockopt(_acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
            }
#endif
            _acceptor.bind(endpoint);
            _acceptor.listen();
		} catch (asio::system_error& e) {
            LOG_ERR_MSG("%s do_listen error: %s", desc().c_str(), e.what());
			return -1;
		}
        LOG_MSG(LogLevel::Debug, "%s do_listen done", desc().c_str());
		return 0;
	}

    void do_accept() {
        auto user = std::make_shared<tcpsock_user>(_ioProcessor.get_strand(0), _worker->get_strand(), true, _protocol.get(), (tcpsock_listener*)this);
        _acceptor.async_accept(user->sock(), [user, self = shared_from_this()](const std::error_code ec) {
            self->handle_accept(user, ec);
        });
    }

    void handle_accept(std::shared_ptr<tcpsock_user> user, const std::error_code ec) {
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                // normal close
            } else if (ec == asio::error::try_again || ec == asio::error::interrupted) {
                do_accept();
            } else {
                LOG_ERR_MSG("%s handle_accept error: %s", desc().c_str(), ec.message().c_str());
            }
            return;
        }

        // success
        LOG_MSG(LogLevel::Debug, "%s handle_accept success, user=(%s)", desc().c_str(), user->desc().c_str());
        {
            std::lock_guard<std::mutex> lock(_lockUQ);
            _userQueue.insert(user);
        }
        user->start();
        do_accept();
    }

    void fire_timer() {
		_timer.expires_after(std::chrono::seconds(ScanInterval));
		_timer.async_wait([self = shared_from_this()](const std::error_code& ec) {
            self->on_timer(ec);
        });
	}

	void on_timer(const std::error_code& ec) {
        if (ec) {
            LOG_MSG(LogLevel::Warn, "(%s) on_timer error: %s", desc().c_str(), ec.message().c_str());
            return;
        }
        int size = 0;
        TimeUnitSec now = socket_utils::currentTimeSecs();
        {
            std::lock_guard<std::mutex> lock(_lockUQ);
            size = _userQueue.size();
            for (auto& user : _userQueue) {
                user->on_tick(now);
            }
        }
        if (++_scanCount >= LogEveryNScan) {
            LOG_MSG(LogLevel::Debug, "%s: current users=%d", desc().c_str(), size);
            _scanCount = 0;
        }
		fire_timer();
	}

    std::string desc() {
        return "asio_tcp_server";
    }

private:
    enum { ScanInterval = 5, LogEveryNScan = 12 };
    asio_worker                         _ioProcessor;
    tcp::acceptor                       _acceptor;
    asio::steady_timer                  _timer;
    std::string                         _address;
    unsigned short                      _port;
    std::unique_ptr<asio_worker>        _worker;
    user_queue                          _userQueue;
    std::mutex                          _lockUQ;
    int                                 _scanCount{0};
    std::unique_ptr<tcp_message_protocol> _protocol;
};

class tcpsock_client {
public:
    tcpsock_client(const std::string& host, int port, tcp_message_protocol* protocol, std::unique_ptr<tcpsock_listener> listener)
        : _worker(host+"-"+std::to_string(port), 2), _listener(std::move(listener)), _host(host), _port(port), _protocol(protocol),
          _hb_timer(_worker.get(0)) {
        assert(_listener != nullptr);
    }

    virtual ~tcpsock_client() = default;

    void start() {
        _worker.start();
        start_hb_timer();
        LOG_MSG(LogLevel::Debug, "%s start", desc().c_str());
    }

    void stop() {
        _hb_timer.cancel();
        _worker.stop();
        _worker.join();
        LOG_MSG(LogLevel::Debug, "%s stopped", desc().c_str());
    }

    int connect() {
        if (_user) {
            _user->stop();
            _user.reset();
        }
		try {
            _user = std::make_shared<tcpsock_user>(_worker.get_strand(0), _worker.get_strand(1), false, _protocol.get(), _listener.get());
            tcp::resolver resolver(_worker.get(0));
			tcp::resolver::results_type resolve_result = resolver.resolve(_host, std::to_string(_port));
			asio::connect(_user->sock(), resolve_result);
		} catch (asio::system_error& e) {
			LOG_ERR_MSG("%s connect error(%s), target=(%s:%d)", desc().c_str(), e.what(), _host.c_str(), _port);
            _user.reset();
			return -1;
		}
		LOG_MSG(LogLevel::Debug, "%s connect success, target=(%s:%d)", desc().c_str(), _host.c_str(), _port);
        _user->start();
		return 0;
    }

    void async_connect(std::function<void(int rc)> handler, int timeout_sec = 2) {
        if (_user) {
            _user->stop();
            _user.reset();
        }

        auto done  = std::make_shared<bool>(false);
        auto timer = std::make_shared<asio::steady_timer>(_worker.get(0));

        timer->expires_after(std::chrono::seconds(timeout_sec));
        timer->async_wait([this, done](const std::error_code& ec) {
            if (ec) return;
            if (*done) return;
            *done = true;
            LOG_ERR_MSG("%s async_connect timeout", desc().c_str());
            if (_user) _user->sock().cancel();
        });

        auto resolver = std::make_shared<tcp::resolver>(_worker.get(0));
        resolver->async_resolve(_host, std::to_string(_port),
            [this, done, timer, resolver, handler = std::move(handler)](const std::error_code& ec, tcp::resolver::results_type results) mutable {
                if (*done) return;
                if (ec) {
                    timer->cancel();
                    *done = true;
                    LOG_ERR_MSG("%s async_connect resolve error(%s)", desc().c_str(), ec.message().c_str());
                    handler(-1);
                    return;
                }
                _user = std::make_shared<tcpsock_user>(_worker.get_strand(0), _worker.get_strand(1), false, _protocol.get(), _listener.get());
                asio::async_connect(_user->sock(), results,
                    [this, done, timer, handler = std::move(handler)](const std::error_code& ec, const tcp::endpoint&) mutable {
                        timer->cancel();
                        if (*done) return;
                        *done = true;
                        if (ec) {
                            LOG_ERR_MSG("%s async_connect error(%s)", desc().c_str(), ec.message().c_str());
                            _user.reset();
                            handler(-2);
                            return;
                        }
                        LOG_MSG(LogLevel::Debug, "%s async_connect success, target=(%s:%d)", desc().c_str(), _host.c_str(), _port);
                        _user->start();
                        handler(0);
                    });
            });
    }

    void disconnect() {
        if (_user && _user->stop()) {
            LOG_MSG(LogLevel::Debug, "%s disconnect, target=(%s:%d)", desc().c_str(), _host.c_str(), _port);
        }
    }

    void post(std::unique_ptr<tcp_message> msg) {
        if (_user) _user->post(std::move(msg));
    }

    std::string desc() {
        return "asio_tcp_client";
    }

private:

#ifdef NDEBUG
    enum { ScanInterval = 30 };
#else
    enum { ScanInterval = 3 };
#endif

    void start_hb_timer() {
        _hb_timer.expires_after(std::chrono::seconds(ScanInterval));
        _hb_timer.async_wait([this](const std::error_code& ec) {
            if (ec) return;
            TimeUnitSec now = socket_utils::currentTimeSecs();
            if (_user) _user->on_tick(now);
            start_hb_timer();
        });
    }

    asio_worker                       _worker;
    std::unique_ptr<tcpsock_listener> _listener;
    std::shared_ptr<tcpsock_user>     _user;
    std::string                       _host;
    int                               _port;
    asio::steady_timer                _hb_timer;
    std::unique_ptr<tcp_message_protocol> _protocol;
};

}
