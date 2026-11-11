//
// MainPage.xaml.cpp
// Implementation of the MainPage class.
//

#include "pch.h"
#include "MainPage.xaml.h"

#include "Core/Geo.h"

using namespace MappeCpp;

using namespace concurrency;
using namespace Platform::Collections;
using namespace Platform;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Devices::Geolocation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Navigation;

namespace {
const double ItalyLat = 41.9;
const double ItalyLon = 12.5;
const double ItalyZoom = 6.0;
const wchar_t* const KeyLat = L"map.lat";
const wchar_t* const KeyLon = L"map.lon";
const wchar_t* const KeyZoom = L"map.zoom";
const long long SaveIntervalMs = 2000;

String^ S(const std::wstring& s) { return ref new String(s.c_str()); }
std::wstring W(String^ s) { return s != nullptr ? std::wstring(s->Data()) : std::wstring(); }
// Narrow ASCII text (numbers, URLs) to wide.
std::wstring Nw(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring StatusName(PositionStatus st) {
    switch (st) {
        case PositionStatus::Ready: return L"pronto";
        case PositionStatus::Initializing: return L"inizializzazione";
        case PositionStatus::NoData: return L"nessun dato";
        case PositionStatus::Disabled: return L"disattivata";
        case PositionStatus::NotInitialized: return L"non inizializzata";
        case PositionStatus::NotAvailable: return L"non disponibile";
        default: return L"sconosciuto";
    }
}
}  // namespace

MainPage::MainPage()
    : _initialized(false), _searchSeq(0), _reverseSeq(0), _hasPress(false), _pressLat(0), _pressLon(0),
      _geoStatus(L"-"), _geoError(L"-"), _searchInfo(L"-"), _reverseInfo(L"-") {
    InitializeComponent();

    _map = ref new TileMapView();
    RootGrid->Children->InsertAt(0, _map);
    _map->LongPressed += ref new EventHandler<LatLonArgs^>(this, &MainPage::Map_LongPressed);

    _searchTimer = ref new DispatcherTimer();
    TimeSpan debounce;
    debounce.Duration = 400LL * 10000LL;
    _searchTimer->Interval = debounce;
    _searchTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::SearchTimer_Tick);

    _saveTimer = ref new DispatcherTimer();
    TimeSpan every;
    every.Duration = SaveIntervalMs * 10000LL;
    _saveTimer->Interval = every;
    _saveTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::SaveTimer_Tick);
    _saveTimer->Start();

    // Hardware Back: closes the open layers first, then lets the system exit the app.
    Windows::Phone::UI::Input::HardwareButtons::BackPressed += ref new EventHandler<Windows::Phone::UI::Input::BackPressedEventArgs^>(this, &MainPage::OnBackPressed);
}

void MainPage::OnBackPressed(Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e) {
    if (DiagPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        DiagPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        e->Handled = true;
    } else if (PressPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        ClosePress_Click(nullptr, nullptr);
        e->Handled = true;
    } else if (ResultsList->Visibility == Windows::UI::Xaml::Visibility::Visible) {
        ResultsList->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        e->Handled = true;
    } else if (SearchBox->FocusState != Windows::UI::Xaml::FocusState::Unfocused) {
        // Dropping the focus hides the on-screen keyboard.
        this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        e->Handled = true;
    }
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e) {
    Page::OnNavigatedTo(e);
    if (_initialized) return;
    _initialized = true;

    ShowCrash();
    RestoreView();

    // Location: the 'location' capability in the manifest is enough on WP8.1.
    try {
        _geo = ref new Geolocator();
        _geo->DesiredAccuracyInMeters = static_cast<unsigned int>(50);
        _geo->ReportInterval = 2000;
        _geo->MovementThreshold = 5;
        _geo->PositionChanged += ref new TypedEventHandler<Geolocator^, PositionChangedEventArgs^>(this, &MainPage::OnPositionChanged);
        _geo->StatusChanged += ref new TypedEventHandler<Geolocator^, StatusChangedEventArgs^>(this, &MainPage::OnStatusChanged);
        ShowGeoStatus(_geo->LocationStatus);
        RequestPosition();
    } catch (Platform::Exception^ ex) {
        _geo = nullptr;
        _geoError = W(ex->Message);
        SetGeo(L"Posizione non disponibile: " + _geoError);
    }
}

