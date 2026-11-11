#pragma once

#include "App.g.h"

namespace ProvaChiamate
{
    ref class App sealed
    {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
    };
}
