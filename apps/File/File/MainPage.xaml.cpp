#include "pch.h"
#include "MainPage.xaml.h"
#include "EditorPage.xaml.h"
#include "Core/Fs.h"
#include "Core/Text.h"

using namespace File;
using namespace Platform;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;

namespace
{
    std::vector<std::wstring> clipboardPaths;
    unsigned int refreshGeneration = 0;
    std::wstring pendingNotice;
    bool clipboardCut = false;

    struct Place
    {
        const wchar_t* label;
        const wchar_t* path;
    };

    std::wstring ToWString(String^ value)
    {
        return value == nullptr ? std::wstring() : std::wstring(value->Data(), value->Length());
    }

    String^ ToPlatformString(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    std::wstring NormalizePath(std::wstring path)
    {
        size_t first = path.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
            return std::wstring();
        size_t last = path.find_last_not_of(L" \t\r\n");
        path = path.substr(first, last - first + 1);

        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (path.size() > 3 && path[path.size() - 1] == L'\\')
            path.erase(path.size() - 1);

        return path;
    }

    std::wstring JoinPath(const std::wstring& parent, const std::wstring& name)
    {
        if (parent.empty())
            return name;
        if (parent[parent.size() - 1] == L'\\')
            return parent + name;
        return parent + L"\\" + name;
    }

    std::wstring ParentPath(const std::wstring& path)
    {
        std::wstring normalized = NormalizePath(path);
        if (normalized.size() <= 3)
            return normalized;
        size_t slash = normalized.find_last_of(L'\\');
        if (slash == std::wstring::npos)
            return normalized;
        if (slash == 2 && normalized.size() >= 3 && normalized[1] == L':')
            return normalized.substr(0, 3);
        if (slash == 0)
            return normalized.substr(0, 1);
        return normalized.substr(0, slash);
    }

    bool SamePath(const std::wstring& left, const std::wstring& right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (towlower(left[i]) != towlower(right[i]))
                return false;
        }
        return true;
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        return value;
    }

    std::wstring FormatSize(uint64_t size)
    {
        std::wstringstream stream;
        if (size < 1024)
            stream << size << L" B";
        else if (size < 1024 * 1024)
            stream << std::fixed << std::setprecision(1) << (static_cast<double>(size) / 1024.0) << L" KB";
        else if (size < 1024ULL * 1024ULL * 1024ULL)
            stream << std::fixed << std::setprecision(1) << (static_cast<double>(size) / (1024.0 * 1024.0)) << L" MB";
        else
            stream << std::fixed << std::setprecision(2) << (static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
        return stream.str();
    }

    std::wstring FormatDate(const FILETIME& time)
    {
        SYSTEMTIME systemTime;
        if (!FileTimeToSystemTime(&time, &systemTime))
            return L"data non disponibile";

        std::wstringstream stream;
        stream << std::setfill(L'0')
            << std::setw(4) << systemTime.wYear << L'-'
            << std::setw(2) << systemTime.wMonth << L'-'
            << std::setw(2) << systemTime.wDay << L' '
            << std::setw(2) << systemTime.wHour << L':'
            << std::setw(2) << systemTime.wMinute;
        return stream.str();
    }

    std::wstring FormatAttributes(DWORD attributes)
    {
        std::wstringstream stream;
        stream << L"0x" << std::uppercase << std::hex << attributes;
        return stream.str();
    }

    bool IsTextExtension(const std::wstring& name)
    {
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos)
            return false;

        std::wstring extension = Lower(name.substr(dot));
        const wchar_t* extensions[] =
        {
            L".txt", L".log", L".xml", L".json", L".ini", L".cfg", L".csv",
            L".md", L".html", L".js", L".css", L".reg", L".provxml",
            L".policy", L".cs", L".cpp", L".h"
        };
        for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        {
            if (extension == extensions[i])
                return true;
        }
        return false;
    }

    Button^ MakePlaceButton(String^ label)
    {
        Button^ button = ref new Button();
        button->Content = label;
        button->FontSize = 14;
        button->Padding = Thickness(9, 2, 9, 2);
        button->Margin = Thickness(2, 1, 2, 1);
        return button;
    }

    String^ ExceptionText(Exception^ exception)
    {
        return exception != nullptr && exception->Message != nullptr ?
            exception->Message : ref new String(L"errore sconosciuto");
    }
}

