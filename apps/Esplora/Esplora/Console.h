#pragma once
namespace Esplora
{
    // Console di diagnostica dal PC (solo lettura). Legge http://192.168.1.148:8765/esplora-cmd.txt e posta in /esplora-out.
    void SetConsoleEnabled(bool on);
    bool IsConsoleEnabled();
    // marks the command waiting on the PC as done without running it; done(text) on the UI thread
    void SkipPendingCommand(std::function<void(std::wstring)> done);
    void PostLog(const std::wstring& name, const std::wstring& text);
}
