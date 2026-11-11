#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace EngineTest;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
}

// One page and no navigation: the Back key keeps its default behaviour (closes the app).
void App::OnLaunched(LaunchActivatedEventArgs^)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
        frame->Navigate(MainPage::typeid);
    }
    Window::Current->Activate();
}
