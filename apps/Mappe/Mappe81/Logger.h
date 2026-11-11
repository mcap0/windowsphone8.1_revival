// Logger: lines for the PC (POST /mappe to tools/inbox) and the debug output. Write() is thread-safe;
// the UI thread sends the buffered lines every few seconds.
#pragma once

#include <string>

class Logger {
public:
    static void Write(const std::wstring& line);
    static void WriteUtf8(const char* line);
    // Starts the timer that posts the buffer (UI thread).
    static void Start(Platform::String^ inboxUrl);
    // Sends a named block right away (e.g. a base64 screenshot to /mappe-shot).
    static void PostNow(Platform::String^ name, Platform::String^ body);
};
