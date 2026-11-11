#include "pch.h"
#include "ImagePage.xaml.h"
#include "TgService.h"

#include <windows.storage.h>
#include <windows.storage.streams.h>

using namespace Telegram;
using namespace Platform;
using namespace concurrency;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace
{
    std::wstring SafeKey(const std::string& key)
    {
        std::wstring value = Telegram::Utf16(key);
        for (size_t i = 0; i < value.size(); ++i)
            if (value[i] == L'/' || value[i] == L'\\' || value[i] == L':' || value[i] == L'*' ||
                value[i] == L'?' || value[i] == L'"' || value[i] == L'<' || value[i] == L'>' ||
                value[i] == L'|')
                value[i] = L'_';
        if (value.empty())
            value = L"photo";
        if (value.size() > 80)
            value.resize(80);
        return value;
    }
}

ImagePage::ImagePage()
{
    InitializeComponent();
}

void ImagePage::OnNavigatedTo(NavigationEventArgs^ e)
{
    mediaKey_ = FromP(dynamic_cast<String^>(e->Parameter));
    bytes_.clear();
    LoadingText->Text = L"Caricamento...";
    LoadingText->Visibility = Windows::UI::Xaml::Visibility::Visible;
    TgService::Get().GetMedia(mediaKey_, [this](const std::string& bytes, bool ok)
    {
        if (!ok || bytes.empty())
        {
            LoadingText->Text = L"Impossibile caricare l'immagine";
            return;
        }

        bytes_ = bytes;
        DecodeImage(bytes, [this](Windows::UI::Xaml::Media::Imaging::BitmapImage^ bitmap)
        {
            if (bitmap == nullptr)
            {
                LoadingText->Text = L"Formato immagine non supportato";
                return;
            }
            Photo->Source = bitmap;
            LoadingText->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
        });
    }, [this](int64_t received, int64_t total)
    {
        SetProgress(received, total);
    });
}

void ImagePage::Scroller_SizeChanged(Object^, SizeChangedEventArgs^ e)
{
    // inside a ScrollViewer the image gets infinite space and shows at full size: fit it to the screen
    Photo->Width = e->NewSize.Width;
    Photo->Height = e->NewSize.Height;
}

void ImagePage::SetProgress(int64_t received, int64_t total)
{
    if (total <= 0)
        return;
    int percent = static_cast<int>(received * 100 / total);
    wchar_t text[48];
    swprintf_s(text, 48, L"Caricamento... %d%%", percent);
    LoadingText->Text = ref new String(text);
}

void ImagePage::Save_Click(Object^, RoutedEventArgs^)
{
    if (bytes_.empty())
        return;
    std::wstring path = TgService::Get().SaveToDownloads(bytes_, L"Telegram_" + SafeKey(mediaKey_) + L".jpg");
    auto dialog = ref new Windows::UI::Popups::MessageDialog(path.empty() ? L"Salvataggio non riuscito" :
        L"Salvata in Download\\Telegram", L"Telegram");
    create_task(dialog->ShowAsync());
}
