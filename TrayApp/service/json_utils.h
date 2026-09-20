#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace JsonUtil {

constexpr size_t kMaxJsonBytes = 1024 * 1024;
constexpr int kMaxJsonDepth = 64;

class JsonValidator {
public:
    explicit JsonValidator(const std::string& input) : input_(input) {}

    bool Parse() {
        if (input_.size() > kMaxJsonBytes) return false;
        SkipWhitespace();
        if (!ParseValue()) return false;
        SkipWhitespace();
        return position_ == input_.size();
    }

private:
    void SkipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool ParseString() {
        if (position_ >= input_.size() || input_[position_] != '"') return false;
        ++position_;
        while (position_ < input_.size()) {
            unsigned char c =
                static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            if (escaped == 'u') {
                for (int i = 0; i < 4; ++i) {
                    if (position_ >= input_.size() ||
                        !std::isxdigit(static_cast<unsigned char>(
                            input_[position_]))) {
                        return false;
                    }
                    ++position_;
                }
            } else if (escaped != '"' && escaped != '\\' &&
                       escaped != '/' && escaped != 'b' &&
                       escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') {
                return false;
            }
        }
        return false;
    }

    bool ParseNumber() {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-')
            ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9')
                return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const size_t fractionStart = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       input_[position_]))) {
                ++position_;
            }
            if (position_ == fractionStart) return false;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const size_t exponentStart = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       input_[position_]))) {
                ++position_;
            }
            if (position_ == exponentStart) return false;
        }
        return position_ > start;
    }

    bool ParseArray() {
        if (position_ >= input_.size() || input_[position_] != '[')
            return false;
        if (++depth_ > kMaxJsonDepth) {
            --depth_;
            return false;
        }
        ++position_;
        SkipWhitespace();
        bool result = true;
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
        } else {
            for (;;) {
                if (!ParseValue()) {
                    result = false;
                    break;
                }
                SkipWhitespace();
                if (position_ < input_.size() && input_[position_] == ']') {
                    ++position_;
                    break;
                }
                if (position_ >= input_.size() || input_[position_++] != ',') {
                    result = false;
                    break;
                }
                SkipWhitespace();
            }
        }
        --depth_;
        return result;
    }

    bool ParseObject() {
        if (position_ >= input_.size() || input_[position_] != '{')
            return false;
        if (++depth_ > kMaxJsonDepth) {
            --depth_;
            return false;
        }
        ++position_;
        SkipWhitespace();
        bool result = true;
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
        } else {
            for (;;) {
                if (!ParseString()) {
                    result = false;
                    break;
                }
                SkipWhitespace();
                if (position_ >= input_.size() || input_[position_++] != ':' ||
                    !ParseValue()) {
                    result = false;
                    break;
                }
                SkipWhitespace();
                if (position_ < input_.size() && input_[position_] == '}') {
                    ++position_;
                    break;
                }
                if (position_ >= input_.size() || input_[position_++] != ',') {
                    result = false;
                    break;
                }
                SkipWhitespace();
            }
        }
        --depth_;
        return result;
    }

    bool ParseValue() {
        SkipWhitespace();
        if (position_ >= input_.size()) return false;
        switch (input_[position_]) {
        case '"': return ParseString();
        case '{': return ParseObject();
        case '[': return ParseArray();
        case 't':
            if (input_.compare(position_, 4, "true") == 0) {
                position_ += 4;
                return true;
            }
            return false;
        case 'f':
            if (input_.compare(position_, 5, "false") == 0) {
                position_ += 5;
                return true;
            }
            return false;
        case 'n':
            if (input_.compare(position_, 4, "null") == 0) {
                position_ += 4;
                return true;
            }
            return false;
        default:
            return ParseNumber();
        }
    }

    const std::string& input_;
    size_t position_ = 0;
    int depth_ = 0;
};

inline bool IsValidJson(const std::string& json) {
    return json.size() <= kMaxJsonBytes && JsonValidator(json).Parse();
}

inline std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
inline std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '\\': o += "\\\\"; break; case '"': o += "\\\""; break;
        case '\n': o += "\\n"; break; case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break; default: o += c;
        }
    }
    return o;
}
inline void SkipWhitespace(const std::string& json, size_t& position) {
    while (position < json.size() &&
           std::isspace(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
}

inline bool SkipJsonString(const std::string& json, size_t& position) {
    if (position >= json.size() || json[position] != '"') return false;
    ++position;
    bool escaped = false;
    while (position < json.size()) {
        const char c = json[position++];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return true;
        }
    }
    return false;
}

inline bool SkipJsonValue(const std::string& json, size_t& position) {
    SkipWhitespace(json, position);
    if (position >= json.size()) return false;
    if (json[position] == '"') return SkipJsonString(json, position);

    if (json[position] == '{' || json[position] == '[') {
        const char opening = json[position];
        const char closing = opening == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position < json.size()) {
            const char c = json[position++];
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == opening) {
                ++depth;
            } else if (c == closing && --depth == 0) {
                return true;
            }
        }
        return false;
    }

    while (position < json.size() && json[position] != ',' &&
           json[position] != '}') {
        ++position;
    }
    return position > 0;
}

