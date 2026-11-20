// 扩展模块：哈希、文字整理、翻译请求拼装（lemonshot_extras.h）。
#include "lemonshot_extras.h"
#include "test.h"

#include <string>

namespace {

std::string hexs(const uint8_t *p, int n) {
    std::string o;
    char b[3];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        o += b;
    }
    return o;
}

std::string take(char *p) {
    std::string s = p ? p : "";
    ls_free(p);
    return s;
}

}  // namespace

TEST(extras_hashes_match_known_vectors) {
    uint8_t h[32];
    ls_md5("", 0, h);
    CHECK(hexs(h, 16) == "d41d8cd98f00b204e9800998ecf8427e");
    ls_md5("The quick brown fox jumps over the lazy dog", 43, h);
    CHECK(hexs(h, 16) == "9e107d9d372bb6826bd81d3542a419d6");
    ls_sha256("abc", 3, h);
    CHECK(hexs(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string m(1000, 'a');
    ls_sha256(m.data(), m.size(), h);
    CHECK(hexs(h, 32) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    ls_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, h);
    CHECK(hexs(h, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(extras_text_punct_and_cjk) {
    CHECK(take(ls_text_ascii_punct("你好，世界。（测试）“引号”Ａ１")) == "你好,世界.(测试)\"引号\"A1");
    CHECK(ls_text_is_cjk("这是中文 text") == 1);
    CHECK(ls_text_is_cjk("Hello world 你") == 0);
}

TEST(extras_text_layout_rows_and_paragraphs) {
    ls_text_line lines[] = {
        {"第二行", {10, 40, 80, 18}, 1}, {"Hello", {10, 10, 50, 18}, 1}, {"world", {70, 11, 50, 18}, 1},
        {"新段落", {10, 120, 80, 18}, 1}, {"继续", {10, 142, 80, 18}, 1},
    };
    CHECK(take(ls_text_layout(lines, 5, 0)) == "Hello world\n第二行\n新段落\n继续");
    CHECK(take(ls_text_layout(lines, 5, LS_TEXT_JOIN_LINES)) == "Hello world第二行\n新段落继续");
    ls_text_line hy[] = {{"inter-", {0, 0, 50, 18}, 1}, {"national", {0, 22, 80, 18}, 1}};
    CHECK(take(ls_text_layout(hy, 2, LS_TEXT_JOIN_LINES)) == "international");
}

TEST(extras_translate_microsoft_two_steps) {
    ls_translator *tr = ls_translate_begin(LS_TR_MICROSOFT, "{}", "Hello", "auto", "auto", 1700000000);
    ls_http_request rq;
    CHECK(ls_translate_request(tr, &rq) == LS_TR_SEND);
    CHECK(std::string(rq.method) == "GET");
    CHECK(ls_translate_response(tr, 200, "tok123", 6) == LS_TR_SEND);
    ls_translate_request(tr, &rq);
    CHECK(std::string(rq.method) == "POST");
    CHECK(std::string(rq.url).find("to=zh-Hans") != std::string::npos);
    CHECK(std::string(rq.headers).find("Bearer tok123") != std::string::npos);
    const char *resp = "[{\"translations\":[{\"text\":\"\\u4f60\\u597d\",\"to\":\"zh-Hans\"}]}]";
    CHECK(ls_translate_response(tr, 200, resp, (int32_t)std::strlen(resp)) == LS_TR_DONE);
    CHECK(std::string(ls_translate_result(tr)) == "你好");
    ls_translate_end(tr);
}

TEST(extras_translate_missing_key_and_errors) {
    ls_http_request rq;
    ls_translator *tr = ls_translate_begin(LS_TR_BAIDU, "{\"baidu\":{\"appId\":\"\"}}", "Hello", "auto", "zh-Hans", 1);
    CHECK(ls_translate_request(tr, &rq) == LS_TR_ERROR);
    CHECK(std::string(ls_translate_result(tr)) == "NEED_KEY");
    ls_translate_end(tr);
    tr = ls_translate_begin(LS_TR_BAIDU, "{\"baidu\":{\"appId\":\"1\",\"secret\":\"s\"}}", "Hello", "auto", "zh-Hans", 1);
    CHECK(ls_translate_request(tr, &rq) == LS_TR_SEND);
    const char *err = "{\"error_code\":\"54001\",\"error_msg\":\"Invalid Sign\"}";
    CHECK(ls_translate_response(tr, 200, err, (int32_t)std::strlen(err)) == LS_TR_ERROR);
    CHECK(std::string(ls_translate_result(tr)) == "54001 Invalid Sign");
    ls_translate_end(tr);
}

TEST(extras_translate_tencent_signature_shape) {
    ls_translator *tr = ls_translate_begin(LS_TR_TENCENT, "{\"tencent\":{\"secretId\":\"AKID\",\"secretKey\":\"KEY\"}}", "你好", "auto", "en",
                                           1551113065);
    ls_http_request rq;
    CHECK(ls_translate_request(tr, &rq) == LS_TR_SEND);
    std::string h = rq.headers;
    CHECK(h.find("Credential=AKID/2019-02-25/tmt/tc3_request") != std::string::npos);
    CHECK(h.find("X-TC-Region: ap-guangzhou") != std::string::npos);
    CHECK(std::string(rq.body) == "{\"SourceText\":\"你好\",\"Source\":\"auto\",\"Target\":\"en\",\"ProjectId\":0}");
    ls_translate_end(tr);
}
