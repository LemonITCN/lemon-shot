#include "Util/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ls {

Json Json::MakeBool(bool v) {
    Json j;
    j.type_ = Bool;
    j.b_ = v;
    return j;
}
Json Json::MakeNumber(double v) {
    Json j;
    j.type_ = Number;
    j.n_ = v;
    return j;
}
Json Json::MakeString(const std::string &v) {
    Json j;
    j.type_ = String;
    j.s_ = v;
    return j;
}
Json Json::MakeArray() {
    Json j;
    j.type_ = Array;
    return j;
}
Json Json::MakeObject() {
    Json j;
    j.type_ = Object;
    return j;
}

void Json::Push(Json v) {
    if (type_ != Array) {
        Clear();
        type_ = Array;
    }
    items_.push_back(std::move(v));
}

const Json *Json::Find(const std::string &key) const {
    if (type_ != Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

Json *Json::Find(const std::string &key) {
    if (type_ != Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

Json &Json::Set(const std::string &key, Json v) {
    if (type_ != Object) {
        Clear();
        type_ = Object;
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);
            return items_[i];
        }
    }
    keys_.push_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

bool Json::Remove(const std::string &key) {
    if (type_ != Object) return false;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            keys_.erase(keys_.begin() + (long)i);
            items_.erase(items_.begin() + (long)i);
            return true;
        }
    }
    return false;
}

static bool IsIndex(const std::string &seg, size_t &idx) {
    if (seg.empty()) return false;
    size_t v = 0;
    for (char c : seg) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (size_t)(c - '0');
    }
    idx = v;
    return true;
}

