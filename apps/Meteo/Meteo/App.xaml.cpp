#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Meteo;
using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App() {
    InitializeComponent();
    HardwareButtons::BackPressed += ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

// from a place's own tile, Arguments = that place
void App::OnLaunched(LaunchActivatedEventArgs^ e) {
    Show(e->Arguments != nullptr && !e->Arguments->IsEmpty() ? e->Arguments : nullptr);
    RegisterTileTask();
}

// meteo:?luogo=Palermo&giorno=AAAA-MM-GG (from the Assistente)
void App::OnActivated(IActivatedEventArgs^ e) {
    auto p = dynamic_cast<ProtocolActivatedEventArgs^>(e);
    Show(p != nullptr && p->Uri != nullptr ? p->Uri->RawUri : nullptr);
    RegisterTileTask();
}

void App::Show(String^ uri) {
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr || dynamic_cast<MainPage^>(frame->Content) == nullptr) {
        frame->Navigate(MainPage::typeid, uri);
    } else {
        safe_cast<MainPage^>(frame->Content)->Open(uri);
    }
    Window::Current->Activate();
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e) {
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr && frame->CanGoBack) {
        frame->GoBack();
        e->Handled = true;
    }
}

// the Start tile follows the home place every 30 minutes, also with the app closed
void App::RegisterTileTask() {
    create_task(BackgroundExecutionManager::RequestAccessAsync()).then([](task<BackgroundAccessStatus> t) {
        try {
            t.get();
            auto tasks = BackgroundTaskRegistration::AllTasks;
            for (auto it = tasks->First(); it->HasCurrent; it->MoveNext())
                if (it->Current->Value->Name == L"MeteoTile") return;
            auto builder = ref new BackgroundTaskBuilder();
            builder->Name = L"MeteoTile";
            builder->TaskEntryPoint = L"MeteoTask.TileTask";
            builder->SetTrigger(ref new TimeTrigger(30, false));
            builder->AddCondition(ref new SystemCondition(SystemConditionType::InternetAvailable));
            builder->Register();
        } catch (Exception^ ex) {
            MeteoCore::Log(L"registrazione task: " + std::wstring(ex->Message->Data()));
        }
    });
}
