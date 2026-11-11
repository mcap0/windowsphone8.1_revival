#include "pch.h"
#include "MainPage.xaml.h"
#include "ValuePage.xaml.h"
#include "EditorPage.xaml.h"
#include "Console.h"
#include "Ui.h"
#include "Core/Root.h"
#include "Core/Fs.h"
#include "Core/Cmd.h"
#include <sstream>

using namespace Esplora;
using namespace Platform;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;

namespace
{
    std::vector<std::wstring> g_clip;     // percorsi copiati/tagliati
    bool g_clipCut = false;

    std::wstring Join(const std::wstring& dir, const std::wstring& name)
    {
        if (dir.empty()) return name;
        return dir[dir.size() - 1] == L'\\' ? dir + name : dir + L"\\" + name;
    }

    std::wstring ParentOf(const std::wstring& p)
    {
        if (p.size() <= 3) return p;
        size_t s = p.find_last_of(L'\\');
        if (s == std::wstring::npos) return p;
        if (s == 2 && p[1] == L':') return p.substr(0, 3);
        return p.substr(0, s);
    }

    std::wstring CleanPath(std::wstring p)
    {
        size_t a = p.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) return L"";
        p = p.substr(a, p.find_last_not_of(L" \t\r\n") - a + 1);
        for (size_t i = 0; i < p.size(); i++) if (p[i] == L'/') p[i] = L'\\';
        while (p.size() > 3 && p[p.size() - 1] == L'\\') p.erase(p.size() - 1);
        if (p.size() == 2 && p[1] == L':') p += L'\\';
        return p;
    }

    // files the phone's own apps open (Office, PDF reader, music, video, photos): tap = LaunchFileAsync
    bool IsDocumentName(const std::wstring& name)
    {
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring e = RegText::LowerCopy(name.substr(dot));
        static const wchar_t* docs[] = { L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".one", L".pdf", L".rtf",
            L".mp3", L".m4a", L".wma", L".wav", L".aac", L".flac", L".mp4", L".wmv", L".avi", L".mov", L".3gp",
            L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".epub", L".vcf", L".ics" };
        for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) if (e == docs[i]) return true;
        return false;
    }

    bool IsTextName(const std::wstring& name)
    {
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) return true;
        std::wstring e = RegText::LowerCopy(name.substr(dot));
        static const wchar_t* bin[] = { L".dll", L".exe", L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".mp3", L".mp4", L".wma", L".wmv", L".zip", L".cab",
            L".appx", L".xap", L".mui", L".sys", L".pdb", L".dat", L".ttf", L".otf", L".bin", L".efi", L".mbn", L".hiv", L".db", L".sqlite", L".jxr", L".wav", L".m4a", L".mbs", L".dcc", L".dfc", L".xamlbin", L".pri", L".nlp", L".lex" };
        for (size_t i = 0; i < sizeof(bin) / sizeof(bin[0]); i++) if (e == bin[i]) return false;
        return !IsDocumentName(name);
    }

    String^ Attr(DWORD a)
    {
        std::wstring r;
        if (a & FILE_ATTRIBUTE_READONLY) r += L"sola lettura, ";
        if (a & FILE_ATTRIBUTE_HIDDEN) r += L"nascosto, ";
        if (a & FILE_ATTRIBUTE_SYSTEM) r += L"sistema, ";
        if (a & FILE_ATTRIBUTE_DIRECTORY) r += L"cartella, ";
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) r += L"collegamento, ";
        if (r.empty()) return L"nessuno";
        return S(r.substr(0, r.size() - 2));
    }

    Item^ MakeItem(const std::wstring& name, const wchar_t* glyph, const std::wstring& details, const std::wstring& key, int kind)
    {
        Item^ i = ref new Item();
        i->Name = S(name); i->Glyph = S(glyph); i->Details = S(details); i->Key = S(key); i->Kind = kind;
        return i;
    }

    Item^ ItemFromSource(Object^ source)
    {
        DependencyObject^ cur = dynamic_cast<DependencyObject^>(source);
        while (cur != nullptr)
        {
            ListViewItem^ c = dynamic_cast<ListViewItem^>(cur);
            if (c != nullptr)
            {
                Item^ it = dynamic_cast<Item^>(c->Content);
                return it != nullptr ? it : dynamic_cast<Item^>(c->DataContext);
            }
            cur = VisualTreeHelper::GetParent(cur);
        }
        return nullptr;
    }

    const wchar_t* kFolder = L"\xE188";   // Symbol.Folder in Segoe UI Symbol (WP8.1)
    const wchar_t* kFile = L"\xE160";     // Symbol.Document
    const wchar_t* kValue = L"\x2261";
    // the files a phone without root shows (Documenti, Musica, Immagini, Video, Download)
    const wchar_t* kHome = L"C:\\Data\\Users\\Public";
}

