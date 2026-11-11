// Win32 functions WebKit's code calls but WP8.1 doesn't export, provided at link level as the import slots
// (__imp_X) the callers expect. Only add a function here when the phone really lacks it (tools/checkimports.py).
#include <windows.h>

// WTF soft-linking and mimalloc load system DLLs by name; the phone only has LoadLibraryExW.
static HMODULE WINAPI wp81LoadLibraryW(LPCWSTR name)
{
    return LoadLibraryExW(name, NULL, 0);
}

static HMODULE WINAPI wp81LoadLibraryA(LPCSTR name)
{
    WCHAR wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH))
        return NULL;
    return LoadLibraryExW(wide, NULL, 0);
}

// mimalloc asks for SeLockMemoryPrivilege before using large OS pages: not available to phone apps.
static BOOL WINAPI wp81LookupPrivilegeValueW(LPCWSTR system, LPCWSTR name, PLUID luid)
{
    (void)system; (void)name; (void)luid;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

// std::filesystem::copy_file in the MSVC STL calls CopyFileW; the phone has CopyFile2.
static BOOL WINAPI wp81CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    COPYFILE2_EXTENDED_PARAMETERS parameters = { sizeof(parameters), failIfExists ? COPY_FILE_FAIL_IF_EXISTS : 0 };
    return SUCCEEDED(CopyFile2(from, to, &parameters));
}

void* __imp_CopyFileW = (void*)wp81CopyFileW;
void* __imp_LoadLibraryW = (void*)wp81LoadLibraryW;
void* __imp_LoadLibraryA = (void*)wp81LoadLibraryA;
void* __imp_LookupPrivilegeValueW = (void*)wp81LookupPrivilegeValueW;

// ---- kernel32 functions the phone has under another name ----

// Skia's SkSemaphore.
static HANDLE WINAPI wp81CreateSemaphoreW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name)
{
    return CreateSemaphoreExW(attributes, initial, maximum, name, 0, SEMAPHORE_ALL_ACCESS);
}

// HarfBuzz maps font files it is given by path (we hand it DirectWrite fonts instead).
static HANDLE WINAPI wp81CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD sizeHigh,
    DWORD sizeLow, LPCSTR name)
{
    if (name) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    return CreateFileMappingFromApp(file, attributes, protect, ((ULONG64)sizeHigh << 32) | sizeLow, NULL);
}

// LibreSSL's and curl's rename().
static BOOL WINAPI wp81MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    WCHAR wideFrom[MAX_PATH], wideTo[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, from, -1, wideFrom, MAX_PATH) || !MultiByteToWideChar(CP_ACP, 0, to, -1, wideTo, MAX_PATH))
        return FALSE;
    return MoveFileExW(wideFrom, wideTo, flags);
}

// curl checks the Windows version; ntdll has the same check.
NTSYSAPI LONG NTAPI RtlVerifyVersionInfo(PRTL_OSVERSIONINFOEXW, ULONG, ULONGLONG);
static BOOL WINAPI wp81VerifyVersionInfoW(LPOSVERSIONINFOEXW info, DWORD typeMask, DWORDLONG conditionMask)
{
    LONG status = RtlVerifyVersionInfo((PRTL_OSVERSIONINFOEXW)info, typeMask, conditionMask);
    if (status) {
        SetLastError(ERROR_OLD_WIN_VERSION);
        return FALSE;
    }
    return TRUE;
}

// No thread-pool waits for phone apps: WebCore's network-state watcher stays on its default (online). libxml2 no
// longer needs them (LIBXML_STATIC_FOR_DLL, see deps/build.sh).
static BOOL WINAPI wp81RegisterWaitForSingleObject(PHANDLE wait, HANDLE object, WAITORTIMERCALLBACK callback, PVOID context,
    ULONG milliseconds, ULONG flags)
{
    (void)object; (void)callback; (void)context; (void)milliseconds; (void)flags;
    *wait = NULL;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL WINAPI wp81UnregisterWait(HANDLE wait)
{
    (void)wait;
    return TRUE;
}

// ---- desktop features the phone doesn't have: they report "not available", as the real calls do on failure ----
// Clipboard (PasteboardWin), drag and drop (DragDataWin, ClipboardUtilitiesWin), cursors and icons (CursorWin,
// IconWin), GDI drawing into HDCs (GraphicsContextWin, Pasteboard images), MSAA notifications (AXObjectCacheWin),
// keyboard state, registry MIME types (MIMETypeRegistryWin keeps its built-in table), the system beep.
// (The clipboard itself is further down: the engine has its own.)

static BOOL WINAPI wp81False1(void* a) { (void)a; return FALSE; }
static BOOL WINAPI wp81True1(void* a) { (void)a; return TRUE; }
static void* WINAPI wp81Null1(void* a) { (void)a; return NULL; }
static void* WINAPI wp81Null2(void* a, void* b) { (void)a; (void)b; return NULL; }
static int WINAPI wp81Zero1(void* a) { (void)a; return 0; }
static int WINAPI wp81Zero2(void* a, void* b) { (void)a; (void)b; return 0; }
static int WINAPI wp81Zero3(void* a, void* b, void* c) { (void)a; (void)b; (void)c; return 0; }
static void WINAPI wp81Nothing1(void* a) { (void)a; }
static void WINAPI wp81Nothing4(void* a, void* b, void* c, void* d) { (void)a; (void)b; (void)c; (void)d; }

static HWND WINAPI wp81CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x, int y,
    int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter)
{
    (void)exStyle; (void)className; (void)windowName; (void)style; (void)x; (void)y; (void)width; (void)height;
    (void)parent; (void)menu; (void)instance; (void)parameter;
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

static LRESULT WINAPI wp81DefWindowProcW(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    (void)window; (void)message; (void)wParam; (void)lParam;
    return 0;
}

static HANDLE WINAPI wp81CreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset)
{
    (void)dc; (void)info; (void)usage; (void)section; (void)offset;
    if (bits)
        *bits = NULL;
    return NULL;
}

