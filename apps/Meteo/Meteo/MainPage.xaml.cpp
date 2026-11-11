#include "pch.h"
#include "MainPage.xaml.h"
#include "PlacesPage.xaml.h"
#include "../Shared/Tile.h"

using namespace Meteo;
using namespace MeteoCore;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;

namespace {

std::wstring Upper(const std::wstring& s) {  // towupper/towlower in the C locale leave accents alone (UMIDITA with a lowercase accented a)
    std::wstring r(s);
    if (!r.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.c_str(), static_cast<int>(s.size()), &r[0],
                      static_cast<int>(r.size()), nullptr, nullptr, 0);
    return r;
}

std::wstring Lower(const std::wstring& s) {
    std::wstring r(s);
    if (!r.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), &r[0],
                      static_cast<int>(r.size()), nullptr, nullptr, 0);
    return r;
}

std::wstring City(const std::wstring& name) {  // "Palermo, Sicilia" -> "Palermo"
    size_t comma = name.find(L',');
    return comma == std::wstring::npos ? name : name.substr(0, comma);
}

Brush^ Accent() {
    Object^ brush = nullptr;
    if (Application::Current->Resources->HasKey(L"PhoneAccentBrush"))
        brush = Application::Current->Resources->Lookup(L"PhoneAccentBrush");
    auto b = dynamic_cast<Brush^>(brush);
    return b != nullptr ? b : ref new SolidColorBrush(Windows::UI::Colors::DodgerBlue);
}

Brush^ White() { return ref new SolidColorBrush(Windows::UI::Colors::White); }

// Haru's look: small grey semibold captions, values in the large phone font, the sky in the accent colour
TextBlock^ Label(const std::wstring& text) {
    auto t = ref new TextBlock();
    t->Text = ref new String(Upper(text).c_str());
    t->FontSize = 15;
    t->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
    t->Foreground = ref new SolidColorBrush(Windows::UI::ColorHelper::FromArgb(0xFF, 0x99, 0x99, 0x99));
    return t;
}

// Haru's weather icon (Assets\Haru, MIT License)
Image^ WeatherIcon(const std::wstring& symbol, bool day, double size) {
    auto image = ref new Image();
    std::wstring uri = std::wstring(L"ms-appx:///Assets/Haru/") + Icon(symbol, day) + L".png";
    image->Source = ref new Windows::UI::Xaml::Media::Imaging::BitmapImage(ref new Uri(ref new String(uri.c_str())));
    image->Width = size;
    image->Height = size;
    image->Stretch = Stretch::Uniform;
    return image;
}

TextBlock^ Value(const std::wstring& text, double size) {
    auto t = ref new TextBlock();
    t->Text = ref new String(text.c_str());
    t->FontSize = size;
    t->TextWrapping = TextWrapping::Wrap;
    return t;
}

StackPanel^ Field(const std::wstring& label, const std::wstring& value, double size) {
    auto p = ref new StackPanel();
    p->Margin = Thickness(0, 0, 12, 12);
    p->Children->Append(Label(label));
    p->Children->Append(Value(value, size));
    return p;
}

// square accent box: "GIO 14:00", "SAB 24/10"
Border^ AccentBox(const std::wstring& text) {
    auto t = ref new TextBlock();
    t->Text = ref new String(Upper(text).c_str());
    t->FontSize = 15;
    t->FontWeight = Windows::UI::Text::FontWeights::SemiBold;
    auto b = ref new Border();
    b->Background = Accent();
    b->Padding = Thickness(6);
    b->HorizontalAlignment = HorizontalAlignment::Left;
    b->Child = t;
    return b;
}