MainPage::MainPage()
    : selecting_(false), suppressSelection_(false)
{
    InitializeComponent();

    currentPath_ = ref new String(L"C:\\Data\\Users\\Public");
    placeRoot_ = ref new String(L"C:\\Data\\Users\\Public");

    String^ crashedIn = nullptr;
    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey("listing"))
        {
            crashedIn = dynamic_cast<String^>(values->Lookup("listing"));
            if (crashedIn != nullptr)
                pendingNotice = L"  \u2014  l'app si era chiusa aprendo " + ToWString(crashedIn);
            values->Remove("listing");
            values->Remove("lastPath");
        }
        auto saved = values->HasKey("lastPath") ? values->Lookup("lastPath") : nullptr;
        String^ savedPath = dynamic_cast<String^>(saved);
        if (savedPath != nullptr && Fs::IsDirectory(ToWString(savedPath)))
        {
            currentPath_ = savedPath;
            placeRoot_ = savedPath;
        }
    }
    catch (...)
    {
    }

    FilesList->ItemClick += ref new ItemClickEventHandler(this, &MainPage::OnItemClick);
    FilesList->RightTapped += ref new RightTappedEventHandler(this, &MainPage::OnListRightTapped);
    FilesList->SelectionChanged += ref new SelectionChangedEventHandler(this, &MainPage::OnSelectionChanged);

    GoButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        NavigateTo(PathBox->Text, true);
    });
    UpButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        GoUp();
    });

    const Place places[] =
    {
        { L"Telefono", L"C:\\Data\\Users\\Public" },
        { L"Download", L"C:\\Data\\Users\\Public\\Downloads" },
        { L"Musica", L"C:\\Data\\Users\\Public\\Music" },
        { L"Immagini", L"C:\\Data\\Users\\Public\\Pictures" },
        { L"Documenti", L"C:\\Data\\Users\\Public\\Documents" },
        { L"Radice C:\\", L"C:\\" },
        { L"Dati app", L"C:\\Data\\Users\\DefApps\\AppData" }
    };

    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); ++i)
    {
        String^ label = ref new String(places[i].label);
        String^ path = ref new String(places[i].path);
        Button^ button = MakePlaceButton(label);
        button->Click += ref new RoutedEventHandler([this, path](Object^, RoutedEventArgs^)
        {
            NavigateTo(path, true);
        });
        PlacesPanel->Children->Append(button);
    }

    if (Fs::IsDirectory(L"D:\\"))
    {
        String^ sdPath = ref new String(L"D:\\");
        Button^ sdButton = MakePlaceButton(L"Scheda SD");
        sdButton->Click += ref new RoutedEventHandler([this, sdPath](Object^, RoutedEventArgs^)
        {
            NavigateTo(sdPath, true);
        });
        PlacesPanel->Children->InsertAt(5, sdButton);
    }

    auto appBar = ref new AppBar();
    auto actions = ref new StackPanel();
    actions->Orientation = Orientation::Horizontal;

    auto makeButton = [actions](String^ label, Symbol symbol)
    {
        AppBarButton^ button = ref new AppBarButton();
        button->Label = label;
        button->Icon = ref new SymbolIcon(symbol);
        actions->Children->Append(button);
        return button;
    };

    AppBarButton^ pasteButton = makeButton(L"incolla", Symbol::Copy);
    AppBarButton^ folderButton = makeButton(L"cartella", Symbol::Add);
    AppBarButton^ textButton = makeButton(L"file testo", Symbol::Add);
    AppBarButton^ refreshButton = makeButton(L"aggiorna", Symbol::Refresh);
    AppBarButton^ selectButton = makeButton(L"seleziona", Symbol::List);
    AppBarButton^ copyButton = makeButton(L"copia", Symbol::Copy);
    AppBarButton^ cutButton = makeButton(L"taglia", Symbol::Edit);
    AppBarButton^ deleteButton = makeButton(L"elimina", Symbol::Delete);

    pasteButton->IsEnabled = !clipboardPaths.empty();
    copyButton->IsEnabled = false;
    cutButton->IsEnabled = false;
    deleteButton->IsEnabled = false;

    pasteButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        PasteClipboard();
    });
    folderButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        PromptForName(true);
    });
    textButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        PromptForName(false);
    });
    refreshButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        Refresh();
    });
    selectButton->Click += ref new RoutedEventHandler(
        [this, copyButton, cutButton, deleteButton, selectButton](Object^, RoutedEventArgs^)
    {
        selecting_ = !selecting_;
        suppressSelection_ = true;
        FilesList->SelectedItems->Clear();
        FilesList->SelectionMode = selecting_ ? ListViewSelectionMode::Multiple : ListViewSelectionMode::None;
        FilesList->IsItemClickEnabled = !selecting_;
        suppressSelection_ = false;
        copyButton->IsEnabled = false;
        cutButton->IsEnabled = false;
        deleteButton->IsEnabled = false;
        selectButton->Label = selecting_ ? L"fine" : L"seleziona";
        Refresh();
    });
    copyButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        CopySelection(false);
    });
    cutButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        CopySelection(true);
    });
    deleteButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        std::vector<std::wstring> paths;
        for (unsigned int i = 0; i < FilesList->SelectedItems->Size; ++i)
        {
            FileItem^ item = dynamic_cast<FileItem^>(FilesList->SelectedItems->GetAt(i));
            if (item != nullptr)
                paths.push_back(ToWString(item->FullPath));
        }
        DeletePaths(paths);
    });

    appBar->Content = actions;
    BottomAppBar = appBar;

    PathBox->Text = currentPath_;
    Refresh();
}

