#include "pch.h"
#include "MainPage.xaml.h"
#include "Logger.h"
#include "navit_wp81.h"

#include <cmath>

using namespace Mappe81;
using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::Devices::Geolocation;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::SpeechSynthesis;
using namespace Windows::Networking::BackgroundTransfer;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Navigation;

namespace {
const wchar_t* const InboxUrl = L"http://192.168.1.148:8765/mappe";
const wchar_t* const MapUrl = L"http://192.168.1.148:8765/maps/sicilia.bin";
const wchar_t* const MapFile = L"sicilia.bin";

MainPage^ s_page;
std::string g_conf;
char* g_argv[3];

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const char* s) {
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

void SetEnv(const char* name, String^ value) {
    navit_wp81_setenv(name, Utf8(value->Data()).c_str(), 1);
}

// --- hooks called on the Navit thread ---
void LogHook(const char* text) { Logger::WriteUtf8(text); }
void SpeechHook(const char* text) {
    Logger::Write(L"voce: " + Wide(text));
    if (s_page) s_page->SpeakLater(Wide(text));
}
void KeepOnHook() { if (s_page) s_page->KeepScreenOnLater(); }
void ReadyHook(struct navit*) { if (s_page) s_page->NavitReadyLater(); }
void CaptureDone(unsigned char* bgra, int w, int h, void*) {
    Array<unsigned char>^ a = nullptr;
    if (bgra) a = ref new Array<unsigned char>(bgra, static_cast<unsigned int>(w * h * 4));
    free(bgra);
    if (s_page) s_page->ShotReady(a, w, h);
}

std::wstring Hex(HRESULT hr) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    return b;
}
}  // namespace

MainPage::MainPage()
    : _scale(1), _mapReady(false), _navitStarted(false), _downloading(false), _hasFix(false), _lastLat(0), _lastLon(0),
      _displayActive(false), _speaking(false), _finger1(0), _finger2(0), _down1(false), _down2(false), _pinchStart(0) {
    InitializeComponent();
    s_page = this;
    MapPanel->SizeChanged += ref new SizeChangedEventHandler(this, &MainPage::Panel_SizeChanged);
    MapPanel->PointerPressed += ref new PointerEventHandler(this, &MainPage::Panel_PointerPressed);
    MapPanel->PointerMoved += ref new PointerEventHandler(this, &MainPage::Panel_PointerMoved);
    MapPanel->PointerReleased += ref new PointerEventHandler(this, &MainPage::Panel_PointerReleased);
    MapPanel->PointerCanceled += ref new PointerEventHandler(this, &MainPage::Panel_PointerReleased);
    Voice->MediaEnded += ref new RoutedEventHandler(this, &MainPage::Voice_MediaEnded);
    Voice->MediaFailed += ref new ExceptionRoutedEventHandler([this](Object^, ExceptionRoutedEventArgs^ e) {
        Logger::Write(L"voce: riproduzione fallita " + std::wstring(e->ErrorMessage->Data()));
        _speaking = false;
        SpeakNext();
    });
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e) {
    Page::OnNavigatedTo(e);
    if (_display != nullptr) return;
    _display = ref new Windows::System::Display::DisplayRequest();
    _scale = static_cast<float>(DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel);
    Logger::Start(ref new String(InboxUrl));
    Logger::Write(L"Mappe 1.0 avvio, scala " + std::to_wstring(_scale));
    LogVoices();
    CheckMapThenStart();
}

void MainPage::SetStatus(const std::wstring& text) {
    StatusText->Text = ref new String(text.c_str());
}

void MainPage::LogVoices() {
    try {
        std::wstring all;
        VoiceInformation^ italian = nullptr;
        for (auto v : SpeechSynthesizer::AllVoices) {
            all += std::wstring(v->DisplayName->Data()) + L" (" + v->Language->Data() + L"); ";
            if (italian == nullptr && std::wstring(v->Language->Data()).compare(0, 2, L"it") == 0) italian = v;
        }
        Logger::Write(L"voci: " + all);
        _synth = ref new SpeechSynthesizer();
        if (italian != nullptr) {
            _synth->Voice = italian;
            Logger::Write(L"voce italiana: " + std::wstring(italian->DisplayName->Data()));
        } else {
            Logger::Write(L"nessuna voce italiana installata");
        }
    } catch (Exception^ ex) {
        Logger::Write(L"voci: errore " + Hex(ex->HResult));
    }
}