void MainPage::OnNavigatedFrom(NavigationEventArgs^ e) {
    SaveView();
    Page::OnNavigatedFrom(e);
}

// ---- View memory ---------------------------------------------------------------

void MainPage::RestoreView() {
    double lat = ItalyLat, lon = ItalyLon, zoom = ItalyZoom;
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (values->HasKey(S(KeyLat)) && values->HasKey(S(KeyLon)) && values->HasKey(S(KeyZoom))) {
        auto a = dynamic_cast<IBox<double>^>(values->Lookup(S(KeyLat)));
        auto b = dynamic_cast<IBox<double>^>(values->Lookup(S(KeyLon)));
        auto c = dynamic_cast<IBox<double>^>(values->Lookup(S(KeyZoom)));
        if (a != nullptr && b != nullptr && c != nullptr) {
            lat = a->Value;
            lon = b->Value;
            zoom = c->Value;
        }
    }
    _map->CenterOn(lat, lon, zoom);
}

void MainPage::SaveView() {
    if (_map == nullptr) return;
    auto values = ApplicationData::Current->LocalSettings->Values;
    values->Insert(S(KeyLat), _map->CenterLat());
    values->Insert(S(KeyLon), _map->CenterLon());
    values->Insert(S(KeyZoom), _map->CurrentZoom());
}

void MainPage::ShowCrash() {
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (!values->HasKey(L"lastCrash")) return;
    SetStatus(L"Errore precedente: " + W(dynamic_cast<String^>(values->Lookup(L"lastCrash"))));
}

void MainPage::SaveTimer_Tick(Object^ sender, Object^ e) {
    SaveView();
    if (DiagPanel->Visibility == Windows::UI::Xaml::Visibility::Visible) RefreshDiag();
}

// ---- Location --------------------------------------------------------------

void MainPage::RequestPosition() {
    if (_geo == nullptr) return;
    TimeSpan maxAge;
    maxAge.Duration = 5LL * 60LL * 10000000LL;   // a fix up to 5 minutes old is good enough
    TimeSpan timeout;
    timeout.Duration = 20LL * 10000000LL;        // 20 s to get a fix (network or GPS)
    create_task(_geo->GetGeopositionAsync(maxAge, timeout)).then([this](task<Geoposition^> t) {
        try {
            ShowPosition(t.get());
        } catch (Platform::Exception^ ex) {
            ShowPositionError(ex);
        }
    }, task_continuation_context::use_current());
}

void MainPage::ShowPosition(Geoposition^ pos) {
    _lastPos = pos;
    _geoStatus = L"Ready";
    auto c = pos->Coordinate;
    bool first = !_hadFix;
    _hadFix = true;
    _map->SetUserLocation(c->Point->Position.Latitude, c->Point->Position.Longitude, c->Accuracy);
    if (first) _map->CenterOn(c->Point->Position.Latitude, c->Point->Position.Longitude, 16.0);
    SetGeo(L"");
}

void MainPage::ShowPositionError(Platform::Exception^ ex) {
    _geoError = W(ex->Message);
    if (_geo == nullptr) {
        SetGeo(L"Posizione non disponibile.");
        return;
    }
    PositionStatus st = _geo->LocationStatus;
    _geoStatus = StatusName(st);
    if (st == PositionStatus::Disabled) {
        SetGeo(L"Attiva la posizione nelle impostazioni del telefono.");
    } else if (st == PositionStatus::NotAvailable) {
        SetGeo(L"GPS non disponibile su questo dispositivo.");
    } else if (ex->HResult == static_cast<int>(0x800705B4) || st == PositionStatus::Initializing || st == PositionStatus::NoData) {
        // Cold GPS start without network location can take minutes: PositionChanged keeps listening.
        SetGeo(L"Cerco i satelliti... (all'aperto o vicino a una finestra \u00e8 pi\u00f9 veloce)");
    } else {
        SetGeo(L"Posizione non disponibile (stato: " + _geoStatus + L"). " + _geoError);
    }
}

