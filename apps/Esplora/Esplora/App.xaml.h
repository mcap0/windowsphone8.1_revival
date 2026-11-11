#pragma once
#include "App.g.h"
namespace Esplora
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
        virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;
        virtual void OnFileActivated(Windows::ApplicationModel::Activation::FileActivatedEventArgs^ e) override;
    private:
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
    };
}
