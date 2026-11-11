#pragma once

#include "MainPage.g.h"
#include "Game.h"

namespace Serpente
{
    public ref class MainPage sealed
    {
    public:
        MainPage();
        void Pause();
        bool HandleBack();

    private:
        enum State { Ready, Playing, Paused, Over };

        void OnSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        void OnTick(Platform::Object^ sender, Platform::Object^ e);
        void OnTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnManipulationDelta(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e);
        void OnManipulationCompleted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^ e);
        void Build(double width, double height);
        void Start();
        void Resume();
        void Draw();
        void ShowScore();
        void SetState(State s);
        Windows::UI::Xaml::Shapes::Rectangle^ Square(Windows::UI::Xaml::Media::Brush^ brush);

        std::unique_ptr<Game> game_;
        State state_;
        double cell_;
        int best_;
        bool swipeTaken_;
        Windows::UI::Xaml::DispatcherTimer^ timer_;
        Platform::Collections::Vector<Windows::UI::Xaml::Shapes::Rectangle^>^ parts_;
        Windows::UI::Xaml::Shapes::Rectangle^ food_;
        Windows::UI::Xaml::Media::SolidColorBrush^ bodyBrush_;
        Windows::UI::Xaml::Media::SolidColorBrush^ headBrush_;
        Windows::System::Display::DisplayRequest^ awake_;
    };
}
