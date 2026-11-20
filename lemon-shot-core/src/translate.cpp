// 翻译请求：按服务拼请求（含签名）、解析响应。两端只负责执行 HTTP，结果和错误处理完全一致。
#include "lemonshot_extras.h"
#include "json_mini.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {

// ---------------------------------------------------------------- 工具
std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string o;
    o.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        o += d[p[i] >> 4];
        o += d[p[i] & 15];
    }
    return o;
}

std::string md5_hex(const std::string &s) {
    uint8_t h[16];
    ls_md5(s.data(), s.size(), h);
    return hex(h, 16);
}

std::string sha256_hex(const std::string &s) {
    uint8_t h[32];
    ls_sha256(s.data(), s.size(), h);
    return hex(h, 32);
}

std::string hmac(const std::string &key, const std::string &data) {
    uint8_t h[32];
    ls_hmac_sha256(key.data(), key.size(), data.data(), data.size(), h);
    return std::string((const char *)h, 32);
}

std::string url_encode(const std::string &s) {
    std::string o;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            o += buf;
        }
    }
    return o;
}

size_t utf8_count(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// 按码点截取 [start, start+len)
std::string utf8_sub(const std::string &s, size_t start, size_t len) {
    size_t cp = 0, i = 0, a = std::string::npos, b = s.size();
    for (; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        if (cp == start) a = i;
        if (cp == start + len) { b = i; break; }
        ++cp;
    }
    if (a == std::string::npos) return std::string();
    return s.substr(a, b - a);
}

std::string utc_date(int64_t t) {
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return buf;
}

// ---------------------------------------------------------------- 语言代码
struct LangMap {
    const char *code;   // 本项目代码（i18n 的 lang.*）
    const char *ms, *baidu, *tencent, *youdao, *deepl, *english;
};

const LangMap kLangs[] = {
    {"zh-Hans", "zh-Hans", "zh", "zh", "zh-CHS", "ZH-HANS", "Simplified Chinese"},
    {"zh-Hant", "zh-Hant", "cht", "zh-TW", "zh-CHT", "ZH-HANT", "Traditional Chinese"},
    {"en", "en", "en", "en", "en", "EN-US", "English"},
    {"ja", "ja", "jp", "ja", "ja", "JA", "Japanese"},
    {"ko", "ko", "kor", "ko", "ko", "KO", "Korean"},
    {"fr", "fr", "fra", "fr", "fr", "FR", "French"},
    {"de", "de", "de", "de", "de", "DE", "German"},
    {"es", "es", "spa", "es", "es", "ES", "Spanish"},
    {"ru", "ru", "ru", "ru", "ru", "RU", "Russian"},
    {"pt", "pt", "pt", "pt", "pt", "PT-BR", "Portuguese"},
    {"it", "it", "it", "it", "it", "IT", "Italian"},
    {"ar", "ar", "ara", "ar", "ar", "AR", "Arabic"},
    {"th", "th", "th", "th", "th", "", "Thai"},
    {"vi", "vi", "vie", "vi", "vi", "VI", "Vietnamese"},
};

const LangMap *find_lang(const std::string &code) {
    for (const auto &l : kLangs)
        if (code == l.code) return &l;
    return nullptr;
}

// ---------------------------------------------------------------- 微软令牌缓存
std::mutex g_ms_mutex;
std::string g_ms_token;
int64_t g_ms_token_time = 0;

const char *kMsHosts[] = {"https://api-edge.cognitivetranslator.com", "https://api.cognitive.microsofttranslator.com"};

}  // namespace

struct ls_translator {
    int provider = 0;
    lsjson::Value config;      // translate 节
    std::string text, from, to;
    int64_t now = 0;
    int state = LS_TR_SEND;
    int step = 0;              // 微软：0 取令牌，1 用第一个主机翻译，2 用备用主机翻译
    bool retried_token = false;
    std::string result;
    // 当前请求的存储
    std::string method, url, headers, body;

