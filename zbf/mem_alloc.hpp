#ifndef mem_alloc_hpp
#define mem_alloc_hpp
// 2025-9
// set MALLOC_CONF env
// export MALLOC_CONF="background_thread:true,dirty_decay_ms:5000,muzzy_decay_ms:30000,metadata_thp:auto"
// export MALLOC_CONF="background_thread:true,dirty_decay_ms:5000,muzzy_decay_ms:30000,narenas:16,metadata_thp:auto,tcache:true,percpu_arena:percpu"
// MALLOC_CONF="stats_print:true" ./your_app

#include <cerrno>
#include <stdlib.h>
#include <mutex>
#include <typeinfo>
#include <unordered_map>

#include "log_utils.hpp"

#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#define ZBF_TRACE_MEMORY

namespace zbf {

#ifdef ZBF_TRACE_MEMORY // def ZBF_TRACE_MEMORY

#define _num2str_(x)               #x
#define _xnum2str_(x)              _num2str_(x)
#define ZBF_MALLOC(sz)             zbf::malloc (sz,    __FILE__ "_" _xnum2str_(__LINE__))
#define ZBF_REALLOC(m,sz)          zbf::realloc(m, sz, __FILE__ "_" _xnum2str_(__LINE__))
#define ZBF_CALLOC(n,sz)           zbf::calloc (n, sz, __FILE__ "_" _xnum2str_(__LINE__))
#define ZBF_FREE(m)                zbf::free   (m,     __FILE__ "_" _xnum2str_(__LINE__))


typedef std::pair<std::string, int>                        position_and_size;
inline static std::unordered_map<void*, position_and_size> s_memTrackTable;
inline static std::mutex                                   s_memttlock;


inline void logMemTrackStat(bool logDetail = true) {
    std::unordered_map<void*, position_and_size> memTrackTable;
    {
        std::lock_guard<std::mutex> lock(s_memttlock);
        memTrackTable = s_memTrackTable;
    }

    LOG_MSG(LogLevel::Debug, "#logMemTrackStat, current=%d", memTrackTable.size());
    if (logDetail) {
        for (auto item : memTrackTable) {
            position_and_size& ps = item.second;
            LOG_MSG(LogLevel::Debug, "#logMemTrackStat: [%p:%d] at %s", item.first, ps.second, ps.first.c_str());
        }
    }
}

#define _TRACK_MEM(mem, size, tag) { \
    std::lock_guard<std::mutex> lock(s_memttlock); \
    s_memTrackTable.insert(std::make_pair(mem, std::make_pair(tag, size))); \
}
#define _RETRACK_MEM(mem, new_mem, size, tag) { \
    std::lock_guard<std::mutex> lock(s_memttlock); \
    s_memTrackTable.erase(mem); \
    s_memTrackTable.insert(std::make_pair(new_mem, std::make_pair(tag, size))); \
}
#define _UNTRACK_MEM(mem) { \
    std::lock_guard<std::mutex> lock(s_memttlock); \
    s_memTrackTable.erase(mem); \
}

inline void* malloc(size_t size, const char* tag) {
    void* mem = ::malloc(size);
    if (mem) {
        LOG_CHK_MSG("[%p:%d] malloc() at %s", mem, size, tag);
        _TRACK_MEM(mem, size, tag);
        return mem;
    }
    LOG_ERR_MSG("malloc fail, size=%lu, error=%s", size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void* realloc(void* mem, size_t size, const char* tag) {
    void* new_mem = ::realloc(mem, size);
    if (new_mem) {
        LOG_CHK_MSG("[%p:%d] realloc(%p) at %s", new_mem, size, mem, tag);
        _RETRACK_MEM(mem, new_mem, size, tag);
        return new_mem;
    }
    LOG_ERR_MSG("realloc fail, size=%lu, error=%s", size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void* calloc(size_t n, size_t size, const char* tag) {
    void* mem = ::calloc(n, size);
    if (mem) {
        LOG_CHK_MSG("[%p:%dx%d] calloc() at %s", mem, n, size, tag);
        _TRACK_MEM(mem, n*size, tag);
        return mem;
    }
    LOG_ERR_MSG("calloc fail, n=%lu, size=%lu, error=%s", n, size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void free(void* mem, const char* tag) {
    if (mem == nullptr) return;
    LOG_CHK_MSG("[%p] free() at %s", mem, tag);
    _UNTRACK_MEM(mem);
    ::free(mem);
}

template <typename T>
class object_tracker {
public:
    object_tracker() {
        // self = dynamic_cast<T*>(this); // not work
        T* self = (T*) this;
        LOG_CHK_MSG("[%p:%d] construct like (%s)", self, sizeof(T), typeid(T).name());
        _TRACK_MEM((void*)self, sizeof(T), typeid(T).name());
    }

    virtual ~object_tracker() {
        T* self = (T*) this;
        LOG_CHK_MSG("[%p] destruct like (%s)", self, typeid(T).name());
        _UNTRACK_MEM((void*)self);
    }
};


#else // undef ZBF_TRACE_MEMORY
#define ZBF_MALLOC(sz)       zbf::malloc(sz)
#define ZBF_REALLOC(m,sz)    zbf::realloc(m, sz)
#define ZBF_CALLOC(n,sz)     zbf::calloc(n, sz)
#define ZBF_FREE(m)          zbf::free(m)

inline void logMemTrackStat(bool logDetail = true) {}

inline void* malloc(size_t size) {
    void* mem = ::malloc(size);
    if (mem) {
        return mem;
    }
    LOG_ERR_MSG("malloc fail, size=%lu, error=%s", size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void* realloc(void* mem, size_t size) {
    void* new_mem = ::realloc(mem, size);
    if (new_mem) {
        return new_mem;
    }
    LOG_ERR_MSG("realloc fail, size=%lu, error=%s", size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void* calloc(size_t n, size_t size) {
    void* mem = ::calloc(n, size);
    if (mem) {
        return mem;
    }
    LOG_ERR_MSG("calloc fail, n=%lu, size=%lu, error=%s", n, size, strerror(errno));
    exit(1);
    return nullptr;
}

inline void free(void* mem) {
    if (mem == nullptr) return;
    ::free(mem);
}

template <typename T>
class object_tracker {
};

#endif // ZBF_TRACE_MEMORY

#ifdef USE_JEMALLOC
inline void logJemallocStat() {
    malloc_stats_print(nullptr, nullptr, "g");
}

inline bool isJemallocActive() {
    bool active = false;
    size_t sz = sizeof(active);
    mallctl("config.debug", &active, &sz, nullptr, 0);
    return true;
}
#else
inline void logJemallocStat() {}
inline bool isJemallocActive() { return false; }
#endif

} // namespace zbf

#endif // mem_alloc_hpp

