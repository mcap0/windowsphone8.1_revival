#include "pch.h"
#include "ValuePage.xaml.h"
#include "Ui.h"
#include "Core/Root.h"
#include "Core/Fs.h"

using namespace Esplora;
using namespace Platform;
using namespace concurrency;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace
{
    const uint32_t kTypes[] = { RegText::Sz, RegText::ExpandSz, RegText::MultiSz, RegText::Dword, RegText::Qword, RegText::Binary, RegText::None, RegText::DwordBE, RegText::Link };
    const int kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);
}

ValuePage::ValuePage() : isNew_(true)
{
    InitializeComponent();
    for (int i = 0; i < kTypeCount; i++) TypeBox->Items->Append(S(RegText::TypeName(kTypes[i])));
    TypeBox->SelectedIndex = 0;

    auto bar = ref new CommandBar();
    auto save = ref new AppBarButton(); save->Label = L"salva"; save->Icon = ref new SymbolIcon(Symbol::Save);
    save->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { Save(); });
    auto del = ref new AppBarButton(); del->Label = L"elimina"; del->Icon = ref new SymbolIcon(Symbol::Delete);
    del->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { Delete(); });
    auto cancel = ref new AppBarButton(); cancel->Label = L"annulla"; cancel->Icon = ref new SymbolIcon(Symbol::Cancel);
    cancel->Click += ref new RoutedEventHandler([](Object^, RoutedEventArgs^) { GoBackFrame(); });
    bar->PrimaryCommands->Append(save);
    bar->PrimaryCommands->Append(del);
    bar->PrimaryCommands->Append(cancel);
    BottomAppBar = bar;
}

void ValuePage::OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e)
{
    Page::OnNavigatedTo(e);
    String^ arg = dynamic_cast<String^>(e->Parameter);
    if (arg == nullptr) return;
    std::wstring a = W(arg);
    size_t p1 = a.find(L'\n'), p2 = a.find(L'\n', p1 + 1);
    RegText::ParseKeyPath(a.substr(0, p1), key_);
    origName_ = a.substr(p1 + 1, p2 - p1 - 1);
    isNew_ = a.substr(p2 + 1) == L"1";
    KeyText->Text = S(RegText::Format(key_));
    NameBox->Text = S(origName_);
    if (isNew_) return;
    NameBox->IsReadOnly = true;   // rinominare un valore = nuovo valore + elimina
    std::wstring err;
    HKEY k; LONG r = Root::RegOpen(Root::Hive(key_.hive), key_.sub, KEY_READ, k);
    if (r) { StatusText->Text = S(L"Lettura non riuscita: " + Fs::ErrorText((DWORD)r)); return; }
    std::vector<RegText::Value> vals; Root::RegEnumValues(k, vals); Root::RegClose(k);
    for (size_t i = 0; i < vals.size(); i++)
    {
        if (vals[i].name != origName_) continue;
        for (int t = 0; t < kTypeCount; t++) if (kTypes[t] == vals[i].type) TypeBox->SelectedIndex = t;
        std::wstring text = RegText::ValueToText(vals[i].type, vals[i].data);
        // per i numeri si modifica solo la parte esadecimale/decimale: mostra il decimale
        if (vals[i].type == RegText::Dword || vals[i].type == RegText::DwordBE || vals[i].type == RegText::Qword)
        {
            size_t o = text.find(L'('); if (o != std::wstring::npos) text = text.substr(o + 1, text.size() - o - 2);
        }
        DataBox->Text = S(text);
        return;
    }
    StatusText->Text = L"Valore non trovato";
}

void ValuePage::Save()
{
    int ti = TypeBox->SelectedIndex; if (ti < 0) ti = 0;
    uint32_t type = kTypes[ti];
    std::wstring name = W(NameBox->Text);
    std::vector<unsigned char> data; std::wstring err;
    if (!RegText::TextToValue(type, W(DataBox->Text), data, err)) { StatusText->Text = S(err); return; }
    if (IsSafeRegPath(key_)) { DoSave(data, type, name); return; }
    Confirm(L"Registro di sistema", S(L"Scrivere " + RegText::Format(key_) + L" : " + (name.empty() ? L"(predefinito)" : name) + L" ?\nUn valore sbagliato pu\u00f2 impedire l'avvio del telefono."), L"Scrivi")
        .then([this, data, type, name](bool y) { if (y) DoSave(data, type, name); }, task_continuation_context::use_current());
}

void ValuePage::DoSave(const std::vector<unsigned char>& data, uint32_t type, const std::wstring& name)
{
    HKEY k; LONG r = Root::RegOpen(Root::Hive(key_.hive), key_.sub, KEY_SET_VALUE, k);
    if (r) { StatusText->Text = S(L"Apertura in scrittura non riuscita: " + Fs::ErrorText((DWORD)r)); return; }
    r = Root::RegSet(k, name, type, data.data(), (DWORD)data.size());
    Root::RegClose(k);
    if (r) { StatusText->Text = S(L"Scrittura non riuscita: " + Fs::ErrorText((DWORD)r)); return; }
    GoBackFrame();
}

void ValuePage::Delete()
{
    if (isNew_) { GoBackFrame(); return; }
    std::wstring msg = L"Eliminare il valore \"" + origName_ + L"\"?";
    if (!IsSafeRegPath(key_)) msg += L"\n\nATTENZIONE: registro di sistema.";
    Confirm(L"Elimina valore", S(msg), L"Elimina").then([this](bool y)
    {
        if (!y) return;
        HKEY k; LONG r = Root::RegOpen(Root::Hive(key_.hive), key_.sub, KEY_SET_VALUE, k);
        if (r == 0) { r = Root::RegDelValue(k, origName_); Root::RegClose(k); }
        if (r) StatusText->Text = S(L"Eliminazione non riuscita: " + Fs::ErrorText((DWORD)r)); else GoBackFrame();
    }, task_continuation_context::use_current());
}