    std::string cfg(const char *section, const char *key) const { return config[section][key].as_string(); }
    void fail(const std::string &msg) {
        state = LS_TR_ERROR;
        result = msg;
    }
    void done(const std::string &text_out) {
        state = LS_TR_DONE;
        result = text_out;
    }
    bool build();
    int handle(int status, const std::string &resp);
};

bool ls_translator::build() {
    const LangMap *tl = find_lang(to);
    const LangMap *fl = from == "auto" ? nullptr : find_lang(from);
    if (!tl) {
        fail("Unsupported target language");
        return false;
    }
    method = "POST";
    headers.clear();
    body.clear();
    switch (provider) {
    case LS_TR_MICROSOFT: {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(g_ms_mutex);
            if (!g_ms_token.empty() && now - g_ms_token_time < 8 * 60) token = g_ms_token;
        }
        if (token.empty()) {
            step = 0;
            method = "GET";
            url = "https://edge.microsoft.com/translate/auth";
            return true;
        }
        if (step == 0) step = 1;
        url = std::string(kMsHosts[step == 2 ? 1 : 0]) + "/translate?api-version=3.0&to=" + tl->ms;
        if (fl) url += std::string("&from=") + fl->ms;
        headers = "Authorization: Bearer " + token + "\nContent-Type: application/json";
        body = "[{\"Text\":" + lsjson::quote(text) + "}]";
        return true;
    }
    case LS_TR_BAIDU: {
        std::string appid = cfg("baidu", "appId"), secret = cfg("baidu", "secret");
        if (appid.empty() || secret.empty()) { fail("NEED_KEY"); return false; }
        std::string salt = std::to_string(now % 1000000007);
        std::string sign = md5_hex(appid + text + salt + secret);
        url = "https://fanyi-api.baidu.com/api/trans/vip/translate";
        headers = "Content-Type: application/x-www-form-urlencoded";
        body = "q=" + url_encode(text) + "&from=" + (fl ? fl->baidu : "auto") + "&to=" + tl->baidu + "&appid=" + url_encode(appid) +
               "&salt=" + salt + "&sign=" + sign;
        return true;
    }
    case LS_TR_TENCENT: {
        std::string id = cfg("tencent", "secretId"), key = cfg("tencent", "secretKey"), region = cfg("tencent", "region");
        if (id.empty() || key.empty()) { fail("NEED_KEY"); return false; }
        if (region.empty()) region = "ap-guangzhou";
        const std::string host = "tmt.tencentcloudapi.com";
        body = "{\"SourceText\":" + lsjson::quote(text) + ",\"Source\":\"" + (fl ? fl->tencent : "auto") + "\",\"Target\":\"" + tl->tencent +
               "\",\"ProjectId\":0}";
        const std::string ctype = "application/json; charset=utf-8";
        std::string canonical = "POST\n/\n\ncontent-type:" + ctype + "\nhost:" + host + "\nx-tc-action:texttranslate\n\ncontent-type;host;x-tc-action\n" +
                                sha256_hex(body);
        std::string date = utc_date(now);
        std::string ts = std::to_string(now);
        std::string scope = date + "/tmt/tc3_request";
        std::string to_sign = "TC3-HMAC-SHA256\n" + ts + "\n" + scope + "\n" + sha256_hex(canonical);
        std::string k_date = hmac("TC3" + key, date);
        std::string k_service = hmac(k_date, "tmt");
        std::string k_signing = hmac(k_service, "tc3_request");
        std::string sig = hmac(k_signing, to_sign);
        url = "https://" + host + "/";
        headers = "Content-Type: " + ctype + "\nHost: " + host + "\nX-TC-Action: TextTranslate\nX-TC-Version: 2018-03-21\nX-TC-Timestamp: " + ts +
                  "\nX-TC-Region: " + region + "\nAuthorization: TC3-HMAC-SHA256 Credential=" + id + "/" + scope +
                  ", SignedHeaders=content-type;host;x-tc-action, Signature=" + hex((const uint8_t *)sig.data(), sig.size());
        return true;
    }
    case LS_TR_YOUDAO: {
        std::string app = cfg("youdao", "appKey"), secret = cfg("youdao", "appSecret");
        if (app.empty() || secret.empty()) { fail("NEED_KEY"); return false; }
        size_t n = utf8_count(text);
        std::string input = n <= 20 ? text : utf8_sub(text, 0, 10) + std::to_string(n) + utf8_sub(text, n - 10, 10);
        std::string salt = std::to_string(now % 1000000007);
        std::string curtime = std::to_string(now);
        std::string sign = sha256_hex(app + input + salt + curtime + secret);
        url = "https://openapi.youdao.com/api";
        headers = "Content-Type: application/x-www-form-urlencoded";
        body = "q=" + url_encode(text) + "&from=" + (fl ? fl->youdao : "auto") + "&to=" + tl->youdao + "&appKey=" + url_encode(app) +
               "&salt=" + salt + "&sign=" + sign + "&signType=v3&curtime=" + curtime;
        return true;
    }
    case LS_TR_DEEPL: {
        std::string key = cfg("deepl", "authKey");
        if (key.empty()) { fail("NEED_KEY"); return false; }
        if (!*tl->deepl) { fail("Unsupported target language"); return false; }
        bool free_key = key.size() > 3 && key.compare(key.size() - 3, 3, ":fx") == 0;
        url = free_key ? "https://api-free.deepl.com/v2/translate" : "https://api.deepl.com/v2/translate";
        headers = "Authorization: DeepL-Auth-Key " + key + "\nContent-Type: application/json";
        body = "{\"text\":[" + lsjson::quote(text) + "],\"target_lang\":\"" + tl->deepl + "\"}";
        return true;
    }
    case LS_TR_OPENAI: {
        std::string base = cfg("openai", "baseUrl"), key = cfg("openai", "apiKey"), model = cfg("openai", "model");
        if (base.empty() || model.empty()) { fail("NEED_KEY"); return false; }
        while (!base.empty() && base.back() == '/') base.pop_back();
        url = base + "/chat/completions";
        headers = "Content-Type: application/json";
        if (!key.empty()) headers += "\nAuthorization: Bearer " + key;
        std::string sys = std::string("You are a translation engine. Translate the user's text into ") + tl->english +
                          ". Keep line breaks. Output only the translation, without quotes or explanations.";
        body = "{\"model\":" + lsjson::quote(model) + ",\"temperature\":0,\"messages\":[{\"role\":\"system\",\"content\":" + lsjson::quote(sys) +
               "},{\"role\":\"user\",\"content\":" + lsjson::quote(text) + "}]}";
        return true;
    }
    default:
        fail("Unknown provider");
        return false;
    }
}

