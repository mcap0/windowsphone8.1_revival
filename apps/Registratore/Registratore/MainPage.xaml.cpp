#include "pch.h"
#include "MainPage.xaml.h"
#include "AudioDecode.h"

using namespace Registratore;
using namespace Platform;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Media::Capture;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace
{
    // recording formats tried in order: the first one the phone's encoders accept is used
    const wchar_t* kExt[] = { L".m4a", L".wav", L".wma" };
    MediaEncodingProfile^ Profile(int i)
    {
        if (i == 0) return MediaEncodingProfile::CreateM4a(AudioEncodingQuality::Medium);
        if (i == 1) return MediaEncodingProfile::CreateWav(AudioEncodingQuality::Medium);
        return MediaEncodingProfile::CreateWma(AudioEncodingQuality::Medium);
    }

    String^ Hex(int v)
    {
        wchar_t b[16];
        swprintf_s(b, L"0x%08X", static_cast<unsigned int>(v));
        return ref new String(b);
    }
}

MainPage::MainPage() : startTick_(0), recording_(false), busy_(false)
{
    InitializeComponent();
    items_ = ref new Platform::Collections::Vector<RecItem^>();
    List->ItemsSource = items_;
    mics_ = ref new Platform::Collections::Vector<MicItem^>();
    MicList->ItemsSource = mics_;
    timer_ = ref new DispatcherTimer();
    TimeSpan ts;
    ts.Duration = 2500000;
    timer_->Interval = ts;
    timer_->Tick += ref new EventHandler<Object^>([this](Object^, Object^)
    {
        unsigned long long s = (GetTickCount64() - startTick_) / 1000;
        wchar_t b[16];
        swprintf_s(b, L"%llu:%02llu", s / 60, s % 60);
        TimerText->Text = ref new String(b);
    });
    create_task(Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(Windows::Devices::Enumeration::DeviceClass::AudioCapture))
        .then([this](task<Windows::Devices::Enumeration::DeviceInformationCollection^> t)
    {
        try
        {
            auto all = t.get();
            String^ line = L"audio capture devices:";
            for (unsigned int i = 0; i < all->Size; ++i)
                line += L" [" + all->GetAt(i)->Name + L" enabled=" + (all->GetAt(i)->IsEnabled ? L"1" : L"0") + L"]";
            Log(line);
        }
        catch (Exception^ ex) { Log(L"FindAllAsync failed " + ex->Message); }
    });
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"Registrazioni", CreationCollisionOption::OpenIfExists))
        .then([this](StorageFolder^ f) { folder_ = f; LoadList(); });
}

void MainPage::Log(String^ line)
{
    // diagnostics to the PC inbox (tools/inbox); failures are ignored
    auto http = ref new Windows::Web::Http::HttpClient();
    create_task(http->PostAsync(ref new Uri(L"http://192.168.1.148:8765/registratore"),
                                ref new Windows::Web::Http::HttpStringContent(line)))
        .then([http](task<Windows::Web::Http::HttpResponseMessage^> t) { try { t.get(); } catch (Exception^) {} });
}

void MainPage::LoadList()
{
    create_task(folder_->GetFilesAsync()).then([this](Windows::Foundation::Collections::IVectorView<StorageFile^>^ files)
    {
        std::vector<StorageFile^> v;
        for (unsigned int i = 0; i < files->Size; ++i) v.push_back(files->GetAt(i));
        std::sort(v.begin(), v.end(), [](StorageFile^ a, StorageFile^ b) { return a->DateCreated.UniversalTime > b->DateCreated.UniversalTime; });
        items_->Clear();
        for (size_t i = 0; i < v.size(); ++i)
        {
            auto item = ref new RecItem(v[i]);
            items_->Append(item);
            Describe(item);
        }
    });
}