void MainPage::ShowGeoStatus(PositionStatus status) {
    _geoStatus = StatusName(status);
    switch (status) {
        case PositionStatus::Disabled:
            SetGeo(L"Attiva la posizione nelle impostazioni del telefono.");
            break;
        case PositionStatus::NotAvailable:
            SetGeo(L"GPS non disponibile su questo dispositivo.");
            break;
        case PositionStatus::NoData:
        case PositionStatus::Initializing:
        case PositionStatus::NotInitialized:
            SetGeo(L"Cerco la posizione...");
            break;
        default:
            break;
    }
}

void MainPage::OnPositionChanged(Geolocator^ sender, PositionChangedEventArgs^ e) {
    Geoposition^ pos = e->Position;
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, pos]() {
        ShowPosition(pos);
    }));
}

void MainPage::OnStatusChanged(Geolocator^ sender, StatusChangedEventArgs^ e) {
    PositionStatus st = e->Status;
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, st]() {
        ShowGeoStatus(st);
    }));
}

void MainPage::Locate_Click(Object^ sender, RoutedEventArgs^ e) {
    if (_lastPos == nullptr) {
        SetGeo(L"Cerco la posizione...");
        RequestPosition();
        return;
    }
    _map->CenterOn(_lastPos->Coordinate->Point->Position.Latitude,
                   _lastPos->Coordinate->Point->Position.Longitude, 16.0);
}

void MainPage::SetGeo(const std::wstring& text) {
    GeoText->Text = S(text);
    GeoText->Visibility = text.empty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
}

// ---- Search ------------------------------------------------------------------

void MainPage::SetStatus(const std::wstring& text) {
    StatusText->Text = S(text);
    StatusText->Visibility = text.empty() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
}

void MainPage::SearchBox_TextChanged(Object^ sender, TextChangedEventArgs^ e) {
    _searchTimer->Stop();
    _searchTimer->Start();
}

void MainPage::SearchTimer_Tick(Object^ sender, Object^ e) {
    _searchTimer->Stop();
    RunSearch(W(SearchBox->Text));
}

