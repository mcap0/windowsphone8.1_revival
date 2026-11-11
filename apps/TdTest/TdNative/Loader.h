#pragma once
// Native side of the TDLib test: loads packaged DLLs with the real LoadPackagedLibrary (C# P/Invoke cannot
// reach it on WP8.1) and, when a DLL fails, walks its import table to name the missing DLL or function.
namespace TdNative
{
    public ref class Loader sealed
    {
    public:
        // "ok" or "ERRORE <code>"
        static Platform::String^ TryLoad(Platform::String^ name);
        // one line per imported DLL of the packaged file: loaded or not, and the functions it lacks
        static Platform::String^ CheckImports(Platform::String^ name);
        // td_execute getOption version through GetProcAddress
        static Platform::String^ TdVersion();
    };
}
