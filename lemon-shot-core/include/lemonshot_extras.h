/*
 * Lemon Shot 公共内核：文字识别、二维码、WebP、文本整理、翻译请求、哈希。
 * 约定同 lemonshot.h。本头文件里的字符串结果由调用方提供缓冲区，或由内核分配后用 ls_free 释放。
 */
#ifndef LEMONSHOT_EXTRAS_H
#define LEMONSHOT_EXTRAS_H

#include "lemonshot.h"

#ifdef __cplusplus
extern "C" {
#endif

void ls_free(void *p);

/* ------------------------------------------------------------------ 文字识别结果整理（两端共用） */

/* 一行识别结果。box 为图像像素坐标的轴对齐外接框。 */
typedef struct {
    const char *text; /* UTF-8 */
    ls_rect box;
    float confidence;
} ls_text_line;

enum {
    LS_TEXT_JOIN_LINES = 1,    /* 同一段落内的行合并（中文直接相连，西文加空格） */
    LS_TEXT_ASCII_PUNCT = 2    /* 中文标点改为英文半角标点 */
};

/* 按阅读顺序（先按行聚类再从左到右）整理成文本：同一行的片段用空格（中文不加）连接，
 * 行间距明显大于行高时判为新段落（空一行）。返回 ls_free 释放的 UTF-8 字符串。 */
char *ls_text_layout(const ls_text_line *lines, int32_t count, int32_t flags);
/* 只做标点转换。返回 ls_free 释放的字符串。 */
char *ls_text_ascii_punct(const char *utf8);
/* 判断文本是否以中日韩文字为主（用于翻译目标语言 auto）。 */
int32_t ls_text_is_cjk(const char *utf8);

/* ------------------------------------------------------------------ 文字识别引擎（Windows 用；Mac 用 Vision） */

typedef struct ls_ocr ls_ocr;

/* 从内存加载 PP-OCR ncnn 模型与字典（字典每行一个字符）。threads 为 0 时用 CPU 核数。失败返回空。
 * 编译内核时未启用 OCR（LEMONSHOT_WITH_OCR 未定义）时始终返回空。 */
ls_ocr *ls_ocr_create(const void *det_param, size_t det_param_size, const void *det_bin, size_t det_bin_size,
                      const void *rec_param, size_t rec_param_size, const void *rec_bin, size_t rec_bin_size,
                      const char *dict, size_t dict_size, int32_t threads);
void ls_ocr_destroy(ls_ocr *ocr);
/* 识别整张图，返回行数（<0 失败）。结果在下一次调用前有效。 */
int32_t ls_ocr_run(ls_ocr *ocr, const ls_image *image);
void ls_ocr_line_at(const ls_ocr *ocr, int32_t index, ls_text_line *out);

/* ------------------------------------------------------------------ 二维码与条码 */

/* 识别图中全部二维码/条码，按从上到下排列，用换行连接。没有返回空指针。用 ls_free 释放。
 * 编译时未启用（LEMONSHOT_WITH_QR 未定义）始终返回空。 */
char *ls_barcode_decode(const ls_image *image);

/* ------------------------------------------------------------------ WebP */

/* 编码 WebP。lossless 为 1 时无损（quality 表示压缩力度 0-100），否则有损（quality 0-100）。
 * 成功返回 0，*out 用 ls_free 释放。编译时未启用（LEMONSHOT_WITH_WEBP 未定义）返回 -1。 */
int32_t ls_webp_encode(const ls_image *image, int32_t lossless, float quality, uint8_t **out, size_t *out_size);

/* ------------------------------------------------------------------ 翻译请求（两端只负责发 HTTP） */

typedef enum {
    LS_TR_MICROSOFT = 0,
    LS_TR_BAIDU = 1,
    LS_TR_TENCENT = 2,
    LS_TR_YOUDAO = 3,
    LS_TR_DEEPL = 4,
    LS_TR_OPENAI = 5
} ls_translate_provider;

int32_t ls_translate_provider_from_name(const char *name); /* "microsoft" 等，未知返回 -1 */

typedef struct {
    const char *method;  /* "GET" 或 "POST" */
    const char *url;
    const char *headers; /* 每行一个 "Name: value"，用 \n 分隔 */
    const char *body;    /* 可空 */
    int32_t body_size;
} ls_http_request;

typedef struct ls_translator ls_translator;

/* config 为设置里 translate 节的 JSON 原文（含各家密钥）；now_unix 为当前 Unix 秒（签名用）。
 * from/to 为 i18n 的语言代码（"auto"、"zh-Hans"、"en"…）。 */
ls_translator *ls_translate_begin(int32_t provider, const char *config_json, const char *text, const char *from,
                                  const char *to, int64_t now_unix);
void ls_translate_end(ls_translator *tr);

enum {
    LS_TR_SEND = 0,   /* 发出 ls_translate_request 给出的请求，把响应交给 ls_translate_response */
    LS_TR_DONE = 1,   /* 完成，ls_translate_result 取译文 */
    LS_TR_ERROR = 2   /* 失败，ls_translate_result 取错误说明（英文短句或服务返回的错误） */
};

/* 当前步骤需要发的请求；返回 LS_TR_SEND / DONE / ERROR。 */
int32_t ls_translate_request(ls_translator *tr, ls_http_request *out);
/* 把 HTTP 状态码和响应体交给内核，返回下一步（可能还要再发一次，例如微软先取令牌）。 */
int32_t ls_translate_response(ls_translator *tr, int32_t status, const char *body, int32_t body_size);
/* DONE 时为译文；ERROR 时为错误说明，其中 "NEED_KEY" 表示所选服务的密钥没填（平台显示 translate.needKey）。 */
const char *ls_translate_result(const ls_translator *tr);
/* 微软令牌在进程内缓存 8 分钟；由内核管理（线程安全）。 */

/* ------------------------------------------------------------------ 哈希 */

void ls_md5(const void *data, size_t size, uint8_t out[16]);
void ls_sha256(const void *data, size_t size, uint8_t out[32]);
void ls_hmac_sha256(const void *key, size_t key_size, const void *data, size_t size, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* LEMONSHOT_EXTRAS_H */
