//
// App.xaml.cpp
// Implementation of the App class.
//

#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace MappeCpp;

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

App::App() {
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e) {
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr) {
        rootFrame = ref new Frame();
        rootFrame->CacheSize = 1;
        Window::Current->Content = rootFrame;
    }
    if (rootFrame->Content == nullptr) {
        if (!rootFrame->Navigate(MainPage::typeid, e->Arguments)) {
            throw ref new FailureException("Failed to create initial page");
        }
    }
    Window::Current->Activate();
}

// Remember the map view when the app is suspended (the page may not get OnNavigatedFrom).
void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e) {
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) return;
    auto page = dynamic_cast<MainPage^>(frame->Content);
    if (page != nullptr) page->SaveView();
}

// Keeps the app alive and stores the error so that MainPage can show it on the phone.
void App::OnUnhandled(Object^ sender, UnhandledExceptionEventArgs^ e) {
    e->Handled = true;
    String^ msg = e->Message != nullptr ? e->Message : ref new String(L"errore sconosciuto");
    ApplicationData::Current->LocalSettings->Values->Insert(ref new String(L"lastCrash"), msg);
}
