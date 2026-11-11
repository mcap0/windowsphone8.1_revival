#include "pch.h"
#include "Voip.h"

#include <roapi.h>
#include <eventtoken.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

namespace
{
    struct TimeSpanAbi { long long Duration; };

    MIDL_INTERFACE("716654be-28e8-495e-b657-8053074b4150")
    IVoipPhoneCall : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE add_EndRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_EndRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_HoldRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_HoldRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_ResumeRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_ResumeRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_AnswerRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_AnswerRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_RejectRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_RejectRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE NotifyCallHeld() = 0;
        virtual HRESULT STDMETHODCALLTYPE NotifyCallActive() = 0;
        virtual HRESULT STDMETHODCALLTYPE NotifyCallEnded() = 0;
    };

    MIDL_INTERFACE("0d5f3579-f6dd-4e10-9f25-3d8c91376ee8")
    IVoipCallCoordinator : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE add_MuteRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_MuteRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE add_UnmuteRequested(IUnknown* handler, ::EventRegistrationToken* token) = 0;
        virtual HRESULT STDMETHODCALLTYPE remove_UnmuteRequested(::EventRegistrationToken token) = 0;
        virtual HRESULT STDMETHODCALLTYPE RequestNewIncomingCall(HSTRING context, HSTRING contactName, HSTRING contactNumber,
            IUnknown* contactImage, HSTRING serviceName, IUnknown* brandingImage, HSTRING callDetails, IUnknown* ringtone,
            unsigned int media, TimeSpanAbi ringTimeout, IVoipPhoneCall** call) = 0;
    };

    MIDL_INTERFACE("10fb55bb-e07d-407e-bc39-f7cc3641d979")
    IVoipCallCoordinatorStatics : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE GetDefault(IVoipCallCoordinator** coordinator) = 0;
    };

    MIDL_INTERFACE("3e04061a-ce7d-49c2-a2b8-7f49b6eaebc5")
    ICallRejectEventArgs : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE get_RejectReason(int* value) = 0;
    };

    // TypedEventHandler<VoipPhoneCall, X>: the three instantiations' IIDs (pinterface GUIDs from the SDK header)
    const IID IID_AnswerHandler = { 0xd31bfb3a, 0x4430, 0x5d06, { 0x80, 0xc8, 0x7b, 0x87, 0xbb, 0xa6, 0xda, 0xc1 } };
    const IID IID_RejectHandler = { 0x141863d2, 0xc712, 0x5abe, { 0xbf, 0x93, 0x36, 0x65, 0x5c, 0x93, 0x5e, 0x63 } };
    const IID IID_StateHandler = { 0x80e8f32d, 0x0b5b, 0x568e, { 0x88, 0x61, 0x25, 0x01, 0x83, 0xb2, 0x7e, 0x8e } };

    // One delegate object; QueryInterface answers its own handler IID (and IUnknown / IAgileObject).
    class Handler : public IUnknown
    {
    public:
        Handler(const IID& iid, std::function<void(IInspectable*)> fn) : refs_(1), iid_(iid), fn_(fn) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** out) override
        {
            if (riid == IID_IUnknown || riid == iid_ || riid == __uuidof(IAgileObject))
            {
                *out = this;
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&refs_);
            if (r == 0) delete this;
            return r;
        }
        // vtable slot 3 of every delegate: Invoke(sender, args)
        virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable* args)
        {
            if (fn_) fn_(args);
            return S_OK;
        }

    private:
        volatile ULONG refs_;
        IID iid_;
        std::function<void(IInspectable*)> fn_;
    };

    std::wstring Hex(const wchar_t* what, HRESULT hr)
    {
        wchar_t b[96];
        swprintf_s(b, L"%s 0x%08X", what, static_cast<unsigned>(hr));
        return b;
    }
}

namespace Voip
{
    class Call
    {
    public:
        ComPtr<IVoipPhoneCall> call;
        Events events;
    };

    Call* RequestIncoming(const std::wstring& name, const std::wstring& number, const std::wstring& service,
                          const Options& o, const Events& events, std::wstring& err)
    {
        ComPtr<IVoipCallCoordinatorStatics> statics;
        HRESULT hr = RoGetActivationFactory(HStringReference(L"Windows.Phone.Networking.Voip.VoipCallCoordinator").Get(),
                                            __uuidof(IVoipCallCoordinatorStatics), &statics);
        if (FAILED(hr)) { err = Hex(L"RoGetActivationFactory", hr); return nullptr; }
        ComPtr<IVoipCallCoordinator> coord;
        hr = statics->GetDefault(&coord);
        if (FAILED(hr)) { err = Hex(L"GetDefault", hr); return nullptr; }

        Windows::Foundation::Uri^ image = o.image != nullptr ? ref new Windows::Foundation::Uri(o.image) : nullptr;
        Windows::Foundation::Uri^ ring = o.ringtone != nullptr ? ref new Windows::Foundation::Uri(o.ringtone) : nullptr;
        TimeSpanAbi timeout = { o.ringSeconds * 10000000LL };
        Call* c = new Call();
        c->events = events;
        hr = coord->RequestNewIncomingCall(HStringReference(o.context.c_str()).Get(), HStringReference(name.c_str()).Get(),
                                           HStringReference(number.c_str()).Get(), reinterpret_cast<IUnknown*>(image),
                                           HStringReference(service.c_str()).Get(), reinterpret_cast<IUnknown*>(image),
                                           HStringReference(o.details.c_str()).Get(), reinterpret_cast<IUnknown*>(ring),
                                           1 /* Audio */, timeout, &c->call);
        if (FAILED(hr)) { err = Hex(L"RequestNewIncomingCall", hr); delete c; return nullptr; }

        ::EventRegistrationToken t;
        Handler* h = new Handler(IID_AnswerHandler, [c](IInspectable*) { if (c->events.answered) c->events.answered(); });
        hr = c->call->add_AnswerRequested(h, &t); h->Release();
        if (FAILED(hr)) err += Hex(L" add_AnswerRequested", hr);
        h = new Handler(IID_RejectHandler, [c](IInspectable* args)
        {
            int reason = -1;
            ComPtr<ICallRejectEventArgs> a;
            if (args && SUCCEEDED(args->QueryInterface(__uuidof(ICallRejectEventArgs), &a))) a->get_RejectReason(&reason);
            if (c->events.rejected) c->events.rejected(reason);
        });
        hr = c->call->add_RejectRequested(h, &t); h->Release();
        if (FAILED(hr)) err += Hex(L" add_RejectRequested", hr);
        h = new Handler(IID_StateHandler, [c](IInspectable*) { if (c->events.endRequested) c->events.endRequested(); });
        hr = c->call->add_EndRequested(h, &t); h->Release();
        if (FAILED(hr)) err += Hex(L" add_EndRequested", hr);
        h = new Handler(IID_StateHandler, [c](IInspectable*) { if (c->events.holdRequested) c->events.holdRequested(); });
        c->call->add_HoldRequested(h, &t); h->Release();
        h = new Handler(IID_StateHandler, [c](IInspectable*) { if (c->events.resumeRequested) c->events.resumeRequested(); });
        c->call->add_ResumeRequested(h, &t); h->Release();
        return c;
    }

    HRESULT NotifyActive(Call* call) { return call ? call->call->NotifyCallActive() : E_POINTER; }

    HRESULT NotifyEnded(Call* call)
    {
        if (!call) return E_POINTER;
        HRESULT hr = call->call->NotifyCallEnded();
        return hr;   // the Call stays alive (its handlers may still fire); a probe leaks it on purpose
    }
}
