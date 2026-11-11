#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Fs.h"
#include <algorithm>
#include <sstream>

namespace
{
    std::wstring GetAttributesPath(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data)
    {
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return L"";
        return Fs::ErrorText(GetLastError());
    }

    HANDLE OpenFile(const std::wstring& path, DWORD access, DWORD creation, DWORD attributes)
    {
        CREATEFILE2_EXTENDED_PARAMETERS parameters;
        ZeroMemory(&parameters, sizeof(parameters));
        parameters.dwSize = sizeof(parameters);
        parameters.dwFileAttributes = attributes;
        return CreateFile2(path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            creation, &parameters);
    }

    std::wstring DeletePath(const std::wstring& path, bool recursive)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        std::wstring error = GetAttributesPath(path, data);
        if (!error.empty())
            return error;

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            if (DeleteFileW(path.c_str()))
                return L"";
            return Fs::ErrorText(GetLastError());
        }

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            if (RemoveDirectoryW(path.c_str()))
                return L"";
            return Fs::ErrorText(GetLastError());
        }

        if (!recursive)
        {
            if (RemoveDirectoryW(path.c_str()))
                return L"";
            return Fs::ErrorText(GetLastError());
        }

        std::vector<Fs::Entry> entries;
        error = Fs::List(path, entries);
        if (!error.empty())
            return error;

        for (size_t i = 0; i < entries.size(); ++i)
        {
            std::wstring child = path;
            if (!child.empty() && child[child.size() - 1] != L'\\')
                child += L'\\';
            child += entries[i].name;
            error = DeletePath(child, true);
            if (!error.empty())
                return error;
        }

        if (RemoveDirectoryW(path.c_str()))
            return L"";
        return Fs::ErrorText(GetLastError());
    }

    std::wstring CopyFileContents(const std::wstring& source, const std::wstring& destination)
    {
        HANDLE sourceFile = OpenFile(source, GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        if (sourceFile == INVALID_HANDLE_VALUE)
            return Fs::ErrorText(GetLastError());

        HANDLE destinationFile = OpenFile(destination, GENERIC_WRITE, CREATE_NEW, FILE_ATTRIBUTE_NORMAL);
        if (destinationFile == INVALID_HANDLE_VALUE)
        {
            DWORD code = GetLastError();
            CloseHandle(sourceFile);
            return Fs::ErrorText(code);
        }

        unsigned char buffer[65536];
        DWORD errorCode = ERROR_SUCCESS;
        bool succeeded = true;

        for (;;)
        {
            DWORD amountRead = 0;
            if (!::ReadFile(sourceFile, buffer, sizeof(buffer), &amountRead, nullptr))
            {
                errorCode = GetLastError();
                succeeded = false;
                break;
            }

            if (amountRead == 0)
                break;

            DWORD position = 0;
            while (position < amountRead)
            {
                DWORD amountWritten = 0;
                if (!::WriteFile(destinationFile, buffer + position,
                    amountRead - position, &amountWritten, nullptr))
                {
                    errorCode = GetLastError();
                    succeeded = false;
                    break;
                }

                if (amountWritten == 0)
                {
                    errorCode = ERROR_WRITE_FAULT;
                    succeeded = false;
                    break;
                }

                position += amountWritten;
            }

            if (!succeeded)
                break;
        }

        if (succeeded && !FlushFileBuffers(destinationFile))
        {
            errorCode = GetLastError();
            succeeded = false;
        }

        CloseHandle(sourceFile);
        CloseHandle(destinationFile);

        if (!succeeded)
        {
            DeleteFileW(destination.c_str());
            return Fs::ErrorText(errorCode);
        }

        return L"";
    }

    std::wstring CopyPath(const std::wstring& source, const std::wstring& destination)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        std::wstring error = GetAttributesPath(source, data);
        if (!error.empty())
            return error;

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return CopyFileContents(source, destination);

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return Fs::ErrorText(ERROR_NOT_SUPPORTED);

        if (!CreateDirectoryW(destination.c_str(), nullptr))
            return Fs::ErrorText(GetLastError());

        std::vector<Fs::Entry> entries;
        error = Fs::List(source, entries);
        if (!error.empty())
            return error;

        for (size_t i = 0; i < entries.size(); ++i)
        {
            std::wstring childSource = source;
            if (!childSource.empty() && childSource[childSource.size() - 1] != L'\\')
                childSource += L'\\';
            childSource += entries[i].name;

            std::wstring childDestination = destination;
            if (!childDestination.empty() && childDestination[childDestination.size() - 1] != L'\\')
                childDestination += L'\\';
            childDestination += entries[i].name;

            error = CopyPath(childSource, childDestination);
            if (!error.empty())
                return error;
        }
        return L"";
    }
}

namespace Fs
{
    Entry::Entry() : isDir(false), size(0), attributes(0)
    {
        modified.dwLowDateTime = 0;
        modified.dwHighDateTime = 0;
    }

