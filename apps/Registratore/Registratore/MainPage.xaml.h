#pragma once

#include "MainPage.g.h"

namespace Registratore
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class RecItem sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;
        property Platform::String^ Title { Platform::String^ get() { return title_; } }
        property Platform::String^ Detail { Platform::String^ get() { return detail_; } }
        property Platform::String^ Glyph { Platform::String^ get() { return playing_ ? L"\u25a0" : L"\u25b6"; } }

    internal:
        RecItem(Windows::Storage::StorageFile^ f) : file(f), playing_(false) {}
        Windows::Storage::StorageFile^ file;
        void SetText(Platform::String^ t, Platform::String^ d) { title_ = t; detail_ = d; Raise(L"Title"); Raise(L"Detail"); }
        void SetPlaying(bool p) { playing_ = p; Raise(L"Glyph"); }
        bool Playing() const { return playing_; }

    private:
        void Raise(Platform::String^ n) { PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(n)); }
        Platform::String^ title_;
        Platform::String^ detail_;
        bool playing_;
    };

    [Windows::UI::Xaml::Data::Bindable]
    public ref class MicItem sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;
        property Platform::String^ Name { Platform::String^ get() { return name_; } }
        property Platform::String^ Detail { Platform::String^ get() { return detail_; } }

    internal:
        MicItem(Platform::String^ id, Platform::String^ name) : id_(id), name_(name), detail_(L"non ancora provato") {}
        Platform::String^ Id() { return id_; }
        void SetDetail(Platform::String^ d) { detail_ = d; PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(L"Detail")); }

    private:
        Platform::String^ id_;
        Platform::String^ name_;
        Platform::String^ detail_;
    };

    public ref class MainPage sealed
    {
    public:
        MainPage();
        bool HandleBack();
        void StopForSuspend();

    internal:
        void Record_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Delete_Tapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void Player_Opened(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Mics_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Mic_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void Mic_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void Player_State(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void List_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void Item_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void Player_Ended(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Player_Failed(Platform::Object^ sender, Windows::UI::Xaml::ExceptionRoutedEventArgs^ e);

    private:
        void LoadList();
        void Describe(RecItem^ item);
        void StartRecording();
        void TryProfile(int index);
        void StopRecording();
        void SetRecordingUi(bool on);
        void StopPlayback();
        void Log(Platform::String^ line);
        void Delete(RecItem^ item);
        void SaveToMusic(RecItem^ item);
        void Analyze(RecItem^ item);
        Platform::String^ ChosenDevice(Windows::Devices::Enumeration::DeviceInformationCollection^ all);
        Platform::String^ SavedMicId();
        Platform::String^ SavedMicName();
        void TestMic(MicItem^ item);

        Platform::Collections::Vector<RecItem^>^ items_;
        Platform::Collections::Vector<MicItem^>^ mics_;
        Windows::Media::Capture::MediaCapture^ capture_;
        Windows::Storage::StorageFolder^ folder_;
        Windows::Storage::StorageFile^ recFile_;
        Windows::UI::Xaml::DispatcherTimer^ timer_;
        unsigned long long startTick_;
        bool recording_;
        bool busy_;
        RecItem^ playing_;
    };
}
