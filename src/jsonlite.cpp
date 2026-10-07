#include "jsonlite.h"

#include <cstdio>
#include <map>
#include <vector>

namespace jsonlite {

static void SkipWs(const wchar_t*& p, const wchar_t* end) {
    while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) p++;
}

static bool ParseString(const wchar_t*& p, const wchar_t* end, std::wstring& out) {
    if (p >= end || *p != L'"') return false;
    p++;
    out.clear();
    while (p < end && *p != L'"') {
        if (*p == L'\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case L'n': out.push_back(L'\n'); break;
                case L't': out.push_back(L'\t'); break;
                case L'r': out.push_back(L'\r'); break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/': out.push_back(L'/'); break;
                case L'"': out.push_back(L'"'); break;
                case L'b': out.push_back(L'\b'); break;
                case L'f': out.push_back(L'\f'); break;
                case L'u': {
                    if (p + 4 < end) {
                        wchar_t hex[5] = { p[1], p[2], p[3], p[4], 0 };
                        out.push_back((wchar_t)wcstol(hex, nullptr, 16));
                        p += 4;
                    }
                    break;
                }
                default: out.push_back(*p); break;
            }
            p++;
        } else {
            out.push_back(*p++);
        }
    }
    if (p >= end) return false;
    p++;  // 收尾引号
    return true;
}

static Value ParseValue(const wchar_t*& p, const wchar_t* end);

static bool ParseObject(const wchar_t*& p, const wchar_t* end, Value& v) {
    if (*p != L'{') return false;
    p++;
    v.kind = Value::OBJ;
    SkipWs(p, end);
    if (p < end && *p == L'}') { p++; return true; }
    for (;;) {
        SkipWs(p, end);
        std::wstring key;
        if (!ParseString(p, end, key)) return false;
        SkipWs(p, end);
        if (p >= end || *p != L':') return false;
        p++;
        SkipWs(p, end);
        v.obj[key] = ParseValue(p, end);
        SkipWs(p, end);
        if (p < end && *p == L',') { p++; continue; }
        if (p < end && *p == L'}') { p++; return true; }
        return false;
    }
}

static bool ParseArray(const wchar_t*& p, const wchar_t* end, Value& v) {
    if (*p != L'[') return false;
    p++;
    v.kind = Value::ARR;
    SkipWs(p, end);
    if (p < end && *p == L']') { p++; return true; }
    for (;;) {
        SkipWs(p, end);
        v.arr.push_back(ParseValue(p, end));
        SkipWs(p, end);
        if (p < end && *p == L',') { p++; continue; }
        if (p < end && *p == L']') { p++; return true; }
        return false;
    }
}

static Value ParseValue(const wchar_t*& p, const wchar_t* end) {
    Value v;
    SkipWs(p, end);
    if (p >= end) return v;
    if (*p == L'{') {
        ParseObject(p, end, v);
        return v;
    }
    if (*p == L'[') {
        ParseArray(p, end, v);
        return v;
    }
    if (*p == L'"') {
        v.kind = Value::STR;
        ParseString(p, end, v.str);
        return v;
    }
    if (wcsncmp(p, L"true", 4) == 0) {
        v.kind = Value::BOOL;
        v.b = true;
        p += 4;
        return v;
    }
    if (wcsncmp(p, L"false", 5) == 0) {
        v.kind = Value::BOOL;
        v.b = false;
        p += 5;
        return v;
    }
    if (wcsncmp(p, L"null", 4) == 0) {
        p += 4;
        return v;
    }
    {
        wchar_t* stop = nullptr;
        double d = wcstod(p, &stop);
        if (stop && stop != p) {
            v.kind = Value::NUM;
            v.num = d;
            p = stop;
        }
    }
    return v;
}

static std::wstring Escape(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default: out.push_back(c); break;
        }
    }
    out += L"\"";
    return out;
}

static void DumpValue(const Value& v, int indent, std::wstring& out) {
    auto pad = [&](int n) { for (int i = 0; i < n; i++) out += L"  "; };
    switch (v.kind) {
        case Value::NUL: out += L"null"; break;
        case Value::BOOL: out += v.b ? L"true" : L"false"; break;
        case Value::NUM: {
            wchar_t buf[64];
            if (v.num == (long long)v.num)
                swprintf(buf, 64, L"%lld", (long long)v.num);
            else
                swprintf(buf, 64, L"%.6g", v.num);
            out += buf;
            break;
        }
        case Value::STR: out += Escape(v.str); break;
        case Value::OBJ: {
            if (v.obj.empty()) { out += L"{}"; break; }
            out += L"{\n";
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) out += L",\n";
                first = false;
                pad(indent + 1);
                out += Escape(kv.first) + L": ";
                DumpValue(kv.second, indent + 1, out);
            }
            out += L"\n";
            pad(indent);
            out += L"}";
            break;
        }
        case Value::ARR: {
            if (v.arr.empty()) { out += L"[]"; break; }
            out += L"[\n";
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out += L",\n";
                pad(indent + 1);
                DumpValue(v.arr[i], indent + 1, out);
            }
            out += L"\n";
            pad(indent);
            out += L"]";
            break;
        }
    }
}

Value Parse(const std::wstring& text) {
    const wchar_t* p = text.c_str();
    const wchar_t* end = p + text.size();
    return ParseValue(p, end);
}

std::wstring Dump(const Value& v) {
    std::wstring out;
    DumpValue(v, 0, out);
    return out;
}

const Value& Value::child(const wchar_t* key) const {
    static const Value kEmpty;
    auto it = obj.find(key);
    return it == obj.end() ? kEmpty : it->second;
}

Value MakeObj() {
    Value v;
    v.kind = Value::OBJ;
    return v;
}
Value MakeInt(int i) {
    Value v;
    v.kind = Value::NUM;
    v.num = i;
    return v;
}
Value MakeNum(double d) {
    Value v;
    v.kind = Value::NUM;
    v.num = d;
    return v;
}
Value MakeBool(bool b) {
    Value v;
    v.kind = Value::BOOL;
    v.b = b;
    return v;
}
Value MakeStr(const std::wstring& s) {
    Value v;
    v.kind = Value::STR;
    v.str = s;
    return v;
}

std::wstring ReadFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD sz = GetFileSize(h, nullptr);
    std::wstring out(sz / 2, L'\0');
    DWORD rd = 0;
    if (sz >= 2) ::ReadFile(h, &out[0], sz, &rd, nullptr);
    CloseHandle(h);
    if (!out.empty() && out[0] == 0xFEFF) out.erase(out.begin());  // 去 BOM
    return out;
}

bool WriteFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD wr = 0;
    ::WriteFile(h, &bom, 2, &wr, nullptr);
    ::WriteFile(h, text.c_str(), (DWORD)(text.size() * sizeof(wchar_t)), &wr, nullptr);
    CloseHandle(h);
    return true;
}

}  // namespace jsonlite