// picture + CONDIZIONE + the sky in the accent colour
Grid^ Condition(const std::wstring& symbol, bool day, double glyphSize, double textSize) {
    auto g = ref new Grid();
    auto c0 = ref new ColumnDefinition();
    c0->Width = GridLength(glyphSize + 30);
    g->ColumnDefinitions->Append(c0);
    g->ColumnDefinitions->Append(ref new ColumnDefinition());
    auto icon = WeatherIcon(symbol, day, glyphSize);
    icon->HorizontalAlignment = HorizontalAlignment::Left;
    icon->VerticalAlignment = VerticalAlignment::Center;
    g->Children->Append(icon);
    auto text = ref new StackPanel();
    text->VerticalAlignment = VerticalAlignment::Center;
    text->Children->Append(Label(L"condizione"));
    auto sky = Value(Describe(symbol), textSize);
    sky->Foreground = Accent();
    sky->FontWeight = Windows::UI::Text::FontWeights::SemiLight;
    sky->Margin = Thickness(0, -4, 0, 0);
    text->Children->Append(sky);
    Grid::SetColumn(text, 1);
    g->Children->Append(text);
    return g;
}

// fields in equal columns
Grid^ Fields(const std::vector<std::pair<std::wstring, std::wstring>>& fields, int columns, double size) {
    auto g = ref new Grid();
    for (int i = 0; i < columns; ++i) {
        auto c = ref new ColumnDefinition();
        c->Width = GridLength(columns == 4 && i == 3 ? 1.5 : 1.0, GridUnitType::Star);  // wind needs more room
        g->ColumnDefinitions->Append(c);
    }
    int rows = static_cast<int>((fields.size() + columns - 1) / columns);
    for (int i = 0; i < rows; ++i) g->RowDefinitions->Append(ref new RowDefinition());
    for (size_t i = 0; i < fields.size(); ++i) {
        auto f = Field(fields[i].first, fields[i].second, size);
        Grid::SetColumn(f, static_cast<int>(i % columns));
        Grid::SetRow(f, static_cast<int>(i / columns));
        g->Children->Append(f);
    }
    return g;
}

std::wstring Short(double degrees) {  // where the wind comes from: N, NE, E, SE, S, SO, O, NO
    static const wchar_t* names[] = {L"N", L"NE", L"E", L"SE", L"S", L"SO", L"O", L"NO"};
    return names[static_cast<int>((degrees + 22.5) / 45.0) % 8];
}

int WeekDay(const std::wstring& date) {
    int y = 0, m = 0, d = 0;
    swscanf_s(date.c_str(), L"%d-%d-%d", &y, &m, &d);
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    FileTimeToSystemTime(&ft, &st);
    return st.wDayOfWeek;
}

const wchar_t* DayShort(const std::wstring& date) {
    static const wchar_t* names[] = {L"dom", L"lun", L"mar", L"mer", L"gio", L"ven", L"sab"};
    return names[WeekDay(date)];
}

std::wstring DayMonth(const std::wstring& date) {  // "2026-10-24" -> "24/10"
    return date.size() >= 10 ? date.substr(8, 2) + L"/" + date.substr(5, 2) : date;
}

std::wstring Two(int v) {
    wchar_t b[8];
    swprintf_s(b, L"%02d", v);
    return b;
}

ScrollViewer^ Column(StackPanel^ panel) {
    panel->Margin = Thickness(0, 0, 0, 30);
    auto s = ref new ScrollViewer();
    s->Content = panel;
    return s;
}

}  // namespace

MainPage::MainPage() : _hasData(false), _started(false) {
    InitializeComponent();
    NavigationCacheMode = Windows::UI::Xaml::Navigation::NavigationCacheMode::Required;
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e) {
    if (!_started) {
        _started = true;
        Open(dynamic_cast<String^>(e->Parameter));
        return;
    }
    if (e->NavigationMode == NavigationMode::Back) {  // from the places page: the place chosen there
        Place shown = Shown();
        if (!SamePlace(shown, _place) || shown.name != _place.name) ShowPlace(shown, L"");
        else UpdateHomeButton();
    }
}

