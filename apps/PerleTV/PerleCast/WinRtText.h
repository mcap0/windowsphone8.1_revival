#ifndef PERLECAST_WINRTTEXT_H
#define PERLECAST_WINRTTEXT_H

#include <string>
#include <windows.h>

namespace PerleCast {

inline std::string ToUtf8(Platform::String^ value)
{
    if (value == nullptr || value->IsEmpty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
                        &result[0], size, nullptr, nullptr);
    return result;
}

inline Platform::String^ FromUtf8(const std::string& value)
{
    if (value.empty()) return ref new Platform::String(L"");
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return ref new Platform::String(L"");
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        &result[0], size);
    return ref new Platform::String(result.c_str(), size);
}

} // namespace PerleCast

#endif