void MainPage::NavigateTo(String^ path, bool setBoundary)
{
    std::wstring normalized = NormalizePath(ToWString(path));
    if (normalized.empty())
    {
        SetStatus(L"Inserire un percorso valido");
        return;
    }

    if (!Fs::IsDirectory(normalized))
    {
        SetStatus(ToPlatformString(L"Cartella non accessibile: " + normalized + L" \u2014 " +
            Fs::ErrorText(GetLastError())));
        return;
    }

    currentPath_ = ToPlatformString(normalized);
    if (setBoundary)
        placeRoot_ = currentPath_;
    PathBox->Text = currentPath_;
    Refresh();
}

void MainPage::Refresh()
{
    std::wstring path = ToWString(currentPath_);
    unsigned int generation = ++refreshGeneration;
    SetStatus(L"Carico\u2026");

    // marker: if the process dies while listing, the next start says where and does not reopen it
    try { ApplicationData::Current->LocalSettings->Values->Insert("listing", currentPath_); }
    catch (...) {}

    struct Listing
    {
        std::vector<Fs::Entry> entries;
        std::wstring error;
    };

    // list and sort off the UI thread: C:\Windows has thousands of entries
    create_task([path]()
    {
        auto result = std::make_shared<Listing>();
        try
        {
            result->error = Fs::List(path, result->entries);
            std::vector<std::pair<std::wstring, size_t>> keys;
            keys.reserve(result->entries.size());
            for (size_t i = 0; i < result->entries.size(); ++i)
                keys.push_back(std::make_pair(
                    std::wstring(result->entries[i].isDir ? L"0" : L"1") + Lower(result->entries[i].name), i));
            std::sort(keys.begin(), keys.end());
            std::vector<Fs::Entry> sorted;
            sorted.reserve(keys.size());
            for (size_t i = 0; i < keys.size(); ++i)
                sorted.push_back(result->entries[keys[i].second]);
            result->entries.swap(sorted);
        }
        catch (...)
        {
            result->entries.clear();
            result->error = L"lettura cartella fallita";
        }
        return result;
    }, task_continuation_context::use_arbitrary()).then([this, path, generation](std::shared_ptr<Listing> result)
    {
        if (generation != refreshGeneration)
            return;
        try { ApplicationData::Current->LocalSettings->Values->Remove("listing"); }
        catch (...) {}

        auto items = ref new Platform::Collections::Vector<Object^>();
        if (!result->error.empty())
        {
            FilesList->ItemsSource = items;
            SetStatus(ToPlatformString(L"Errore: " + result->error));
            return;
        }

        const std::vector<Fs::Entry>& entries = result->entries;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const Fs::Entry& entry = entries[i];
            FileItem^ item = ref new FileItem();
            item->Name = ToPlatformString(entry.name);
            item->Glyph = ref new String(entry.isDir ? L"\xE8B7" : L"\xE8A5");
            item->IsDir = entry.isDir;
            item->Size = entry.size;
            item->FullPath = ToPlatformString(JoinPath(path, entry.name));
            item->Modified = ToPlatformString(FormatDate(entry.modified));
            item->Attributes = ToPlatformString(FormatAttributes(entry.attributes));

            std::wstring details;
            if (entry.isDir)
                details = L"Cartella  \u2022  " + FormatDate(entry.modified);
            else
                details = FormatSize(entry.size) + L"  \u2022  " + FormatDate(entry.modified);
            item->Details = ToPlatformString(details);
            items->Append(item);
        }

        FilesList->ItemsSource = items;
        SetStatus(ToPlatformString(std::to_wstring(entries.size()) +
            (entries.size() == 1 ? L" elemento" : L" elementi") + pendingNotice));
        pendingNotice.clear();
    }, task_continuation_context::use_current());
}

