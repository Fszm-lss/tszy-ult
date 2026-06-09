#ifndef socket_poll_hpp
#define socket_poll_hpp

#ifdef _WIN32
#include <winsock2.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#else
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace zbf {

#define MAX_EVENT     64

struct event {
	void* ud;
	int fd;
	bool read;
	bool write;
	bool error;
	bool eof;
};

#ifdef _WIN32

// poll()-based implementation for Windows (MSYS2)

class socket_poll {
	struct fd_entry {
		int fd;
		void* ud;
		short events;  // POLLIN | POLLOUT
	};

	inline static std::map<int, std::vector<fd_entry>> _instances;
	inline static std::mutex _mutex;
	inline static int _next_id = 0;

	static fd_entry* _find_fd(std::vector<fd_entry>& entries, int fd) {
		for (auto& e : entries) {
			if (e.fd == fd) return &e;
		}
		return nullptr;
	}

public:
	static bool sp_invalid(int efd) {
		return efd == 0;
	}

	static int sp_create() {
		std::lock_guard<std::mutex> lock(_mutex);
		int id = ++_next_id;
		_instances[id] = {};
		return id;
	}

	static void sp_release(int efd) {
		std::lock_guard<std::mutex> lock(_mutex);
		_instances.erase(efd);
	}

	static int sp_add(int efd, int fd, bool write_enable = false) {
		return sp_add_ex(efd, fd, nullptr, write_enable);
	}

	static int sp_add_ex(int efd, int fd, void* ud, bool write_enable = false) {
		std::lock_guard<std::mutex> lock(_mutex);
		auto& entries = _instances[efd];
		if (_find_fd(entries, fd)) return 1;  // already registered
		entries.push_back({fd, ud, (short)(POLLIN | (write_enable ? POLLOUT : 0))});
		return 0;
	}

	static void sp_del(int efd, int fd) {
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _instances.find(efd);
		if (it == _instances.end()) return;
		auto& entries = it->second;
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[fd](const fd_entry& e) { return e.fd == fd; }), entries.end());
	}

	static int sp_enable(int efd, int fd, bool read_enable, bool write_enable) {
		return sp_enable_ex(efd, fd, nullptr, read_enable, write_enable);
	}

	static int sp_enable_ex(int efd, int fd, void* ud, bool read_enable, bool write_enable) {
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _instances.find(efd);
		if (it == _instances.end()) return 1;
		auto* entry = _find_fd(it->second, fd);
		if (!entry) return 1;
		entry->events = (short)((read_enable ? POLLIN : 0) | (write_enable ? POLLOUT : 0));
		if (ud) entry->ud = ud;
		return 0;
	}

	static int sp_wait(int efd, struct event* e, int max, int timeout) {
		if (max > MAX_EVENT) max = MAX_EVENT;

		std::vector<fd_entry> snapshot;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			auto it = _instances.find(efd);
			if (it == _instances.end()) return -1;
			snapshot = it->second;
		}

		int nfds = (int)snapshot.size();
		if (nfds == 0) {
			// WSAPoll with nfds=0 returns WSAEINVAL, sleep instead
			if (timeout > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
			return 0;
		}

		std::vector<WSAPOLLFD> pfd(nfds);
		for (int i = 0; i < nfds; i++) {
			pfd[i].fd = snapshot[i].fd;
			pfd[i].events = snapshot[i].events;
			pfd[i].revents = 0;
		}

		int n = WSAPoll(pfd.data(), nfds, timeout > 0 ? timeout : -1);
		if (n <= 0) return n;

		int count = 0;
		for (int i = 0; i < nfds && count < max; i++) {
			if (pfd[i].revents == 0) continue;
			short rev = pfd[i].revents;
			e[count].ud = snapshot[i].ud;
			e[count].fd = snapshot[i].fd;
			e[count].read = (rev & POLLIN) != 0;
			e[count].write = (rev & POLLOUT) != 0;
			e[count].error = (rev & POLLERR) != 0;
			e[count].eof = (rev & POLLHUP) != 0;
			count++;
		}
		return count;
	}
};

#else

// epoll-based implementation for Linux

class socket_poll {
public:
	static bool sp_invalid(int efd) {
		return efd == -1;
	}

	static int sp_create() {
		return epoll_create1(0);
	}

	static void sp_release(int efd) {
		close(efd);
	}

	static int sp_add(int efd, int fd, bool write_enable = false) {
		struct epoll_event ev;
		ev.events = EPOLLIN;
		if (write_enable) ev.events |= EPOLLOUT;
		ev.data.fd = fd;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
			return 1;
		}
		return 0;
	}

	static int sp_add_ex(int efd, int fd, void* ud, bool write_enable = false) {
		struct epoll_event ev;
		ev.events = EPOLLIN;
		if (write_enable) ev.events |= EPOLLOUT;
		ev.data.ptr = ud;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
			return 1;
		}
		return 0;
	}

	static void sp_del(int efd, int fd) {
		epoll_ctl(efd, EPOLL_CTL_DEL, fd, nullptr);
	}

	static int sp_enable(int efd, int fd, bool read_enable, bool write_enable) {
		struct epoll_event ev;
		ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
		ev.data.fd = fd;
		if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev) == -1) {
			return 1;
		}
		return 0;
	}

	static int sp_enable_ex(int efd, int fd, void* ud, bool read_enable, bool write_enable) {
		struct epoll_event ev;
		ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
		ev.data.ptr = ud;
		if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev) == -1) {
			return 1;
		}
		return 0;
	}

	// ud or fd can only check one
	static int sp_wait(int efd, struct event* e, int max, int timeout) {
		if (max > MAX_EVENT) max = MAX_EVENT;
		struct epoll_event ev[max];
		int n = epoll_wait(efd, ev, max, timeout);
		int i;
		for (i = 0; i < n; i++) {
			e[i].ud = ev[i].data.ptr;
			e[i].fd = ev[i].data.fd;
			unsigned flag = ev[i].events;
			e[i].write = (flag & EPOLLOUT) != 0;
			e[i].read = (flag & EPOLLIN) != 0;
			e[i].error = (flag & EPOLLERR) != 0;
			e[i].eof = (flag & (EPOLLHUP|EPOLLRDHUP)) != 0;
		}
		return n;
	}
};

#endif

}

#endif  // socket_poll_hpp
