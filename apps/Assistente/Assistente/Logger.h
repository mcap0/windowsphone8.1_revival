// Logger: lines for the PC (POST /<name> to tools/inbox) and the debug output. Write() is thread-safe;
// the UI thread sends the buffered lines every few seconds. Every line also goes to LocalFolder\log.txt,
// which Start() sends as the previous session (the last lines before a crash).
#pragma once

#include <string>

class Logger {
public:
    static void Write(const std::wstring& line);
    static void WriteUtf8(const char* line);
    // Starts the timer that posts the buffer (UI thread).
    static void Start(Platform::String^ inboxUrl);
    // Sends the buffered lines now instead of at the next tick (UI thread).
    static void FlushNow();
    // Sends a named block right away (e.g. a longer report).
    static void PostNow(Platform::String^ name, Platform::String^ body);
};
