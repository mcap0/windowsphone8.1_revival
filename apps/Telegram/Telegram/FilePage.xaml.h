#pragma once

#include "FilePage.g.h"

namespace Telegram
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class FileEntry sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        property Platform::String^ Name { Platform::String^ get() { return name_; } }
        property Platform::String^ Detail { Platform::String^ get() { return detail_; } }
        property Platform::String^ Glyph
        {
            Platform::String^ get() { return isDir_ ? L"\xD83D\xDCC1" : selected_ ? L"\x2611" : L"\x2610"; }
        }
        property Windows::UI::Xaml::Media::Brush^ Highlight
        {
            Windows::UI::Xaml::Media::Brush^ get()
            {
                return ref new Windows::UI::Xaml::Media::SolidColorBrush(selected_
                    ? Windows::UI::ColorHelper::FromArgb(0x55, 0x22, 0x9E, 0xD9) : Windows::UI::Colors::Transparent);
            }
        }
        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;

    internal:
        FileEntry(Platform::String^ name, Platform::String^ detail, bool isDir, bool selected)
            : name_(name), detail_(detail), isDir_(isDir), selected_(selected) {}
        bool IsDir() const { return isDir_; }
        bool Selected() const { return selected_; }
        void SetSelected(bool on)
        {
            selected_ = on;
            PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(L"Glyph"));
            PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(L"Highlight"));
        }

    private:
        Platform::String^ name_;
        Platform::String^ detail_;
        bool isDir_;
        bool selected_;
    };

    // Browses C:\Data\Users\Public (the rooted phone lets apps read it) and hands the chosen files to ChatPage
    // through TgService::pendingAttachments. Tap a file = select/unselect.
    public ref class FilePage sealed
    {
    public:
        FilePage();
        bool HandleBack();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    internal:
        void Entries_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void SendSelected_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

    private:
        void Show(const std::wstring& dir);
        void UpdateStatus();
        std::wstring dir_;
        std::vector<std::wstring> selected_;   // full paths, kept across folders in tap order
    };
}
