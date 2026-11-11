#ifndef PERLECAST_TIMEOUT_H
#define PERLECAST_TIMEOUT_H

#include <ppltasks.h>
#include <windows.foundation.h>
#include <windows.system.threading.h>

namespace PerleCast {

// Waits for an async operation, cancelling it after the given seconds (Windows.Web.Http has no timeout).
// A timeout or cancellation becomes a Platform::FailureException like the other network errors.
template <class TResult, class TProgress>
TResult WaitAtMost(Windows::Foundation::IAsyncOperationWithProgress<TResult, TProgress>^ operation, int seconds)
{
    Windows::Foundation::TimeSpan delay;
    delay.Duration = static_cast<long long>(seconds) * 10000000LL;
    Windows::System::Threading::ThreadPoolTimer^ timer = Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        ref new Windows::System::Threading::TimerElapsedHandler(
            [operation](Windows::System::Threading::ThreadPoolTimer^) { operation->Cancel(); }), delay);
    try
    {
        TResult result = concurrency::create_task(operation).get();
        timer->Cancel();
        return result;
    }
    catch (Platform::Exception^)
    {
        timer->Cancel();
        throw;
    }
    catch (...)
    {
        timer->Cancel();
        throw ref new Platform::FailureException(ref new Platform::String(L"tempo scaduto"));
    }
}

} // namespace PerleCast

#endif
