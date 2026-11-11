#pragma once
#include "App.g.h"
namespace Orologio
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
        virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;
    private:
        void Show(Platform::String^ args);
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
    };
}