MainPage::MainPage() : regSearchMode_(false), inited_(false)
{
    InitializeComponent();
    filePath_ = kHome;
    {
        bool on = true;   // TODO versione finale: false
        try { auto v = Windows::Storage::ApplicationData::Current->LocalSettings->Values->Lookup(L"console"); if (v != nullptr) on = (bool)v; } catch (...) {}
        SetConsoleEnabled(on);
    }
    BuildAppBar();
    Tabs->SelectionChanged += ref new SelectionChangedEventHandler([this](Object^, SelectionChangedEventArgs^)
    {
        if (Tabs->SelectedIndex != 0) FileEndSelect();
        switch (Tabs->SelectedIndex)
        {
        case 0: BottomAppBar = barFiles_; break;
        case 1: BottomAppBar = barReg_; break;
        default: BottomAppBar = barSys_; break;
        }
    });
    BottomAppBar = barFiles_;

    FilePathBox->KeyDown += ref new KeyEventHandler([this](Object^, KeyRoutedEventArgs^ e)
    {
        if (e->Key == Windows::System::VirtualKey::Enter) FileOpenPath(CleanPath(W(FilePathBox->Text)));
    });
    FileGoButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileOpenPath(CleanPath(W(FilePathBox->Text))); });
    FileList->ItemClick += ref new ItemClickEventHandler([this](Object^, ItemClickEventArgs^ e) { FileOpenItem(dynamic_cast<Item^>(e->ClickedItem), false); });
    FileList->RightTapped += ref new RightTappedEventHandler([this](Object^, RightTappedRoutedEventArgs^ e)
    {
        Item^ it = ItemFromSource(e->OriginalSource);
        if (it != nullptr) FileMenu(it, dynamic_cast<FrameworkElement^>(e->OriginalSource));
    });

    RegPathBox->KeyDown += ref new KeyEventHandler([this](Object^, KeyRoutedEventArgs^ e)
    {
        if (e->Key != Windows::System::VirtualKey::Enter) return;
        RegText::KeyPath p;
        if (RegText::ParseKeyPath(W(RegPathBox->Text), p)) RegOpenPath(p); else SetRegStatus(L"Percorso non valido (usa HKLM\\..., HKCU\\..., HKCR, HKU, HKCC)");
    });
    RegGoButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        RegText::KeyPath p;
        if (RegText::ParseKeyPath(W(RegPathBox->Text), p)) RegOpenPath(p); else SetRegStatus(L"Percorso non valido (usa HKLM\\..., HKCU\\..., HKCR, HKU, HKCC)");
    });
    RegList->ItemClick += ref new ItemClickEventHandler([this](Object^, ItemClickEventArgs^ e) { RegItemTapped(dynamic_cast<Item^>(e->ClickedItem)); });
    RegList->RightTapped += ref new RightTappedEventHandler([this](Object^, RightTappedRoutedEventArgs^ e)
    {
        Item^ it = ItemFromSource(e->OriginalSource);
        if (it != nullptr) RegMenu(it, dynamic_cast<FrameworkElement^>(e->OriginalSource));
    });
    SysInit();
}

void MainPage::OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e)
{
    Page::OnNavigatedTo(e);
    if (!inited_)
    {
        inited_ = true;
        FilePathBox->Text = S(filePath_);
        FileRefresh();
        RegRefresh();
    }
    else Reload();
}

void MainPage::Reload()
{
    FileRefresh();
    RegRefresh();
}

void MainPage::SetFileStatus(const std::wstring& s) { FileStatus->Text = S(s); }
void MainPage::SetRegStatus(const std::wstring& s) { RegStatus->Text = S(s); }

// ---------------------------------------------------------------- barre

