// 内核内部用的极简 JSON：解析（对象、数组、字符串含 \u 转义与代理对、数字、布尔、null）与字符串转义。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lsjson {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value &operator[](const char *key) const {
        static const Value kNull;
        if (type != Object) return kNull;
        for (const auto &kv : obj)
            if (kv.first == key) return kv.second;
        return kNull;
    }
    const Value &operator[](size_t i) const {
        static const Value kNull;
        if (type != Array || i >= arr.size()) return kNull;
        return arr[i];
    }
    const Value &operator[](int i) const { return (*this)[(size_t)(i < 0 ? 0x7fffffff : i)]; }
    bool is_null() const { return type == Null; }
    std::string as_string(const char *fallback = "") const {
        if (type == String) return str;
        if (type == Number) {
            char buf[32];
            long long ll = (long long)num;
            if ((double)ll == num) std::snprintf(buf, sizeof buf, "%lld", ll);
            else std::snprintf(buf, sizeof buf, "%g", num);
            return buf;
        }
        return fallback;
    }
};

class Parser {
public:
    explicit Parser(const char *s, size_t n) : p_(s), end_(s + n) {}
    bool parse(Value &out) {
        ws();
        if (!value(out)) return false;
        ws();
        return true;
    }

private:
    const char *p_;
    const char *end_;

    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool lit(const char *s) {
        size_t n = std::strlen(s);
        if ((size_t)(end_ - p_) < n || std::strncmp(p_, s, n) != 0) return false;
        p_ += n;
        return true;
    }
    static void put_utf8(std::string &o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t &v) {
        if (end_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') { o += c; continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(o, cp);
                break;
            }
            default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    bool value(Value &v) {
        ws();
        if (p_ >= end_) return false;
        char c = *p_;
        if (c == '{') {
            v.type = Value::Object;
            ++p_;
            ws();
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            while (true) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                Value child;
                if (!value(child)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++p_;
            ws();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            while (true) {
                Value child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Value::String; return string(v.str); }
        if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Value::Null; return true; }
        char *e = nullptr;
        std::string tmp(p_, (size_t)std::min<ptrdiff_t>(end_ - p_, 64));
        double d = std::strtod(tmp.c_str(), &e);
        if (e == tmp.c_str()) return false;
        p_ += (e - tmp.c_str());
        v.type = Value::Number;
        v.num = d;
        return true;
    }
};

inline bool parse(const char *s, size_t n, Value &out) {
    if (!s) return false;
    Parser ps(s, n);
    return ps.parse(out);
}

// 把 UTF-8 字符串写成 JSON 字符串字面量（带引号）
inline std::string quote(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            } else {
                o += (char)c;
            }
        }
    }
    o += '"';
    return o;
}

}  // namespace lsjson