static BOOL WINAPI wp81BitBlt(HDC dc, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD rop)
{
    (void)dc; (void)x; (void)y; (void)width; (void)height; (void)source; (void)sourceX; (void)sourceY; (void)rop;
    return FALSE;
}

static BOOL WINAPI wp81DrawIconEx(HDC dc, int x, int y, HICON icon, int width, int height, UINT step, HBRUSH brush, UINT flags)
{
    (void)dc; (void)x; (void)y; (void)icon; (void)width; (void)height; (void)step; (void)brush; (void)flags;
    return FALSE;
}

static HBITMAP WINAPI wp81CreateBitmap(int width, int height, UINT planes, UINT bitsPerPixel, const void* bits)
{
    (void)width; (void)height; (void)planes; (void)bitsPerPixel; (void)bits;
    return NULL;
}

static HBITMAP WINAPI wp81CreateCompatibleBitmap(HDC dc, int width, int height)
{
    (void)dc; (void)width; (void)height;
    return NULL;
}

static UINT WINAPI wp81ExtractIconExW(LPCWSTR file, int index, HICON* largeIcons, HICON* smallIcons, UINT count)
{
    (void)file; (void)index; (void)largeIcons; (void)smallIcons; (void)count;
    return 0;
}

static UINT WINAPI wp81DragQueryFileW(HANDLE drop, UINT index, LPWSTR file, UINT length)
{
    (void)drop; (void)index; (void)length;
    if (file && length)
        file[0] = 0;
    return 0;
}

static LONG WINAPI wp81SHGetValueW(HKEY key, LPCWSTR subKey, LPCWSTR value, DWORD* type, void* data, DWORD* size)
{
    (void)key; (void)subKey; (void)value; (void)type; (void)data; (void)size;
    return ERROR_FILE_NOT_FOUND;
}

static DWORD WINAPI wp81GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR defaultValue, LPWSTR out, DWORD size,
    LPCWSTR file)
{
    (void)section; (void)key; (void)file;
    if (!out || !size)
        return 0;
    lstrcpynW(out, defaultValue ? defaultValue : L"", (int)size);
    return (DWORD)lstrlenW(out);
}