void MainPage::BuildAppBar()
{
    auto mkBar = [](AppBarButton^ a, AppBarButton^ b, AppBarButton^ c, AppBarButton^ d, AppBarButton^ more) -> CommandBar^
    {
        auto bar = ref new CommandBar();
        if (a) bar->PrimaryCommands->Append(a);
        if (b) bar->PrimaryCommands->Append(b);
        if (c) bar->PrimaryCommands->Append(c);
        if (d) bar->PrimaryCommands->Append(d);
        if (more) bar->SecondaryCommands->Append(more);
        return bar;
    };
    auto btn = [](const wchar_t* label, Symbol sym) -> AppBarButton^
    {
        auto b = ref new AppBarButton(); b->Label = ref new String(label); b->Icon = ref new SymbolIcon(sym); return b;
    };

    auto fHome = btn(L"home", Symbol::Home), fNew = btn(L"file", Symbol::Add), fDir = btn(L"cartella", Symbol::NewFolder),
         fPaste = btn(L"incolla", Symbol::Paste), fRef = btn(L"aggiorna", Symbol::Refresh);
    fHome->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileOpenPath(kHome); });
    fNew->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileNew(false); });
    fDir->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileNew(true); });
    fPaste->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FilePaste(); });
    fRef->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileRefresh(); });
    barFiles_ = mkBar(fHome, fNew, fDir, fPaste, fRef);

    // selection mode (tieni premuto \u2192 Seleziona): copy / cut to the clipboard, then "incolla" in the target folder
    auto xCopy = btn(L"copia", Symbol::Copy), xCut = btn(L"taglia", Symbol::Cut), xDel = btn(L"elimina", Symbol::Delete),
         xAll = btn(L"tutti", Symbol::SelectAll);
    xCopy->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileClipSelected(false); });
    xCut->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileClipSelected(true); });
    xDel->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileDeleteSelected(); });
    xAll->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { FileList->SelectAll(); });
    barSelect_ = mkBar(xCopy, xCut, xDel, xAll, nullptr);

    auto rVal = btn(L"valore", Symbol::Add), rKey = btn(L"chiave", Symbol::NewFolder), rFind = btn(L"cerca", Symbol::Find), rRef = btn(L"aggiorna", Symbol::Refresh);
    rVal->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { RegNewValue(); });
    rKey->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { RegNewKey(); });
    rFind->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { RegFind(); });
    rRef->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { regSearchMode_ = false; RegRefresh(); });
    barReg_ = mkBar(rVal, rKey, rFind, rRef, nullptr);

    auto sRef = btn(L"aggiorna", Symbol::Refresh);
    sRef->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { SysRefresh(); });
    barSys_ = mkBar(sRef, nullptr, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- file

void MainPage::FileOpenPath(const std::wstring& path)
{
    if (path.empty()) return;
    if (Fs::IsDirectory(path)) { filePath_ = path; FilePathBox->Text = S(path); FileRefresh(); return; }
    if (Fs::Exists(path)) { Frame->Navigate(EditorPage::typeid, S(path)); return; }
    SetFileStatus(L"Non esiste: " + path);
}

void MainPage::FileRefresh()
{
    std::vector<Fs::Entry> entries;
    std::wstring err = Fs::List(filePath_, entries);
    auto items = ref new Platform::Collections::Vector<Object^>();
    if (!err.empty())
    {
        FileList->ItemsSource = items;
        SetFileStatus(L"Errore su " + filePath_ + L": " + err);
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const Fs::Entry& a, const Fs::Entry& b)
    {
        if (a.isDir != b.isDir) return a.isDir;
        return RegText::LowerCopy(a.name) < RegText::LowerCopy(b.name);
    });
    for (size_t i = 0; i < entries.size(); i++)
    {
        const Fs::Entry& e = entries[i];
        std::wstring d = (e.isDir ? std::wstring(L"cartella") : RegText::FormatSize(e.size)) + L"  \u2022  " + FormatDate(e.modified);
        if (e.attributes & FILE_ATTRIBUTE_HIDDEN) d += L"  \u2022  nascosto";
        if (e.attributes & FILE_ATTRIBUTE_SYSTEM) d += L"  \u2022  sistema";
        Item^ it = MakeItem(e.name, e.isDir ? kFolder : kFile, d, Join(filePath_, e.name), e.isDir ? 0 : 1);
        it->Size = e.size;
        items->Append(it);
    }
    FileList->ItemsSource = items;
    std::wstringstream s; s << entries.size() << (entries.size() == 1 ? L" elemento" : L" elementi");
    if (!g_clip.empty()) s << L"  \u2022  negli appunti: " << g_clip.size() << (g_clipCut ? L" (taglia)" : L" (copia)");
    SetFileStatus(s.str());
}

void MainPage::FileOpenItem(Item^ item, bool asText)
{
    if (item == nullptr) return;
    std::wstring p = W(item->Key);
    if (item->Kind == 0) { filePath_ = p; FilePathBox->Text = S(p); FileRefresh(); return; }
    if (asText || IsTextName(W(item->Name))) { Frame->Navigate(EditorPage::typeid, S(p)); return; }
    if (IsDocumentName(W(item->Name))) { FileLaunch(item); return; }
    Item^ it = item;
    FileMenu(it, FileList);
}

