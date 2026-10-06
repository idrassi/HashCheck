/**
 * Exercise the production checksum loader and parser without registering the DLL.
 * Allocation failures are injected locally; no large input files are needed.
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

// Compile the real parser with replaceable allocation calls. This executable
// doesn't register the shell extension or run any hashing workers.
#define malloc VerificationTestMalloc
#define SLAddItem TestAddItem
#define SLSetContextSize TestSetContextSize
#define ReadFile TestReadFile
#include "../HashVerify.cpp"
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

int main()
{
    try
    {
        TestParser();
        TestLoader();
        puts("Verification loading and allocation failure tests passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
