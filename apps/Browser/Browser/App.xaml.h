#pragma once

#include "App.g.h"

namespace Browser
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
        // Back from the photo picker of a page's <input type=file> (PickFileContinuation).
        virtual void OnActivated(Windows::ApplicationModel::Activation::IActivatedEventArgs^ e) override;
    };
}
