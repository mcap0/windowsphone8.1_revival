// App.xaml.cpp: creates the frame with the map page; unhandled errors go to the PC log instead of closing the app.
#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "Logger.h"

using namespace Mappe81;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App() {
    InitializeComponent();
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e) {
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr)
        frame->Navigate(MainPage::typeid, e->Arguments);
    Window::Current->Activate();
}

void App::OnUnhandled(Object^ sender, UnhandledExceptionEventArgs^ e) {
    e->Handled = true;
    Logger::Write(L"unhandled: " + std::wstring(e->Message != nullptr ? e->Message->Data() : L"?"));
}
