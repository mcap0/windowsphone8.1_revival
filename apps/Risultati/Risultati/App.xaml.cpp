#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Risultati;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
}

// SearchCM (patched NavigateInPlace) launches us with the search URL as arguments, every time -> is pressed.
void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    Show(e->Arguments);
}

void App::OnActivated(IActivatedEventArgs^ e)
{
    if (e->Kind == ActivationKind::Protocol)
        Show(safe_cast<ProtocolActivatedEventArgs^>(e)->Uri->RawUri);
    else
        Show(nullptr);
}

void App::Show(String^ args)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        frame->ContentTransitions = nullptr;  // no turnstile: our page must look like the Bing one it replaces
        Window::Current->Content = frame;
    }
    auto page = dynamic_cast<MainPage^>(frame->Content);
    if (page == nullptr)
        frame->Navigate(MainPage::typeid, args);
    else
        page->Start(args);
    Window::Current->Activate();
}

void App::OnUnhandled(Object^, UnhandledExceptionEventArgs^ e)
{
    e->Handled = true;
    ApplicationData::Current->LocalSettings->Values->Insert("lastCrash", e->Message);
}
