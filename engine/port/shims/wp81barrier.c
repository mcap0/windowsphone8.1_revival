// SQLite calls MSVC's _ReadWriteBarrier(), which clang provides as an intrinsic only on x86: on ARM it becomes a call.
// A compiler barrier, which is what the intrinsic is (no CPU fence).
void _ReadWriteBarrier(void)
{
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
