using System;

namespace Mappe.Core
{
    /// <summary>
    /// Decides when a search-as-you-type query should fire. Time is passed in (milliseconds)
    /// so the logic can be tested without a timer.
    /// </summary>
    public sealed class SearchDebouncer
    {
        private readonly int _delayMs;
        private readonly int _minChars;
        private string _pending;
        private long _dueAt;
        private string _lastFired;

        public SearchDebouncer(int delayMs, int minChars)
        {
            _delayMs = delayMs;
            _minChars = minChars;
        }

        public bool HasPending { get { return _pending != null; } }

        /// <summary>Registers a text change at time nowMs; restarts the delay.</summary>
        public void Touch(string text, long nowMs)
        {
            _pending = (text ?? string.Empty).Trim();
            _dueAt = nowMs + _delayMs;
        }

        /// <summary>Returns true (and the query) once the delay has elapsed and the query is new and long enough.</summary>
        public bool TryFire(long nowMs, out string query)
        {
            query = null;
            if (_pending == null || nowMs < _dueAt) return false;
            string text = _pending;
            _pending = null;
            if (text.Length < _minChars || text == _lastFired) return false;
            _lastFired = text;
            query = text;
            return true;
        }

        /// <summary>Forgets any pending query (for example after Enter was pressed).</summary>
        public void Cancel()
        {
            _pending = null;
        }

        /// <summary>Forces the next identical query to fire again.</summary>
        public void ResetLast()
        {
            _lastFired = null;
        }
    }
}