// Other apps only get files through a StorageFile: a copy in LocalFolder\apri (we read any path with Win32),
// then the phone picks the app for the extension (Office, PDF reader, Musica...).
void MainPage::FileLaunch(Item^ item)
{
    // audio goes to our Musica (it plays the library song with the same file name), never to Xbox Music
    std::wstring name = W(item->Name);
    size_t dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    for (auto& c : ext) c = towlower(c);
    if (ext == L".mp3" || ext == L".m4a" || ext == L".wma" || ext == L".wav" || ext == L".aac" || ext == L".flac")
    {
        // Musica can only read the music library: a file elsewhere is copied into Music\Esplora first
        std::wstring path = W(item->Key);
        std::wstring lower = path;
        for (auto& c : lower) c = towlower(c);
        static const std::wstring libraries[] = { L"c:\\data\\users\\public\\music\\", L"d:\\music\\" };
        if (lower.compare(0, libraries[0].size(), libraries[0]) != 0 && lower.compare(0, libraries[1].size(), libraries[1]) != 0)
        {
            std::wstring dir = L"C:\\Data\\Users\\Public\\Music\\Esplora";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring copy = dir + L"\\" + name;
            Fs::Delete(copy, false);
            if (Fs::Copy(path, copy).empty()) path = copy;
        }
        auto uri = ref new Uri(L"musica:file?name=" + Uri::EscapeComponent(item->Name) + L"&path=" + Uri::EscapeComponent(S(path)));
        create_task(Windows::System::Launcher::LaunchUriAsync(uri)).then([this, item](task<bool> t)
        {
            bool ok = false;
            try { ok = t.get(); } catch (Exception^) {}
            SetFileStatus(ok ? L"Aperto in Musica: " + W(item->Name) : L"Musica non si apre");
        }, task_continuation_context::use_current());
        return;
    }
    std::wstring dir = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\apri";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring copy = dir + L"\\" + W(item->Name);
    Fs::Delete(copy, false);
    std::wstring err = Fs::Copy(W(item->Key), copy);
    if (!err.empty()) { SetFileStatus(L"Non riesco a copiarlo per aprirlo: " + err); return; }
    SetFileStatus(L"Apro " + W(item->Name) + L"\u2026");
    create_task(Windows::Storage::StorageFile::GetFileFromPathAsync(S(copy)))
        .then([](Windows::Storage::StorageFile^ f) { return create_task(Windows::System::Launcher::LaunchFileAsync(f)); })
        .then([this, item](task<bool> t)
    {
        bool ok = false;
        try { ok = t.get(); } catch (Exception^) {}
        SetFileStatus(ok ? L"Aperto: " + W(item->Name) : L"Nessuna app apre " + W(item->Name) + L": tieni premuto \u2192 Apri come testo");
    }, task_continuation_context::use_current());
}

void MainPage::FileMenu(Item^ item, FrameworkElement^ anchor)
{
    auto flyout = ref new MenuFlyout();
    auto add = [this, flyout](const wchar_t* text, std::function<void()> fn)
    {
        auto mi = ref new MenuFlyoutItem(); mi->Text = ref new String(text);
        mi->Click += ref new RoutedEventHandler([fn](Object^, RoutedEventArgs^) { fn(); });
        flyout->Items->Append(mi);
    };
    if (item->Kind == 1) add(L"Apri come testo", [this, item]() { FileOpenItem(item, true); });
    if (item->Kind == 1) add(L"Apri con un'app", [this, item]() { FileLaunch(item); });
    add(L"Seleziona", [this, item]() { FileBeginSelect(item); });
    add(L"Copia", [this, item]() { g_clip.clear(); g_clip.push_back(W(item->Key)); g_clipCut = false; SetFileStatus(L"Copiato: " + W(item->Name)); });
    add(L"Taglia", [this, item]() { g_clip.clear(); g_clip.push_back(W(item->Key)); g_clipCut = true; SetFileStatus(L"Tagliato: " + W(item->Name)); });
    add(L"Rinomina", [this, item]() { FileRename(item); });
    add(L"Elimina", [this, item]() { FileDelete(item); });
    add(L"Propriet\u00e0", [this, item]() { FileProps(item); });
    flyout->ShowAt(anchor != nullptr ? anchor : static_cast<FrameworkElement^>(FileList));
}

