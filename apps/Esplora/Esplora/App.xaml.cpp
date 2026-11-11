#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "EditorPage.xaml.h"
#include "Console.h"

using namespace Esplora;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App()
{
    InitializeComponent();
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        frame->CacheSize = 3;
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid, e->Arguments);
    Window::Current->Activate();
}

// "esplora:<anything>" links: the exact URI received goes to share/inbox/esplora-uri.txt (to see what other apps pass on)
void App::OnActivated(IActivatedEventArgs^ e)
{
    auto p = dynamic_cast<ProtocolActivatedEventArgs^>(e);
    if (p != nullptr) PostLog(L"esplora-uri", std::wstring(p->Uri->RawUri->Data()));
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        frame->CacheSize = 3;
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr) frame->Navigate(MainPage::typeid);
    Window::Current->Activate();
}

// A text file opened from another app (mail, WhatsApp, Telegram, Office...): a copy in LocalFolder\apri goes to the
// editor; saving writes the copy and, when the other app allows it, the original too. Back returns to that app.
void App::OnFileActivated(FileActivatedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        frame->CacheSize = 3;
        Window::Current->Content = frame;
    }
    Window::Current->Activate();
    if (e->Files->Size == 0) { if (frame->Content == nullptr) frame->Navigate(MainPage::typeid); return; }
    auto file = dynamic_cast<Windows::Storage::StorageFile^>(e->Files->GetAt(0));
    if (file == nullptr) { if (frame->Content == nullptr) frame->Navigate(MainPage::typeid); return; }
    auto local = Windows::Storage::ApplicationData::Current->LocalFolder;
    concurrency::create_task(local->CreateFolderAsync(L"apri", Windows::Storage::CreationCollisionOption::OpenIfExists))
        .then([file](Windows::Storage::StorageFolder^ dir)
    {
        return concurrency::create_task(file->CopyAsync(dir, file->Name, Windows::Storage::NameCollisionOption::ReplaceExisting));
    }).then([frame, file](concurrency::task<Windows::Storage::StorageFile^> t)
    {
        Windows::Storage::StorageFile^ copy = nullptr;
        try { copy = t.get(); } catch (Platform::Exception^) {}
        if (copy == nullptr) { if (frame->Content == nullptr) frame->Navigate(MainPage::typeid); return; }
        EditorPage::SetOrigin(file);
        frame->Navigate(EditorPage::typeid, copy->Path);
    }, concurrency::task_continuation_context::use_current());
}

void App::OnBackPressed(Platform::Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) return;
    auto main = dynamic_cast<MainPage^>(frame->Content);
    if (main != nullptr)
    {
        if (main->HandleHardwareBack()) e->Handled = true;
        return;
    }
    auto editor = dynamic_cast<EditorPage^>(frame->Content);
    if (editor != nullptr && editor->HandleHardwareBack()) { e->Handled = true; return; }
    if (frame->CanGoBack) { frame->GoBack(); e->Handled = true; }
}
