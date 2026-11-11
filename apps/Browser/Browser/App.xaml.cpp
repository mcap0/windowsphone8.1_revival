#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Browser;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
}

// One page and no navigation: the Back key keeps its default behaviour (closes the app). A page's tile on Start
// launches with its address as the argument: a tab for it, in the running app or at the first start.
void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
        frame->Navigate(MainPage::typeid, e->Arguments);
    }
    else if (e->Arguments && e->Arguments->Length())
    {
        if (auto page = dynamic_cast<MainPage^>(frame->Content))
            page->OpenFromLaunch(e->Arguments->Data());
    }
    Window::Current->Activate();
}

// The address of an internet:u/<address> link (internet:<address> works too), or null.
static Platform::String^ ProtocolAddress(IActivatedEventArgs^ e)
{
    auto protocol = dynamic_cast<ProtocolActivatedEventArgs^>(e);
    if (protocol == nullptr || protocol->Uri == nullptr)
        return nullptr;
    std::wstring raw = protocol->Uri->RawUri->Data();
    const wchar_t* prefixes[] = { L"internet:u/", L"internet://u/", L"internet:" };
    for (auto prefix : prefixes)
    {
        size_t length = wcslen(prefix);
        if (raw.size() > length && _wcsnicmp(raw.c_str(), prefix, length) == 0)
            return ref new Platform::String(raw.substr(length).c_str());
    }
    return nullptr;
}

// Another app opened a link here (internet:), or the WP8.1 pickers bring their result back (they suspend the app). If
// the system ended the app meanwhile, it starts again with its tabs, and the photos have no page waiting any more.
void App::OnActivated(IActivatedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto address = ProtocolAddress(e);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
        frame->Navigate(MainPage::typeid, address);
    }
    else if (address != nullptr)
    {
        if (auto page = dynamic_cast<MainPage^>(frame->Content))
            page->OpenFromLaunch(address->Data());
    }
    else if (auto picked = dynamic_cast<FileOpenPickerContinuationEventArgs^>(e))
    {
        if (auto page = dynamic_cast<MainPage^>(frame->Content))
            page->OnFilesPicked(picked->Files);
    }
    Window::Current->Activate();
}
