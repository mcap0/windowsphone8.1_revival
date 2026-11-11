#pragma once

#include <functional>
#include <string>

// The WP8 VoIP stack (Windows.Phone.Networking.Voip: native ringing screen, lock screen answer, call audio) from a
// Runtime app. The class is in the phone's firmware (CommsProxy.dll) but not in the WP8.1 Runtime SDK's metadata, so
// it is called through its ABI: interface layouts and IIDs from the Windows 10 SDK's windows.phone.networking.voip.h
// (WinRT interfaces never change). Needs ID_CAP_VOIP (WindowsPhoneReservedAppInfo.xml). PLAN §69.
namespace Voip
{
    struct Events
    {
        std::function<void()> answered;
        std::function<void(int reason)> rejected;   // 0 ignored, 1 timed out, 2 other call, 3 emergency, 4 bad state
        std::function<void()> endRequested;
        std::function<void()> holdRequested;
        std::function<void()> resumeRequested;
    };

    class Call;

    // Starts ringing with the system's incoming call screen. Returns null and fills err on failure.
    struct Options
    {
        std::wstring context = L"/call", details = L"";
        Platform::String^ image = nullptr;      // file path or ms-appx URI, or null
        Platform::String^ ringtone = nullptr;   // same
        int ringSeconds = 30;
    };
    Call* RequestIncoming(const std::wstring& name, const std::wstring& number, const std::wstring& service,
                          const Options& o, const Events& events, std::wstring& err);
    HRESULT NotifyActive(Call* call);
    HRESULT NotifyEnded(Call* call);   // also releases the call
}