void MainPage::Describe(RecItem^ item)
{
    auto cal = ref new Windows::Globalization::Calendar();
    cal->SetDateTime(item->file->DateCreated);
    wchar_t title[64];
    swprintf_s(title, L"%02d/%02d  %02d:%02d", cal->Day, cal->Month, cal->Hour, cal->Minute);
    String^ t = ref new String(title);
    item->SetText(t, item->file->FileType);
    create_task(item->file->Properties->GetMusicPropertiesAsync()).then([item, t](task<MusicProperties^> mt)
    {
        try
        {
            auto m = mt.get();
            unsigned long long s = m->Duration.Duration / 10000000;
            wchar_t d[64];
            swprintf_s(d, L"%llu:%02llu  \u00b7  %s", s / 60, s % 60, item->file->FileType->Data() + 1);
            item->SetText(t, ref new String(d));
        }
        catch (Exception^) {}
    });
}

void MainPage::Delete_Tapped(Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    auto element = dynamic_cast<FrameworkElement^>(sender);
    auto item = element == nullptr ? nullptr : dynamic_cast<RecItem^>(element->DataContext);
    if (item != nullptr) Delete(item);
}

void MainPage::Player_Opened(Object^, RoutedEventArgs^)
{
    wchar_t b[64];
    swprintf_s(b, L"player opened, %lld ms", Player->NaturalDuration.TimeSpan.Duration / 10000);
    Log(ref new String(b));
}

void MainPage::Player_State(Object^, RoutedEventArgs^)
{
    wchar_t b[96];
    swprintf_s(b, L"player state %d volume %.2f muted %d", static_cast<int>(Player->CurrentState), Player->Volume, Player->IsMuted ? 1 : 0);
    Log(ref new String(b));
}

// decode the new recording and log how loud it is: tells a silent microphone from a broken player
void MainPage::Analyze(RecItem^ item)
{
    std::wstring path(item->file->Path->Data());
    create_task([path]()
    {
        std::string wav, err;
        int peak = -1;
        double secs = 0;
        bool ok = DecodeToPcmWav(path, wav, err, &peak, &secs);
        wchar_t b[200];
        swprintf_s(b, L"decode %s: %S peak %d%% %.1f s %u bytes", ok ? L"ok" : L"FAILED", err.c_str(), peak, secs,
                   static_cast<unsigned>(wav.size()));
        return ref new String(b);
    }).then([this](String^ line)
    {
        Log(line);
        bool silent = wcsstr(line->Data(), L"peak 0%") || wcsstr(line->Data(), L"peak 1%");
        StatusText->Text = silent ? L"Registrazione muta: usa \u2026 \u2192 test microfoni" : L"Tocca per registrare";
    }, task_continuation_context::use_current());
}

void MainPage::Record_Tapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    if (busy_) return;
    if (recording_) StopRecording();
    else StartRecording();
}

void MainPage::SetRecordingUi(bool on)
{
    RecordDot->Visibility = on ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    StopSquare->Visibility = on ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    StatusText->Text = on ? L"Registrazione... tocca per fermare" : L"Tocca per registrare";
    if (on) { startTick_ = GetTickCount64(); TimerText->Text = L"0:00"; timer_->Start(); }
    else timer_->Stop();
}

void MainPage::StartRecording()
{
    StopPlayback();
    busy_ = true;
    StatusText->Text = L"Avvio microfono...";
    capture_ = ref new MediaCapture();
    auto settings = ref new MediaCaptureInitializationSettings();
    settings->StreamingCaptureMode = StreamingCaptureMode::Audio;
    create_task(Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(Windows::Devices::Enumeration::DeviceClass::AudioCapture))
        .then([this, settings](task<Windows::Devices::Enumeration::DeviceInformationCollection^> t)
    {
        try { settings->AudioDeviceId = ChosenDevice(t.get()); }
        catch (Exception^) {}
        return create_task(capture_->InitializeAsync(settings));
    }, task_continuation_context::use_current()).then([this](task<void> t)
    {
        try { t.get(); }
        catch (Exception^ ex)
        {
            Log(L"InitializeAsync failed " + Hex(ex->HResult) + L" " + ex->Message);
            StatusText->Text = L"Microfono non disponibile: " + ex->Message;
            capture_ = nullptr;
            busy_ = false;
            return;
        }
        auto dc = capture_->AudioDeviceController;
        wchar_t b[200];
        swprintf_s(b, L"InitializeAsync ok device %s muted %d volume %.0f",
                   capture_->MediaCaptureSettings->AudioDeviceId->Data(), dc->Muted ? 1 : 0, dc->VolumePercent);
        Log(ref new String(b));
        try { dc->Muted = false; dc->VolumePercent = 100; } catch (Exception^ ex) { Log(L"set volume failed " + ex->Message); }
        TryProfile(0);
    });
}

