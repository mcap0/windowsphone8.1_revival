#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Orologio;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App()
{
    InitializeComponent();
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

// also the toast of the timer opens the app here (Arguments = "timer")
void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    Show(e->Arguments);
}

// orologio:timer?secondi=N (from Assistente): start a timer of N seconds
void App::OnActivated(IActivatedEventArgs^ e)
{
    if (e->Kind == ActivationKind::Protocol)
    {
        auto p = safe_cast<ProtocolActivatedEventArgs^>(e);
        Show(p->Uri != nullptr ? p->Uri->RawUri : nullptr);
    }
    else Show(nullptr);
}

void App::Show(Platform::String^ args)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid, args);
    else
    {
        auto main = dynamic_cast<MainPage^>(frame->Content);
        if (main != nullptr) main->OnLaunchArguments(args);
    }
    Window::Current->Activate();
}

void App::OnBackPressed(Platform::Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) return;
    auto main = dynamic_cast<MainPage^>(frame->Content);
    if (main != nullptr && main->HandleHardwareBack()) e->Handled = true;
}
