#include "pch.h"
#include "NowPlayingPage.xaml.h"
#include "Services.h"

using namespace Musica;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

NowPlayingPage::NowPlayingPage()
    : _updatingSlider(false),
      _seeking(false),
      _stateHandlerToken(0)
{
    InitializeComponent();
    QueueList->ItemsSource = MusicService::GetQueue();
}

void NowPlayingPage::OnNavigatedTo(NavigationEventArgs^)
{
    QueueList->ItemsSource = MusicService::GetQueue();
    if (_stateHandlerToken == 0)
        _stateHandlerToken = MusicService::AddStateChangedHandler(
            ref new EventHandler<Object^>(this, &NowPlayingPage::OnPlayerStateChanged));
    MusicService::StartPlayerMonitoring();
    Refresh();
}

void NowPlayingPage::OnNavigatedFrom(NavigationEventArgs^)
{
    MusicService::StopPlayerMonitoring();
    if (_stateHandlerToken != 0)
    {
        MusicService::RemoveStateChangedHandler(_stateHandlerToken);
        _stateHandlerToken = 0;
    }
}

void NowPlayingPage::OnPlayerStateChanged(Object^, Object^)
{
    Refresh();
}

void NowPlayingPage::Refresh()
{
    NowTitle->Text = MusicService::GetNowTitle();
    NowArtist->Text = MusicService::GetNowArtist();
    NowAlbum->Text = MusicService::GetNowAlbum();

    MiniTitle->Text = MusicService::GetNowTitle();
    MiniArtist->Text = MusicService::GetNowArtist();
    MiniButton->Text = MusicService::IsPlaying()
        ? ref new Platform::String(L"\u275A\u275A")
        : ref new Platform::String(L"\u25B6");

    PlayButton->Text = MusicService::IsPlaying()
        ? ref new Platform::String(L"\u275A\u275A")
        : ref new Platform::String(L"\u25B6");

    ShuffleButton->Text = MusicService::GetShuffle()
        ? ref new Platform::String(L"\u21C4 casuale: si")
        : ref new Platform::String(L"\u21C4 casuale: no");

    const int repeat = MusicService::GetRepeatMode();
    RepeatButton->Text = repeat == 0
        ? ref new Platform::String(L"\u267B ripeti: no")
        : (repeat == 1
            ? ref new Platform::String(L"\u267B ripeti: tutti")
            : ref new Platform::String(L"\u267B ripeti: brano"));

    const double duration = MusicService::GetDurationSeconds();
    double position = MusicService::GetPositionSeconds();
    if (position < 0.0)
        position = 0.0;

    _updatingSlider = true;
    PositionSlider->Maximum = duration > 0.0 ? duration : 1.0;
    if (duration > 0.0 && position > duration)
        position = duration;
    if (duration <= 0.0)
        position = 0.0;
    if (!_seeking)
        PositionSlider->Value = position;
    _updatingSlider = false;

    PositionText->Text = MusicService::FormatTime(_seeking
        ? PositionSlider->Value : position);
    DurationText->Text = MusicService::FormatTime(duration);
}

void NowPlayingPage::OnPrevious(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::Previous();
}

void NowPlayingPage::OnNext(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::Next();
}

void NowPlayingPage::OnPlayPause(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}

void NowPlayingPage::OnShuffle(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::ToggleShuffle();
    Refresh();
}

void NowPlayingPage::OnRepeat(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::ToggleRepeat();
    Refresh();
}

void NowPlayingPage::OnPositionChanged(
    Object^,
    Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^)
{
    if (_updatingSlider)
        return;

    PositionText->Text = MusicService::FormatTime(PositionSlider->Value);
    if (!_seeking)
        MusicService::Seek(PositionSlider->Value);
}

void NowPlayingPage::OnPositionPointerPressed(Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
    _seeking = true;
}

void NowPlayingPage::OnPositionPointerReleased(Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
    if (!_seeking)
        return;

    _seeking = false;
    MusicService::Seek(PositionSlider->Value);
}

void NowPlayingPage::OnQueueClick(Object^, ItemClickEventArgs^ e)
{
    Song^ song = dynamic_cast<Song^>(e->ClickedItem);
    if (song == nullptr)
        return;

    Windows::Foundation::Collections::IVector<Song^>^ songs = MusicService::GetQueue();
    for (unsigned int i = 0; i < songs->Size; ++i)
        if (songs->GetAt(i) == song)
        {
            MusicService::PlaySongs(songs, i, false);
            return;
        }
}

void NowPlayingPage::OnMiniTapped(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
}

void NowPlayingPage::OnPlayClick(Object^, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}