void MainPage::TryProfile(int index)
{
    if (index > 2)
    {
        StatusText->Text = L"Nessun formato di registrazione funziona (vedi log)";
        capture_ = nullptr;
        busy_ = false;
        return;
    }
    auto cal = ref new Windows::Globalization::Calendar();
    wchar_t name[64];
    swprintf_s(name, L"rec_%04d%02d%02d_%02d%02d%02d%s", cal->Year, cal->Month, cal->Day, cal->Hour, cal->Minute, cal->Second, kExt[index]);
    create_task(folder_->CreateFileAsync(ref new String(name), CreationCollisionOption::GenerateUniqueName))
        .then([this, index](StorageFile^ file)
    {
        recFile_ = file;
        return create_task(capture_->StartRecordToStorageFileAsync(Profile(index), file));
    }).then([this, index](task<void> t)
    {
        try { t.get(); }
        catch (Exception^ ex)
        {
            Log(L"record " + ref new String(kExt[index]) + L" failed " + Hex(ex->HResult) + L" " + ex->Message);
            if (recFile_ != nullptr) create_task(recFile_->DeleteAsync()).then([](task<void> d) { try { d.get(); } catch (Exception^) {} });
            recFile_ = nullptr;
            TryProfile(index + 1);
            return;
        }
        Log(L"recording " + ref new String(kExt[index]));
        recording_ = true;
        busy_ = false;
        SetRecordingUi(true);
    });
}

void MainPage::StopRecording()
{
    if (!recording_ || capture_ == nullptr) return;
    busy_ = true;
    recording_ = false;
    SetRecordingUi(false);
    StatusText->Text = L"Salvataggio...";
    auto capture = capture_;
    create_task(capture->StopRecordAsync()).then([this, capture](task<void> t)
    {
        try { t.get(); Log(L"stopped ok"); }
        catch (Exception^ ex) { Log(L"StopRecordAsync failed " + Hex(ex->HResult) + L" " + ex->Message); }
        capture_ = nullptr;
        busy_ = false;
        StatusText->Text = L"Tocca per registrare";
        if (recFile_ != nullptr)
        {
            auto item = ref new RecItem(recFile_);
            items_->InsertAt(0, item);
            Describe(item);
            Analyze(item);
            recFile_ = nullptr;
        }
    });
}

void MainPage::StopForSuspend()
{
    if (recording_) StopRecording();
    StopPlayback();
}

bool MainPage::HandleBack()
{
    if (MicPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) { MicPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed; return true; }
    if (recording_) { StopRecording(); return true; }
    if (playing_ != nullptr) { StopPlayback(); return true; }
    return false;
}

void MainPage::StopPlayback()
{
    if (playing_ != nullptr) { playing_->SetPlaying(false); playing_ = nullptr; }
    Player->Stop();
}

void MainPage::List_ItemClick(Object^, ItemClickEventArgs^ e)
{
    auto item = dynamic_cast<RecItem^>(e->ClickedItem);
    if (item == nullptr || recording_) return;
    bool same = playing_ == item;
    StopPlayback();
    if (same) return;
    playing_ = item;
    item->SetPlaying(true);
    create_task(item->file->OpenAsync(FileAccessMode::Read)).then([this, item](task<IRandomAccessStream^> t)
    {
        try { Player->SetSource(t.get(), item->file->ContentType); }
        catch (Exception^ ex) { StatusText->Text = ex->Message; StopPlayback(); }
    });
}

void MainPage::Player_Ended(Object^, RoutedEventArgs^) { StopPlayback(); }

