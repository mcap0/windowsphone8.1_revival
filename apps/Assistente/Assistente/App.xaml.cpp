#include "pch.h"
#include "App.xaml.h"
#include "OfflineCommand.h"
#include "MainPage.xaml.h"
#include "Logger.h"

using namespace Assistente;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App() {
    InitializeComponent();
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
    Resuming += ref new Windows::Foundation::EventHandler<Object^>(this, &App::OnResuming);
}

void App::Show(String^ reason) {
    OfflineCommand::Prepare();  // grammar and network: start them before the page
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) {
        frame = ref new Frame();
        frame->ContentTransitions = nullptr;  // no turnstile on open: straight from the Search key to the page
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid, reason);
    else {
        auto page = dynamic_cast<MainPage^>(frame->Content);
        if (page != nullptr) page->Listen(reason);
    }
    Window::Current->Activate();
}

void App::OnLaunched(LaunchActivatedEventArgs^ e) {
    Show(L"riquadro");
}

void App::OnActivated(IActivatedEventArgs^ e) {
    if (e->Kind == ActivationKind::Protocol) {
        auto p = safe_cast<ProtocolActivatedEventArgs^>(e);
        Show(p->Uri != nullptr ? p->Uri->RawUri : L"assistente:");
    } else {
        Show(L"attivazione");
    }
}

// Coming back to an assistant left open: listen again, as a fresh start would.
void App::OnResuming(Object^, Object^) {
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto page = frame != nullptr ? dynamic_cast<MainPage^>(frame->Content) : nullptr;
    if (MainPage::NoListenOnResume) {   // back from a call: the user did not ask for anything
        MainPage::NoListenOnResume = false;
        return;
    }
    if (page != nullptr) page->Listen(L"ripresa");
}

void App::OnUnhandled(Object^, UnhandledExceptionEventArgs^ e) {
    e->Handled = true;
    Logger::Write(L"unhandled: " + std::wstring(e->Message != nullptr ? e->Message->Data() : L"?"));
    Logger::FlushNow();
}