void MainPage::GoUp()
{
    std::wstring current = ToWString(currentPath_);
    std::wstring boundary = ToWString(placeRoot_);
    if (SamePath(current, boundary))
    {
        SetStatus(L"Sei nella radice di questo percorso");
        return;
    }

    std::wstring parent = ParentPath(current);
    if (parent.empty() || SamePath(parent, current))
        return;

    currentPath_ = ToPlatformString(parent);
    PathBox->Text = currentPath_;
    Refresh();
}

bool MainPage::HandleHardwareBack()
{
    std::wstring current = ToWString(currentPath_);
    std::wstring boundary = ToWString(placeRoot_);
    if (!SamePath(current, boundary))
    {
        std::wstring parent = ParentPath(current);
        if (!parent.empty() && !SamePath(parent, current))
        {
            currentPath_ = ToPlatformString(parent);
            PathBox->Text = currentPath_;
            Refresh();
            return true;
        }
    }
    return false;
}

void MainPage::SaveView()
{
    try
    {
        ApplicationData::Current->LocalSettings->Values->Insert("lastPath", currentPath_);
    }
    catch (...)
    {
    }
}

void MainPage::OpenItem(FileItem^ item, bool forceText)
{
    if (item == nullptr)
        return;

    if (item->IsDir)
    {
        currentPath_ = item->FullPath;
        PathBox->Text = currentPath_;
        Refresh();
        return;
    }

    String^ path = item->FullPath;
    std::wstring name = ToWString(item->Name);
    if (forceText || IsTextExtension(name))
    {
        Frame->Navigate(EditorPage::typeid, path);
        return;
    }

    SetStatus(L"Apertura del file\u2026");
    create_task(StorageFile::GetFileFromPathAsync(path)).then([this](task<StorageFile^> openTask)
    {
        try
        {
            StorageFile^ storageFile = openTask.get();
            create_task(Launcher::LaunchFileAsync(storageFile)).then([this](task<bool> launchTask)
            {
                try
                {
                    if (!launchTask.get())
                        SetStatus(L"Windows non ha trovato un'app per aprire il file");
                    else
                        SetStatus(L"File aperto");
                }
                catch (Exception^ exception)
                {
                    SetStatus(L"Errore di apertura: " + ExceptionText(exception));
                }
            });
        }
        catch (Exception^ exception)
        {
            SetStatus(L"Errore di apertura: " + ExceptionText(exception));
        }
    });
}

void MainPage::OnItemClick(Object^, ItemClickEventArgs^ e)
{
    FileItem^ item = dynamic_cast<FileItem^>(e->ClickedItem);
    OpenItem(item, false);
}

void MainPage::OnListRightTapped(Object^, RightTappedRoutedEventArgs^ e)
{
    DependencyObject^ current = dynamic_cast<DependencyObject^>(e->OriginalSource);
    FileItem^ item = nullptr;
    while (current != nullptr)
    {
        ListViewItem^ container = dynamic_cast<ListViewItem^>(current);
        if (container != nullptr)
        {
            item = dynamic_cast<FileItem^>(container->Content);
            if (item == nullptr)
                item = dynamic_cast<FileItem^>(container->DataContext);
            break;
        }
        current = VisualTreeHelper::GetParent(current);
    }

    if (item != nullptr)
        ShowContextMenu(item);
}