void MainPage::CheckMapThenStart() {
    SetStatus(L"Controllo la mappa...");
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"navit", CreationCollisionOption::OpenIfExists))
        .then([](StorageFolder^ f) { return f->CreateFolderAsync(L"maps", CreationCollisionOption::OpenIfExists); })
        .then([](StorageFolder^ maps) { return maps->GetFileAsync(ref new String(MapFile)); })
        .then([](StorageFile^ f) { return f->GetBasicPropertiesAsync(); })
        .then([this](task<FileProperties::BasicProperties^> t) {
            try {
                auto p = t.get();
                Logger::Write(L"mappa presente: " + std::to_wstring(p->Size) + L" byte");
                if (p->Size > 1024 * 1024) {
                    _mapReady = true;
                    StartNavitIfReady();
                    return;
                }
            } catch (Exception^) {
            }
            DownloadMap();
        });
}

void MainPage::DownloadMap() {
    if (_downloading) return;
    _downloading = true;
    SetStatus(L"Scarico la mappa della Sicilia dal PC (Wi-Fi di casa)...");
    Logger::Write(L"download mappa da " + std::wstring(MapUrl));
    create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(L"navit", CreationCollisionOption::OpenIfExists))
        .then([](StorageFolder^ f) { return f->CreateFolderAsync(L"maps", CreationCollisionOption::OpenIfExists); })
        .then([](StorageFolder^ maps) { return maps->CreateFileAsync(L"sicilia.bin.part", CreationCollisionOption::ReplaceExisting); })
        .then([this](StorageFile^ part) {
            auto downloader = ref new BackgroundDownloader();
            DownloadOperation^ op = downloader->CreateDownload(ref new Uri(ref new String(MapUrl)), part);
            auto async = op->StartAsync();
            async->Progress = ref new AsyncOperationProgressHandler<DownloadOperation^, DownloadOperation^>(
                [this](IAsyncOperationWithProgress<DownloadOperation^, DownloadOperation^>^, DownloadOperation^ p) {
                    auto pr = p->Progress;
                    int pct = pr.TotalBytesToReceive ? static_cast<int>(100.0 * pr.BytesReceived / pr.TotalBytesToReceive) : 0;
                    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, pct]() {
                        SetStatus(L"Scarico la mappa della Sicilia: " + std::to_wstring(pct) + L" %");
                    }));
                });
            return create_task(async).then([part](DownloadOperation^) {
                return part->RenameAsync(ref new String(MapFile), NameCollisionOption::ReplaceExisting);
            });
        })
        .then([this](task<void> t) {
            _downloading = false;
            try {
                t.get();
                Logger::Write(L"mappa scaricata");
                _mapReady = true;
                StartNavitIfReady();
            } catch (Exception^ ex) {
                Logger::Write(L"download fallito " + Hex(ex->HResult) + L" " + ex->Message->Data());
                SetStatus(L"Download della mappa fallito: serve il Wi-Fi di casa (PC acceso). " + std::wstring(ex->Message->Data()));
            }
        });
}

void MainPage::Panel_SizeChanged(Object^, SizeChangedEventArgs^ e) {
    if (!_navitStarted) {
        StartNavitIfReady();
        return;
    }
    int pw = static_cast<int>(e->NewSize.Width * _scale + 0.5);
    int ph = static_cast<int>(e->NewSize.Height * _scale + 0.5);
    navit_wp81_gfx_resize(pw, ph);
}

void MainPage::StartNavitIfReady() {
    if (_navitStarted || !_mapReady) return;
    double w = MapPanel->ActualWidth, h = MapPanel->ActualHeight;
    if (w < 1 || h < 1) return;
    int pw = static_cast<int>(w * _scale + 0.5), ph = static_cast<int>(h * _scale + 0.5);
    std::wstring err;
    if (!_dx.Create(MapPanel, pw, ph, _scale, err)) {
        Logger::Write(L"grafica: " + err);
        SetStatus(L"Errore grafica: " + err);
        return;
    }
    Logger::Write(_dx.Describe());
    navit_wp81_gfx_init(_dx.Context(), _dx.SwapChain(), _dx.DWrite(), _dx.Wic(), pw, ph, 334.0f);

    String^ pkg = Package::Current->InstalledLocation->Path + L"\\navit";
    String^ user = ApplicationData::Current->LocalFolder->Path + L"\\navit";
    SetEnv("NAVIT_PREFIX", pkg);
    SetEnv("NAVIT_SHAREDIR", pkg);
    SetEnv("NAVIT_LIBDIR", pkg);
    SetEnv("NAVIT_LOCALEDIR", pkg + L"\\locale");
    SetEnv("NAVIT_USER_DATADIR", user);
    SetEnv("HOME", user);
    navit_wp81_setenv("LANG", "it_IT", 1);
    g_conf = Utf8(std::wstring(pkg->Data()) + L"\\navit.xml");
    g_argv[0] = const_cast<char*>("navit");
    g_argv[1] = &g_conf[0];
    g_argv[2] = nullptr;

    navit_wp81_log_hook = LogHook;
    navit_wp81_speech_hook = SpeechHook;
    navit_wp81_keep_screen_on_hook = KeepOnHook;
    navit_wp81_ready_hook = ReadyHook;

    _navitStarted = true;
    SetStatus(L"Avvio Navit...");
    Logger::Write(L"avvio navit con " + std::wstring(pkg->Data()));
    ThreadPool::RunAsync(ref new WorkItemHandler([](IAsyncAction^) {
        int r = navit_wp81_main(2, g_argv);
        Logger::Write(L"navit terminato con " + std::to_wstring(r));
    }), WorkItemPriority::Normal, WorkItemOptions::TimeSliced);
    StartGps();
}

