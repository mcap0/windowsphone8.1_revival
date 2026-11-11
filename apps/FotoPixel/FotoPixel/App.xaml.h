#pragma once
#include "App.g.h"
namespace FotoPixel
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
    private:
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
    };
}
