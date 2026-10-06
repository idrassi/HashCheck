/**
 * Exercise checksum loading and the verification dialog without registering the DLL.
 * Clipboard operations are simulated; placement tests use a private registry key.
 */

#include "../globals.h"
#include "../HashCheckCommon.h"
#include "../UnicodeHelpers.h"
#include <uxtheme.h>
#include <Strsafe.h>
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static int AllocationsBeforeFailure = -1;
static int ItemsBeforeFailure = -1;
static bool FailIndexAllocation = false;
static int ReadFailure = 0;

extern "C" void* __cdecl VerificationTestMalloc(size_t size)
{
    if (AllocationsBeforeFailure == 0) return NULL;
    if (AllocationsBeforeFailure > 0) --AllocationsBeforeFailure;
    return malloc(size);
}

static PVOID SLFAPI TestAddItem(HSIMPLELIST list, LPCVOID data, UINT size)
{
    if (ItemsBeforeFailure == 0) return NULL;
    if (ItemsBeforeFailure > 0) --ItemsBeforeFailure;
    return SLAddItem(list, data, size);
}

static PVOID SLSAPI TestSetContextSize(HSIMPLELIST list, UINT size)
{
    return FailIndexAllocation ? NULL : SLSetContextSize(list, size);
}

static BOOL WINAPI TestReadFile(HANDLE file, LPVOID buffer, DWORD size,
    LPDWORD read, LPOVERLAPPED overlapped)
{
    if (ReadFailure)
    {
        *read = 0;
        SetLastError(ERROR_READ_FAULT);
        return ReadFailure == 2;
    }
    return ReadFile(file, buffer, size, read, overlapped);
}

// Keep clipboard and window visibility tests local to this process.
static HGLOBAL CopiedText = NULL;
static UINT CopiedFormat = 0;
static bool FailClipboardOpen = false;
static bool FailClipboardAllocation = false;
static bool FailClipboardTransfer = false;
static bool ClipboardOpen = false;
static UINT RequestedWindowState = SW_HIDE;
static bool TestControlDown = false;
static bool SimulateMinimized = false;
static bool SimulateRestoreMaximized = false;

static BOOL WINAPI TestOpenClipboard(HWND)
{
    ClipboardOpen = !FailClipboardOpen;
    return ClipboardOpen;
}
static BOOL WINAPI TestEmptyClipboard()
{
    if (CopiedText) GlobalFree(CopiedText);
    CopiedText = NULL;
    return TRUE;
}
static HANDLE WINAPI TestSetClipboardData(UINT format, HANDLE data)
{
    if (FailClipboardTransfer) return NULL;
    CopiedFormat = format;
    CopiedText = (HGLOBAL)data;
    return data;
}
static BOOL WINAPI TestCloseClipboard()
{
    ClipboardOpen = false;
    return TRUE;
}
static HGLOBAL WINAPI TestClipboardAlloc(UINT flags, SIZE_T size)
{
    return FailClipboardAllocation ? NULL : GlobalAlloc(flags, size);
}
static SHORT WINAPI TestGetKeyState(int key)
{
    return key == VK_CONTROL ? (TestControlDown ? (SHORT)0x8000 : 0) : GetKeyState(key);
}
static BOOL WINAPI TestGetWindowPlacement(HWND window, WINDOWPLACEMENT* placement)
{
    BOOL result = GetWindowPlacement(window, placement);
    if (result && SimulateMinimized)
    {
        placement->showCmd = SW_SHOWMINIMIZED;
        placement->flags = SimulateRestoreMaximized ? WPF_RESTORETOMAXIMIZED : 0;
    }
    return result;
}
static BOOL WINAPI TestSetWindowPlacement(HWND window, const WINDOWPLACEMENT* placement)
{
    RequestedWindowState = placement->showCmd;
    WINDOWPLACEMENT hidden = *placement;
    hidden.showCmd = SW_HIDE;
    return SetWindowPlacement(window, &hidden);
}

