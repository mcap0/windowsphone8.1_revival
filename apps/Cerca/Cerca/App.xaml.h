#pragma once

#include "App.g.h"

namespace Cerca
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

    private:
        void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
        void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);
        void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
    };
}