int ls_translator::handle(int status, const std::string &resp) {
    lsjson::Value v;
    bool ok = lsjson::parse(resp.data(), resp.size(), v);
    switch (provider) {
    case LS_TR_MICROSOFT:
        if (step == 0) {
            if (status != 200 || resp.empty()) { fail("HTTP " + std::to_string(status)); return state; }
            std::string token = resp;
            while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
            {
                std::lock_guard<std::mutex> lock(g_ms_mutex);
                g_ms_token = token;
                g_ms_token_time = now;
            }
            step = 1;
            build();
            return state;
        }
        if (status == 401 && !retried_token) {
            retried_token = true;
            {
                std::lock_guard<std::mutex> lock(g_ms_mutex);
                g_ms_token.clear();
            }
            step = 0;
            build();
            return state;
        }
        if (status != 200 && step == 1) {
            step = 2;
            build();
            return state;
        }
        if (status == 200 && ok) {
            std::string out;
            for (const auto &item : v.arr) {
                std::string t = item["translations"][0]["text"].as_string();
                if (!out.empty() && !t.empty()) out += '\n';
                out += t;
            }
            done(out);
        } else {
            std::string msg = ok ? v["error"]["message"].as_string() : std::string();
            fail(msg.empty() ? "HTTP " + std::to_string(status) : msg);
        }
        return state;
    case LS_TR_BAIDU:
        if (ok && v["trans_result"].type == lsjson::Value::Array) {
            std::string out;
            for (const auto &r : v["trans_result"].arr) {
                if (!out.empty()) out += '\n';
                out += r["dst"].as_string();
            }
            done(out);
        } else if (ok && !v["error_code"].is_null()) {
            fail(v["error_code"].as_string() + " " + v["error_msg"].as_string());
        } else {
            fail("HTTP " + std::to_string(status));
        }
        return state;
    case LS_TR_TENCENT: {
        const lsjson::Value &r = v["Response"];
        if (ok && !r["Error"].is_null()) fail(r["Error"]["Code"].as_string() + " " + r["Error"]["Message"].as_string());
        else if (ok && r["TargetText"].type == lsjson::Value::String) done(r["TargetText"].str);
        else fail("HTTP " + std::to_string(status));
        return state;
    }
    case LS_TR_YOUDAO:
        if (ok && v["errorCode"].as_string() == "0" && v["translation"].type == lsjson::Value::Array) {
            std::string out;
            for (const auto &t : v["translation"].arr) {
                if (!out.empty()) out += '\n';
                out += t.as_string();
            }
            done(out);
        } else if (ok) {
            fail("errorCode " + v["errorCode"].as_string());
        } else {
            fail("HTTP " + std::to_string(status));
        }
        return state;
    case LS_TR_DEEPL:
        if (status == 200 && ok) done(v["translations"][0]["text"].as_string());
        else fail(ok && !v["message"].is_null() ? v["message"].as_string() : "HTTP " + std::to_string(status));
        return state;
    case LS_TR_OPENAI:
        if (status == 200 && ok) {
            std::string out = v["choices"][0]["message"]["content"].as_string();
            while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
            done(out);
        } else {
            fail(ok && !v["error"]["message"].is_null() ? v["error"]["message"].as_string() : "HTTP " + std::to_string(status));
        }
        return state;
    }
    fail("Unknown provider");
    return state;
}

