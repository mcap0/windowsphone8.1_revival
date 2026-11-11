using System;
using System.Collections.Generic;

namespace Mappe.Core
{
    /// <summary>Thread-safe least-recently-used cache.</summary>
    public sealed class LruCache<TKey, TValue>
    {
        private readonly int _capacity;
        private readonly Dictionary<TKey, LinkedListNode<KeyValuePair<TKey, TValue>>> _map =
            new Dictionary<TKey, LinkedListNode<KeyValuePair<TKey, TValue>>>();
        private readonly LinkedList<KeyValuePair<TKey, TValue>> _order = new LinkedList<KeyValuePair<TKey, TValue>>();
        private readonly object _lock = new object();

        public LruCache(int capacity)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            _capacity = capacity;
        }

        public int Capacity { get { return _capacity; } }

        public int Count
        {
            get { lock (_lock) { return _map.Count; } }
        }

        public bool TryGet(TKey key, out TValue value)
        {
            lock (_lock)
            {
                LinkedListNode<KeyValuePair<TKey, TValue>> node;
                if (_map.TryGetValue(key, out node))
                {
                    _order.Remove(node);
                    _order.AddFirst(node);
                    value = node.Value.Value;
                    return true;
                }
                value = default(TValue);
                return false;
            }
        }

        public void Put(TKey key, TValue value)
        {
            lock (_lock)
            {
                LinkedListNode<KeyValuePair<TKey, TValue>> node;
                if (_map.TryGetValue(key, out node))
                {
                    _order.Remove(node);
                    _map.Remove(key);
                }
                node = _order.AddFirst(new KeyValuePair<TKey, TValue>(key, value));
                _map[key] = node;
                while (_map.Count > _capacity)
                {
                    LinkedListNode<KeyValuePair<TKey, TValue>> last = _order.Last;
                    _order.RemoveLast();
                    _map.Remove(last.Value.Key);
                }
            }
        }

        public bool Remove(TKey key)
        {
            lock (_lock)
            {
                LinkedListNode<KeyValuePair<TKey, TValue>> node;
                if (!_map.TryGetValue(key, out node)) return false;
                _order.Remove(node);
                _map.Remove(key);
                return true;
            }
        }

        public void Clear()
        {
            lock (_lock)
            {
                _map.Clear();
                _order.Clear();
            }
        }
    }
}