void MainPage::Player_Failed(Object^, ExceptionRoutedEventArgs^ e)
{
    StatusText->Text = L"Riproduzione non riuscita: " + e->ErrorMessage;
    Log(L"play failed " + e->ErrorMessage);
    StopPlayback();
}

void MainPage::Item_Holding(Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
    auto element = dynamic_cast<FrameworkElement^>(sender);
    auto item = element == nullptr ? nullptr : dynamic_cast<RecItem^>(element->DataContext);
    if (item == nullptr) return;
    auto menu = ref new MenuFlyout();
    auto save = ref new MenuFlyoutItem();
    save->Text = L"salva in Musica";
    save->Click += ref new RoutedEventHandler([this, item](Object^, RoutedEventArgs^) { SaveToMusic(item); });
    auto del = ref new MenuFlyoutItem();
    del->Text = L"elimina";
    del->Click += ref new RoutedEventHandler([this, item](Object^, RoutedEventArgs^) { Delete(item); });
    menu->Items->Append(save);
    menu->Items->Append(del);
    menu->ShowAt(element);
}

void MainPage::Delete(RecItem^ item)
{
    if (playing_ == item) StopPlayback();
    unsigned int index;
    if (items_->IndexOf(item, &index)) items_->RemoveAt(index);
    create_task(item->file->DeleteAsync()).then([](task<void> t) { try { t.get(); } catch (Exception^) {} });
}

void MainPage::SaveToMusic(RecItem^ item)
{
    create_task(Windows::Storage::KnownFolders::MusicLibrary->CreateFolderAsync(L"Registrazioni", CreationCollisionOption::OpenIfExists))
        .then([item](StorageFolder^ f) { return item->file->CopyAsync(f, item->file->Name, NameCollisionOption::GenerateUniqueName); })
        .then([this](task<StorageFile^> t)
    {
        try { t.get(); StatusText->Text = L"Copiata in Musica\\Registrazioni"; }
        catch (Exception^ ex) { StatusText->Text = ex->Message; }
    });
}

// ---------------------------------------------------------------- microphone choice

// The microphone used for recordings: the one picked in the list (kept in the app's settings) while it is still
// there; otherwise a Bluetooth headset (any capture device not from the phone's own QCAUD driver), then a plugged-in
// headset ("Headset Microphone"), otherwise the phone's "Enhanced Audio Recording Device": the default device
// records silence without a headset.
String^ MainPage::ChosenDevice(Windows::Devices::Enumeration::DeviceInformationCollection^ all)
{
    String^ saved = SavedMicId();
    String^ enhanced = nullptr;
    String^ headset = nullptr;
    for (unsigned int i = 0; i < all->Size; ++i)
    {
        auto d = all->GetAt(i);
        if (!d->IsEnabled) continue;
        if (saved != nullptr && wcscmp(d->Id->Data(), saved->Data()) == 0) return d->Id;
        if (!wcsstr(d->Name->Data(), L"QCAUD") || wcsstr(d->Name->Data(), L"Bluetooth") || wcsstr(d->Name->Data(), L"Hands-Free")) return d->Id;
        if (wcsncmp(d->Name->Data(), L"Headset Microphone", 18) == 0) headset = d->Id;
        if (wcsstr(d->Name->Data(), L"Enhanced Audio Recording")) enhanced = d->Id;
    }
    if (headset != nullptr) return headset;
    return enhanced != nullptr ? enhanced : ref new String(L"");
}

String^ MainPage::SavedMicId()
{
    auto values = ApplicationData::Current->LocalSettings->Values;
    return values->HasKey(L"micId") ? dynamic_cast<String^>(values->Lookup(L"micId")) : nullptr;
}

String^ MainPage::SavedMicName()
{
    auto values = ApplicationData::Current->LocalSettings->Values;
    return values->HasKey(L"micName") ? dynamic_cast<String^>(values->Lookup(L"micName")) : ref new String(L"automatico");
}

