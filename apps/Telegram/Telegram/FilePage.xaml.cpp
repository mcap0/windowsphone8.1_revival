#include "pch.h"
#include "FilePage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    const wchar_t* kRoot = L"C:\\Data\\Users\\Public";

    std::wstring SizeText(unsigned long long n)
    {
        wchar_t b[32];
        if (n >= 1024 * 1024) swprintf_s(b, L"%.1f MB", n / (1024.0 * 1024.0));
        else swprintf_s(b, L"%llu KB", (n + 1023) / 1024);
        return b;
    }
}

FilePage::FilePage()
{
    InitializeComponent();
}

void FilePage::OnNavigatedTo(NavigationEventArgs^)
{
    selected_.clear();
    Show(kRoot);
}

void FilePage::Show(const std::wstring& dir)
{
    dir_ = dir;
    PathText->Text = ref new String(dir.c_str());
    std::vector<std::pair<std::wstring, std::pair<bool, unsigned long long> > > dirs, files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            (isDir ? dirs : files).push_back(std::make_pair(n, std::make_pair(isDir, size)));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto byName = [](const std::pair<std::wstring, std::pair<bool, unsigned long long> >& a,
                     const std::pair<std::wstring, std::pair<bool, unsigned long long> >& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    auto items = ref new Platform::Collections::Vector<FileEntry^>();
    for (size_t i = 0; i < dirs.size(); ++i)
        items->Append(ref new FileEntry(ref new String(dirs[i].first.c_str()), L"cartella", true, false));
    for (size_t i = 0; i < files.size(); ++i)
    {
        std::wstring full = dir + L"\\" + files[i].first;
        bool on = std::find(selected_.begin(), selected_.end(), full) != selected_.end();
        items->Append(ref new FileEntry(ref new String(files[i].first.c_str()),
                                        ref new String(SizeText(files[i].second.second).c_str()), false, on));
    }
    Entries->ItemsSource = items;
    UpdateStatus();
}

void FilePage::Entries_ItemClick(Object^, ItemClickEventArgs^ e)
{
    auto entry = dynamic_cast<FileEntry^>(e->ClickedItem);
    if (entry == nullptr) return;
    std::wstring path = dir_ + L"\\" + entry->Name->Data();
    if (entry->IsDir()) { Show(path); return; }
    auto it = std::find(selected_.begin(), selected_.end(), path);
    bool on = it == selected_.end();
    if (on) selected_.push_back(path);
    else selected_.erase(it);
    entry->SetSelected(on);
    UpdateStatus();
}

void FilePage::UpdateStatus()
{
    wchar_t text[64];
    if (selected_.empty()) swprintf_s(text, L"tocca i file da allegare");
    else swprintf_s(text, L"%u file selezionati", static_cast<unsigned int>(selected_.size()));
    SelectionStatus->Text = ref new String(text);
}

void FilePage::SendSelected_Click(Object^, RoutedEventArgs^)
{
    if (selected_.empty()) return;
    std::vector<std::wstring> paths;
    paths.swap(selected_);
    TgService::Get().pendingAttachments.swap(paths);
    if (Frame->CanGoBack) Frame->GoBack();
}

bool FilePage::HandleBack()
{
    if (_wcsicmp(dir_.c_str(), kRoot) == 0) return false;
    Show(dir_.substr(0, dir_.find_last_of(L'\\')));
    return true;
}