void MainPage::SearchBox_KeyDown(Object^ sender, KeyRoutedEventArgs^ e) {
    if (e->Key != VirtualKey::Enter) return;
    e->Handled = true;
    _searchTimer->Stop();
    RunSearch(W(SearchBox->Text));
    this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::SearchFocus_Click(Object^ sender, RoutedEventArgs^ e) {
    SearchBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void MainPage::RunSearch(const std::wstring& raw) {
    int seq = ++_searchSeq;
    std::wstring query = Trim(raw);
    if (query.empty()) {
        ResultsList->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        ResultsList->ItemsSource = nullptr;
        SetStatus(L"");
        return;
    }
    SetStatus(L"Cerco \"" + query + L"\"...");

    bool hasBias = _lastPos != nullptr;
    double blat = hasBias ? _lastPos->Coordinate->Point->Position.Latitude : 0.0;
    double blon = hasBias ? _lastPos->Coordinate->Point->Position.Longitude : 0.0;
    std::string url = mappe::PhotonSearchUrl(query, 8, hasBias, blat, blon);

    HttpGetTextAsync(url).then([this, seq, query](task<String^> t) {
        if (seq != _searchSeq) return;
        try {
            Vector<PlaceItem^>^ items = ParsePhoton(t.get());
            _searchInfo = L"ok, " + std::to_wstring(items->Size) + L" risultati";
            ResultsList->ItemsSource = items;
            ResultsList->Visibility = items->Size > 0 ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
            SetStatus(items->Size > 0 ? std::to_wstring(items->Size) + L" risultati" : std::wstring(L"Nessun risultato"));
        } catch (Platform::Exception^ ex) {
            _searchInfo = L"errore: " + W(ex->Message);
            ResultsList->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            SetStatus(L"Ricerca non riuscita: " + W(ex->Message));
        }
    }, task_continuation_context::use_current());
}

void MainPage::ResultsList_ItemClick(Object^ sender, ItemClickEventArgs^ e) {
    PlaceItem^ place = dynamic_cast<PlaceItem^>(e->ClickedItem);
    if (place == nullptr) return;
    _map->CenterOn(place->Lat, place->Lon, 17.0);
    _map->SetSearchPin(place->Lat, place->Lon, place->Title);
    ResultsList->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    SetStatus(W(place->Title));
}

// ---- Long press -------------------------------------------------------------

void MainPage::Map_LongPressed(Object^ sender, LatLonArgs^ e) {
    _hasPress = true;
    _pressLat = e->Lat;
    _pressLon = e->Lon;
    _map->SetPressPin(_pressLat, _pressLon);
    CoordBox->Text = S(Nw(mappe::FormatCoord(_pressLat)) + L", " + Nw(mappe::FormatCoord(_pressLon)));
    AddressText->Text = S(L"Cerco l'indirizzo...");
    PressPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    ReverseGeocode(_pressLat, _pressLon);
}

void MainPage::ReverseGeocode(double lat, double lon) {
    int seq = ++_reverseSeq;
    HttpGetTextAsync(mappe::PhotonReverseUrl(lat, lon)).then([this, seq](task<String^> t) {
        if (seq != _reverseSeq) return;
        try {
            Vector<PlaceItem^>^ items = ParsePhoton(t.get());
            if (items->Size == 0) {
                _reverseInfo = L"nessun risultato";
                AddressText->Text = S(L"Nessun indirizzo trovato");
                return;
            }
            _reverseInfo = L"ok";
            PlaceItem^ first = items->GetAt(0);
            AddressText->Text = S(W(first->Title) + L"\n" + W(first->Subtitle));
        } catch (Platform::Exception^ ex) {
            _reverseInfo = L"errore: " + W(ex->Message);
            AddressText->Text = S(L"Indirizzo non disponibile: " + W(ex->Message));
        }
    }, task_continuation_context::use_current());
}

void MainPage::CopyCoords_Click(Object^ sender, RoutedEventArgs^ e) {
    CoordBox->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    CoordBox->SelectAll();
}

void MainPage::OpenBrowser_Click(Object^ sender, RoutedEventArgs^ e) {
    if (!_hasPress) return;
    double lat = _pressLat, lon = _pressLon;
    Uri^ app = ref new Uri(S(Nw(mappe::OsmLink(lat, lon, 18, "metrosurf"))));
    create_task(Launcher::LaunchUriAsync(app)).then([lat, lon](task<bool> t) {
        bool ok = false;
        try {
            ok = t.get();
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            Launcher::LaunchUriAsync(ref new Uri(S(Nw(mappe::OsmLink(lat, lon, 18, "https")))));
        }
    }, task_continuation_context::use_current());
}

void MainPage::ClosePress_Click(Object^ sender, RoutedEventArgs^ e) {
    PressPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    _hasPress = false;
    _map->ClearPressPin();
}

// ---- Diagnostics ------------------------------------------------------------

void MainPage::Diag_Click(Object^ sender, RoutedEventArgs^ e) {
    DiagPanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
    RefreshDiag();
}

void MainPage::Diag_Refresh_Click(Object^ sender, RoutedEventArgs^ e) {
    RefreshDiag();
}

void MainPage::Diag_Close_Click(Object^ sender, RoutedEventArgs^ e) {
    DiagPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::RefreshDiag() {
    std::wstring t = L"Mappe 0.2.0.0\n";
    t += W(_map->StatsText()) + L"\n";
    double lat = _map->CenterLat(), lon = _map->CenterLon();
    t += L"Centro: " + Nw(mappe::FormatCoord(lat)) + L", " + Nw(mappe::FormatCoord(lon)) +
         L", zoom " + Nw(mappe::FormatCoord(_map->CurrentZoom())) + L"\n";
    t += L"GPS stato: " + _geoStatus + L", ultimo errore: " + _geoError + L"\n";
    if (_lastPos != nullptr) {
        auto c = _lastPos->Coordinate;
        t += L"GPS: " + Nw(mappe::FormatCoord(c->Point->Position.Latitude)) + L", " +
             Nw(mappe::FormatCoord(c->Point->Position.Longitude)) + L", precisione " +
             Nw(mappe::FormatCoord(c->Accuracy)) + L" m\n";
    }
    t += L"Ricerca: " + _searchInfo + L"\n";
    t += L"Reverse: " + _reverseInfo + L"\n";
    DiagText->Text = S(t);
}