// Compile the real parser with replaceable allocation calls. This executable
// doesn't register the shell extension or run any hashing workers.
#define malloc VerificationTestMalloc
#define SLAddItem TestAddItem
#define SLSetContextSize TestSetContextSize
#define ReadFile TestReadFile
#define OpenClipboard TestOpenClipboard
#define EmptyClipboard TestEmptyClipboard
#define SetClipboardData TestSetClipboardData
#define CloseClipboard TestCloseClipboard
#define GlobalAlloc TestClipboardAlloc
#define SetWindowPlacement TestSetWindowPlacement
#define GetWindowPlacement TestGetWindowPlacement
#define GetKeyState TestGetKeyState
#include "../HashVerify.cpp"
#undef GetKeyState
#undef GetWindowPlacement
#undef SetWindowPlacement
#undef GlobalAlloc
#undef CloseClipboard
#undef SetClipboardData
#undef EmptyClipboard
#undef OpenClipboard
#undef ReadFile
#undef SLSetContextSize
#undef SLAddItem
#undef malloc

HMODULE g_hModThisDll;
CREF g_cRefThisDll;
volatile BOOL g_bActCtxCreated;
HANDLE g_hActCtx;
UINT16 g_uWinVer;

// Resolve worker dependencies without linking the hashing engines. Reaching one
// of these means the test accidentally started a worker instead of just loading.
VOID WHAPI WHInitEx(PWHCTXEX) { abort(); }
VOID WHAPI WHUpdateEx(PWHCTXEX, PCBYTE, UINT) { abort(); }
VOID WHAPI WHFinishEx(PWHCTXEX, PWHRESULTEX) { abort(); }

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct ParseInput
{
    HASHVERIFYCONTEXT Context = {};
    std::vector<wchar_t> Text;
    wchar_t Path[32] = L"checksums.sha256";

    ParseInput(const wchar_t* text, const wchar_t* extension = L".sha256")
        : Text(text, text + wcslen(text) + 1)
    {
        wcscpy_s(Path, L"checksums");
        wcscat_s(Path, extension);
        Context.pszPath = Path;
        Context.pszFileData = Text.data();
        HCNormalizeString(Context.pszFileData);
        Context.hList = SLCreateEx(TRUE);
        Require(Context.hList != NULL, "Create parser list");
    }

    ~ParseInput() { SLRelease(Context.hList); }
};

static const wchar_t TwoEntries[] =
    L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 *first.txt\r\n"
    L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 *second.txt\r\n";

static void TestParser()
{
    {
        ParseInput input(TwoEntries);
        Require(HashVerifyParseData(&input.Context), "Parse complete file");
        Require(input.Context.cTotal == 2 && input.Context.index, "Keep both entries");
        Require(wcscmp(input.Context.index[1]->pszDisplayName, L"second.txt") == 0,
            "Keep second filename");
    }
    for (int accepted = 0; accepted < 2; ++accepted)
    {
        ParseInput input(TwoEntries);
        ItemsBeforeFailure = accepted;
        SetLastError(ERROR_SUCCESS);
        Require(!HashVerifyParseData(&input.Context), "Reject incomplete parse");
        Require(GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "Report item allocation failure");
        Require(input.Context.cTotal == 0 && !input.Context.index, "Expose no partial results");
        ItemsBeforeFailure = -1;
    }
    {
        ParseInput input(TwoEntries);
        FailIndexAllocation = true;
        SetLastError(ERROR_SUCCESS);
        Require(!HashVerifyParseData(&input.Context), "Reject missing index");
        Require(GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "Report index allocation failure");
        Require(input.Context.cTotal == 0 && !input.Context.index, "Expose no unindexed results");
        FailIndexAllocation = false;
    }
    for (const wchar_t* text : { L"", L"; comment only\r\n" })
    {
        ParseInput input(text);
        Require(HashVerifyParseData(&input.Context), "Accept empty or comment-only file");
        Require(input.Context.cTotal == 0 && !input.Context.index, "Empty file has no index");
    }
    {
        ParseInput input(L"first.txt ABCDEF01\r\nsecond.txt 12345678\r\n", L".sfv");
        Require(HashVerifyParseData(&input.Context), "Parse SFV entries");
        Require(input.Context.cTotal == 2 && input.Context.whctxFlags == WHEX_CHECKCRC32,
            "Preserve SFV format detection");
    }
}

struct TempFile
{
    wchar_t Path[MAX_PATH];

    TempFile(const void* data, DWORD size)
    {
        wchar_t directory[MAX_PATH];
        Require(GetTempPathW(countof(directory), directory) != 0, "Find temp directory");
        Require(GetTempFileNameW(directory, L"HCV", 0, Path) != 0, "Create test file");
        HANDLE file = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            DeleteFileW(Path);
            throw std::runtime_error("Open test file");
        }
        DWORD written = 0;
        BOOL success = WriteFile(file, data, size, &written, NULL);
        CloseHandle(file);
        if (!success || written != size)
        {
            DeleteFileW(Path);
            throw std::runtime_error("Write test file");
        }
    }

    ~TempFile() { DeleteFileW(Path); }
};

