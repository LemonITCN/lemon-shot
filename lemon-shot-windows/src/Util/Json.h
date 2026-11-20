// 极简 JSON：解析、序列化（保持键顺序）、点路径读写、深合并。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ls {

class Json {
public:
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json MakeBool(bool v);
    static Json MakeNumber(double v);
    static Json MakeString(const std::string &v);
    static Json MakeArray();
    static Json MakeObject();

    // 解析失败返回 false，out 为 Null
    static bool Parse(const char *data, size_t size, Json &out);
    static bool Parse(const std::string &s, Json &out) { return Parse(s.data(), s.size(), out); }
    std::string Dump(bool pretty = true) const;

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Null; }
    bool IsBool() const { return type_ == Bool; }
    bool IsNumber() const { return type_ == Number; }
    bool IsString() const { return type_ == String; }
    bool IsArray() const { return type_ == Array; }
    bool IsObject() const { return type_ == Object; }

    bool AsBool(bool def = false) const { return type_ == Bool ? b_ : (type_ == Number ? n_ != 0 : def); }
    double AsNumber(double def = 0) const { return type_ == Number ? n_ : (type_ == Bool ? (b_ ? 1 : 0) : def); }
    const std::string &AsString() const { return s_; }

    // 数组
    size_t Size() const { return items_.size(); }
    const Json &At(size_t i) const { return items_[i]; }
    Json &At(size_t i) { return items_[i]; }
    void Push(Json v);
    void Clear() { keys_.clear(); items_.clear(); }

    // 对象
    const Json *Find(const std::string &key) const;
    Json *Find(const std::string &key);
    Json &Set(const std::string &key, Json v);  // 不存在时追加到末尾
    bool Remove(const std::string &key);
    const std::vector<std::string> &Keys() const { return keys_; }
    const Json &ValueAt(size_t i) const { return items_[i]; }

    // 点路径："capture.detectWindows"；数组下标也可以用数字段："annotate.palette.0"
    const Json *Path(const std::string &dotted) const;
    Json *Path(const std::string &dotted);
    // 写入路径：中间缺少的对象自动创建
    void SetPath(const std::string &dotted, Json v);

    bool GetBool(const std::string &path, bool def) const;
    double GetNumber(const std::string &path, double def) const;
    std::string GetString(const std::string &path, const std::string &def) const;

    // 用 over 逐键覆盖本对象（对象递归合并，其他类型整体替换），未知键原样保留
    void DeepMerge(const Json &over);
    bool Equals(const Json &o) const;

private:
    void DumpTo(std::string &out, bool pretty, int depth) const;

    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<std::string> keys_;  // 对象的键（与 items_ 一一对应）
    std::vector<Json> items_;         // 数组元素或对象的值
};

}  // namespace ls
