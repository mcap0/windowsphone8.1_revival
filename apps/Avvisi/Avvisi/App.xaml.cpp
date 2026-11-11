#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Avvisi;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App()
{
    InitializeComponent();
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

// the toasts of the background task open the app here too (Arguments "start" / "tick")
void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid, e->Arguments);
    Window::Current->Activate();
}

// one page only: Back leaves the app (the background task keeps running)
void App::OnBackPressed(Platform::Object^, BackPressedEventArgs^ e)
{
    e->Handled = false;
}
