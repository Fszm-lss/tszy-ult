#ifndef signal_utils_hpp
#define signal_utils_hpp

#include <initializer_list>
#include <signal.h>
#include <cstdio>

#ifdef _WIN32
#ifndef SIGPIPE
#define SIGPIPE 0
#endif
#else
#include <string.h>
#endif

namespace zbf {

typedef void (*SIGNAL_HANDLER)(int);

class signal_utils {
public:
    static void handleSignal(SIGNAL_HANDLER onSignal, int sig) {
        ::signal(sig, onSignal);
    }

    static void handleSignal(SIGNAL_HANDLER onSignal, std::initializer_list<int> sigs) {
        for (int sig : sigs) {
            handleSignal(onSignal, sig);
        }
    }

    static void ignoreSignal(int sig) {
#ifdef _WIN32
        if (sig == SIGPIPE) return;  // SIGPIPE placeholder, no-op on Windows
#endif
        ::signal(sig, SIG_IGN);
    }

    static void ignoreSignal(std::initializer_list<int> sigs) {
        for (int sig : sigs) {
            ignoreSignal(sig);
        }
    }

    static const char* strsignal(int sig) {
        static char buf[32];
#ifdef _WIN32
        snprintf(buf, sizeof(buf), "signal %d", sig);
#else
        const char* s = ::strsignal(sig);
        if (s) return s;
        snprintf(buf, sizeof(buf), "signal %d", sig);
#endif
        return buf;
    }
};

}

#endif