inline bool FindTopLevelMember(const std::string& json, const char* key,
                               size_t& valueStart, size_t& valueEnd) {
    if (!key || !IsValidJson(json)) return false;
    size_t position = 0;
    SkipWhitespace(json, position);
    if (position >= json.size() || json[position++] != '{') return false;
    SkipWhitespace(json, position);
    if (position < json.size() && json[position] == '}') return false;

    const std::string expectedKey = key;
    for (;;) {
        const size_t keyStart = position;
        if (!SkipJsonString(json, position)) return false;
        const size_t keyEnd = position;
        SkipWhitespace(json, position);
        if (position >= json.size() || json[position++] != ':') return false;
        SkipWhitespace(json, position);
        const size_t start = position;
        if (!SkipJsonValue(json, position)) return false;
        const bool isExpected =
            keyEnd == keyStart + expectedKey.size() + 2 &&
            json.compare(keyStart + 1, expectedKey.size(), expectedKey) == 0;
        if (isExpected) {
            valueStart = start;
            valueEnd = position;
            while (valueEnd > valueStart &&
                   std::isspace(static_cast<unsigned char>(
                       json[valueEnd - 1]))) {
                --valueEnd;
            }
            return true;
        }
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == '}') return false;
        if (position >= json.size() || json[position++] != ',') return false;
        SkipWhitespace(json, position);
    }
}

inline bool DecodeJsonString(const std::string& json, size_t start,
                             size_t end, std::string& out) {
    if (start >= end || json[start] != '"') return false;
    size_t position = start + 1;
    out.clear();
    bool escaped = false;
    while (position < end) {
        const char c = json[position++];
        if (escaped) {
            switch (c) {
            case '"': case '\\': case '/': out.push_back(c); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (position + 4 > end) return false;
                for (size_t i = 0; i < 4; ++i) {
                    if (!std::isxdigit(static_cast<unsigned char>(
                            json[position + i]))) return false;
                }
                out.append(json, position - 2, 6);
                position += 4;
                break;
            default: return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return position == end;
        } else {
            out.push_back(c);
        }
    }
    return false;
}

inline bool ExtractString(const std::string& json, const char* key,
                          std::string& out) {
    size_t start = 0;
    size_t end = 0;
    return FindTopLevelMember(json, key, start, end) &&
           DecodeJsonString(json, start, end, out);
}

inline bool ExtractInt64(const std::string& json, const char* key,
                         long long& out) {
    size_t start = 0;
    size_t end = 0;
    if (!FindTopLevelMember(json, key, start, end) || start >= end)
        return false;

    size_t position = start;
    if (json[position] == '-') ++position;
    const size_t digitsStart = position;
    while (position < end &&
           std::isdigit(static_cast<unsigned char>(json[position]))) {
        ++position;
    }
    if (position == digitsStart || position != end) return false;
    try {
        out = std::stoll(json.substr(start, end - start));
        return true;
    } catch (...) {
        return false;
    }
}
inline std::string Base64UrlDecode(std::string s) {
    if (s.empty() || s.size() % 4 == 1) return {};
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
              c == '_')) return {};
    }
    std::replace(s.begin(), s.end(), '-', '+');
    std::replace(s.begin(), s.end(), '_', '/');
    while (s.size() % 4) s += '=';
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(s.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr))
        return {};
    std::vector<BYTE> data(bytes);
    if (!CryptStringToBinaryA(s.c_str(), 0, CRYPT_STRING_BASE64, data.data(), &bytes, nullptr, nullptr))
        return {};
    return std::string((char*)data.data(), bytes);
}
inline long long JwtExp(const std::string& jwt) {
    const size_t a = jwt.find('.');
    if (a == std::string::npos || a == 0) return 0;
    const size_t b = jwt.find('.', a + 1);
    if (b == std::string::npos || b == a + 1 || b + 1 >= jwt.size() ||
        jwt.find('.', b + 1) != std::string::npos) return 0;
    const std::string payload = Base64UrlDecode(jwt.substr(a + 1, b - a - 1));
    if (payload.empty() || !IsValidJson(payload)) return 0;
    long long exp = 0;
    return ExtractInt64(payload, "exp", exp) ? exp : 0;
}
inline long long UnixNow() { return static_cast<long long>(time(nullptr)); }
}
