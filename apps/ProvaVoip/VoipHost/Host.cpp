// ProvaVoip.VoipHost.exe: the out-of-process server a Silverlight 8.1 VoIP app needs (PLAN §69.8).
//
// The phone runs a Silverlight 8.1 app's VoIP agents (the VoipHttpIncomingCallTask agent woken by a raw push, keep-alive,
// call in progress, foreground lifetime) only through the package's "AgHost.BackgroundVoipTask" background task
// (PhoneBrokerClient.dll: agent types 8-10 -> that entry point), and a controlChannel task must name an out-of-process
// server (ServerName, APPX1684): this process. It only registers as a WinRT server, as Telegram WP's
// VoipBackendServerHost does; the system activates AgHost.BackgroundVoipTask (AgHostSvcs.dll) inside it, and that hosts
// the managed agents. We serve no class of our own: the manifest's placeholder class is never asked for.
//
// No C runtime and no C++/CX: a Silverlight package does not depend on the VCLibs framework package, so an exe that
// imports msvcr120_app.dll / vccorlib120_app.dll never starts (1.5.0.0: VoipBackgroundProcess.Launch timed out,
// 0x80070102). Entry point HostMain, only api-ms-win-* imports; no globals with constructors, no new, no exceptions.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.applicationmodel.core.h>

using ABI::Windows::ApplicationModel::Core::ICoreApplication;
using ABI::Windows::Foundation::IGetActivationFactory;

namespace
{
    bool SameIid(REFIID a, REFIID b)
    {
        const DWORD* x = reinterpret_cast<const DWORD*>(&a);
        const DWORD* y = reinterpret_cast<const DWORD*>(&b);
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
    }

    // Lives on HostMain's stack for the whole life of the server (RunWithActivationFactories blocks): no refcount.
    class NoFactories : public IGetActivationFactory
    {
    public:
        IFACEMETHODIMP QueryInterface(REFIID riid, void** object)
        {
            if (SameIid(riid, __uuidof(IUnknown)) || SameIid(riid, __uuidof(IInspectable)) ||
                SameIid(riid, __uuidof(IGetActivationFactory)) || SameIid(riid, __uuidof(IAgileObject)))
            {
                *object = static_cast<IGetActivationFactory*>(this);
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }
        IFACEMETHODIMP_(ULONG) AddRef() { return 2; }
        IFACEMETHODIMP_(ULONG) Release() { return 1; }
        IFACEMETHODIMP GetIids(ULONG* count, IID** iids) { *count = 0; *iids = nullptr; return S_OK; }
        IFACEMETHODIMP GetRuntimeClassName(HSTRING* name) { *name = nullptr; return S_OK; }
        IFACEMETHODIMP GetTrustLevel(TrustLevel* level) { *level = BaseTrust; return S_OK; }
        IFACEMETHODIMP GetActivationFactory(HSTRING, IInspectable** factory)
        {
            *factory = nullptr;
            return CLASS_E_CLASSNOTAVAILABLE;
        }
    };
}

extern "C" int __stdcall HostMain()
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    {
        NoFactories factories;
        // In the SDK this is a const WCHAR array (selectany), static data: no initializer runs.
        const auto& className = RuntimeClass_Windows_ApplicationModel_Core_CoreApplication;
        HSTRING_HEADER header;
        HSTRING name;
        ICoreApplication* app = nullptr;
        hr = WindowsCreateStringReference(className, ARRAYSIZE(className) - 1, &header, &name);
        if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, __uuidof(ICoreApplication), reinterpret_cast<void**>(&app));
        if (SUCCEEDED(hr))
        {
            hr = app->RunWithActivationFactories(&factories);
            app->Release();
        }
    }
    RoUninitialize();
    return SUCCEEDED(hr) ? 0 : 2;
}