// ---- the clipboard: the engine's own ----
// WP8.1 gives an app no clipboard at all (no user32 one, no Windows.ApplicationModel.DataTransfer.Clipboard). WebCore's
// PasteboardWin stays as it is and works on this one: the Win32 clipboard calls over a table of formats that lives in
// the process. So copy, cut and paste (execCommand, navigator.clipboard, the events with their clipboardData) work
// between the pages and tabs of the app, HTML and URLs included; nothing leaves or enters the app this way (text does
// through the app: wp81ClipboardChanged). Web thread only, as the Pasteboard itself.
//
// "Global" memory is fixed heap memory: the handle is the pointer, as GMEM_FIXED has it on Windows, and the three
// calls agree with each other (the phone has GlobalAlloc and GlobalFree but no GlobalSize).
static HGLOBAL WINAPI wp81GlobalAlloc(UINT flags, SIZE_T size)
{
    (void)flags;
    return (HGLOBAL)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static HGLOBAL WINAPI wp81GlobalFree(HGLOBAL memory)
{
    if (memory)
        HeapFree(GetProcessHeap(), 0, memory);
    return NULL;
}

static LPVOID WINAPI wp81GlobalLock(HGLOBAL memory) { return (LPVOID)memory; }

static SIZE_T WINAPI wp81GlobalSize(HGLOBAL memory)
{
    SIZE_T size = memory ? HeapSize(GetProcessHeap(), 0, memory) : 0;
    return size == (SIZE_T)-1 ? 0 : size;
}

#define WP81_CLIPBOARD_FORMATS 24
#define WP81_CLIPBOARD_NAME 64
static struct { UINT format; HANDLE data; } wp81Clipboard[WP81_CLIPBOARD_FORMATS];
static int wp81ClipboardCount;
static wchar_t wp81ClipboardNames[WP81_CLIPBOARD_FORMATS][WP81_CLIPBOARD_NAME];
static int wp81ClipboardNameCount;
static BOOL wp81ClipboardTextSet;

// Set by the engine: text was put on the clipboard (called when the writer closes it).
void (*wp81ClipboardChanged)(const wchar_t* text);

static BOOL WINAPI wp81OpenClipboard(HWND owner)
{
    (void)owner;
    wp81ClipboardTextSet = FALSE;
    return TRUE;
}

static BOOL WINAPI wp81CloseClipboard(void)
{
    if (wp81ClipboardTextSet && wp81ClipboardChanged) {
        int i;
        wp81ClipboardTextSet = FALSE;
        for (i = 0; i < wp81ClipboardCount; i++) {
            if (wp81Clipboard[i].format == CF_UNICODETEXT && wp81Clipboard[i].data)
                wp81ClipboardChanged((const wchar_t*)wp81Clipboard[i].data);
        }
    }
    return TRUE;
}

static BOOL WINAPI wp81EmptyClipboard(void)
{
    int i;
    for (i = 0; i < wp81ClipboardCount; i++)
        wp81GlobalFree((HGLOBAL)wp81Clipboard[i].data);
    wp81ClipboardCount = 0;
    return TRUE;
}

// The clipboard owns the data from here on. A null handle (delayed rendering on Windows: WebKit's "smart paste" marker)
// makes the format present with no data.
static HANDLE WINAPI wp81SetClipboardData(UINT format, HANDLE data)
{
    int i;
    for (i = 0; i < wp81ClipboardCount && wp81Clipboard[i].format != format; i++) { }
    if (i == WP81_CLIPBOARD_FORMATS)
        return NULL;
    if (i == wp81ClipboardCount) {
        wp81ClipboardCount++;
        wp81Clipboard[i].format = format;
    } else if (wp81Clipboard[i].data != data)
        wp81GlobalFree((HGLOBAL)wp81Clipboard[i].data);
    wp81Clipboard[i].data = data;
    if (format == CF_UNICODETEXT && data)
        wp81ClipboardTextSet = TRUE;
    return data;
}

static HANDLE WINAPI wp81GetClipboardData(UINT format)
{
    int i;
    for (i = 0; i < wp81ClipboardCount; i++) {
        if (wp81Clipboard[i].format == format)
            return wp81Clipboard[i].data;
    }
    return NULL;
}

static BOOL WINAPI wp81IsClipboardFormatAvailable(UINT format)
{
    int i;
    for (i = 0; i < wp81ClipboardCount; i++) {
        if (wp81Clipboard[i].format == format)
            return TRUE;
    }
    return FALSE;
}

// Registered formats are numbered from 0xC000 as on Windows; the same name gives the same number.
static UINT WINAPI wp81RegisterClipboardFormatW(LPCWSTR name)
{
    int i;
    if (!name || !*name || lstrlenW(name) >= WP81_CLIPBOARD_NAME)
        return 0;
    for (i = 0; i < wp81ClipboardNameCount; i++) {
        if (!lstrcmpiW(wp81ClipboardNames[i], name))
            return 0xC000 + (UINT)i;
    }
    if (wp81ClipboardNameCount == WP81_CLIPBOARD_FORMATS)
        return 0;
    lstrcpynW(wp81ClipboardNames[wp81ClipboardNameCount], name, WP81_CLIPBOARD_NAME);
    return 0xC000 + (UINT)wp81ClipboardNameCount++;
}

static HRESULT WINAPI wp81OleGetClipboard(void** dataObject)
{
    if (dataObject)
        *dataObject = NULL;
    return E_NOTIMPL;
}

static void* WINAPI wp81OleDuplicateData(HANDLE source, UINT format, UINT flags)
{
    (void)source; (void)format; (void)flags;
    return NULL;
}

// The phone's UI font for CSS system fonts (SystemFontDatabaseWin) and Skia's default family.
static void wp81SystemFont(LOGFONTW* font)
{
    ZeroMemory(font, sizeof(*font));
    font->lfHeight = -16;
    font->lfWeight = FW_NORMAL;
    font->lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(font->lfFaceName, L"Segoe WP", LF_FACESIZE);
}

static BOOL WINAPI wp81SystemParametersInfoW(UINT action, UINT size, PVOID data, UINT update)
{
    (void)update;
    if (action == SPI_GETNONCLIENTMETRICS && data && size >= sizeof(NONCLIENTMETRICSW)) {
        NONCLIENTMETRICSW* metrics = (NONCLIENTMETRICSW*)data;
        UINT cbSize = metrics->cbSize;
        ZeroMemory(metrics, size);
        metrics->cbSize = cbSize;
        wp81SystemFont(&metrics->lfCaptionFont);
        wp81SystemFont(&metrics->lfSmCaptionFont);
        wp81SystemFont(&metrics->lfMenuFont);
        wp81SystemFont(&metrics->lfStatusFont);
        wp81SystemFont(&metrics->lfMessageFont);
        return TRUE;
    }
    if (action == SPI_GETICONTITLELOGFONT && data && size >= sizeof(LOGFONTW)) {
        wp81SystemFont((LOGFONTW*)data);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

void* __imp_CreateSemaphoreW = (void*)wp81CreateSemaphoreW;
void* __imp_CreateFileMappingA = (void*)wp81CreateFileMappingA;
void* __imp_MoveFileExA = (void*)wp81MoveFileExA;
void* __imp_VerifyVersionInfoW = (void*)wp81VerifyVersionInfoW;
void* __imp_RegisterWaitForSingleObject = (void*)wp81RegisterWaitForSingleObject;
void* __imp_UnregisterWait = (void*)wp81UnregisterWait;
void* __imp_GetPrivateProfileStringW = (void*)wp81GetPrivateProfileStringW;
void* __imp_GlobalAlloc = (void*)wp81GlobalAlloc;
void* __imp_GlobalFree = (void*)wp81GlobalFree;
void* __imp_GlobalLock = (void*)wp81GlobalLock;
void* __imp_GlobalUnlock = (void*)wp81True1;
void* __imp_GlobalSize = (void*)wp81GlobalSize;

void* __imp_SystemParametersInfoW = (void*)wp81SystemParametersInfoW;
void* __imp_GetSystemMetrics = (void*)wp81Zero1;
void* __imp_GetKeyState = (void*)wp81Zero1;
void* __imp_GetAsyncKeyState = (void*)wp81Zero1;
void* __imp_MessageBeep = (void*)wp81True1;
void* __imp_NotifyWinEvent = (void*)wp81Nothing4;

void* __imp_OpenClipboard = (void*)wp81OpenClipboard;
void* __imp_CloseClipboard = (void*)wp81CloseClipboard;
void* __imp_EmptyClipboard = (void*)wp81EmptyClipboard;
void* __imp_GetClipboardData = (void*)wp81GetClipboardData;
void* __imp_SetClipboardData = (void*)wp81SetClipboardData;
void* __imp_IsClipboardFormatAvailable = (void*)wp81IsClipboardFormatAvailable;
void* __imp_RegisterClipboardFormatW = (void*)wp81RegisterClipboardFormatW;
void* __imp_OleGetClipboard = (void*)wp81OleGetClipboard;
void* __imp_OleDuplicateData = (void*)wp81OleDuplicateData;
void* __imp_ReleaseStgMedium = (void*)wp81Nothing1;
void* __imp_DragFinish = (void*)wp81Nothing1;
void* __imp_DragQueryFileW = (void*)wp81DragQueryFileW;

void* __imp_CreateWindowExW = (void*)wp81CreateWindowExW;
void* __imp_RegisterClassW = (void*)wp81Zero1;
void* __imp_DefWindowProcW = (void*)wp81DefWindowProcW;
void* __imp_LoadCursorW = (void*)wp81Null2;
void* __imp_DestroyIcon = (void*)wp81True1;
void* __imp_CreateIconIndirect = (void*)wp81Null1;
void* __imp_DrawIconEx = (void*)wp81DrawIconEx;
void* __imp_ExtractIconExW = (void*)wp81ExtractIconExW;
void* __imp_SHGetValueW = (void*)wp81SHGetValueW;

void* __imp_GetDC = (void*)wp81Null1;
void* __imp_ReleaseDC = (void*)wp81Zero2;
void* __imp_CreateCompatibleDC = (void*)wp81Null1;
void* __imp_DeleteDC = (void*)wp81False1;
void* __imp_CreateDIBSection = (void*)wp81CreateDIBSection;
void* __imp_CreateBitmap = (void*)wp81CreateBitmap;
void* __imp_CreateCompatibleBitmap = (void*)wp81CreateCompatibleBitmap;
void* __imp_BitBlt = (void*)wp81BitBlt;
void* __imp_SelectObject = (void*)wp81Null2;
void* __imp_DeleteObject = (void*)wp81False1;
void* __imp_GetObjectW = (void*)wp81Zero3;
void* __imp_GetStockObject = (void*)wp81Null1;
void* __imp_SetBkMode = (void*)wp81Zero2;
void* __imp_SetGraphicsMode = (void*)wp81Zero2;
void* __imp_SetWorldTransform = (void*)wp81False1;