void MainPage::Open(String^ uri) {
    if (uri == nullptr || uri->IsEmpty()) {
        ShowPlace(Shown(), L"");
        return;
    }
    Place fromTile;
    if (PlaceFromArguments(uri->Data(), fromTile)) {
        ShowPlace(fromTile, L"");
        return;
    }
    std::wstring place, day;
    try {
        auto query = (ref new Uri(uri))->QueryParsed;
        for (unsigned i = 0; i < query->Size; ++i) {
            std::wstring name = query->GetAt(i)->Name->Data();
            if (name == L"luogo") place = query->GetAt(i)->Value->Data();
            if (name == L"giorno") day = query->GetAt(i)->Value->Data();
        }
    } catch (Exception^) {}
    if (place.empty()) {
        Place here = {0, 0, L"Posizione attuale", true, L""};
        ShowPlace(here, day);
        return;
    }
    // a favourite with that name, otherwise search it
    for (const Place& p : Favorites()) {
        if (!p.here && Lower(City(p.name)) == Lower(place)) {
            ShowPlace(p, day);
            return;
        }
    }
    Sections->Title = ref new String(Upper(place).c_str());
    SetStatus(L"cerco " + place + L"\u2026");
    create_task(FindPlace(place)).then([this, place, day](Place p) {
        if (!p.error.empty()) {
            SetStatus(p.error);
            return;
        }
        ShowPlace(p, day);
    }, task_continuation_context::use_current());
}

void MainPage::ShowPlace(const Place& place, const std::wstring& day) {
    _place = place;
    _day = day;
    SetShown(place);
    Sections->Title = ref new String(Upper(place.name).c_str());
    UpdateHomeButton();
    Forecast cached;
    _hasData = LoadCache(place, cached);
    if (_hasData) {
        cached.place.name = place.name;
        Render(cached);
    } else {
        Sections->Items->Clear();
        _status = nullptr;
        _week = nullptr;
    }
    Refresh();
}

// the bottom line of "adesso"; without a forecast yet, a column with only this line
void MainPage::SetStatus(const std::wstring& text) {
    if (_status == nullptr) {
        _status = Value(text, 18);
        _status->Opacity = 0.7;
        PutSection(0, L"adesso", _status);
        while (Sections->Items->Size > 1) Sections->Items->RemoveAtEnd();
    }
    _status->Text = ref new String(text.c_str());
}

void MainPage::Refresh() {
    if (_hasData) SetStatus(L"aggiornamento\u2026");
    else SetStatus(L"scarico le previsioni\u2026");
    Place place = _place;
    task<Place> where = place.here ? CurrentPlace() : task_from_result(place);
    where.then([](Place p) {
             if (!p.error.empty()) {
                 Forecast none;
                 none.place = p;
                 none.error = p.error;
                 return task_from_result(none);
             }
             return Download(p);
         })
        .then([this, place](Forecast f) {
            if (!SamePlace(place, _place)) return;  // the user moved to another place meanwhile
            if (!f.error.empty()) {
                SetStatus(_hasData ? L"previsione delle " + _updated + L", non aggiornata: " + f.error : f.error);
                return;
            }
            if (!place.here) f.place.name = place.name;  // for the position: the town found there
            else Sections->Title = ref new String(Upper(f.place.name).c_str());
            _hasData = true;
            Render(f);
            if (SamePlace(place, Home()) || IsPinned(f.place)) DrawTile(f);
            if (!SamePlace(place, Home())) RefreshTile();
        }, task_continuation_context::use_current());
}

// the Start tile always shows the home place, whatever place is on screen
void MainPage::RefreshTile() {
    Place home = Home();
    task<Place> where = home.here ? CurrentPlace() : task_from_result(home);
    where.then([](Place p) {
             if (!p.error.empty()) {
                 Forecast none;
                 none.error = p.error;
                 return task_from_result(none);
             }
             return Download(p);
         })
        .then([this](Forecast f) {
            if (!f.error.empty()) {
                Log(L"tile: " + f.error);
                return;
            }
            DrawTile(f);
        }, task_continuation_context::use_current());
}

void MainPage::DrawTile(const Forecast& f) {
    try {
        UpdateTile(f);
        Log(L"tile aggiornata: " + f.place.name + L" " + Degrees(f.temp) + L" " + Describe(f.symbol));
    } catch (Exception^ ex) {
        Log(L"tile: " + std::wstring(ex->Message->Data()));
    }
}

