#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "tscrypto.h"

#include <cstdlib>
#include <exception>

using namespace FotoPixel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

namespace
{
    // TsNet draws its random bytes from here (without it ts::randomBytes aborts)
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
    Resuming += ref new Windows::Foundation::EventHandler<Platform::Object^>([](Platform::Object^, Platform::Object^)
    {
        auto frame = dynamic_cast<Frame^>(Window::Current->Content);
        auto main = frame == nullptr ? nullptr : dynamic_cast<MainPage^>(frame->Content);
        if (main != nullptr) main->Resume();
    });
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
    // what made the app close, in run.log (tools/phonelog 9dce49e3-42b3-43e8-9cd4-5bb5843d3b1c)
    UnhandledException += ref new UnhandledExceptionEventHandler([](Platform::Object^, UnhandledExceptionEventArgs^ e)
    {
        std::wstring m = e->Message == nullptr ? L"" : e->Message->Data();
        char b[16];
        sprintf_s(b, "%08X", static_cast<unsigned>(e->Exception.Value));
        Log("CRASH 0x" + std::string(b) + " " + Narrow(m));
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

void App::OnBackPressed(Platform::Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto main = frame == nullptr ? nullptr : dynamic_cast<MainPage^>(frame->Content);
    if (main != nullptr && main->HandleBack()) e->Handled = true;
}
