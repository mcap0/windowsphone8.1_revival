#include "pch.h"
#include "App.xaml.h"
#include "tscrypto.h"
#include "MainPage.xaml.h"

#include <cstdlib>
#include <exception>

using namespace Telecomando;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

namespace
{
    // TsNet (mbedTLS, WebSocket masks) draws its random bytes from here: without it ts::randomBytes calls abort(), which on
    // the phone ends the app with no log line (the TLS ClientHello needs 32 random bytes before anything is sent)
    void WinRandom(uint8_t* out, size_t n)
    {
        Platform::Array<unsigned char>^ a;
        Windows::Security::Cryptography::CryptographicBuffer::CopyToByteArray(
            Windows::Security::Cryptography::CryptographicBuffer::GenerateRandom(static_cast<unsigned int>(n)), &a);
        memcpy(out, a->Data, n);
    }
}

App::App()
{
    InitializeComponent();
    ts::setRandomSource(&WinRandom);
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    Resuming += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &App::OnResuming);
    // what made the app close, in run.log (read from the PC with tools/phonelog <product id>)
    UnhandledException += ref new UnhandledExceptionEventHandler([](Platform::Object^, UnhandledExceptionEventArgs^ e)
    {
        std::wstring m = e->Message == nullptr ? L"" : e->Message->Data();
        Log("CRASH 0x" + [](unsigned v) { char b[16]; sprintf_s(b, "%08X", v); return std::string(b); }(static_cast<unsigned>(e->Exception.Value)) +
            " " + std::string(m.begin(), m.end()));
    });
    std::set_terminate([] { Log("CRASH std::terminate"); abort(); });
}

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid);
    Window::Current->Activate();
}

static MainPage^ Main()
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    return frame == nullptr ? nullptr : dynamic_cast<MainPage^>(frame->Content);
}

void App::OnBackPressed(Platform::Object^, BackPressedEventArgs^ e)
{
    auto main = Main();
    if (main != nullptr && main->HandleBack()) e->Handled = true;
}

// the TV connection is closed while the app is in the background and opened again on return
void App::OnSuspending(Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^)
{
    auto main = Main();
    if (main != nullptr) main->Suspend();
}

void App::OnResuming(Platform::Object^, Platform::Object^)
{
    auto main = Main();
    if (main != nullptr) main->Resume();
}
