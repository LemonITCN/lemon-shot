// 文字识别结果整理：阅读顺序、合并换行、标点转换、中日韩判断。两端（Vision / PaddleOCR）的结果都经过这里，输出规则一致。
#include "lemonshot_extras.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 解码一个 UTF-8 码点，返回字节数（非法字节按 1 字节、码点 0xFFFD）
int decode(const unsigned char *s, size_t n, uint32_t &cp) {
    unsigned char c = s[0];
    if (c < 0x80) { cp = c; return 1; }
    int len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || (size_t)len > n) { cp = 0xFFFD; return 1; }
    cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) { cp = 0xFFFD; return 1; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    return len;
}

void encode(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

std::vector<uint32_t> codepoints(const char *s) {
    std::vector<uint32_t> v;
    if (!s) return v;
    size_t n = std::strlen(s);
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        i += (size_t)decode(p + i, n - i, cp);
        v.push_back(cp);
    }
    return v;
}

// 中日韩文字与全角标点（合并时两侧都是这类字符就不加空格）
bool is_cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FA1F) ||
           (c >= 0x3000 && c <= 0x303F);
}

uint32_t first_cp(const std::string &s) {
    if (s.empty()) return 0;
    uint32_t cp;
    decode((const unsigned char *)s.data(), s.size(), cp);
    return cp;
}

uint32_t last_cp(const std::string &s) {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    uint32_t cp;
    decode((const unsigned char *)s.data() + i, s.size() - i, cp);
    return cp;
}

void trim(std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    s = s.substr(a, b - a);
}

// 把 b 接到 a 后面：两侧都是中日韩字符不加空格；a 以连字符结尾且两侧是西文字母时去掉连字符直接相连
void append_joined(std::string &a, const std::string &b) {
    if (a.empty()) { a = b; return; }
    if (b.empty()) return;
    uint32_t l = last_cp(a), r = first_cp(b);
    if (l == '-' && a.size() >= 2) {
        uint32_t ll = (uint32_t)(unsigned char)a[a.size() - 2];
        if (ll < 0x80 && std::isalpha((int)ll) && r < 0x80 && std::isalpha((int)r)) {
            a.pop_back();
            a += b;
            return;
        }
    }
    if (is_cjk(l) || is_cjk(r)) a += b;
    else { a += ' '; a += b; }
}

char *dup(const std::string &s) {
    char *p = (char *)std::malloc(s.size() + 1);
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

struct Item {
    std::string text;
    ls_rect box;
};

struct Row {
    std::vector<Item> items;
    float top = 0, bottom = 0;
    float cy() const { return (top + bottom) / 2; }
    float h() const { return bottom - top; }
};

}  // namespace

extern "C" int32_t ls_text_is_cjk(const char *utf8) {
    int cjk = 0, other = 0;
    for (uint32_t c : codepoints(utf8)) {
        if (c >= 0x2E80 && c <= 0x9FFF) ++cjk;
        else if (c >= 0xAC00 && c <= 0xD7AF) ++cjk;
        else if (c < 0x80 && std::isalpha((int)c)) ++other;
        else if (c >= 0x80 && !is_cjk(c)) ++other;
    }
    return cjk > 0 && cjk >= other ? 1 : 0;
}

extern "C" char *ls_text_ascii_punct(const char *utf8) {
    std::string o;
    for (uint32_t c : codepoints(utf8)) {
        switch (c) {
        case 0xFF0C: case 0x3001: o += ','; continue;       // ， 、
        case 0x3002: case 0xFF61: o += '.'; continue;       // 。
        case 0xFF1B: o += ';'; continue;
        case 0xFF1A: o += ':'; continue;
        case 0xFF1F: o += '?'; continue;
        case 0xFF01: o += '!'; continue;
        case 0xFF08: o += '('; continue;
        case 0xFF09: o += ')'; continue;
        case 0x3010: case 0x3014: o += '['; continue;       // 【 〔
        case 0x3011: case 0x3015: o += ']'; continue;
        case 0x300A: case 0x3008: o += '<'; continue;       // 《 〈
        case 0x300B: case 0x3009: o += '>'; continue;
        case 0x201C: case 0x201D: case 0x300C: case 0x300D: case 0x300E: case 0x300F: o += '"'; continue;
        case 0x2018: case 0x2019: o += '\''; continue;
        case 0x2026: o += "..."; continue;                  // …
        case 0x2014: o += '-'; continue;                    // —（「——」变成「--」）
        case 0xFF5E: case 0x301C: o += '~'; continue;
        case 0x3000: o += ' '; continue;                    // 全角空格
        default: break;
        }
        if (c >= 0xFF01 && c <= 0xFF5E) {                   // 其余全角 ASCII（字母、数字、符号）
            o += (char)(c - 0xFEE0);
            continue;
        }
        encode(o, c);
    }
    return dup(o);
}

extern "C" char *ls_text_layout(const ls_text_line *lines, int32_t count, int32_t flags) {
    std::vector<Item> items;
    for (int32_t i = 0; i < count; ++i) {
        if (!lines[i].text) continue;
        Item it{lines[i].text, lines[i].box};
        trim(it.text);
        if (!it.text.empty()) items.push_back(it);
    }
    // 1. 按竖直中心排序后聚成行：中心落在当前行高度一半以内算同一行
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        return a.box.y + a.box.h / 2 < b.box.y + b.box.h / 2;
    });
    std::vector<Row> rows;
    for (const Item &it : items) {
        float cy = it.box.y + it.box.h / 2;
        if (!rows.empty()) {
            Row &r = rows.back();
            float tol = std::min(r.h(), it.box.h) * 0.5f;
            if (std::abs(cy - r.cy()) <= tol) {
                r.items.push_back(it);
                r.top = std::min(r.top, it.box.y);
                r.bottom = std::max(r.bottom, it.box.y + it.box.h);
                continue;
            }
        }
        Row r;
        r.items.push_back(it);
        r.top = it.box.y;
        r.bottom = it.box.y + it.box.h;
        rows.push_back(r);
    }
    // 2. 行内从左到右；行文本
    std::vector<std::string> texts;
    for (Row &r : rows) {
        std::sort(r.items.begin(), r.items.end(), [](const Item &a, const Item &b) { return a.box.x < b.box.x; });
        std::string t;
        for (const Item &it : r.items) {
            if (t.empty()) t = it.text;
            else if (is_cjk(last_cp(t)) && is_cjk(first_cp(it.text))) t += it.text;
            else { t += ' '; t += it.text; }
        }
        texts.push_back(t);
    }
    // 3. 段落：与上一行的空隙大于两行平均行高的 0.8 倍，或缩进明显不同时视为新段落
    std::string out;
    std::string para;
    for (size_t i = 0; i < rows.size(); ++i) {
        bool new_para = false;
        if (i > 0) {
            float gap = rows[i].top - rows[i - 1].bottom;
            float avg = (rows[i].h() + rows[i - 1].h()) / 2;
            new_para = gap > avg * 0.8f;
        }
        if (flags & LS_TEXT_JOIN_LINES) {
            if (new_para && !para.empty()) {
                if (!out.empty()) out += '\n';
                out += para;
                para.clear();
            }
            append_joined(para, texts[i]);
        } else {
            if (i > 0) out += '\n';
            out += texts[i];
        }
    }
    if ((flags & LS_TEXT_JOIN_LINES) && !para.empty()) {
        if (!out.empty()) out += '\n';
        out += para;
    }
    if (flags & LS_TEXT_ASCII_PUNCT) {
        char *conv = ls_text_ascii_punct(out.c_str());
        return conv;
    }
    return dup(out);
}