const Json *Json::Path(const std::string &dotted) const {
    const Json *cur = this;
    size_t start = 0;
    while (cur) {
        size_t dot = dotted.find('.', start);
        std::string seg = dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        size_t idx = 0;
        if (cur->type_ == Array && IsIndex(seg, idx))
            cur = idx < cur->items_.size() ? &cur->items_[idx] : nullptr;
        else
            cur = cur->Find(seg);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

Json *Json::Path(const std::string &dotted) { return const_cast<Json *>(static_cast<const Json *>(this)->Path(dotted)); }

void Json::SetPath(const std::string &dotted, Json v) {
    Json *cur = this;
    size_t start = 0;
    for (;;) {
        size_t dot = dotted.find('.', start);
        std::string seg = dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        size_t idx = 0;
        if (dot == std::string::npos) {
            if (cur->type_ == Array && IsIndex(seg, idx)) {
                while (cur->items_.size() <= idx) cur->items_.push_back(Json());
                cur->items_[idx] = std::move(v);
            } else {
                cur->Set(seg, std::move(v));
            }
            return;
        }
        Json *next = nullptr;
        if (cur->type_ == Array && IsIndex(seg, idx)) {
            while (cur->items_.size() <= idx) cur->items_.push_back(Json());
            next = &cur->items_[idx];
        } else {
            next = cur->Find(seg);
            if (!next || (next->type_ != Object && next->type_ != Array)) next = &cur->Set(seg, MakeObject());
        }
        cur = next;
        start = dot + 1;
    }
}

bool Json::GetBool(const std::string &path, bool def) const {
    const Json *j = Path(path);
    return j ? j->AsBool(def) : def;
}

double Json::GetNumber(const std::string &path, double def) const {
    const Json *j = Path(path);
    return (j && (j->IsNumber() || j->IsBool())) ? j->AsNumber(def) : def;
}

std::string Json::GetString(const std::string &path, const std::string &def) const {
    const Json *j = Path(path);
    return (j && j->IsString()) ? j->s_ : def;
}

void Json::DeepMerge(const Json &over) {
    if (type_ != Object || over.type_ != Object) {
        *this = over;
        return;
    }
    for (size_t i = 0; i < over.keys_.size(); ++i) {
        Json *mine = Find(over.keys_[i]);
        if (mine && mine->type_ == Object && over.items_[i].type_ == Object)
            mine->DeepMerge(over.items_[i]);
        else
            Set(over.keys_[i], over.items_[i]);
    }
}

bool Json::Equals(const Json &o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
    case Null: return true;
    case Bool: return b_ == o.b_;
    case Number: return n_ == o.n_;
    case String: return s_ == o.s_;
    case Array:
        if (items_.size() != o.items_.size()) return false;
        for (size_t i = 0; i < items_.size(); ++i)
            if (!items_[i].Equals(o.items_[i])) return false;
        return true;
    case Object:
        if (keys_.size() != o.keys_.size()) return false;
        for (size_t i = 0; i < keys_.size(); ++i) {
            const Json *v = o.Find(keys_[i]);
            if (!v || !items_[i].Equals(*v)) return false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 解析

namespace {

struct Parser {
    const char *p;
    const char *end;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }

    static void AppendUtf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool str(std::string &out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool value(Json &out) {
        if (++depth > 64) return false;
        ws();
        if (p >= end) return false;
        bool ok = false;
        char c = *p;
        if (c == '{') {
            ++p;
            out = Json::MakeObject();
            ws();
            if (p < end && *p == '}') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!str(key)) break;
                    ws();
                    if (p >= end || *p != ':') break;
                    ++p;
                    Json v;
                    if (!value(v)) break;
                    out.Set(key, std::move(v));
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        ++p;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '[') {
            ++p;
            out = Json::MakeArray();
            ws();
            if (p < end && *p == ']') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    Json v;
                    if (!value(v)) break;
                    out.Push(std::move(v));
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        ++p;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '"') {
            std::string s;
            ok = str(s);
            if (ok) out = Json::MakeString(s);
        } else if (c == 't' && end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            p += 4;
            out = Json::MakeBool(true);
            ok = true;
        } else if (c == 'f' && end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            p += 5;
            out = Json::MakeBool(false);
            ok = true;
        } else if (c == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            p += 4;
            out = Json();
            ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            char buf[64];
            size_t n = 0;
            while (p < end && n < sizeof buf - 1 &&
                   ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
                buf[n++] = *p++;
            buf[n] = 0;
            char *e = nullptr;
            double v = std::strtod(buf, &e);
            ok = e && *e == 0 && n > 0;
            if (ok) out = Json::MakeNumber(v);
        }
        --depth;
        return ok;
    }
};

void EscapeTo(std::string &out, const std::string &s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

}  // namespace

bool Json::Parse(const char *data, size_t size, Json &out) {
    out = Json();
    if (!data) return false;
    // 跳过 UTF-8 BOM
    if (size >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        data += 3;
        size -= 3;
    }
    Parser ps{data, data + size};
    Json v;
    if (!ps.value(v)) return false;
    ps.ws();
    if (ps.p != ps.end) return false;
    out = std::move(v);
    return true;
}

void Json::DumpTo(std::string &out, bool pretty, int depth) const {
    auto indent = [&](int d) {
        if (!pretty) return;
        out += '\n';
        for (int i = 0; i < d; ++i) out += "  ";
    };
    switch (type_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: {
        char buf[40];
        if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 1e15) {
            snprintf(buf, sizeof buf, "%.0f", n_);
        } else if (!std::isfinite(n_)) {
            snprintf(buf, sizeof buf, "0");
        } else {
            snprintf(buf, sizeof buf, "%.15g", n_);
            if (std::strtod(buf, nullptr) != n_) snprintf(buf, sizeof buf, "%.17g", n_);
        }
        out += buf;
        break;
    }
    case String: EscapeTo(out, s_); break;
    case Array: {
        if (items_.empty()) {
            out += "[]";
            break;
        }
        // 全是标量的短数组写在一行
        bool flat = true;
        for (auto &it : items_)
            if (it.type_ == Array || it.type_ == Object) flat = false;
        out += '[';
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i) out += flat && pretty ? ", " : ",";
            if (!flat) indent(depth + 1);
            items_[i].DumpTo(out, pretty, depth + 1);
        }
        if (!flat) indent(depth);
        out += ']';
        break;
    }
    case Object: {
        if (keys_.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (i) out += ',';
            indent(depth + 1);
            EscapeTo(out, keys_[i]);
            out += pretty ? ": " : ":";
            items_[i].DumpTo(out, pretty, depth + 1);
        }
        indent(depth);
        out += '}';
        break;
    }
    }
}

std::string Json::Dump(bool pretty) const {
    std::string out;
    DumpTo(out, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

}  // namespace ls
