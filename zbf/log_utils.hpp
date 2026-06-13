#ifndef log_utils_hpp
#define log_utils_hpp

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <climits>
#include <string>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace zbf {

// ({}) is gcc extend, also can use inline
// Windows+MSYS2: __FILE__ uses '/', but runtime path strings may use '\', so check both
#ifdef _WIN32
#define SHORT_FILENAME(x) ({ const char* p = std::strrchr(x, '/'); if (!p) p = std::strrchr(x, '\\'); p ? p + 1 : x; })
#else
#define SHORT_FILENAME(x) ({ const char* p = std::strrchr(x, '/'); p ? p + 1 : x; })
#endif

#define LOG_MSG(level, fmt, args...) zbf::log_utils::log(level, "%s %d: " fmt, SHORT_FILENAME(__FILE__), __LINE__, ##args)
#define LOG_ERR_MSG(fmt, args...)    LOG_MSG(zbf::LogLevel::Error, fmt, ##args)
#define LOG_CHK_MSG(fmt, args...)    if (log_utils::enable_check) { LOG_MSG(zbf::LogLevel::Check, fmt, ##args); }
#define USE_SHORTID  0

enum LogLevel {
	Fatal     = 0,
    Check     = 1,
	Error     = 2,
	Warn      = 3,
	Info      = 4,
	Debug     = 5,
	Trace     = 6,
    TraceMore = 7,
};

class log_utils {
public:
    static void log(unsigned int level, const char* fmt, ...) {
        if (level > log_level) return;

        std::lock_guard<std::mutex> lock(_lockLOG);
        char szText[2048];
        memset(szText, 0, sizeof(szText));

        const char* title = levelDesc(level);
        size_t len = strlen(title);
        memcpy(szText, title, len);
        szText[len++] = ' ';
        
        // Cache time string per second to avoid localtime/strftime on every log line
        struct timespec ts;
        if (!clock_gettime(CLOCK_REALTIME, &ts)) {
            if (ts.tv_sec != _lastTimeSec) {
                struct tm timeinfo;
#ifdef _WIN32
                localtime_s(&timeinfo, &ts.tv_sec);
#else
                localtime_r(&ts.tv_sec, &timeinfo);
#endif
                _cachedTimeLen = strftime(_cachedTimeStr, sizeof(_cachedTimeStr), "%Y/%m/%d %H:%M:%S", &timeinfo);
                _lastTimeSec = ts.tv_sec;
            }
            memcpy(szText + len, _cachedTimeStr, _cachedTimeLen);
            len += _cachedTimeLen;
            len += snprintf(szText+len, sizeof(szText)-len, ".%03ld ", ts.tv_nsec/1000000);
        }

        len += snprintf(szText+len, sizeof(szText)-len, "%07d ", getPrintId());

		va_list args;
		va_start(args, fmt);
		vsnprintf(szText+len, sizeof(szText)-len, fmt, args);
		va_end(args);
        
        if (_file) {
            fprintf(_file, "%s\n", szText);
            fflush(_file);
        }
#ifndef NDEBUG
        fprintf(stdout, "%s\n", szText);
        fflush(stdout);
#endif
    }

    static const char* levelDesc(unsigned int level) {
        switch (level)
        {
        case Debug:     return "[Debug]";
        case Error:     return "[Error]";
        case Info:      return "[ Info]";
        case Warn:      return "[ Warn]";
        case Trace:     return "[Trace]";
        case TraceMore: return "[ More]";
        case Check:     return "[Check]";
        case Fatal:     return "[Fatal]";
        default:        return "[Trace]";
        }
    }

    static int getThreadId() {
#ifdef _WIN32
        return (int)GetCurrentThreadId();
#else
        return gettid();
#endif
    }

    static int getPrintId() {
#if USE_SHORTID
        return getShortId();
#else
        return getThreadId();
#endif
    }

    static int getProcessId() {
#ifdef _WIN32
        return (int)GetCurrentProcessId();
#else
        return getpid();
#endif
    }

    static void registerThreadId(short id) {
        std::lock_guard<std::mutex> lock(_lockLOG);
        _thd2id[getThreadId()] = id;
    }
    
    static int getShortId() {
        auto it = _thd2id.find(getThreadId());
        if (it != _thd2id.end())
            return it->second;
        return 0;
    }

    static int open(const char* path, unsigned int level = LogLevel::Trace, bool truncate = false) {
        if (_file) return 1;
        if (!path) return -1;

        const char* mode = truncate ? "w" : "a";
        _file = fopen(path, mode);
        if (!_file) return -2;

        log_level = level;
        LOG_MSG(LogLevel::Info, "logger open, path=%s, level=%s", path, levelDesc(level));
        return 0;
    }

    static void close() {
        LOG_MSG(LogLevel::Info, "logger close");
        if (_file) {
            fclose(_file);
            _file = nullptr;
        }
    }

    static void enableChkLog(bool enable) {
        enable_check = enable;
    }

    // from misc_utils (merged)
    static std::string getModulePath() {
#ifdef _WIN32
        char szFilePath[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameA(NULL, szFilePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) return std::string(szFilePath);
        return "";
#else
        char szFilePath[PATH_MAX] = {0};
        int len = readlink("/proc/self/exe", szFilePath, PATH_MAX);
        if (len > 0) {
            return std::string(szFilePath);
        }
        return "";
#endif
    }

    static std::string createLogPath(bool appendPid = true) {
        if (appendPid) {
            return getModulePath() + std::string("-") + std::to_string(getProcessId()) + std::string(".log");
        }
        return getModulePath() + std::string(".log");
    }

public:
    inline static  unsigned int log_level{LogLevel::Trace};
    inline static  bool         enable_check{false};
private:
    inline static  FILE*        _file{nullptr};
    inline static  std::mutex   _lockLOG;
    inline static  std::unordered_map<int, short> _thd2id;
    inline static  time_t       _lastTimeSec{0};
    inline static  char         _cachedTimeStr[32] = {};
    inline static  size_t       _cachedTimeLen{0};
};

}

#endif