void MainPage::Render(const Forecast& f) {
    _forecast = f;
    UpdateHomeButton();
    int selected = Sections->SelectedIndex;
    _updated = f.updated;
    std::wstring today = Today();
    SYSTEMTIME now;
    GetLocalTime(&now);

    // adesso
    {
        auto panel = ref new StackPanel();
        auto condition = Condition(f.symbol, f.day, 100, 40);
        condition->Margin = Thickness(0, 10, 0, 22);
        panel->Children->Append(condition);
        double rain = 0;
        for (const Hour& h : f.hours)
            if (h.date == today && h.hour == now.wHour) rain = h.rain;
        const Day& d = f.days[0];
        std::vector<std::pair<std::wstring, std::wstring>> fields;
        fields.push_back(std::make_pair(std::wstring(L"temperatura"), Degrees(f.temp)));
        fields.push_back(std::make_pair(std::wstring(L"minima / massima"), Degrees(d.tmin) + L" / " + Degrees(d.tmax)));
        fields.push_back(std::make_pair(std::wstring(L"umidit\u00e0"), std::to_wstring(f.humidity) + L"%"));
        fields.push_back(std::make_pair(std::wstring(L"precipitazioni"), Millimetres(rain)));
        fields.push_back(std::make_pair(std::wstring(L"vento"), Speed(f.wind)));
        fields.push_back(std::make_pair(std::wstring(L"direzione del vento"), Direction(f.windFrom)));
        fields.push_back(std::make_pair(std::wstring(L"pressione"), std::to_wstring(static_cast<int>(f.pressure + 0.5)) + L" hPa"));
        fields.push_back(std::make_pair(std::wstring(L"pioggia oggi"), Millimetres(d.rain)));
        panel->Children->Append(Fields(fields, 2, 30));
        _status = Value(L"previsione delle " + f.updated, 15);
        _status->Opacity = 0.6;
        _status->Margin = Thickness(0, 6, 0, 0);
        panel->Children->Append(_status);
        PutSection(0, L"adesso", Column(panel));
    }

    // prossime 12 ore
    {
        auto panel = ref new StackPanel();
        int count = 0;
        for (const Hour& h : f.hours) {
            if (h.date < today || (h.date == today && h.hour <= now.wHour)) continue;
            if (++count > 12) break;
            auto entry = ref new StackPanel();
            entry->Margin = Thickness(0, 0, 0, 36);
            entry->Children->Append(AccentBox(std::wstring(DayShort(h.date)) + L" " + Two(h.hour) + L":00"));
            auto condition = Condition(h.symbol, h.day, 64, 32);
            condition->Margin = Thickness(0, 8, 0, 8);
            entry->Children->Append(condition);
            std::vector<std::pair<std::wstring, std::wstring>> fields;
            fields.push_back(std::make_pair(std::wstring(L"temp"), Degrees(h.temp)));
            fields.push_back(std::make_pair(std::wstring(L"umid"), std::to_wstring(h.humidity) + L"%"));
            fields.push_back(std::make_pair(std::wstring(L"prec"), Millimetres(h.rain)));
            fields.push_back(std::make_pair(std::wstring(L"vento"), Speed(h.wind) + L" " + Short(h.windFrom)));
            entry->Children->Append(Fields(fields, 4, 18));
            panel->Children->Append(entry);
        }
        PutSection(1, L"prossime 12 ore", Column(panel));
    }

    // prossimi 7 giorni
    {
        auto panel = ref new StackPanel();
        for (const Day& d : f.days) {
            if (d.date < today) continue;
            auto entry = ref new StackPanel();
            entry->Margin = Thickness(0, 0, 0, 36);
            entry->Tag = ref new String(d.date.c_str());
            std::wstring when = d.date == today ? L"oggi " + DayMonth(d.date)
                                                : std::wstring(DayShort(d.date)) + L" " + DayMonth(d.date);
            entry->Children->Append(AccentBox(when));
            auto condition = Condition(d.symbol, true, 64, 32);
            condition->Margin = Thickness(0, 8, 0, 8);
            entry->Children->Append(condition);
            std::vector<std::pair<std::wstring, std::wstring>> fields;
            fields.push_back(std::make_pair(std::wstring(L"temp"), Degrees(d.tmax) + L"/" + Degrees(d.tmin)));
            fields.push_back(std::make_pair(std::wstring(L"umid media"), std::to_wstring(d.humidity) + L"%"));
            fields.push_back(std::make_pair(std::wstring(L"prec"), Millimetres(d.rain)));
            fields.push_back(std::make_pair(std::wstring(L"vento max"), Speed(d.wind) + L" " + Short(d.windFrom)));
            entry->Children->Append(Fields(fields, 4, 18));
            panel->Children->Append(entry);
        }
        _week = Column(panel);
        PutSection(2, L"prossimi 7 giorni", _week);
    }

    if (!_day.empty() && _day != today) SelectDay();
    else if (selected >= 0 && selected < static_cast<int>(Sections->Items->Size)) Sections->SelectedIndex = selected;
}