void MainPage::FileNew(bool folder)
{
    auto name = std::make_shared<std::wstring>();
    Prompt(folder ? L"Nuova cartella" : L"Nuovo file di testo", folder ? L"Nuova cartella" : L"nuovo.txt", name).then([this, name, folder](bool ok)
    {
        if (!ok || name->empty() || name->find(L'\\') != std::wstring::npos) return;
        std::wstring path = Join(filePath_, *name);
        auto act = [this, path, folder]()
        {
            std::wstring err;
            if (folder) err = Fs::CreateDirectory(path);
            else { std::vector<unsigned char> none; err = Fs::Exists(path) ? L"esiste gi\u00e0" : Fs::WriteFile(path, none); }
            if (!err.empty()) SetFileStatus(L"Non riuscito: " + err);
            FileRefresh();
        };
        if (IsSafeFilePath(path)) act();
        else Confirm(L"Zona di sistema", S(L"Creare " + path + L"?"), L"Crea").then([act](bool y) { if (y) act(); });
    }, task_continuation_context::use_current());
}

void MainPage::FilePaste()
{
    if (g_clip.empty()) { SetFileStatus(L"Appunti vuoti"); return; }
    std::vector<std::wstring> src = g_clip; bool cut = g_clipCut; std::wstring dir = filePath_;
    auto act = [this, src, cut, dir]()
    {
        std::wstring errAll;
        for (size_t i = 0; i < src.size(); i++)
        {
            size_t s = src[i].find_last_of(L'\\');
            std::wstring name = s == std::wstring::npos ? src[i] : src[i].substr(s + 1);
            std::wstring dst = Join(dir, name);
            if (Fs::Exists(dst)) dst = Join(dir, L"copia di " + name);
            std::wstring err = cut ? Fs::Move(src[i], dst) : Fs::Copy(src[i], dst);
            if (!err.empty()) errAll += name + L": " + err + L"  ";
        }
        if (cut && errAll.empty()) g_clip.clear();
        FileRefresh();
        if (!errAll.empty()) SetFileStatus(L"Errore: " + errAll);
    };
    if (IsSafeFilePath(dir)) act();
    else Confirm(L"Zona di sistema", S(L"Incollare in " + dir + L"?"), L"Incolla").then([act](bool y) { if (y) act(); });
}

void MainPage::FileBeginSelect(Item^ item)
{
    FileList->IsItemClickEnabled = false;
    FileList->SelectionMode = ListViewSelectionMode::Multiple;
    FileList->SelectedItems->Append(item);
    BottomAppBar = barSelect_;
    SetFileStatus(L"Tocca i file da selezionare, poi copia o taglia e incolla nella cartella di arrivo");
}

void MainPage::FileEndSelect()
{
    if (FileList->SelectionMode == ListViewSelectionMode::None) return;
    FileList->SelectionMode = ListViewSelectionMode::None;
    FileList->IsItemClickEnabled = true;
    if (Tabs->SelectedIndex == 0) BottomAppBar = barFiles_;
}

std::vector<std::wstring> MainPage::FileSelectedPaths()
{
    std::vector<std::wstring> paths;
    for (unsigned i = 0; i < FileList->SelectedItems->Size; i++)
    {
        Item^ it = dynamic_cast<Item^>(FileList->SelectedItems->GetAt(i));
        if (it != nullptr) paths.push_back(W(it->Key));
    }
    return paths;
}

void MainPage::FileClipSelected(bool cut)
{
    std::vector<std::wstring> paths = FileSelectedPaths();
    if (paths.empty()) { SetFileStatus(L"Nessun file selezionato"); return; }
    g_clip = paths; g_clipCut = cut;
    FileEndSelect();
    std::wstringstream s; s << (cut ? L"Tagliati: " : L"Copiati: ") << paths.size() << L" \u2014 apri la cartella di arrivo e premi incolla";
    SetFileStatus(s.str());
}

void MainPage::FileDeleteSelected()
{
    std::vector<std::wstring> paths = FileSelectedPaths();
    if (paths.empty()) { SetFileStatus(L"Nessun file selezionato"); return; }
    std::wstringstream m; m << L"Eliminare definitivamente " << paths.size() << L" elementi (le cartelle con tutto il contenuto)?";
    if (!IsSafeFilePath(filePath_)) m << L"\n\nATTENZIONE: \u00e8 un percorso di sistema.";
    Confirm(L"Elimina", S(m.str()), L"Elimina").then([this, paths](bool y)
    {
        if (!y) return;
        std::wstring errAll;
        for (size_t i = 0; i < paths.size(); i++)
        {
            std::wstring err = Fs::Delete(paths[i], Fs::IsDirectory(paths[i]));
            if (!err.empty()) errAll += paths[i] + L": " + err + L"  ";
        }
        FileEndSelect();
        FileRefresh();
        if (!errAll.empty()) SetFileStatus(L"Eliminazione non riuscita: " + errAll);
    }, task_continuation_context::use_current());
}