void MainPage::NavitReadyLater() {
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this]() {
        SetStatus(_hasFix ? L"Mappa pronta" : L"Mappa pronta, in attesa del GPS...");
        Logger::Write(L"navit pronto");
    }));
}

void MainPage::StartGps() {
    try {
        _geo = ref new Geolocator();
        _geo->DesiredAccuracy = PositionAccuracy::High;
        _geo->ReportInterval = 1000;
        _geo->MovementThreshold = 0;
        _geo->PositionChanged += ref new TypedEventHandler<Geolocator^, PositionChangedEventArgs^>(this, &MainPage::OnPositionChanged);
        _geo->StatusChanged += ref new TypedEventHandler<Geolocator^, StatusChangedEventArgs^>(this, &MainPage::OnStatusChanged);
    } catch (Exception^ ex) {
        Logger::Write(L"gps: " + Hex(ex->HResult));
    }
}

void MainPage::OnStatusChanged(Geolocator^, StatusChangedEventArgs^ e) {
    int status = static_cast<int>(e->Status);
    Logger::Write(L"gps stato " + std::to_wstring(status));
    if (e->Status == PositionStatus::NoData || e->Status == PositionStatus::Disabled) navit_wp81_vehicle_fix(0);
}

void MainPage::OnPositionChanged(Geolocator^, PositionChangedEventArgs^ e) {
    Geocoordinate^ c = e->Position->Coordinate;
    BasicGeoposition p = c->Point->Position;
    double speed = (c->Speed != nullptr && !std::isnan(c->Speed->Value)) ? c->Speed->Value * 3.6 : -1;
    double heading = (c->Heading != nullptr && !std::isnan(c->Heading->Value)) ? c->Heading->Value : -1;
    long long ms = (c->Timestamp.UniversalTime - 116444736000000000LL) / 10000;
    navit_wp81_vehicle_position(p.Latitude, p.Longitude, speed, heading, p.Altitude, c->Accuracy, ms);
    double lat = p.Latitude, lon = p.Longitude, acc = c->Accuracy;
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, lat, lon, acc]() {
        bool first = !_hasFix;
        _hasFix = true;
        _lastLat = lat;
        _lastLon = lon;
        if (first) {
            Logger::Write(L"gps: prima posizione, precisione " + std::to_wstring(static_cast<int>(acc)) + L" m");
            SetStatus(L"GPS: \u00b1" + std::to_wstring(static_cast<int>(acc)) + L" m");
        }
    }));
}

void MainPage::Locate_Click(Object^, RoutedEventArgs^) {
    if (!_hasFix) {
        SetStatus(L"Nessuna posizione GPS ancora (all'aperto ci vuole meno di un minuto)");
        return;
    }
    navit_wp81_center(_lastLat, _lastLon);
    navit_wp81_follow_vehicle();
}

void MainPage::ZoomIn_Click(Object^, RoutedEventArgs^) { navit_wp81_zoom(1); }
void MainPage::ZoomOut_Click(Object^, RoutedEventArgs^) { navit_wp81_zoom(0); }

void MainPage::Download_Click(Object^, RoutedEventArgs^) {
    if (_navitStarted) {
        SetStatus(L"Per aggiornare la mappa chiudi e riapri l'app (aggiornamento in arrivo)");
        return;
    }
    DownloadMap();
}

void MainPage::Shot_Click(Object^, RoutedEventArgs^) {
    if (!_navitStarted) return;
    SetStatus(L"Catturo la mappa...");
    navit_wp81_capture(CaptureDone, nullptr);
}