void MainPage::ShowContextMenu(FileItem^ item)
{
    contextItem_ = item;
    auto flyout = ref new MenuFlyout();

    auto addItem = [this, flyout](String^ text, RoutedEventHandler^ handler)
    {
        auto menuItem = ref new MenuFlyoutItem();
        menuItem->Text = text;
        menuItem->Click += handler;
        flyout->Items->Append(menuItem);
    };

    addItem(L"Apri", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        OpenItem(contextItem_, false);
    }));
    addItem(L"Apri come testo", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        OpenItem(contextItem_, true);
    }));
    addItem(L"Rinomina", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        if (contextItem_ == nullptr)
            return;

        auto dialog = ref new ContentDialog();
        dialog->Title = L"Rinomina";
        dialog->PrimaryButtonText = L"Rinomina";
        dialog->SecondaryButtonText = L"Annulla";
        auto input = ref new TextBox();
        input->Text = contextItem_->Name;
        input->SelectAll();
        dialog->Content = input;
        FileItem^ target = contextItem_;

        create_task(dialog->ShowAsync()).then([this, input, target](ContentDialogResult result)
        {
            if (result != ContentDialogResult::Primary)
                return;

            std::wstring newName = NormalizePath(ToWString(input->Text));
            if (newName.empty() || newName.find(L'\\') != std::wstring::npos)
            {
                SetStatus(L"Nome non valido");
                return;
            }

            std::wstring destination = JoinPath(ToWString(currentPath_), newName);
            std::wstring error = Fs::Move(ToWString(target->FullPath), destination);
            if (!error.empty())
                SetStatus(ToPlatformString(L"Rinomina non riuscita: " + error));
            else
                Refresh();
        });
    }));
    addItem(L"Copia", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        clipboardPaths.clear();
        clipboardPaths.push_back(ToWString(contextItem_->FullPath));
        clipboardCut = false;
        SetStatus(L"Elemento copiato negli appunti dell'app");
    }));
    addItem(L"Taglia", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        clipboardPaths.clear();
        clipboardPaths.push_back(ToWString(contextItem_->FullPath));
        clipboardCut = true;
        SetStatus(L"Elemento pronto per essere spostato");
    }));
    addItem(L"Elimina", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        if (contextItem_ == nullptr)
            return;
        std::vector<std::wstring> paths;
        paths.push_back(ToWString(contextItem_->FullPath));
        DeletePaths(paths);
    }));
    addItem(L"Propriet\u00e0", ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        if (contextItem_ == nullptr)
            return;

        std::wstringstream information;
        information << L"Nome: " << ToWString(contextItem_->Name) << L"\n";
        information << L"Percorso: " << ToWString(contextItem_->FullPath) << L"\n";
        information << L"Tipo: " << (contextItem_->IsDir ? L"Cartella" : L"File") << L"\n";
        if (!contextItem_->IsDir)
            information << L"Dimensione: " << FormatSize(contextItem_->Size) << L"\n";
        information << L"Modificato: " << ToWString(contextItem_->Modified) << L"\n";
        information << L"Attributi: " << ToWString(contextItem_->Attributes);

        auto dialog = ref new ContentDialog();
        dialog->Title = L"Propriet\u00e0";
        dialog->Content = ToPlatformString(information.str());
        dialog->PrimaryButtonText = L"Chiudi";
        create_task(dialog->ShowAsync());
    }));

    flyout->ShowAt(FilesList);
}

void MainPage::CopySelection(bool cut)
{
    std::vector<std::wstring> paths;
    for (unsigned int i = 0; i < FilesList->SelectedItems->Size; ++i)
    {
        FileItem^ item = dynamic_cast<FileItem^>(FilesList->SelectedItems->GetAt(i));
        if (item != nullptr)
            paths.push_back(ToWString(item->FullPath));
    }

    if (paths.empty())
    {
        SetStatus(L"Selezionare almeno un elemento");
        return;
    }

    clipboardPaths = paths;
    clipboardCut = cut;
    SetStatus(cut ? L"Elementi pronti per essere spostati" : L"Elementi copiati negli appunti dell'app");
}