void MainPage::FileDelete(Item^ item)
{
    std::wstring p = W(item->Key); bool dir = item->Kind == 0;
    std::wstring msg = L"Eliminare definitivamente " + p + (dir ? L" con tutto il contenuto?" : L"?");
    if (!IsSafeFilePath(p)) msg += L"\n\nATTENZIONE: \u00e8 un percorso di sistema.";
    Confirm(L"Elimina", S(msg), L"Elimina").then([this, p, dir](bool y)
    {
        if (!y) return;
        std::wstring err = Fs::Delete(p, dir);
        FileRefresh();
        if (!err.empty()) SetFileStatus(L"Eliminazione non riuscita: " + err);
    }, task_continuation_context::use_current());
}

void MainPage::FileRename(Item^ item)
{
    auto name = std::make_shared<std::wstring>();
    std::wstring old = W(item->Key);
    Prompt(L"Rinomina", W(item->Name), name).then([this, name, old](bool ok)
    {
        if (!ok || name->empty() || name->find(L'\\') != std::wstring::npos) return;
        std::wstring dst = Join(ParentOf(old), *name);
        auto act = [this, old, dst]()
        {
            std::wstring err = Fs::Move(old, dst);
            FileRefresh();
            if (!err.empty()) SetFileStatus(L"Rinomina non riuscita: " + err);
        };
        if (IsSafeFilePath(old)) act();
        else Confirm(L"Zona di sistema", S(L"Rinominare " + old + L"?"), L"Rinomina").then([act](bool y) { if (y) act(); });
    }, task_continuation_context::use_current());
}

void MainPage::FileProps(Item^ item)
{
    Fs::Entry e; std::wstring p = W(item->Key);
    std::wstring err = Fs::GetInfo(p, e);
    std::wstringstream s;
    s << p << L"\n\n";
    if (!err.empty()) s << L"Errore: " << err;
    else
    {
        s << (e.isDir ? L"Cartella" : L"File") << L"\n";
        if (!e.isDir) s << L"Dimensione: " << RegText::FormatSize(e.size) << L" (" << e.size << L" byte)\n";
        s << L"Modificato: " << FormatDate(e.modified) << L"\nAttributi: " << W(Attr(e.attributes)) << L" (0x" << std::hex << e.attributes << L")";
    }
    Info(L"Propriet\u00e0", S(s.str()));
}

// ---------------------------------------------------------------- registro

void MainPage::RegOpenPath(const RegText::KeyPath& p)
{
    regSearchMode_ = false;
    regPath_ = p;
    RegRefresh();
}

void MainPage::RegRefresh()
{
    auto items = ref new Platform::Collections::Vector<Object^>();
    if (regSearchMode_) return;
    RegPathBox->Text = S(RegText::Format(regPath_));
    if (regPath_.hive < 0)
    {
        for (int i = 0; i < 5; i++) items->Append(MakeItem(RegText::HiveShort[i], kFolder, RegText::HiveNames[i], RegText::HiveShort[i], 4));
        RegList->ItemsSource = items;
        SetRegStatus(L"Rami del registro");
        return;
    }
    std::wstring err;
    if (!Root::Init(err)) { RegList->ItemsSource = items; SetRegStatus(L"Accesso al registro non disponibile: " + err); return; }
    HKEY k; LONG r = Root::RegOpen(Root::Hive(regPath_.hive), regPath_.sub, KEY_READ, k);
    if (r != 0) { RegList->ItemsSource = items; SetRegStatus(L"Impossibile aprire la chiave: " + Fs::ErrorText((DWORD)r)); return; }
    std::vector<std::wstring> subs; std::vector<RegText::Value> vals;
    LONG r1 = Root::RegEnumSubKeys(k, subs), r2 = Root::RegEnumValues(k, vals);
    Root::RegClose(k);
    std::sort(subs.begin(), subs.end(), [](const std::wstring& a, const std::wstring& b) { return RegText::LowerCopy(a) < RegText::LowerCopy(b); });
    std::sort(vals.begin(), vals.end(), [](const RegText::Value& a, const RegText::Value& b) { return RegText::LowerCopy(a.name) < RegText::LowerCopy(b.name); });
    for (size_t i = 0; i < subs.size(); i++) items->Append(MakeItem(subs[i], kFolder, L"chiave", subs[i], 2));
    for (size_t i = 0; i < vals.size(); i++)
    {
        std::wstring nm = vals[i].name.empty() ? L"(predefinito)" : vals[i].name;
        items->Append(MakeItem(nm, kValue, RegText::TypeName(vals[i].type) + L" = " + RegText::ValueSummary(vals[i].type, vals[i].data), vals[i].name, 3));
    }
    RegList->ItemsSource = items;
    std::wstringstream s; s << subs.size() << L" chiavi, " << vals.size() << L" valori";
    if (r1 || r2) s << L"  \u2022  errore: " << Fs::ErrorText((DWORD)(r1 ? r1 : r2));
    SetRegStatus(s.str());
}

