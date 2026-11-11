/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Serial task queue used by CastService. This implementation avoids dedicated threads and
 * BlockingCollection so it can also run in the Windows Phone 8.1 app container.
 */

using System;
using System.Threading;
using System.Threading.Tasks;

namespace Perle.Cast
{
    /// <summary>Runs actions one after another on the default task scheduler.</summary>
    public sealed class SerialWorker : IDisposable
    {
        readonly object gate = new object();
        Task tail = Task.FromResult(0);
        bool shutdown;

        public SerialWorker(string name) { }

        public void Execute(Action action)
        {
            ExecuteAsync(action);
        }

        Task ExecuteAsync(Action action)
        {
            lock (gate)
            {
                if (shutdown) return Task.FromResult(0);
                tail = tail.ContinueWith(previous =>
                {
                    try { action(); }
                    catch (Exception e) { CastLog.E("Unexpected error on the cast worker", e); }
                }, CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
                return tail;
            }
        }

        /// <summary>Runs action on the worker after initialDelay, then delay after each run.</summary>
        public IDisposable ScheduleWithFixedDelay(Action action, TimeSpan initialDelay, TimeSpan delay)
        {
            return new Repeating(this, action, initialDelay, delay);
        }

        public void Shutdown()
        {
            lock (gate) shutdown = true;
        }

        public void Dispose() { Shutdown(); }

        sealed class Repeating : IDisposable
        {
            readonly CancellationTokenSource cancellation = new CancellationTokenSource();

            public Repeating(SerialWorker worker, Action action, TimeSpan initialDelay, TimeSpan delay)
            {
                Task.Run(async () =>
                {
                    try
                    {
                        await Task.Delay(initialDelay, cancellation.Token).ConfigureAwait(false);
                        while (!cancellation.IsCancellationRequested)
                        {
                            await worker.ExecuteAsync(action).ConfigureAwait(false);
                            await Task.Delay(delay, cancellation.Token).ConfigureAwait(false);
                        }
                    }
                    catch (OperationCanceledException) { }
                });
            }

            public void Dispose()
            {
                cancellation.Cancel();
                cancellation.Dispose();
            }
        }
    }
}