void MainPage::Mics_Click(Object^, RoutedEventArgs^)
{
    if (recording_ || busy_) return;
    StopPlayback();
    String^ name = SavedMicName();
    MicStatus->Text = L"Per le registrazioni: " + name + L". Tocca un microfono per provarlo, tienilo premuto per sceglierlo. Indietro chiude.";
    MicPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    create_task(Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(Windows::Devices::Enumeration::DeviceClass::AudioCapture))
        .then([this](Windows::Devices::Enumeration::DeviceInformationCollection^ all)
    {
        mics_->Clear();
        for (unsigned int i = 0; i < all->Size; ++i)
            mics_->Append(ref new MicItem(all->GetAt(i)->Id, all->GetAt(i)->Name));
        if (mics_->Size == 0) MicStatus->Text = L"Nessun microfono trovato";
    }, task_continuation_context::use_current());
}

void MainPage::Mic_ItemClick(Object^, ItemClickEventArgs^ e)
{
    auto item = dynamic_cast<MicItem^>(e->ClickedItem);
    if (item != nullptr) TestMic(item);
}

void MainPage::Mic_Holding(Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
    auto element = dynamic_cast<FrameworkElement^>(sender);
    auto item = element == nullptr ? nullptr : dynamic_cast<MicItem^>(element->DataContext);
    if (item == nullptr) return;
    auto values = ApplicationData::Current->LocalSettings->Values;
    values->Insert(L"micId", item->Id());
    values->Insert(L"micName", item->Name);
    MicStatus->Text = L"Per le registrazioni: " + item->Name + L". Tienine premuto un altro per cambiarlo.";
    Log(L"mic chosen [" + item->Name + L"] " + item->Id());
}

// Records 3 s from one microphone and reports its loudest sample, its average level and the loudest sample of each
// 250 ms. A microphone that is wired but silent reads 0% everywhere; a loose one drops to 0% in the middle of
// the recording while the others keep their noise floor.
void MainPage::TestMic(MicItem^ item)
{
    if (recording_ || busy_) return;
    busy_ = true;
    StopPlayback();
    item->SetDetail(L"in prova... parla o tocca il foro del microfono");
    MicStatus->Text = L"Prova " + item->Name + L" (3 secondi)";
    auto cap = ref new MediaCapture();
    auto settings = ref new MediaCaptureInitializationSettings();
    settings->StreamingCaptureMode = StreamingCaptureMode::Audio;
    settings->AudioDeviceId = item->Id();
    auto file = std::make_shared<StorageFile^>(nullptr);
    create_task(cap->InitializeAsync(settings)).then([this, cap]()
    {
        return create_task(ApplicationData::Current->TemporaryFolder->CreateFileAsync(L"mictest.m4a", CreationCollisionOption::ReplaceExisting));
    }).then([cap, file](StorageFile^ f)
    {
        *file = f;
        return create_task(cap->StartRecordToStorageFileAsync(Profile(0), f));
    }).then([]()
    {
        return create_task([]() { Sleep(3000); });
    }).then([cap]()
    {
        return create_task(cap->StopRecordAsync());
    }).then([this, item, file](task<void> t)
    {
        std::wstring result;
        try
        {
            t.get();
            std::string wav, err;
            int peak = -1, mean = -1;
            double secs = 0;
            std::vector<int> slices;
            if (DecodeToPcmWav(std::wstring((*file)->Path->Data()), wav, err, &peak, &secs, &mean, &slices))
            {
                result = L"picco " + std::to_wstring(peak) + L"%, medio " + std::to_wstring(mean) + L"%. 250 ms:";
                for (size_t i = 0; i < slices.size(); ++i) result += L" " + std::to_wstring(slices[i]);
            }
            else
                result = std::wstring(L"decodifica: ") + std::wstring(err.begin(), err.end());
        }
        catch (Exception^ ex) { result = std::wstring(L"errore: ") + ex->Message->Data(); }
        String^ r = ref new String(result.c_str());
        item->SetDetail(r);
        Log(L"mic test [" + item->Name + L"] " + item->Id() + L": " + r);
        MicStatus->Text = L"Fatto: " + item->Name;
        busy_ = false;
    }, task_continuation_context::use_current());
}
