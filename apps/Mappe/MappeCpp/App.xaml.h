//
// App.xaml.h
// Declaration of the App class.
//

#pragma once

#include "App.g.h"

namespace MappeCpp {

ref class App sealed {
public:
    App();

    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

private:
    void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
    void OnUnhandled(Platform::Object^ sender, Windows::UI::Xaml::UnhandledExceptionEventArgs^ e);
};

}