static void TestLoader()
{
    const char text[] = "abcdef01 *first.txt\r\n";
    TempFile file(text, sizeof(text) - 1);
    for (int failAfter = 0; failAfter < 2; ++failAfter)
    {
        HASHVERIFYCONTEXT context = {};
        context.pszPath = file.Path;
        AllocationsBeforeFailure = failAfter;
        SetLastError(ERROR_SUCCESS);
        PBYTE buffer = HashVerifyLoadData(&context);
        DWORD error = GetLastError();
        AllocationsBeforeFailure = -1;
        Require(!buffer && !context.pszFileData, "Reject raw or Unicode allocation failure");
        Require(error == ERROR_NOT_ENOUGH_MEMORY, "Preserve loader allocation error");
    }
    for (int failure = 1; failure <= 2; ++failure)
    {
        HASHVERIFYCONTEXT context = {};
        context.pszPath = file.Path;
        ReadFailure = failure;
        PBYTE buffer = HashVerifyLoadData(&context);
        DWORD error = GetLastError();
        ReadFailure = 0;
        Require(!buffer && !context.pszFileData, "Reject failed or incomplete read");
        Require(error == (failure == 1 ? ERROR_READ_FAULT : ERROR_HANDLE_EOF),
            "Preserve read error");
    }
    {
        HASHVERIFYCONTEXT context = {};
        context.pszPath = file.Path;
        PBYTE buffer = HashVerifyLoadData(&context);
        Require(buffer && context.pszFileData, "Load normal text after failures");
        Require(wcsstr(context.pszFileData, L"first.txt") != NULL, "Decode normal text");
        free(buffer);
    }
    {
        const wchar_t unicode[] = L"\xfeff" L"abcdef01 *\x65e5\x672c.txt\r\n";
        TempFile fileUnicode(unicode, sizeof(unicode) - sizeof(wchar_t));
        HASHVERIFYCONTEXT context = {};
        context.pszPath = fileUnicode.Path;
        PBYTE buffer = HashVerifyLoadData(&context);
        Require(buffer && context.pszFileData, "Load UTF-16 file");
        Require(wcsstr(context.pszFileData, L"\x65e5\x672c.txt") != NULL, "Preserve Unicode filename");
        free(buffer);
    }
    {
        TempFile empty("", 0);
        HASHVERIFYCONTEXT context = {};
        context.pszPath = empty.Path;
        PBYTE buffer = HashVerifyLoadData(&context);
        Require(buffer && context.pszFileData && !*context.pszFileData, "Load empty file");
        free(buffer);
    }
}

static INT_PTR CALLBACK VerificationWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG)
    {
        PHASHVERIFYCONTEXT ctx = (PHASHVERIFYCONTEXT)lp;
        ctx->hWnd = window;
        SetWindowLongPtr(window, DWLP_USER, (LONG_PTR)ctx);
        HashVerifyDlgInit(ctx);
        ctx->hWndPBTotal = GetDlgItem(window, IDC_PROG_TOTAL);
        ctx->hWndPBFile = GetDlgItem(window, IDC_PROG_FILE);
        HashVerifyUpdateSummary(ctx, NULL);
        return TRUE;
    }
    return HashVerifyDlgProc(window, message, wp, lp);
}

static HANDLE SetTestDpiContext(HANDLE context)
{
    typedef HANDLE (WINAPI *SETCONTEXT)(HANDLE);
    static SETCONTEXT setContext = (SETCONTEXT)GetProcAddress(GetModuleHandle(L"user32.dll"), "SetThreadDpiAwarenessContext");
    return setContext ? setContext(context) : NULL;
}

struct VerificationWindow
{
    HANDLE PreviousDpiContext = NULL;
    HASHVERIFYCONTEXT Context = {};
    HASHVERIFYITEM Items[4] = {};
    PHVITEM Index[4] = { &Items[0], &Items[1], &Items[2], &Items[3] };
    wchar_t Path[64] = L"C:\\checksums\\verification.sha256";

