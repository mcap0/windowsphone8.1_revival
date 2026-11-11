// Force-included in every Kaldi / OpenFst / vosk-api file of the WP8.1 build (vosk/CMakeLists.txt).
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef intptr_t ssize_t;

#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <type_traits>

// No C++ exceptions on Windows ARM32 with LLVM (vosk/noexcept.py): what was thrown is logged, then the process
// aborts. Vosk's C API used to catch them and return NULL; on the phone a bad model or input now ends the app,
// with the reason in LocalState\vosk-fatal.txt (written by the handler the app installs, see wp81::SetFatalHandler).
namespace wp81 {
typedef void (*FatalHandler)(const char* what);
FatalHandler& Handler();
[[noreturn]] void Die(const char* what);

// anything derived from std::exception (KaldiFatalError...) gives its message; the template alone matched them
// better than the base-class overload and printed "exception"
inline const char* What(const std::string& s) { return s.c_str(); }
inline const char* What(const char* s) { return s; }
template <class T, class = void> struct HasKaldiMessage : std::false_type {};
template <class T> struct HasKaldiMessage<T, decltype((void)std::declval<const T&>().KaldiMessage())> : std::true_type {};
template <class T> const char* What(const T& x) {
    if constexpr (HasKaldiMessage<T>::value) return x.KaldiMessage();  // KaldiFatalError: what() is only its name
    else if constexpr (std::is_base_of<std::exception, T>::value) return x.what();
    else return "exception (not std::exception)";
}

template <class T> [[noreturn]] void fatal(const T& x) { Die(What(x)); }
[[noreturn]] inline void fatal() { Die("rethrow"); }

// the value of a dead catch branch: converts to any reference, never evaluated
struct Never {
    template <class T> operator T&() const { return *static_cast<T*>(nullptr); }
};
inline Never never() { return Never(); }
}  // namespace wp81
#endif