void MainPage::RegItemTapped(Item^ item)
{
    if (item == nullptr) return;
    if (item->Kind == 4) { RegText::KeyPath p; p.hive = 0; for (int i = 0; i < 5; i++) if (W(item->Key) == RegText::HiveShort[i]) p.hive = i; RegOpenPath(p); }
    else if (item->Kind == 2) RegOpenPath(RegText::Child(regPath_, W(item->Key)));
    else if (item->Kind == 6)
    {
        RegText::KeyPath p;
        if (RegText::ParseKeyPath(W(item->Key), p)) RegOpenPath(p);
    }
    else if (item->Kind == 3)
    {
        Frame->Navigate(ValuePage::typeid, S(RegText::Format(regPath_) + L"\n" + W(item->Key) + L"\n0"));
    }
}

void MainPage::RegMenu(Item^ item, FrameworkElement^ anchor)
{
    auto flyout = ref new MenuFlyout();
    auto add = [flyout](const wchar_t* text, std::function<void()> fn)
    {
        auto mi = ref new MenuFlyoutItem(); mi->Text = ref new String(text);
        mi->Click += ref new RoutedEventHandler([fn](Object^, RoutedEventArgs^) { fn(); });
        flyout->Items->Append(mi);
    };
    if (item->Kind == 3)
    {
        add(L"Modifica", [this, item]() { RegItemTapped(item); });
        add(L"Elimina valore", [this, item]() { RegDeleteValue(item); });
    }
    else if (item->Kind == 2)
    {
        add(L"Apri", [this, item]() { RegItemTapped(item); });
        add(L"Mostra percorso (copiabile)", [this, item]()
        {
            Prompt(L"Percorso", RegText::Format(RegText::Child(regPath_, W(item->Key))), std::make_shared<std::wstring>());
        });
        add(L"Elimina chiave", [this, item]() { RegDeleteKey(item); });
    }
    else return;
    flyout->ShowAt(anchor != nullptr ? anchor : static_cast<FrameworkElement^>(RegList));
}

void MainPage::RegNewKey()
{
    if (regPath_.hive < 0) { SetRegStatus(L"Entra prima in un ramo"); return; }
    auto name = std::make_shared<std::wstring>();
    Prompt(L"Nuova chiave", L"", name).then([this, name](bool ok)
    {
        if (!ok || name->empty()) return;
        RegText::KeyPath target = RegText::Child(regPath_, *name);
        auto act = [this, target]()
        {
            HKEY k; LONG r = Root::RegCreate(Root::Hive(target.hive), target.sub, k);
            if (r == 0) Root::RegClose(k);
            RegRefresh();
            if (r) SetRegStatus(L"Creazione non riuscita: " + Fs::ErrorText((DWORD)r));
        };
        if (IsSafeRegPath(target)) act();
        else Confirm(L"Registro di sistema", S(L"Creare la chiave " + RegText::Format(target) + L"?"), L"Crea").then([act](bool y) { if (y) act(); });
    }, task_continuation_context::use_current());
}

void MainPage::RegNewValue()
{
    if (regPath_.hive < 0) { SetRegStatus(L"Entra prima in una chiave"); return; }
    Frame->Navigate(ValuePage::typeid, S(RegText::Format(regPath_) + L"\n\n1"));
}

void MainPage::RegDeleteKey(Item^ item)
{
    RegText::KeyPath target = RegText::Child(regPath_, W(item->Key));
    std::wstring msg = L"Eliminare la chiave " + RegText::Format(target) + L" con tutto il contenuto?";
    if (!IsSafeRegPath(target)) msg += L"\n\nATTENZIONE: \u00e8 il registro di sistema, un errore pu\u00f2 impedire l'avvio del telefono.";
    Confirm(L"Elimina chiave", S(msg), L"Elimina").then([this, target](bool y)
    {
        if (!y) return;
        LONG r = Root::RegDelKeyTree(Root::Hive(target.hive), target.sub);
        RegRefresh();
        if (r) SetRegStatus(L"Eliminazione non riuscita: " + Fs::ErrorText((DWORD)r));
    }, task_continuation_context::use_current());
}

