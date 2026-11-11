#pragma once

// The letter on the right edge of a long list (brani, artisti): it follows the scroll and shows the current letter;
// dragged, it runs through the list, with the letter large in the middle. Square, like the rest of the apps.

#include <functional>
#include "../Core/Match.h"

namespace Musica
{
    ref class LetterRail sealed
    {
    internal:
        typedef std::function<Platform::String^(unsigned int)> NameAt;

        LetterRail(Windows::UI::Xaml::Controls::ListView^ list, Windows::UI::Xaml::Controls::Grid^ host, NameAt nameAt)
            : _list(list), _nameAt(nameAt), _dragging(false)
        {
            using namespace Windows::UI;
            using namespace Windows::UI::Xaml;
            using namespace Windows::UI::Xaml::Controls;
            using namespace Windows::UI::Xaml::Input;
            using namespace Windows::UI::Xaml::Media;

            auto accent = dynamic_cast<Brush^>(Application::Current->Resources->Lookup("PhoneAccentBrush"));
            auto white = ref new SolidColorBrush(Colors::White);

            _rail = ref new Grid();
            _rail->Width = 46;
            _rail->HorizontalAlignment = HorizontalAlignment::Right;
            _rail->Background = ref new SolidColorBrush(Colors::Transparent);
            _rail->ManipulationMode = ManipulationModes::TranslateY;  // the Pivot and the list leave this drag alone

            _letter = ref new TextBlock();
            _letter->FontSize = 20;
            _letter->Foreground = white;
            _letter->HorizontalAlignment = HorizontalAlignment::Center;
            _letter->VerticalAlignment = VerticalAlignment::Center;
            _move = ref new TranslateTransform();
            _thumb = ref new Border();
            _thumb->Width = 36;
            _thumb->Height = ThumbSize;
            _thumb->HorizontalAlignment = HorizontalAlignment::Right;
            _thumb->VerticalAlignment = VerticalAlignment::Top;
            _thumb->Background = accent;
            _thumb->Child = _letter;
            _thumb->RenderTransform = _move;
            _thumb->IsHitTestVisible = false;
            _rail->Children->Append(_thumb);

            _bigLetter = ref new TextBlock();
            _bigLetter->FontSize = 64;
            _bigLetter->Foreground = white;
            _bigLetter->HorizontalAlignment = HorizontalAlignment::Center;
            _bigLetter->VerticalAlignment = VerticalAlignment::Center;
            _big = ref new Border();
            _big->Width = 110;
            _big->Height = 110;
            _big->HorizontalAlignment = HorizontalAlignment::Center;
            _big->VerticalAlignment = VerticalAlignment::Center;
            _big->Background = accent;
            _big->Child = _bigLetter;
            _big->IsHitTestVisible = false;
            _big->Visibility = Visibility::Collapsed;

            host->Children->Append(_big);
            host->Children->Append(_rail);

            _rail->PointerPressed += ref new PointerEventHandler(this, &LetterRail::OnPressed);
            _rail->PointerMoved += ref new PointerEventHandler(this, &LetterRail::OnMoved);
            _rail->PointerReleased += ref new PointerEventHandler(this, &LetterRail::OnReleased);
            _rail->PointerCaptureLost += ref new PointerEventHandler(this, &LetterRail::OnReleased);
            _rail->SizeChanged += ref new SizeChangedEventHandler(
                [this](Platform::Object^, SizeChangedEventArgs^) { Refresh(); });
            list->Loaded += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { Refresh(); });
        }

        // the thumb where the list is; the list may have just filled or scrolled
        void Refresh()
        {
            using namespace Windows::UI::Xaml;
            if (_view == nullptr)
            {
                _view = FindView(_list);
                if (_view != nullptr)
                    _view->ViewChanged += ref new Windows::Foundation::EventHandler<
                        Windows::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs^>(
                            [this](Platform::Object^, Windows::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs^) { Refresh(); });
            }
            const unsigned int count = _list->Items->Size;
            const bool show = _view != nullptr && count > 0 && _view->ScrollableHeight > 0;
            _rail->Visibility = show ? Visibility::Visible : Visibility::Collapsed;
            if (!show || _dragging)
                return;
            const double place = _view->VerticalOffset / _view->ScrollableHeight;
            ShowAt(place);
            auto panel = dynamic_cast<Windows::UI::Xaml::Controls::ItemsStackPanel^>(_list->ItemsPanelRoot);
            if (panel != nullptr && panel->FirstVisibleIndex >= 0)
                SetLetter(static_cast<unsigned int>(panel->FirstVisibleIndex));
        }

    private:
        static const int ThumbSize = 44;

        Windows::UI::Xaml::Controls::ListView^ _list;
        Windows::UI::Xaml::Controls::ScrollViewer^ _view;
        Windows::UI::Xaml::Controls::Grid^ _rail;
        Windows::UI::Xaml::Controls::Border^ _thumb;
        Windows::UI::Xaml::Controls::Border^ _big;
        Windows::UI::Xaml::Controls::TextBlock^ _letter;
        Windows::UI::Xaml::Controls::TextBlock^ _bigLetter;
        Windows::UI::Xaml::Media::TranslateTransform^ _move;
        NameAt _nameAt;
        bool _dragging;

        static Windows::UI::Xaml::Controls::ScrollViewer^ FindView(Windows::UI::Xaml::DependencyObject^ root)
        {
            using namespace Windows::UI::Xaml;
            using namespace Windows::UI::Xaml::Media;
            const int count = VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                DependencyObject^ child = VisualTreeHelper::GetChild(root, i);
                auto view = dynamic_cast<Windows::UI::Xaml::Controls::ScrollViewer^>(child);
                if (view == nullptr)
                    view = FindView(child);
                if (view != nullptr)
                    return view;
            }
            return nullptr;
        }

        void ShowAt(double place)
        {
            const double room = _rail->ActualHeight - ThumbSize;
            _move->Y = room > 0 ? place * room : 0;
        }

        void SetLetter(unsigned int index)
        {
            if (index >= _list->Items->Size)
                return;
            Platform::String^ name = _nameAt(index);
            wchar_t letter[2] = { MusicaCore::indexLetter(name == nullptr ? std::wstring() : std::wstring(name->Data())), 0 };
            Platform::String^ text = ref new Platform::String(letter);
            _letter->Text = text;
            _bigLetter->Text = text;
        }

        void Drag(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
        {
            const double room = _rail->ActualHeight - ThumbSize;
            if (room <= 0 || _view == nullptr)
                return;
            double place = (e->GetCurrentPoint(_rail)->Position.Y - ThumbSize / 2.0) / room;
            place = place < 0 ? 0 : place > 1 ? 1 : place;
            ShowAt(place);
            const unsigned int count = _list->Items->Size;
            if (count > 0)
                SetLetter(static_cast<unsigned int>(place * (count - 1)));
            _view->ChangeView(nullptr, ref new Platform::Box<double>(place * _view->ScrollableHeight), nullptr, true);
        }

        void OnPressed(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
        {
            _dragging = _rail->CapturePointer(e->Pointer);
            if (!_dragging)
                return;
            _big->Visibility = Windows::UI::Xaml::Visibility::Visible;
            Drag(e);
            e->Handled = true;
        }

        void OnMoved(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
        {
            if (!_dragging)
                return;
            Drag(e);
            e->Handled = true;
        }

        void OnReleased(Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e)
        {
            if (!_dragging)
                return;
            _dragging = false;
            _rail->ReleasePointerCaptures();
            _big->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
            Refresh();
            e->Handled = true;
        }
    };
}
