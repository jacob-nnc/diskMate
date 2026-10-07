#include "utils.h"
#include <cstdio>
#include <string>

std::wstring FormatSize(ULONGLONG bytes) {
    wchar_t buf[64];
    const unsigned long long KB = 1024ull;
    const unsigned long long MB = 1024ull * 1024;
    const unsigned long long GB = 1024ull * 1024 * 1024;
    const unsigned long long TB = 1024ull * 1024 * 1024 * 1024;
    if (bytes < KB)
        swprintf(buf, 64, L"%llu B", bytes);
    else if (bytes < MB)
        swprintf(buf, 64, L"%.1f KB", (double)bytes / 1024.0);
    else if (bytes < GB)
        swprintf(buf, 64, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes < TB)
        swprintf(buf, 64, L"%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else
        swprintf(buf, 64, L"%.2f TB", (double)bytes / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    return std::wstring(buf);
}

std::wstring FormatCount(ULONGLONG n) {
    return std::to_wstring(n);
}

std::wstring NormalizeRoot(const std::wstring& in) {
    std::wstring p = in;
    // 去首尾空白
    while (!p.empty() && (p.front() == L' ' || p.front() == L'\t')) p.erase(p.begin());
    while (!p.empty() && (p.back() == L' ' || p.back() == L'\t')) p.pop_back();
    if (p.empty()) return L"C:\\";
    if (p == L"/" || p == L"\\") return L"C:\\";
    // "C:" -> "C:\"
    if (p.size() == 2 && (p[1] == L':' || p[1] == L':')) p.push_back(L'\\');
    // 统一尾部分隔符
    if (p.back() != L'\\' && p.back() != L'/') p.push_back(L'\\');
    return p;
}

bool IsDirPath(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ExtOf(const std::wstring& name) {
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos || pos + 1 >= name.size()) return L"";
    return name.substr(pos);
}

std::wstring GetParentPath(const std::wstring& path) {
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p + L"\\";
    return p.substr(0, pos + 1);
}