    VerificationWindow()
    {
        PreviousDpiContext = SetTestDpiContext((HANDLE)-4); // Per-monitor v2, when available.
        static wchar_t names[][32] = { L"match-\x65e5\x672c.txt", L"mismatch.txt", L"unreadable.txt", L"pending.txt" };
        static wchar_t expected[] = L"0123456789abcdef";
        Context.pszPath = Path;
        Context.index = Index;
        Context.cTotal = countof(Items);
        Context.whctxFlags = WHEX_CHECKSHA256;
        for (UINT i = 0; i < countof(Items); ++i)
        {
            Items[i].pszDisplayName = names[i];
            Items[i].pszExpected = expected;
            Items[i].uStatusID = (UINT8)(i == 3 ? HV_STATUS_MATCH : i + 1);
            wcscpy_s(Items[i].filesize.sz, L"12 bytes");
        }
        wcscpy_s(Items[0].szActual, expected);
        wcscpy_s(Items[1].szActual, L"fedcba9876543210");
        // A worker has filled this field, but its completion message hasn't been handled.
        wcscpy_s(Items[3].szActual, L"unreported-result");
        HWND window = CreateDialogParam(g_hModThisDll, MAKEINTRESOURCE(IDD_HASHVERF),
            NULL, VerificationWindowProc, (LPARAM)&Context);
        Require(window != NULL, "Create hidden verification dialog");
        for (UINT i = 0; i < 3; ++i)
        {
            ++Context.cHandledMsgs;
            HashVerifyUpdateSummary(&Context, &Items[i]);
        }
        HashVerifyUpdateSummary(&Context, NULL);
    }
    ~VerificationWindow()
    {
        DestroyWindow(Context.hWnd);
        if (PreviousDpiContext) SetTestDpiContext(PreviousDpiContext);
    }
};

static RECT ControlRect(HWND window, UINT id)
{
    RECT rect;
    GetWindowRect(GetDlgItem(window, id), &rect);
    MapWindowPoints(NULL, window, (POINT*)&rect, 2);
    return rect;
}