extern "C" {

int32_t ls_translate_provider_from_name(const char *name) {
    static const char *names[] = {"microsoft", "baidu", "tencent", "youdao", "deepl", "openai"};
    if (!name) return -1;
    for (int i = 0; i < 6; ++i)
        if (std::strcmp(name, names[i]) == 0) return i;
    return -1;
}

ls_translator *ls_translate_begin(int32_t provider, const char *config_json, const char *text, const char *from, const char *to,
                                  int64_t now_unix) {
    ls_translator *tr = new ls_translator();
    tr->provider = provider;
    if (config_json) lsjson::parse(config_json, std::strlen(config_json), tr->config);
    tr->text = text ? text : "";
    tr->from = from && *from ? from : "auto";
    tr->to = to && *to ? to : "auto";
    if (tr->to == "auto") tr->to = ls_text_is_cjk(tr->text.c_str()) ? "en" : "zh-Hans";
    tr->now = now_unix;
    if (tr->text.empty()) tr->done("");
    else tr->build();
    return tr;
}

void ls_translate_end(ls_translator *tr) { delete tr; }

int32_t ls_translate_request(ls_translator *tr, ls_http_request *out) {
    std::memset(out, 0, sizeof *out);
    if (tr->state != LS_TR_SEND) return tr->state;
    out->method = tr->method.c_str();
    out->url = tr->url.c_str();
    out->headers = tr->headers.c_str();
    out->body = tr->body.empty() ? nullptr : tr->body.c_str();
    out->body_size = (int32_t)tr->body.size();
    return LS_TR_SEND;
}

int32_t ls_translate_response(ls_translator *tr, int32_t status, const char *body, int32_t body_size) {
    if (tr->state != LS_TR_SEND) return tr->state;
    std::string resp = body && body_size > 0 ? std::string(body, (size_t)body_size) : std::string();
    return tr->handle(status, resp);
}

const char *ls_translate_result(const ls_translator *tr) { return tr->result.c_str(); }

}  // extern "C"