void MainPage::ShotReady(Array<unsigned char>^ bgra, int width, int height) {
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, bgra, width, height]() {
        if (bgra == nullptr) {
            SetStatus(L"Cattura fallita");
            return;
        }
        auto stream = ref new InMemoryRandomAccessStream();
        create_task(BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, stream))
            .then([bgra, width, height](BitmapEncoder^ enc) {
                enc->SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, width, height, 96, 96, bgra);
                return enc->FlushAsync();
            })
            .then([stream]() {
                auto reader = ref new DataReader(stream->GetInputStreamAt(0));
                return create_task(reader->LoadAsync(static_cast<unsigned int>(stream->Size))).then([reader](unsigned int n) {
                    return CryptographicBuffer::EncodeToBase64String(reader->ReadBuffer(n));
                });
            })
            .then([this](task<String^> t) {
                try {
                    Logger::PostNow(L"mappe-shot", t.get());
                    SetStatus(L"Schermata inviata al PC");
                } catch (Exception^ ex) {
                    SetStatus(L"Schermata: errore " + Hex(ex->HResult));
                }
            });
    }));
}

void MainPage::KeepScreenOnLater() {
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this]() {
        if (_displayActive) return;
        try {
            _display->RequestActive();
            _displayActive = true;
        } catch (Exception^) {
        }
    }));
}

void MainPage::SpeakLater(const std::wstring& text) {
    std::wstring copy = text;
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, copy]() {
        _speech.push_back(copy);
        SpeakNext();
    }));
}

void MainPage::SpeakNext() {
    if (_speaking || _speech.empty() || _synth == nullptr) return;
    std::wstring text = _speech.front();
    _speech.pop_front();
    _speaking = true;
    create_task(_synth->SynthesizeTextToStreamAsync(ref new String(text.c_str())))
        .then([this](task<SpeechSynthesisStream^> t) {
            try {
                auto s = t.get();
                Voice->SetSource(s, s->ContentType);
                Voice->Play();
            } catch (Exception^ ex) {
                Logger::Write(L"voce: sintesi fallita " + Hex(ex->HResult));
                _speaking = false;
                SpeakNext();
            }
        });
}

void MainPage::Voice_MediaEnded(Object^, RoutedEventArgs^) {
    _speaking = false;
    SpeakNext();
}

Point MainPage::ToPixels(PointerRoutedEventArgs^ e) {
    Point p = e->GetCurrentPoint(MapPanel)->Position;
    return Point(p.X * _scale, p.Y * _scale);
}

void MainPage::Panel_PointerPressed(Object^, PointerRoutedEventArgs^ e) {
    if (!_navitStarted) return;
    MapPanel->CapturePointer(e->Pointer);
    Point p = ToPixels(e);
    if (!_down1) {
        _down1 = true;
        _finger1 = e->Pointer->PointerId;
        _p1 = p;
        navit_wp81_input_button(1, 1, static_cast<int>(p.X), static_cast<int>(p.Y));
    } else if (!_down2) {
        _down2 = true;
        _finger2 = e->Pointer->PointerId;
        _p2 = p;
        // a second finger: end Navit's drag where it is and start a pinch
        navit_wp81_input_button(0, 1, static_cast<int>(_p1.X), static_cast<int>(_p1.Y));
        _pinchStart = std::sqrt((_p1.X - _p2.X) * (_p1.X - _p2.X) + (_p1.Y - _p2.Y) * (_p1.Y - _p2.Y));
    }
    e->Handled = true;
}

void MainPage::Panel_PointerMoved(Object^, PointerRoutedEventArgs^ e) {
    if (!_navitStarted) return;
    unsigned int id = e->Pointer->PointerId;
    Point p = ToPixels(e);
    if (_down2) {
        if (id == _finger1) _p1 = p;
        else if (id == _finger2) _p2 = p;
        double d = std::sqrt((_p1.X - _p2.X) * (_p1.X - _p2.X) + (_p1.Y - _p2.Y) * (_p1.Y - _p2.Y));
        if (_pinchStart > 0 && d / _pinchStart > 1.6) {
            navit_wp81_zoom(1);
            _pinchStart = d;
        } else if (_pinchStart > 0 && d / _pinchStart < 0.6) {
            navit_wp81_zoom(0);
            _pinchStart = d;
        }
    } else if (_down1 && id == _finger1) {
        _p1 = p;
        navit_wp81_input_motion(static_cast<int>(p.X), static_cast<int>(p.Y));
    }
    e->Handled = true;
}

void MainPage::Panel_PointerReleased(Object^, PointerRoutedEventArgs^ e) {
    if (!_navitStarted) return;
    unsigned int id = e->Pointer->PointerId;
    Point p = ToPixels(e);
    if (_down2) {
        // pinch ends when either finger lifts; ignore the rest of this gesture
        if (id == _finger1 || id == _finger2) {
            _down2 = false;
            _down1 = false;
        }
    } else if (_down1 && id == _finger1) {
        _down1 = false;
        navit_wp81_input_button(0, 1, static_cast<int>(p.X), static_cast<int>(p.Y));
    }
    MapPanel->ReleasePointerCapture(e->Pointer);
    e->Handled = true;
}
