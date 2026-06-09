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

        std::string title = levelDesc(level);
        snprintf(szText, sizeof(szText), "%s ", title.c_str());
        size_t len = title.length() + 1;
        
        struct timespec ts;
        if (!clock_gettime(CLOCK_REALTIME, &ts)) {
            struct tm* timeinfo = localtime(&ts.tv_sec);
            size_t l = strftime(szText+len, sizeof(szText)-len, "%Y/%m/%d %H:%M:%S", timeinfo);
            len += l;
            snprintf(szText+len, sizeof(szText)-len, ".%03ld ", ts.tv_nsec/1000000);
            len += 5;
        }

        snprintf(szText+len, sizeof(szText)-len, "%07d ", getPrintId());
        len += 8;

		va_list args;
		va_start(args, fmt);
		vsnprintf(szText+len, sizeof(szText)-len, fmt, args);
		va_end(args);
        
        if (_file) {
            fprintf(_file, "%s\n", szText);
            fflush(_file);
        }
        // printf("%s\n", szText);
        fprintf(stdout, "%s\n", szText);
        fflush(stdout);
    }

    static std::string levelDesc(unsigned int level) {
        std::string desc;
        switch (level)
        {
        case Debug:
            desc = "[Debug]";
            break;
        case Error:
            desc = "[Error]";
            break;
        case Info:
            desc = "[ Info]";
            break;
        case Warn:
            desc = "[ Warn]";
            break;
        case Trace:
            desc = "[Trace]";
            break;
        case TraceMore:
            desc = "[ More]";
            break;
        case Check:
            desc = "[Check]";
            break;
        case Fatal:
            desc = "[Fatal]";
            break;

        default:
            desc = "[Trace]";
            break;
        }
        return desc;
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
        LOG_MSG(LogLevel::Info, "logger open, path=%s, level=%s", path, levelDesc(level).c_str());
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

    static std::string createLogPath(bool shortMode = true) {
        char szBuf[64] = {0};
        time_t now;
        std::time(&now);
        struct tm* timeinfo = localtime(&now);
        if (shortMode)
            strftime(szBuf, sizeof(szBuf), "%Y%m%d", timeinfo);
        else
            strftime(szBuf, sizeof(szBuf), "%Y%m%d_%H%M%S", timeinfo);
        return getModulePath() + std::string("_") + std::string(szBuf) + std::string(".log");
    }

public:
    inline static  unsigned int log_level{LogLevel::Trace};
    inline static  bool         enable_check{false};
private:
    inline static  FILE*        _file{nullptr};
    inline static  std::mutex   _lockLOG;
    inline static  std::unordered_map<int, short> _thd2id;
};

}

#endif