    std::wstring ErrorText(DWORD code)
    {
        const wchar_t* text = nullptr;
        switch (code)
        {
        case ERROR_ACCESS_DENIED: text = L"accesso negato"; break;
        case ERROR_FILE_NOT_FOUND: text = L"file non trovato"; break;
        case ERROR_PATH_NOT_FOUND: text = L"percorso non trovato"; break;
        case ERROR_SHARING_VIOLATION: text = L"file in uso"; break;
        case ERROR_ALREADY_EXISTS: text = L"esiste gi\u00e0"; break;
        case ERROR_FILE_EXISTS: text = L"file gi\u00e0 esistente"; break;
        case ERROR_DIRECTORY: text = L"percorso non \u00e8 una cartella"; break;
        case ERROR_DISK_FULL: text = L"spazio esaurito"; break;
        case ERROR_NOT_ENOUGH_MEMORY: text = L"memoria insufficiente"; break;
        case ERROR_FILENAME_EXCED_RANGE: text = L"percorso o nome troppo lungo"; break;
        case ERROR_INVALID_NAME: text = L"nome non valido"; break;
        case ERROR_DIR_NOT_EMPTY: text = L"cartella non vuota"; break;
        case ERROR_WRITE_PROTECT: text = L"supporto protetto da scrittura"; break;
        case ERROR_NOT_SUPPORTED: text = L"operazione non supportata"; break;
        case ERROR_GEN_FAILURE: text = L"errore generico"; break;
        default: text = L"errore Win32"; break;
        }

        std::wstringstream stream;
        stream << text << L" (" << code << L")";
        return stream.str();
    }

    std::wstring List(const std::wstring& path, std::vector<Entry>& entries)
    {
        entries.clear();

        std::wstring pattern = path;
        if (!pattern.empty() && pattern[pattern.size() - 1] != L'\\')
            pattern += L'\\';
        pattern += L'*';

        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
            return ErrorText(GetLastError());

        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;

            Entry entry;
            entry.name = data.cFileName;
            entry.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.attributes = data.dwFileAttributes;
            entry.modified = data.ftLastWriteTime;
            entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                static_cast<uint64_t>(data.nFileSizeLow);
            entries.push_back(entry);
        } while (FindNextFileW(find, &data));

        DWORD code = GetLastError();
        FindClose(find);
        if (code != ERROR_NO_MORE_FILES)
            return ErrorText(code);
        return L"";
    }

    std::wstring ReadFile(const std::wstring& path, std::vector<unsigned char>& bytes, size_t maxBytes)
    {
        bytes.clear();
        HANDLE file = OpenFile(path, GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE)
            return ErrorText(GetLastError());

        try
        {
            unsigned char buffer[65536];
            for (;;)
            {
                DWORD read = 0;
                if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr))
                {
                    DWORD code = GetLastError();
                    CloseHandle(file);
                    bytes.clear();
                    return ErrorText(code);
                }
                if (read == 0)
                    break;

                if (bytes.size() > maxBytes ||
                    static_cast<size_t>(read) > maxBytes - bytes.size())
                {
                    CloseHandle(file);
                    bytes.clear();
                    return L"file troppo grande per questa operazione";
                }

                bytes.insert(bytes.end(), buffer, buffer + read);
            }
        }
        catch (...)
        {
            CloseHandle(file);
            bytes.clear();
            return ErrorText(ERROR_NOT_ENOUGH_MEMORY);
        }

        CloseHandle(file);
        return L"";
    }

    std::wstring WriteFile(const std::wstring& path, const std::vector<unsigned char>& bytes)
    {
        HANDLE file = OpenFile(path, GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE)
            return ErrorText(GetLastError());

        size_t position = 0;
        while (position < bytes.size())
        {
            size_t remaining = bytes.size() - position;
            DWORD amount = remaining > 0x7fffffff ? 0x7fffffff : static_cast<DWORD>(remaining);
            DWORD written = 0;
            if (!::WriteFile(file, &bytes[position], amount, &written, nullptr))
            {
                DWORD code = GetLastError();
                CloseHandle(file);
                return ErrorText(code);
            }
            if (written == 0)
            {
                CloseHandle(file);
                return ErrorText(ERROR_WRITE_FAULT);
            }
            position += written;
        }

        if (!FlushFileBuffers(file))
        {
            DWORD code = GetLastError();
            CloseHandle(file);
            return ErrorText(code);
        }
        CloseHandle(file);
        return L"";
    }

    std::wstring GetInfo(const std::wstring& path, Entry& entry)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        std::wstring error = GetAttributesPath(path, data);
        if (!error.empty())
            return error;

        size_t slash = path.find_last_of(L"\\/");
        entry.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (entry.name.empty() && path.size() >= 2 && path[path.size() - 1] == L'\\')
            entry.name = path.substr(0, path.size() - 1);
        entry.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
            static_cast<uint64_t>(data.nFileSizeLow);
        entry.modified = data.ftLastWriteTime;
        entry.attributes = data.dwFileAttributes;
        return L"";
    }

    std::wstring CreateDirectory(const std::wstring& path)
    {
        if (::CreateDirectoryW(path.c_str(), nullptr))
            return L"";
        return ErrorText(GetLastError());
    }

    std::wstring Delete(const std::wstring& path, bool recursive)
    {
        return DeletePath(path, recursive);
    }

    std::wstring Move(const std::wstring& source, const std::wstring& destination)
    {
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_COPY_ALLOWED))
            return L"";
        return ErrorText(GetLastError());
    }

    std::wstring Copy(const std::wstring& source, const std::wstring& destination)
    {
        return CopyPath(source, destination);
    }

    bool Exists(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
    }

    bool IsDirectory(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool CanWrite(const std::wstring& path, std::wstring& error)
    {
        HANDLE file = OpenFile(path, GENERIC_WRITE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = ErrorText(GetLastError());
            return false;
        }
        CloseHandle(file);
        error.clear();
        return true;
    }
}
