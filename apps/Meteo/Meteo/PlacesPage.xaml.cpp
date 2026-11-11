#include "pch.h"
#include "PlacesPage.xaml.h"

using namespace Meteo;
using namespace MeteoCore;
using namespace concurrency;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Navigation;

namespace {

TextBlock^ Text(const std::wstring& text, double size, double opacity) {
    auto t = ref new TextBlock();
    t->Text = ref new String(text.c_str());
    t->FontSize = size;
    t->Opacity = opacity;
    t->TextTrimming = TextTrimming::CharacterEllipsis;
    return t;
}

}  // namespace

PlacesPage::PlacesPage() {
    InitializeComponent();
}

void PlacesPage::OnNavigatedTo(NavigationEventArgs^) {
    Fill();
}

// one row per place: name, then "casa" and the last known weather
void PlacesPage::Fill() {
    _places = Favorites();
    Place home = Home();
    List->Items->Clear();
    for (size_t i = 0; i < _places.size(); ++i) {
        const Place& p = _places[i];
        auto row = ref new StackPanel();
        row->Margin = Thickness(0, 6, 0, 12);
        row->Tag = static_cast<int>(i);
        row->Children->Append(Text(p.name, 28, 1.0));
        std::wstring detail = SamePlace(p, home) ? L"casa \u00b7 sulla tile" : L"";
        Forecast f;
        if (LoadCache(p, f)) detail += (detail.empty() ? L"" : L" \u00b7 ") + Degrees(f.temp) + L" " + Describe(f.symbol);
        if (!detail.empty()) row->Children->Append(Text(detail, 17, 0.65));

        int index = static_cast<int>(i);
        row->Holding += ref new HoldingEventHandler([this, index, row](Object^, HoldingRoutedEventArgs^ e) {
            if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
            auto menu = ref new MenuFlyout();
            auto home = ref new MenuFlyoutItem();
            home->Text = L"imposta come casa";
            home->Click += ref new RoutedEventHandler([this, index](Object^, RoutedEventArgs^) {
                SetHome(_places[index]);
                Fill();
            });
            menu->Items->Append(home);
            if (!_places[index].here) {
                auto remove = ref new MenuFlyoutItem();
                remove->Text = L"rimuovi";
                remove->Click += ref new RoutedEventHandler([this, index](Object^, RoutedEventArgs^) {
                    Place gone = _places[index];
                    _places.erase(_places.begin() + index);
                    SaveFavorites(_places);
                    if (SamePlace(gone, Home())) SetHome(_places[0]);
                    Fill();
                });
                menu->Items->Append(remove);
            }
            menu->ShowAt(row);
        });
        List->Items->Append(row);
    }
}

void PlacesPage::List_ItemClick(Object^, ItemClickEventArgs^ e) {
    auto row = dynamic_cast<StackPanel^>(e->ClickedItem);
    if (row == nullptr) return;
    int index = safe_cast<int>(row->Tag);
    SetShown(_places[index]);
    if (Frame->CanGoBack) Frame->GoBack();
}

void PlacesPage::SearchBox_KeyDown(Object^, KeyRoutedEventArgs^ e) {
    if (e->Key == Windows::System::VirtualKey::Enter) {
        e->Handled = true;
        Add();
    }
}

void PlacesPage::Add_Click(Object^, RoutedEventArgs^) {
    Add();
}

void PlacesPage::Add() {
    std::wstring name = SearchBox->Text->Data();
    if (name.empty()) return;
    Status->Text = L"cerco\u2026";
    Focus(Windows::UI::Xaml::FocusState::Programmatic);  // closes the keyboard
    create_task(FindPlace(name)).then([this](Place p) {
        if (!p.error.empty()) {
            Status->Text = ref new String(p.error.c_str());
            return;
        }
        for (const Place& q : _places)
            if (SamePlace(q, p)) {
                Status->Text = ref new String((p.name + L" c'\u00e8 gi\u00e0").c_str());
                return;
            }
        _places.push_back(p);
        SaveFavorites(_places);
        SearchBox->Text = L"";
        Status->Text = ref new String((L"aggiunto " + p.name).c_str());
        Fill();
    }, task_continuation_context::use_current());
}
