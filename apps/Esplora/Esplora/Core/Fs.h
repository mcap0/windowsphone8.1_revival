#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <stdint.h>

namespace Fs
{
    struct Entry
    {
        std::wstring name;
        bool isDir;
        uint64_t size;
        FILETIME modified;
        DWORD attributes;

        Entry();
    };

    std::wstring ErrorText(DWORD code);
    std::wstring List(const std::wstring& path, std::vector<Entry>& entries);
    std::wstring ReadFile(const std::wstring& path, std::vector<unsigned char>& bytes, size_t maxBytes);
    std::wstring WriteFile(const std::wstring& path, const std::vector<unsigned char>& bytes);
    std::wstring GetInfo(const std::wstring& path, Entry& entry);
    std::wstring CreateDirectory(const std::wstring& path);
    std::wstring Delete(const std::wstring& path, bool recursive);
    std::wstring Move(const std::wstring& source, const std::wstring& destination);
    std::wstring Copy(const std::wstring& source, const std::wstring& destination);
    bool Exists(const std::wstring& path);
    bool IsDirectory(const std::wstring& path);
    bool CanWrite(const std::wstring& path, std::wstring& error);
}
