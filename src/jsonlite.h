#pragma once
// ============================================================================
// jsonlite.h — 极简 JSON（零第三方依赖），供配置读写共用
// 解析/序列化与文件读写都抽在这里，settings 与 theme 各自只关心自己的字段。
// ============================================================================

#include <windows.h>

#include <map>
#include <string>
#include <vector>

namespace jsonlite {

struct Value {
    enum Kind { NUL, BOOL, NUM, STR, OBJ, ARR } kind = NUL;
    bool b = false;
    double num = 0;
    std::wstring str;
    std::map<std::wstring, Value> obj;
    std::vector<Value> arr;

    int asInt(int def) const { return kind == NUM ? (int)num : def; }
    double asNum(double def) const { return kind == NUM ? num : def; }
    bool asBool(bool def) const { return kind == BOOL ? b : def; }
    std::wstring asStr(const std::wstring& def) const { return kind == STR ? str : def; }
    // 子对象（不存在时返回静态空对象，可安全链式访问）
    const Value& child(const wchar_t* key) const;
    bool has(const wchar_t* key) const { return obj.count(key) != 0; }
};

Value Parse(const std::wstring& text);
std::wstring Dump(const Value& v);

// 构造辅助
Value MakeObj();
Value MakeInt(int i);
Value MakeNum(double d);
Value MakeBool(bool b);
Value MakeStr(const std::wstring& s);

// 文件读写（UTF-16 BOM）
std::wstring ReadFile(const std::wstring& path);
bool WriteFile(const std::wstring& path, const std::wstring& text);

}  // namespace jsonlite