// fills column i keeping the PivotItem: clearing the Pivot and adding new items while it is on screen leaves the
// selected column black until the user changes column (forecast arriving after the app opened)
void MainPage::PutSection(unsigned i, Platform::String^ header, UIElement^ content) {
    if (i < Sections->Items->Size) {
        auto item = safe_cast<PivotItem^>(Sections->Items->GetAt(i));
        item->Header = header;
        item->Content = content;
        return;
    }
    auto item = ref new PivotItem();
    item->Header = header;
    item->Content = content;
    Sections->Items->Append(item);
}

// the day asked by the Assistente: the week column, scrolled to that day
void MainPage::SelectDay() {
    Sections->SelectedIndex = 2;
    std::wstring day = _day;
    _day.clear();
    auto week = _week;
    Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low, ref new Windows::UI::Core::DispatchedHandler([week, day]() {
        auto panel = dynamic_cast<StackPanel^>(week->Content);
        if (panel == nullptr) return;
        week->UpdateLayout();
        for (unsigned i = 0; i < panel->Children->Size; ++i) {
            auto entry = dynamic_cast<FrameworkElement^>(panel->Children->GetAt(i));
            if (entry != nullptr && entry->Tag != nullptr && std::wstring(safe_cast<String^>(entry->Tag)->Data()) == day) {
                Point at = entry->TransformToVisual(panel)->TransformPoint(Point(0, 0));
                week->ChangeView(nullptr, static_cast<double>(at.Y), nullptr);
                break;
            }
        }
    }));
}

void MainPage::UpdateHomeButton() {
    bool home = SamePlace(_place, Home());
    HomeButton->Label = home ? L"\u00e8 casa" : L"casa";
    HomeButton->IsEnabled = !home;
    bool pinned = IsPinned(_place);
    PinButton->Label = pinned ? L"\u00e8 su Start" : L"aggiungi a Start";
    PinButton->IsEnabled = !pinned && !_forecast.days.empty() && SamePlace(_forecast.place, _place);
}

// a tile of this place on Start, like Haru's: the phone shows Start, then the tile gets its first forecast
void MainPage::Pin_Click(Object^, RoutedEventArgs^) {
    Forecast f = _forecast;
    if (f.days.empty()) return;
    create_task(PinTile(f)).then([this, f](task<bool> t) {
        try {
            if (t.get()) UpdateTile(f);
        } catch (Exception^ ex) {
            Log(L"fissa su Start: " + std::wstring(ex->Message->Data()));
        }
        UpdateHomeButton();
    }, task_continuation_context::use_current());
}

void MainPage::Places_Click(Object^, RoutedEventArgs^) {
    Frame->Navigate(PlacesPage::typeid);
}

void MainPage::Refresh_Click(Object^, RoutedEventArgs^) {
    Refresh();
}

// this place on the Start tile
void MainPage::Home_Click(Object^, RoutedEventArgs^) {
    SetHome(_place);
    UpdateHomeButton();
    Forecast f;
    if (LoadCache(_place, f)) DrawTile(f);
    else RefreshTile();
}