void MainPage::DeletePaths(const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return;

    auto dialog = ref new ContentDialog();
    dialog->Title = L"Eliminazione";
    dialog->Content = paths.size() == 1 ?
        static_cast<Object^>(ToPlatformString(L"Eliminare definitivamente questo elemento?")) :
        static_cast<Object^>(ToPlatformString(L"Eliminare definitivamente " +
            std::to_wstring(paths.size()) + L" elementi?"));
    dialog->PrimaryButtonText = L"Elimina";
    dialog->SecondaryButtonText = L"Annulla";

    create_task(dialog->ShowAsync()).then([this, paths](ContentDialogResult result)
    {
        if (result != ContentDialogResult::Primary)
            return;

        for (size_t i = 0; i < paths.size(); ++i)
        {
            std::wstring error = Fs::Delete(paths[i], true);
            if (!error.empty())
            {
                SetStatus(ToPlatformString(L"Eliminazione non riuscita: " + error));
                Refresh();
                return;
            }
        }
        SetStatus(L"Elementi eliminati");
        Refresh();
    });
}

void MainPage::PasteClipboard()
{
    if (clipboardPaths.empty())
    {
        SetStatus(L"Appunti vuoti");
        return;
    }

    std::wstring destinationFolder = ToWString(currentPath_);
    std::vector<std::wstring> remaining;
    std::wstring firstError;

    for (size_t i = 0; i < clipboardPaths.size(); ++i)
    {
        const std::wstring source = clipboardPaths[i];
        size_t slash = source.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? source : source.substr(slash + 1);
        std::wstring destination = JoinPath(destinationFolder, name);
        std::wstring error = clipboardCut ? Fs::Move(source, destination) : Fs::Copy(source, destination);
        if (!error.empty())
        {
            if (firstError.empty())
                firstError = error;
            remaining.push_back(source);
        }
    }

    if (clipboardCut)
        clipboardPaths = remaining;

    if (!firstError.empty())
        SetStatus(ToPlatformString(L"Incolla non riuscito: " + firstError));
    else
        SetStatus(L"Elementi incollati");

    Refresh();
}

void MainPage::PromptForName(bool createFolder)
{
    auto dialog = ref new ContentDialog();
    dialog->Title = createFolder ? L"Nuova cartella" : L"Nuovo file di testo";
    dialog->PrimaryButtonText = L"Crea";
    dialog->SecondaryButtonText = L"Annulla";

    auto input = ref new TextBox();
    input->Text = createFolder ? L"Nuova cartella" : L"Nuovo file.txt";
    dialog->Content = input;

    create_task(dialog->ShowAsync()).then([this, input, createFolder](ContentDialogResult result)
    {
        if (result != ContentDialogResult::Primary)
            return;

        std::wstring name = NormalizePath(ToWString(input->Text));
        if (name.empty() || name.find(L'\\') != std::wstring::npos)
        {
            SetStatus(L"Nome non valido");
            return;
        }

        std::wstring path = JoinPath(ToWString(currentPath_), name);
        std::wstring error;
        if (createFolder)
        {
            error = Fs::CreateDirectory(path);
        }
        else
        {
            std::vector<unsigned char> empty;
            error = Fs::WriteFile(path, empty);
        }

        if (!error.empty())
            SetStatus(ToPlatformString(L"Creazione non riuscita: " + error));
        else
            Refresh();
    });
}

void MainPage::SetStatus(String^ text)
{
    StatusText->Text = text == nullptr ? L"" : text;
}

void MainPage::PathBox_KeyDown(Object^, KeyRoutedEventArgs^ e)
{
    if (e->Key == VirtualKey::Enter)
    {
        NavigateTo(PathBox->Text, true);
        e->Handled = true;
    }
}

void MainPage::OnSelectionChanged(Object^, SelectionChangedEventArgs^)
{
    if (suppressSelection_)
        return;

    auto appBar = dynamic_cast<AppBar^>(BottomAppBar);
    if (appBar == nullptr)
        return;
    auto actions = dynamic_cast<StackPanel^>(appBar->Content);
    if (actions == nullptr)
        return;

    bool hasSelection = FilesList->SelectedItems->Size != 0;
    for (unsigned int i = 0; i < actions->Children->Size; ++i)
    {
        AppBarButton^ button = dynamic_cast<AppBarButton^>(actions->Children->GetAt(i));
        if (button != nullptr &&
            (button->Label == L"copia" || button->Label == L"taglia" || button->Label == L"elimina"))
            button->IsEnabled = selecting_ && hasSelection;
        if (button != nullptr && button->Label == L"incolla")
            button->IsEnabled = !clipboardPaths.empty();
    }
}