void MainPage::RegDeleteValue(Item^ item)
{
    std::wstring name = W(item->Key);
    std::wstring msg = L"Eliminare il valore \"" + W(item->Name) + L"\" in " + RegText::Format(regPath_) + L"?";
    if (!IsSafeRegPath(regPath_)) msg += L"\n\nATTENZIONE: \u00e8 il registro di sistema.";
    RegText::KeyPath p = regPath_;
    Confirm(L"Elimina valore", S(msg), L"Elimina").then([this, name, p](bool y)
    {
        if (!y) return;
        HKEY k; LONG r = Root::RegOpen(Root::Hive(p.hive), p.sub, KEY_SET_VALUE, k);
        if (r == 0) { r = Root::RegDelValue(k, name); Root::RegClose(k); }
        RegRefresh();
        if (r) SetRegStatus(L"Eliminazione non riuscita: " + Fs::ErrorText((DWORD)r));
    }, task_continuation_context::use_current());
}

void MainPage::RegFind()
{
    auto text = std::make_shared<std::wstring>();
    Prompt(L"Cerca nel registro", L"", text).then([this, text](bool ok)
    {
        if (!ok || text->empty()) return;
        RegText::KeyPath start = regPath_;
        std::wstring needle = RegText::LowerCopy(*text);
        SetRegStatus(L"Ricerca in corso in " + (start.hive < 0 ? std::wstring(L"tutto il registro") : RegText::Format(start)) + L"...");
        create_task([start, needle]()
        {
            auto hits = std::make_shared<std::vector<Cmd::Hit>>();
            if (start.hive >= 0) Cmd::SearchRegistry(start, needle, 12, 200, 40000, *hits);
            else
            {
                RegText::KeyPath k; k.hive = 2; Cmd::SearchRegistry(k, needle, 12, 200, 40000, *hits);
                k.hive = 1; Cmd::SearchRegistry(k, needle, 12, 200, 10000, *hits);
            }
            return hits;
        }).then([this, text](std::shared_ptr<std::vector<Cmd::Hit>> hits)
        {
            regSearchMode_ = true;
            auto items = ref new Platform::Collections::Vector<Object^>();
            for (size_t i = 0; i < hits->size(); i++)
            {
                const Cmd::Hit& h = (*hits)[i];
                if (h.isKey) items->Append(MakeItem(h.key, kFolder, L"chiave", h.key, 6));
                else items->Append(MakeItem(h.valueName, kValue, h.key + L"  \u2022  " + h.summary, h.key, 6));
            }
            RegList->ItemsSource = items;
            SetRegStatus(L"Risultati per \"" + *text + L"\": " + std::to_wstring(hits->size()) + L" (tocca per aprire la chiave, Indietro per uscire)");
        }, task_continuation_context::use_current());
    }, task_continuation_context::use_current());
}

// ---------------------------------------------------------------- sistema

void MainPage::SysInit()
{
    ConsoleSwitch->IsOn = IsConsoleEnabled();
    ConsoleSwitch->Toggled += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { SetConsoleEnabled(ConsoleSwitch->IsOn); try { Windows::Storage::ApplicationData::Current->LocalSettings->Values->Insert(L"console", ConsoleSwitch->IsOn); } catch (...) {} });
    SkipCmdButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        SkipCmdStatus->Text = L"controllo\u2026";
        SkipPendingCommand([this](std::wstring text) { SkipCmdStatus->Text = ref new String(text.c_str()); });
    });
    SysRefresh();
}

void MainPage::SysRefresh()
{
    std::wstring e;
    SysStatus->Text = Root::Init(e) ? L"Accesso root al registro e ai file: attivo" : S(L"Accesso root non disponibile: " + e);
}

bool MainPage::HandleHardwareBack()
{
    switch (Tabs->SelectedIndex)
    {
    case 0:
        if (FileList->SelectionMode != ListViewSelectionMode::None) { FileEndSelect(); return true; }
        if (filePath_.size() > 3)
        {
            filePath_ = ParentOf(filePath_); FilePathBox->Text = S(filePath_); FileRefresh(); return true;
        }
        return false;
    case 1:
        if (regSearchMode_) { regSearchMode_ = false; RegRefresh(); return true; }
        if (regPath_.hive >= 0) { regPath_ = RegText::Parent(regPath_); RegRefresh(); return true; }
        return false;
    }
    return false;
}
