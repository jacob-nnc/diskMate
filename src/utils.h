#pragma once
#include <windows.h>
#include <string>

// 格式化字节数为可读文本（B/KB/MB/GB/TB）
std::wstring FormatSize(ULONGLONG bytes);

// 格式化整数
std::wstring FormatCount(ULONGLONG n);

// 规范化扫描根路径："C:" -> "C:\"，空 -> "C:\"，统一结尾反斜杠
std::wstring NormalizeRoot(const std::wstring& in);

// 判断路径是否为存在的目录
bool IsDirPath(const std::wstring& path);

// 提取文件扩展名（含点，如 ".jar"）；无扩展名返回空串
std::wstring ExtOf(const std::wstring& name);

// 取父目录路径（含结尾分隔符）："C:\a\b.txt" -> "C:\a\"，"C:\a\b\" -> "C:\a\"
std::wstring GetParentPath(const std::wstring& path);
