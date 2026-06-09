#ifndef RANDOM_UTILS_HPP
#define RANDOM_UTILS_HPP

#include <string>
#include <random>
#include <limits>
#include <utility>

namespace fszm {

enum RandomStringType {
	Numbers,
	Letters,
	CapitalLetters,
    LowercaseLetters,
	NumbersAndLetters,
	NumbersAndCapitalLetters,
	NumbersAndLowercaseLetters,
    VisibleCharsThin,
	VisibleChars,
};

class random_utils {
public:
    static int randomNumber() {
        std::mt19937& engine = get_thread_local_engine();
        std::uniform_int_distribution<int> dist(0, std::numeric_limits<int>::max());
        return dist(engine);
    }

    static int randomNumber(int range) {
        if (range <= 0) return 0;
        std::mt19937& engine = get_thread_local_engine();
        std::uniform_int_distribution<int> dist(0, range - 1);
        return dist(engine);
    }

    static int randomNumber(int min, int max) {
        if (min > max) std::swap(min, max);
        std::mt19937& engine = get_thread_local_engine();
        std::uniform_int_distribution<int> dist(min, max);
        return dist(engine);
    }

	static std::string randomString(RandomStringType st, int length) {
		std::string result;
		switch (st) {
            case Numbers:
                result = randomString(NUMBERS, length);
                break;
            case Letters:
                result = randomString(LETTERS, length);
                break;
            case CapitalLetters:
                result = randomString(CAPITAL_LETTERS, length);
                break;
            case LowercaseLetters:
                result = randomString(LOWER_CASE_LETTERS, length);
                break;
            case NumbersAndLetters:
                result = randomString(NUMBERS_AND_LETTERS, length);
                break;
            case NumbersAndCapitalLetters:
                result = randomString(NUMBERS_AND_CAPLETTERS, length);
                break;
            case NumbersAndLowercaseLetters:
                result = randomString(NUMBERS_AND_LOWLETTERS, length);
                break;
            case VisibleCharsThin:
                result = randomString(VISIBLE_CHARS_THIN, length);
                break;
            case VisibleChars:
                result = randomString(VISIBLE_CHARS, length);
                break;
            default:
                break;
		}
		return result;
	}

	static std::string randomString(const std::string& source, int length) {
		std::string result;
		if (source.empty() || length <= 0) return result;
        result.reserve(length);
		int src_len = static_cast<int>(source.length());
		for (int i = 0; i < length; ++i) {
			int n = randomNumber(src_len);
			result.push_back(source.at(n));
		}
		return result;
	}

private:
    static std::mt19937& get_thread_local_engine() {
        static thread_local std::mt19937 engine(std::random_device{}());
        return engine;
    }

    inline static std::string NUMBERS                = "0123456789";
    inline static std::string LETTERS                = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    inline static std::string CAPITAL_LETTERS        = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    inline static std::string LOWER_CASE_LETTERS     = "abcdefghijklmnopqrstuvwxyz";
    inline static std::string NUMBERS_AND_LETTERS    = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    inline static std::string NUMBERS_AND_CAPLETTERS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    inline static std::string NUMBERS_AND_LOWLETTERS = "0123456789abcdefghijklmnopqrstuvwxyz";
    inline static std::string VISIBLE_CHARS_THIN     = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_-+={}[]|:;,.<>?";
    inline static std::string VISIBLE_CHARS          = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_-+={}[]|:;,.<>?`\"'\\/ ";
};

}

#endif