static void TestWindowLayout()
{
    Require(HashVerifyWindowDpi(NULL) > 0, "Use fallback DPI when the window API returns zero");
    VerificationWindow window;
    HASHVERIFYCONTEXT* ctx = &window.Context;
    HWND hwnd = ctx->hWnd;
    RECT beforeList = ControlRect(hwnd, IDC_LIST);
    RECT beforeExit = ControlRect(hwnd, IDC_EXIT);
    RECT beforeCopy = ControlRect(hwnd, IDC_HV_COPY);
    RECT original;
    GetWindowRect(hwnd, &original);
    LONG width = original.right - original.left;
    LONG height = original.bottom - original.top;
    SetWindowPos(hwnd, NULL, 0, 0, width + 240, height + 160, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT list = ControlRect(hwnd, IDC_LIST), exit = ControlRect(hwnd, IDC_EXIT), copy = ControlRect(hwnd, IDC_HV_COPY);
    Require(list.right == beforeList.right + 240 && list.bottom == beforeList.bottom + 160,
        "List grows with window");
    Require(exit.left == beforeExit.left + 240 && exit.top == beforeExit.top + 160, "Exit stays at bottom right");
    Require(copy.left == beforeCopy.left && copy.top == beforeCopy.top + 160, "Copy stays at bottom left");
    MINMAXINFO limits = {};
    SendMessage(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    Require(limits.ptMinTrackSize.x == width && limits.ptMinTrackSize.y == height, "Keep initial layout as minimum size");
    UINT initialDpi = ctx->layout.initialDpi;
    for (UINT scale : { 150U, 200U, 100U })
    {
        UINT dpi = MulDiv(initialDpi, scale, 100);
        RECT suggested = { original.left, original.top,
            original.left + MulDiv(width + 240, scale, 100), original.top + MulDiv(height + 160, scale, 100) };
        SendMessage(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&suggested);
        SendMessage(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
        Require(limits.ptMinTrackSize.x == MulDiv(width, scale, 100), "Scale minimum width with DPI");
        RECT client;
        GetClientRect(hwnd, &client);
        RECT summary = ControlRect(hwnd, IDC_SUMMARY);
        list = ControlRect(hwnd, IDC_LIST);
        copy = ControlRect(hwnd, IDC_HV_COPY);
        exit = ControlRect(hwnd, IDC_EXIT);
        Require(list.bottom < summary.top && summary.bottom < copy.top, "Keep list and summary apart after DPI change");
        Require(copy.left >= 0 && exit.right <= client.right && exit.bottom <= client.bottom,
            "Keep controls within the resized client area");
        LOGFONT font;
        GetObject((HFONT)SendDlgItemMessage(hwnd, IDC_HV_COPY, WM_GETFONT, 0, 0), sizeof(font), &font);
        Require(font.lfHeight == MulDiv(ctx->layout.font.lfHeight, dpi, initialDpi), "Scale control font with DPI");
    }
    Require(!IsWindowVisible(hwnd), "UI checks must remain hidden");
}

static void TestSystemScaling()
{
    HANDLE previous = SetTestDpiContext((HANDLE)-1); // DPI unaware
    if (!previous) return;
    HASHVERIFYCONTEXT ctx = {};
    wchar_t path[] = L"scaling.sha256";
    ctx.pszPath = path;
    HWND window = CreateDialogParam(g_hModThisDll, MAKEINTRESOURCE(IDD_HASHVERF), NULL,
        VerificationWindowProc, (LPARAM)&ctx);
    Require(window != NULL, "Create dialog in a host that uses system scaling");
    UINT dpi = ctx.layout.dpi;
    RECT before;
    GetWindowRect(window, &before);
    RECT suggested = before;
    suggested.right += 100;
    suggested.bottom += 100;
    SendMessage(window, WM_DPICHANGED, MAKEWPARAM(dpi * 2, dpi * 2), (LPARAM)&suggested);
    RECT after;
    GetWindowRect(window, &after);
    Require(ctx.layout.dpi == dpi && EqualRect(&before, &after), "Leave system scaling to Windows");
    DestroyWindow(window);
    SetTestDpiContext(previous);
}

static std::wstring ClipboardContents()
{
    Require(CopiedText != NULL && CopiedFormat == CF_UNICODETEXT, "Copy Unicode text");
    const wchar_t* data = (const wchar_t*)GlobalLock(CopiedText);
    Require(data != NULL, "Read test clipboard");
    std::wstring text(data);
    GlobalUnlock(CopiedText);
    return text;
}

static void TestResultCopying()
{
    VerificationWindow window;
    HASHVERIFYCONTEXT* ctx = &window.Context;
    ListView_SetItemState(ctx->hWndList, 0, LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(ctx->hWndList, 2, LVIS_SELECTED, LVIS_SELECTED);
    NMLVKEYDOWN key = {};
    key.hdr.hwndFrom = ctx->hWndList;
    key.hdr.idFrom = IDC_LIST;
    key.hdr.code = LVN_KEYDOWN;
    key.wVKey = 'C';
    TestControlDown = true;
    SendMessage(ctx->hWnd, WM_NOTIFY, IDC_LIST, (LPARAM)&key);
    TestControlDown = false;
    std::wstring text = ClipboardContents();
    Require(text.find(L"match-\x65e5\x672c.txt") != text.npos && text.find(L"unreadable.txt") != text.npos,
        "Ctrl+C keeps selected Unicode filenames");
    Require(text.find(L"mismatch.txt") == text.npos && text.find(L"pending.txt") == text.npos,
        "Copy only selected rows");
    Require(text.find(L"File Name\tSize\tStatus\tExpected Checksum\tActual Checksum\r\n") == 0,
        "Include column headings");
    Require(!ClipboardOpen, "Close clipboard after copying");
    key.wVKey = 'A';
    TestControlDown = true;
    SendMessage(ctx->hWnd, WM_NOTIFY, IDC_LIST, (LPARAM)&key);
    TestControlDown = false;
    Require(ListView_GetSelectedCount(ctx->hWndList) == 4, "Ctrl+A selects all rows");

    Require(HashVerifyCopyResults(ctx, IDM_HV_COPY_ALL), "Copy all results");
    text = ClipboardContents();
    Require(text.find(window.Path) == 0 && text.find(L"Match:\t1 of 4 files") != text.npos,
        "Include checksum file and summary counts");
    Require(text.find(L"Remaining:\t1 of 4 files") != text.npos, "Include pending summary count");
    Require(text.find(L"pending.txt\t\tPENDING\t0123456789abcdef\t\r\n") != text.npos,
        "Keep unfinished row pending without mutable result fields");
    Require(text.find(L"unreported-result") == text.npos, "Don't copy a result before its UI update");
    Require(text.find(L"fedcba9876543210") != text.npos, "Keep mismatched actual checksum");

    int order[] = { 2, 0, 1, 4, 3 };
    ListView_SetColumnOrderArray(ctx->hWndList, countof(order), order);
    std::swap(ctx->index[0], ctx->index[2]);
    Require(HashVerifyCopyResults(ctx, IDM_HV_COPY_SELECTED), "Copy reordered results");
    text = ClipboardContents();
    Require(text.find(L"Status\tFile Name\tSize\tActual Checksum\tExpected Checksum\r\n") == 0,
        "Respect displayed column order");
    Require(text.find(L"unreadable.txt") < text.find(L"match-\x65e5\x672c.txt"), "Respect displayed row order");
    Require(HashVerifyCopyResults(ctx, IDM_HV_COPY_SUMMARY), "Copy summary separately");
    text = ClipboardContents();
    Require(text.find(L"Mismatch:\t1 of 4 files") != text.npos && text.find(L"unreadable.txt") == text.npos,
        "Summary copy excludes result rows");

    FailClipboardAllocation = true;
    Require(!HashVerifyCopyResults(ctx, IDM_HV_COPY_ALL), "Report clipboard allocation failure");
    FailClipboardAllocation = false;
    Require(ClipboardContents() == text, "Allocation failure preserves existing clipboard");
    FailClipboardOpen = true;
    Require(!HashVerifyCopyResults(ctx, IDM_HV_COPY_ALL), "Report clipboard contention");
    FailClipboardOpen = false;
    Require(ClipboardContents() == text, "Clipboard contention preserves existing data");
    FailClipboardTransfer = true;
    Require(!HashVerifyCopyResults(ctx, IDM_HV_COPY_ALL), "Handle clipboard transfer failure");
    FailClipboardTransfer = false;
    Require(!ClipboardOpen, "Close clipboard on transfer failure");
    Require(HashVerifyCopyResults(ctx, IDM_HV_COPY_SUMMARY), "Copy succeeds after failures");
    TestEmptyClipboard();
}

struct IsolatedUserRegistry
{
    HKEY Key = NULL;
    wchar_t Path[128];
    IsolatedUserRegistry()
    {
        swprintf_s(Path, L"Software\\HashCheckVerificationTests-%lu-%lu", GetCurrentProcessId(), GetTickCount());
        Require(RegCreateKeyEx(HKEY_CURRENT_USER, Path, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS, NULL, &Key, NULL) == ERROR_SUCCESS, "Create private test settings");
        LSTATUS error = RegOverridePredefKey(HKEY_CURRENT_USER, Key);
        if (error != ERROR_SUCCESS)
        {
            RegCloseKey(Key);
            RegDeleteTree(HKEY_CURRENT_USER, Path);
            throw std::runtime_error("Redirect test settings");
        }
    }
    ~IsolatedUserRegistry()
    {
        RegOverridePredefKey(HKEY_CURRENT_USER, NULL);
        RegCloseKey(Key);
        RegDeleteTree(HKEY_CURRENT_USER, Path);
    }
};

static void TestWindowPlacement()
{
    RECT work = { -1920, 40, 0, 1080 };
    RECT outside = { 8000, 8000, 10000, 10000 };
    SIZE minimum = { 600, 420 };
    RECT fit = HashVerifyFitWindow(outside, &work, minimum);
    Require(EqualRect(&fit, &work), "Fit oversized window on monitor with negative coordinates and taskbar");
    RECT smallRect = { -3000, -200, -2900, -100 };
    fit = HashVerifyFitWindow(smallRect, &work, minimum);
    Require(fit.left == work.left && fit.top == work.top && fit.right - fit.left == minimum.cx,
        "Restore minimum size on the visible monitor");
    HASHVERIFYPLACEMENT invalid = {};
    invalid.version = 1;
    invalid.dpi = 96;
    invalid.normal = { LONG_MIN, 0, LONG_MAX, 600 };
    Require(!HashVerifyValidPlacement(&invalid), "Reject overflowing stored dimensions");
    invalid.normal = { 0, 0, 900, 600 };
    invalid.dpi = 0;
    Require(!HashVerifyValidPlacement(&invalid), "Reject zero stored DPI");

    VerificationWindow window;
    HASHVERIFYCONTEXT* ctx = &window.Context;
    IsolatedUserRegistry registry;
    MONITORINFO monitor = { sizeof(monitor) };
    Require(GetMonitorInfo(MonitorFromWindow(ctx->hWnd, MONITOR_DEFAULTTONEAREST), &monitor) != 0, "Read test monitor");
    RECT target = { monitor.rcWork.left + 20, monitor.rcWork.top + 20,
        monitor.rcWork.left + ctx->layout.minimum.cx + 160, monitor.rcWork.top + ctx->layout.minimum.cy + 100 };
    target = HashVerifyFitWindow(target, &monitor.rcWork, HashVerifyMinimumSize(ctx));
    SetWindowPos(ctx->hWnd, NULL, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
    HashVerifySaveWindow(ctx);
    SetWindowPos(ctx->hWnd, NULL, target.left + 10, target.top + 10, ctx->layout.minimum.cx,
        ctx->layout.minimum.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    HashVerifyRestoreWindow(ctx);
    RECT restored;
    GetWindowRect(ctx->hWnd, &restored);
    Require(EqualRect(&restored, &target), "Restore saved window size and position without taskbar drift");
    Require(RequestedWindowState == SW_SHOWNORMAL, "Restore a normal window");

    HKEY settings;
    Require(RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\HashCheck", 0, KEY_ALL_ACCESS, &settings) == ERROR_SUCCESS,
        "Read private saved placement");
    HASHVERIFYPLACEMENT saved = {};
    DWORD type = 0, size = sizeof(saved);
    Require(RegQueryValueEx(settings, L"VerifyWindow", NULL, &type, (PBYTE)&saved, &size) == ERROR_SUCCESS,
        "Read saved placement record");
    Require(type == REG_BINARY && HashVerifyValidPlacement(&saved), "Store a valid placement record");
    HASHVERIFYPLACEMENT offscreen = saved;
    offscreen.normal = outside;
    RegSetValueEx(settings, L"VerifyWindow", 0, REG_BINARY, (PCBYTE)&offscreen, sizeof(offscreen));
    HashVerifyRestoreWindow(ctx);
    GetWindowRect(ctx->hWnd, &restored);
    MONITORINFO recoveryMonitor = { sizeof(recoveryMonitor) };
    Require(GetMonitorInfo(MonitorFromRect(&outside, MONITOR_DEFAULTTONEAREST), &recoveryMonitor) != 0,
        "Find the monitor nearest the stored position");
    Require(restored.left >= recoveryMonitor.rcWork.left && restored.top >= recoveryMonitor.rcWork.top &&
        restored.right <= recoveryMonitor.rcWork.right && restored.bottom <= recoveryMonitor.rcWork.bottom,
        "Recover a window from a removed monitor");

    RegSetValueEx(settings, L"VerifyWindow", 0, REG_SZ, (PCBYTE)&saved, sizeof(saved));
    HashVerifyRestoreWindow(ctx);
    RECT unchanged;
    GetWindowRect(ctx->hWnd, &unchanged);
    Require(EqualRect(&unchanged, &restored), "Ignore a saved value with the wrong type");
    RegSetValueEx(settings, L"VerifyWindow", 0, REG_BINARY, (PCBYTE)&saved, sizeof(saved) - 1);
    HashVerifyRestoreWindow(ctx);
    GetWindowRect(ctx->hWnd, &unchanged);
    Require(EqualRect(&unchanged, &restored), "Ignore an incomplete placement record");
    RegCloseKey(settings);

    SimulateMinimized = true;
    SimulateRestoreMaximized = true;
    HashVerifySaveWindow(ctx);
    SimulateMinimized = false;
    HashVerifyRestoreWindow(ctx);
    Require(RequestedWindowState == SW_SHOWMAXIMIZED, "Remember maximized state when closed while minimized");
    SimulateMinimized = true;
    SimulateRestoreMaximized = false;
    HashVerifySaveWindow(ctx);
    SimulateMinimized = false;
    HashVerifyRestoreWindow(ctx);
    Require(RequestedWindowState == SW_SHOWNORMAL && !IsWindowVisible(ctx->hWnd),
        "Never reopen minimized; test windows stay hidden");
}

int main()
{
    try
    {
        g_hModThisDll = GetModuleHandle(NULL);
        g_uWinVer = 0x0a00;
        g_hActCtx = INVALID_HANDLE_VALUE;
        SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
        ULONG_PTR activation = ActivateManifest(TRUE);
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&controls);
        TestParser();
        TestLoader();
        TestWindowLayout();
        TestSystemScaling();
        TestResultCopying();
        TestWindowPlacement();
        DeactivateManifest(activation);
        puts("Verification loading, window layout, placement and copying tests passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
