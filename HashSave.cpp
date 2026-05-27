/**
 * HashCheck Shell Extension
 * Original work copyright (C) Kai Liu.  All rights reserved.
 * Modified work copyright (C) 2014, 2016 Christopher Gurnee.  All rights reserved.
 * Modified work copyright (C) 2016 Tim Schlueter.  All rights reserved.
 * Modified work copyright (C) 2021-2026 Mounir IDRASSI.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "globals.h"
#include "HashCheckCommon.h"
#include "HashCalc.h"
#include "SetAppID.h"
#include <Strsafe.h>
#include <vector>
#include <cassert>
#include <algorithm>
#ifdef USE_PPL
#include <ppl.h>
#include <concurrent_vector.h>
#endif

#ifdef UNICODE
#define StrCmpLogical StrCmpLogicalW
#else
#define StrCmpLogical StrCmpIA
#endif

// Control structures, from HashCalc.h
#define  HASHSAVESCRATCH  HASHCALCSCRATCH
#define PHASHSAVESCRATCH PHASHCALCSCRATCH
#define  HASHSAVECONTEXT  HASHCALCCONTEXT
#define PHASHSAVECONTEXT PHASHCALCCONTEXT
#define  HASHSAVEITEM     HASHCALCITEM
#define PHASHSAVEITEM    PHASHCALCITEM

#define HSF_HEADLESS       0x01000000UL
#define HSF_WORKER_FAILED  0x02000000UL

typedef struct {
	HSIMPLELIST hListRaw;
	PWSTR pszOutputPath;
	UINT uHashIndex;
	INT iSaveEncoding;
	INT iSaveEol;
	BOOL bBypassQueue;
} HASHSAVESILENTSPEC, *PHASHSAVESILENTSPEC;



/*============================================================================*\
	Function declarations
\*============================================================================*/

// Entry points / main functions
VOID CALLBACK HashSaveRunDllEx( HWND hWnd, PWSTR pszCmdLine, BOOL bSeparateFiles, BOOL bBypassQueue );
INT CALLBACK HashSaveSilentRunDllEx( HWND hWnd, PWSTR pszCmdLine, BOOL bBypassQueue );
DWORD WINAPI HashSaveThread( PHASHSAVECONTEXT phsctx );
HSIMPLELIST WINAPI HashSaveLoadPathListFile( PWSTR pszPathListFile );
HRESULT WINAPI HashSaveLoadSilentSpecFile( PWSTR pszSpecFile, PHASHSAVESILENTSPEC pSpec );
VOID WINAPI HashSaveFreeSilentSpec( PHASHSAVESILENTSPEC pSpec );
PHASHSAVECONTEXT WINAPI HashSaveCreateContext( HWND hWndOwner, HSIMPLELIST hListRaw );
VOID WINAPI HashSaveReleaseContext( PHASHSAVECONTEXT phsctx );
BOOL WINAPI HashSaveStartThread( PHASHSAVECONTEXT phsctx );
HRESULT WINAPI HashSaveRunSilent( HWND hWndOwner, PHASHSAVESILENTSPEC pSpec );

// Worker thread
VOID __fastcall HashSaveWorkerMain( PHASHSAVECONTEXT phsctx );
BOOL WINAPI HashSaveWriteSeparateResult( PHASHSAVECONTEXT phsctx, PHASHSAVEITEM pItem );

// Dialog general
INT_PTR CALLBACK HashSaveDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
VOID WINAPI HashSaveDlgInit( PHASHSAVECONTEXT phsctx );



/*============================================================================*\
	Entry points / main functions
\*============================================================================*/

VOID CALLBACK HashSave_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	HashSaveRunDllEx(hWnd, pszCmdLine, FALSE, FALSE);
}

VOID CALLBACK HashSaveNoQueue_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	HashSaveRunDllEx(hWnd, pszCmdLine, FALSE, TRUE);
}

VOID CALLBACK HashSaveSeparate_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	HashSaveRunDllEx(hWnd, pszCmdLine, TRUE, FALSE);
}

VOID CALLBACK HashSaveSeparateNoQueue_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	HashSaveRunDllEx(hWnd, pszCmdLine, TRUE, TRUE);
}

VOID CALLBACK HashSaveRunDllEx( HWND hWnd, PWSTR pszCmdLine, BOOL bSeparateFiles, BOOL bBypassQueue )
{
	HSIMPLELIST hListRaw = HashSaveLoadPathListFile(pszCmdLine);
	if (hListRaw)
	{
		PHASHSAVECONTEXT phsctx = HashSaveCreateContext(hWnd, hListRaw);
		if (phsctx && bBypassQueue)
			phsctx->dwFlags |= HCF_BYPASS_QUEUE;
		if (phsctx)
			phsctx->bSeparateFiles = bSeparateFiles;
		if (phsctx)
			HashSaveThread(phsctx);

		SLRelease(hListRaw);
	}
}

INT CALLBACK HashSaveSilent_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	return(HashSaveSilentRunDllEx(hWnd, pszCmdLine, FALSE));
}

INT CALLBACK HashSaveSilentNoQueue_RunDLLW( HWND hWnd, HINSTANCE, PWSTR pszCmdLine, INT )
{
	return(HashSaveSilentRunDllEx(hWnd, pszCmdLine, TRUE));
}

INT CALLBACK HashSaveSilentRunDllEx( HWND hWnd, PWSTR pszCmdLine, BOOL bBypassQueue )
{
	HASHSAVESILENTSPEC spec;
	ZeroMemory(&spec, sizeof(spec));

	HRESULT hr = HashSaveLoadSilentSpecFile(pszCmdLine, &spec);
	if (SUCCEEDED(hr))
		spec.bBypassQueue = bBypassQueue;
	if (SUCCEEDED(hr))
		hr = HashSaveRunSilent(hWnd, &spec);

	HashSaveFreeSilentSpec(&spec);
	return((INT)hr);
}

VOID WINAPI HashSaveStart( HWND hWndOwner, HSIMPLELIST hListRaw, BOOL bSeparateFiles )
{
	// Explorer will be blocking as long as this function is running, so we
	// want to return as quickly as possible and leave the work up to the
	// thread that we are going to spawn

	PHASHSAVECONTEXT phsctx = HashSaveCreateContext(hWndOwner, hListRaw);

	if (phsctx)
	{
		phsctx->bSeparateFiles = bSeparateFiles;

		if (HashSaveStartThread(phsctx))
			return;

		HashSaveReleaseContext(phsctx);
	}
}

HSIMPLELIST WINAPI HashSaveLoadPathListFile( PWSTR pszPathListFile )
{
	static const WCHAR szPathListHeader[] = L"HashCheckPathListV1";

	if (!pszPathListFile || !*pszPathListFile)
		return(NULL);

	HANDLE hFile = CreateFileW(
		pszPathListFile,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE)
		return(NULL);

	LARGE_INTEGER cbFile;
	BOOL bReadOk = GetFileSizeEx(hFile, &cbFile) &&
	               cbFile.HighPart == 0 &&
	               cbFile.LowPart >= sizeof(szPathListHeader) &&
	               (cbFile.LowPart % sizeof(WCHAR)) == 0;

	PWSTR pszBuffer = NULL;
	DWORD cbRead = 0;
	if (bReadOk)
	{
		pszBuffer = (PWSTR)malloc(cbFile.LowPart + sizeof(WCHAR));
		bReadOk = pszBuffer &&
		          ReadFile(hFile, pszBuffer, cbFile.LowPart, &cbRead, NULL) &&
		          cbRead == cbFile.LowPart;
	}

	CloseHandle(hFile);
	DeleteFileW(pszPathListFile);

	HSIMPLELIST hListRaw = NULL;
	if (bReadOk)
	{
		UINT cchBuffer = cbRead / sizeof(WCHAR);
		pszBuffer[cchBuffer] = 0;

		PWSTR pszPath = pszBuffer;
		if (StrCmpW(pszPath, szPathListHeader) == 0)
		{
			pszPath += countof(szPathListHeader);
			hListRaw = SLCreate();

			while (hListRaw && *pszPath)
			{
				if (!SLAddStringI(hListRaw, pszPath))
				{
					SLRelease(hListRaw);
					hListRaw = NULL;
					break;
				}

				pszPath += lstrlenW(pszPath) + 1;
			}

			if (hListRaw && !SLCheck(hListRaw))
			{
				SLRelease(hListRaw);
				hListRaw = NULL;
			}
		}
	}

	free(pszBuffer);
	return(hListRaw);
}

HRESULT WINAPI HashSaveLoadSilentSpecFile( PWSTR pszSpecFile, PHASHSAVESILENTSPEC pSpec )
{
	static const WCHAR szSpecHeader[] = L"HashCheckSaveSpecV1";

	if (!pSpec)
		return(E_POINTER);

	ZeroMemory(pSpec, sizeof(*pSpec));

	if (!pszSpecFile || !*pszSpecFile)
		return(E_INVALIDARG);

	HANDLE hFile = CreateFileW(
		pszSpecFile,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE)
		return(HRESULT_FROM_WIN32(GetLastError()));

	LARGE_INTEGER cbFile;
	BOOL bReadOk = GetFileSizeEx(hFile, &cbFile) &&
	               cbFile.HighPart == 0 &&
	               cbFile.LowPart >= sizeof(szSpecHeader) &&
	               (cbFile.LowPart % sizeof(WCHAR)) == 0;

	PWSTR pszBuffer = NULL;
	DWORD cbRead = 0;
	if (bReadOk)
	{
		pszBuffer = (PWSTR)malloc(cbFile.LowPart + sizeof(WCHAR));
		bReadOk = pszBuffer &&
		          ReadFile(hFile, pszBuffer, cbFile.LowPart, &cbRead, NULL) &&
		          cbRead == cbFile.LowPart;
	}

	DWORD dwReadError = bReadOk ? ERROR_SUCCESS : GetLastError();

	CloseHandle(hFile);
	DeleteFileW(pszSpecFile);

	HRESULT hr = bReadOk ? S_OK : HRESULT_FROM_WIN32(dwReadError ? dwReadError : ERROR_READ_FAULT);

	if (SUCCEEDED(hr))
	{
		UINT cchBuffer = cbRead / sizeof(WCHAR);
		pszBuffer[cchBuffer] = 0;

		PWSTR pszCurrent = pszBuffer;
		PWSTR pszEnd = pszBuffer + cchBuffer;
		auto HasCompleteString = [pszEnd](PWSTR psz) -> BOOL
		{
			return(psz < pszEnd && psz + lstrlenW(psz) < pszEnd);
		};

		if (StrCmpW(pszCurrent, szSpecHeader) != 0)
		{
			hr = E_INVALIDARG;
		}
		else
		{
			pszCurrent += countof(szSpecHeader);

			if (!HasCompleteString(pszCurrent) || !*pszCurrent)
				hr = E_INVALIDARG;
		}

		if (SUCCEEDED(hr))
		{
			size_t cchOutputPath = lstrlenW(pszCurrent) + 1;
			pSpec->pszOutputPath = (PWSTR)malloc(cchOutputPath * sizeof(WCHAR));
			if (!pSpec->pszOutputPath)
			{
				hr = E_OUTOFMEMORY;
			}
			else
			{
				StringCchCopyW(pSpec->pszOutputPath, cchOutputPath, pszCurrent);
				pszCurrent += cchOutputPath;
			}
		}

		if (SUCCEEDED(hr) && !HasCompleteString(pszCurrent))
			hr = E_INVALIDARG;

		if (SUCCEEDED(hr))
		{
			pSpec->uHashIndex = (UINT)StrToIntW(pszCurrent);
			pszCurrent += lstrlenW(pszCurrent) + 1;

			if (!HasCompleteString(pszCurrent))
			{
				hr = E_INVALIDARG;
			}
		}

		if (SUCCEEDED(hr))
		{
			pSpec->iSaveEncoding = StrToIntW(pszCurrent);
			pszCurrent += lstrlenW(pszCurrent) + 1;

			if (!HasCompleteString(pszCurrent))
			{
				hr = E_INVALIDARG;
			}
		}

		if (SUCCEEDED(hr))
		{
			pSpec->iSaveEol = StrToIntW(pszCurrent);
			pszCurrent += lstrlenW(pszCurrent) + 1;

			if (!pSpec->uHashIndex || pSpec->uHashIndex > NUM_HASHES)
				hr = E_INVALIDARG;
		}

		if (SUCCEEDED(hr))
		{
			pSpec->hListRaw = SLCreate();
			if (!pSpec->hListRaw)
			{
				hr = E_OUTOFMEMORY;
			}
			else
			{
				while (pszCurrent < pszEnd && *pszCurrent)
				{
					if (!HasCompleteString(pszCurrent))
					{
						hr = E_INVALIDARG;
						break;
					}

					if (!SLAddStringI(pSpec->hListRaw, pszCurrent))
					{
						hr = E_OUTOFMEMORY;
						break;
					}

					pszCurrent += lstrlenW(pszCurrent) + 1;
				}

				if (SUCCEEDED(hr) && !SLCheck(pSpec->hListRaw))
					hr = E_INVALIDARG;
			}
		}
	}

	free(pszBuffer);

	if (FAILED(hr))
		HashSaveFreeSilentSpec(pSpec);

	return(hr);
}

VOID WINAPI HashSaveFreeSilentSpec( PHASHSAVESILENTSPEC pSpec )
{
	if (!pSpec)
		return;

	SLRelease(pSpec->hListRaw);
	free(pSpec->pszOutputPath);
	ZeroMemory(pSpec, sizeof(*pSpec));
}

PHASHSAVECONTEXT WINAPI HashSaveCreateContext( HWND hWndOwner, HSIMPLELIST hListRaw )
{
	PHASHSAVECONTEXT phsctx = (PHASHSAVECONTEXT)SLSetContextSize(hListRaw, sizeof(HASHSAVECONTEXT));

	if (phsctx)
	{
		ZeroMemory(phsctx, sizeof(HASHSAVECONTEXT));
		phsctx->hWnd = hWndOwner;
		phsctx->hListRaw = hListRaw;
		phsctx->dwFlags = 0;
		phsctx->dwReadBufferSize = GetReadBufferSizeForPath(NULL);
		phsctx->bOuterMultithreaded = FALSE;
		phsctx->hFileOut = INVALID_HANDLE_VALUE;

		InterlockedIncrement(&g_cRefThisDll);
		SLAddRef(hListRaw);
	}

	return(phsctx);
}

VOID WINAPI HashSaveReleaseContext( PHASHSAVECONTEXT phsctx )
{
	HSIMPLELIST hListRaw = phsctx->hListRaw;

	SLRelease(hListRaw);
	InterlockedDecrement(&g_cRefThisDll);
}

BOOL WINAPI HashSaveStartThread( PHASHSAVECONTEXT phsctx )
{
	HANDLE hThread = CreateThreadCRT(HashSaveThread, phsctx);

	if (hThread)
	{
		CloseHandle(hThread);
		return(TRUE);
	}

	return(FALSE);
}

HRESULT WINAPI HashSaveRunSilent( HWND hWndOwner, PHASHSAVESILENTSPEC pSpec )
{
	if (!pSpec || !pSpec->hListRaw || !pSpec->pszOutputPath)
		return(E_INVALIDARG);

	HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninitialize = SUCCEEDED(hrCoInit);
	if (FAILED(hrCoInit) && hrCoInit != RPC_E_CHANGED_MODE)
		return(hrCoInit);

	ULONG_PTR uActCtxCookie = ActivateManifest(TRUE);

	PHASHSAVECONTEXT phsctx = HashSaveCreateContext(hWndOwner, pSpec->hListRaw);
	if (!phsctx)
	{
		DeactivateManifest(uActCtxCookie);
		if (bCoUninitialize)
			CoUninitialize();
		return(E_OUTOFMEMORY);
	}

	phsctx->dwFlags |= HSF_HEADLESS;
	if (pSpec->bBypassQueue)
		phsctx->dwFlags |= HCF_BYPASS_QUEUE;
	phsctx->hWnd = NULL;
	phsctx->status = ACTIVE;
	phsctx->hList = NULL;
	HashCalcPrepare(phsctx);

	HCNormalizeString(pSpec->pszOutputPath);
	StrTrimW(pSpec->pszOutputPath, L" ");

	HRESULT hr = S_OK;
	if (!HashCalcInitSaveToFile(phsctx, pSpec->pszOutputPath, pSpec->uHashIndex,
	                            pSpec->iSaveEncoding, pSpec->iSaveEol))
	{
		hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_CANNOT_MAKE);
	}
	else if (!(phsctx->hList = SLCreateEx(TRUE)))
	{
		hr = E_OUTOFMEMORY;
	}
	else
	{
		HashSaveWorkerMain(phsctx);
		if (phsctx->status == CANCEL_REQUESTED)
			hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
		else if (phsctx->dwFlags & HSF_WORKER_FAILED)
			hr = HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY);

		SLRelease(phsctx->hList);
		phsctx->hList = NULL;
	}

	if (phsctx->hFileOut != INVALID_HANDLE_VALUE)
	{
		if (FAILED(hr))
			HashCalcDeleteFileByHandle(phsctx->hFileOut);

		CloseHandle(phsctx->hFileOut);
		phsctx->hFileOut = INVALID_HANDLE_VALUE;
	}

	HashSaveReleaseContext(phsctx);

	DeactivateManifest(uActCtxCookie);
	if (bCoUninitialize)
		CoUninitialize();

	return(hr);
}

DWORD WINAPI HashSaveThread( PHASHSAVECONTEXT phsctx )
{
	HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninitialize = SUCCEEDED(hrCoInit);

	// First, activate our manifest and AddRef our host
	ULONG_PTR uActCtxCookie = ActivateManifest(TRUE);
	ULONG_PTR uHostCookie = HostAddRef();

	phsctx->status = INACTIVE;
	ZeroMemory(&phsctx->ofn, sizeof(phsctx->ofn));

	if (phsctx->bSeparateFiles)
	{
		HashCalcInitSaveSeparate(phsctx);
	}
	else
	{
		// Calling HashCalcPrepare with a NULL hList will cause it to calculate
		// and set cchPrefix, but it will not copy the data or walk the directories
		// (we will leave that for the worker thread to do); the reason we do a
		// limited scan now is so that we can show the file dialog (which requires
		// cchPrefix for the automatic name generation) as soon as possible
		phsctx->hList = NULL;
		HashCalcPrepare(phsctx);

		// Get a file name from the user
		HashCalcInitSave(phsctx);
	}

	if (phsctx->bSeparateFiles ? phsctx->ofn.nFilterIndex : (phsctx->hFileOut != INVALID_HANDLE_VALUE))
	{
        BOOL bDeletionFailed = TRUE;
		if (phsctx->hList = SLCreateEx(TRUE))
		{
            bDeletionFailed = ! DialogBoxParam(
				g_hModThisDll,
				MAKEINTRESOURCE(IDD_HASHSAVE),
				NULL,
				HashSaveDlgProc,
				(LPARAM)phsctx
			);

			SLRelease(phsctx->hList);
		}

		if (!phsctx->bSeparateFiles)
		{
			CloseHandle(phsctx->hFileOut);

			// Should only happen on Windows XP
			if (bDeletionFailed)
				DeleteFile(phsctx->ofn.lpstrFile);
		}

		if (phsctx->cFileOutErrors)
		{
			TCHAR szMessage[MAX_STRINGMSG];
			LoadString(g_hModThisDll, IDS_HC_SAVE_ERROR, szMessage, countof(szMessage));
			MessageBox(NULL, szMessage, NULL, MB_OK | MB_ICONERROR);
		}
	}

	// This must be the last thing that we free, since this is what supports
	// our context!
	SLRelease(phsctx->hListRaw);

	// Clean up the manifest activation and release our host
	DeactivateManifest(uActCtxCookie);
	HostRelease(uHostCookie);

	InterlockedDecrement(&g_cRefThisDll);
	if (bCoUninitialize)
		CoUninitialize();
	return(0);
}



/*============================================================================*\
	Worker thread
\*============================================================================*/

VOID __fastcall HashSaveWorkerMain( PHASHSAVECONTEXT phsctx )
{
	// Note that ALL message communication to and from the main window MUST
	// be asynchronous, or else there may be a deadlock.
	const bool bHeadless = (phsctx->dwFlags & HSF_HEADLESS) != 0;

	HANDLE hJobSlot = WorkerThreadAcquireJobSlot((PCOMMONCONTEXT)phsctx);
	if (!hJobSlot)
		return;

	JobSlotGuard jobSlotGuard(hJobSlot);

	// Prep: expand directories, max path, etc. (prefix was set by earlier call)
	if (!bHeadless)
		PostMessage(phsctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phsctx, TRUE);
	if (! HashCalcPrepare(phsctx))
	{
		phsctx->dwFlags |= HSF_WORKER_FAILED;
        return;
	}
    HashCalcSetSaveFormat(phsctx);
	if (!bHeadless)
		PostMessage(phsctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phsctx, FALSE);

    // Extract the slist into a vector for parallel_for_each
    std::vector<PHASHSAVEITEM> vecpItems;
    vecpItems.resize(phsctx->cTotal + 1);
    SLBuildIndex(phsctx->hList, (PVOID*)vecpItems.data());
    assert(vecpItems.back() == nullptr);
    vecpItems.pop_back();
    assert(vecpItems.empty() || vecpItems.back() != nullptr);

    phsctx->dwReadBufferSize = GetReadBufferSizeForPath(vecpItems.empty() ? NULL : vecpItems[0]->szPath);

#ifdef USE_PPL
    const bool bMultithreaded = vecpItems.size() > 1 && phsctx->dwReadBufferSize == READ_BUFFER_SIZE_SSD;
    concurrency::concurrent_vector<void*> vecBuffers;  // a vector of all allocated read buffers (one per thread)
    DWORD dwBufferTlsIndex = TlsAlloc();               // TLS index of the current thread's read buffer
    if (dwBufferTlsIndex == TLS_OUT_OF_INDEXES)
	{
		phsctx->dwFlags |= HSF_WORKER_FAILED;
        return;
	}
#else
    constexpr bool bMultithreaded = false;
#endif
    phsctx->bOuterMultithreaded = bMultithreaded ? TRUE : FALSE;

    PBYTE pbTheBuffer;  // file read buffer, used iff not multithreaded
    if (! bMultithreaded)
    {
        pbTheBuffer = (PBYTE)malloc(phsctx->dwReadBufferSize);
        if (pbTheBuffer == NULL)
		{
			phsctx->dwFlags |= HSF_WORKER_FAILED;
            return;
		}
    }

    // Initialize the progress bar update synchronization vars
    CRITICAL_SECTION updateCritSec;
    volatile ULONGLONG cbCurrentMaxSize = 0;
    if (bMultithreaded)
        InitializeCriticalSection(&updateCritSec);

#ifdef _TIMED
    DWORD dwStarted;
    dwStarted = GetTickCount();
#endif

    class CanceledException {};

    // concurrency::parallel_for_each(vecpItems.cbegin(), vecpItems.cend(), ...
    auto per_file_worker = [&](PHASHSAVEITEM pItem)
	{
        WHCTXEX whctx;

        // Indicate which hash type we are after, see WHEX... values in WinHash.h
        whctx.dwFlags = 1 << (phsctx->ofn.nFilterIndex - 1);

        PBYTE pbBuffer;
#ifdef USE_PPL
        if (bMultithreaded)
        {
            // Allocate or retrieve the already-allocated read buffer for the current thread
            pbBuffer = (PBYTE)TlsGetValue(dwBufferTlsIndex);
            if (pbBuffer == NULL)
            {
                pbBuffer = (PBYTE)malloc(phsctx->dwReadBufferSize);
                if (pbBuffer == NULL)
				{
					phsctx->dwFlags |= HSF_WORKER_FAILED;
                    throw CanceledException();
				}
                // Cache the read buffer for the current thread
                vecBuffers.push_back(pbBuffer);
                TlsSetValue(dwBufferTlsIndex, pbBuffer);
            }
        }
#endif

#pragma warning(push)
#pragma warning(disable: 4700 4703)  // potentially uninitialized local pointer variable 'pbBuffer' used
		// Get the hash
		WorkerThreadHashFile(
			(PCOMMONCONTEXT)phsctx,
			pItem->szPath,
			&whctx,
			&pItem->results,
            bMultithreaded ? pbBuffer : pbTheBuffer,
			NULL, 0,
            bMultithreaded ? &updateCritSec : NULL, &cbCurrentMaxSize
#ifdef _TIMED
          , &pItem->dwElapsed
#endif
        );

        if (phsctx->status == PAUSED)
            WaitForSingleObject(phsctx->hUnpauseEvent, INFINITE);
		if (phsctx->status == CANCEL_REQUESTED)
            throw CanceledException();

		// Update the UI
		if (!bHeadless)
		{
			InterlockedIncrement(&phsctx->cSentMsgs);
			PostMessage(phsctx->hWnd, HM_WORKERTHREAD_UPDATE, (WPARAM)phsctx, (LPARAM)pItem);
		}
    };
#pragma warning(pop)

    bool bHashingCompleted = false;
    try
    {
#ifdef USE_PPL
        if (bMultithreaded)
            concurrency::parallel_for_each(vecpItems.cbegin(), vecpItems.cend(), per_file_worker);
        else
#endif
            std::for_each(vecpItems.cbegin(), vecpItems.cend(), per_file_worker);
        bHashingCompleted = true;
    }
    catch (CanceledException) {}  // ignore cancellation requests

    if (bHashingCompleted && phsctx->status != CANCEL_REQUESTED)
    {
        std::stable_sort(
            vecpItems.begin(),
            vecpItems.end(),
            [phsctx](PHASHSAVEITEM pItemA, PHASHSAVEITEM pItemB)
            {
                return(StrCmpLogical(
                    pItemA->szPath + phsctx->cchAdjusted,
                    pItemB->szPath + phsctx->cchAdjusted) < 0);
            }
        );

        for (PHASHSAVEITEM pItem : vecpItems)
		{
			if (phsctx->status == CANCEL_REQUESTED)
				break;

			if (phsctx->bSeparateFiles)
			{
				if (!HashSaveWriteSeparateResult(phsctx, pItem))
					InterlockedIncrement(&phsctx->cFileOutErrors);
			}
			else
			{
				HashCalcWriteResult(phsctx, pItem);
			}
		}
    }

#ifdef _TIMED
    if (bHashingCompleted && phsctx->cTotal > 1 && phsctx->status != CANCEL_REQUESTED && !phsctx->bSeparateFiles)
    {
        union {
            CHAR  szA[MAX_STRINGMSG];
            WCHAR szW[MAX_STRINGMSG];
        } buffer;
        size_t cbBufferLeft;
        if (phsctx->opt.dwSaveEncoding == 1)  // UTF-16
        {
            StringCbPrintfExW(
                buffer.szW,
                sizeof(buffer),
                NULL,
                &cbBufferLeft,
                0,
                L"; Total elapsed: %d ms%s",
                GetTickCount() - dwStarted,
                phsctx->opt.dwSaveEol == 1 ? L"\n" : L"\r\n");
        }
        else                                  // UTF-8 or ANSI
        {
            StringCbPrintfExA(
                buffer.szA,
                sizeof(buffer),
                NULL,
                &cbBufferLeft,
                0,
                "; Total elapsed: %d ms%s",
                GetTickCount() - dwStarted,
                phsctx->opt.dwSaveEol == 1 ? "\n" : "\r\n");
        }
        DWORD dwUnused;
        WriteFile(phsctx->hFileOut, buffer.szA, (DWORD) (sizeof(buffer) - cbBufferLeft), &dwUnused, NULL);
    }
#endif

#ifdef USE_PPL
    if (bMultithreaded)
    {
        for (void* pBuffer : vecBuffers)
            free(pBuffer);
        DeleteCriticalSection(&updateCritSec);
    }
    else
#endif
        free(pbTheBuffer);
}


BOOL WINAPI HashSaveWriteSeparateResult( PHASHSAVECONTEXT phsctx, PHASHSAVEITEM pItem )
{
	TCHAR szOutputPath[MAX_PATH_BUFFER + 16];
	HANDLE hFileOut = INVALID_HANDLE_VALUE;

	if (!HashCalcBuildSeparateOutputPath(phsctx, pItem->szPath, szOutputPath, countof(szOutputPath)))
	{
		return(FALSE);
	}

	DWORD dwCreationDisposition =
		phsctx->wIfExists == IFEXISTS_OVERWRITE ? CREATE_ALWAYS : CREATE_NEW;

	hFileOut = CreateFileWithLongPathRetry(
		szOutputPath,
		FILE_APPEND_DATA | DELETE,
		FILE_SHARE_READ,
		NULL,
		dwCreationDisposition,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hFileOut == INVALID_HANDLE_VALUE)
	{
		if (phsctx->wIfExists == IFEXISTS_KEEP && GetLastError() == ERROR_FILE_EXISTS)
			return(TRUE);

		return(FALSE);
	}

	if (phsctx->opt.dwSaveEncoding == 1)
	{
		WCHAR BOM = 0xFEFF;
		DWORD cbWritten = 0;
		if (!WriteFile(hFileOut, &BOM, sizeof(WCHAR), &cbWritten, NULL) ||
		    cbWritten != sizeof(WCHAR))
		{
			goto fail_and_delete;
		}
	}

	if (phsctx->status == CANCEL_REQUESTED)
		goto cancel_and_delete;

	if (!HashCalcWriteResultToFile(phsctx, hFileOut, pItem, NULL))
		goto fail_and_delete;

	CloseHandle(hFileOut);
	return(TRUE);

cancel_and_delete:
	HashCalcDeleteFileByHandle(hFileOut);
	CloseHandle(hFileOut);
	DeleteFile(szOutputPath);
	return(TRUE);

fail_and_delete:
	HashCalcDeleteFileByHandle(hFileOut);
	CloseHandle(hFileOut);
	DeleteFile(szOutputPath);
	return(FALSE);
}



/*============================================================================*\
	Dialog general
\*============================================================================*/

INT_PTR CALLBACK HashSaveDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHSAVECONTEXT phsctx;

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			phsctx = (PHASHSAVECONTEXT)lParam;

			// Associate the window with the context and vice-versa
			phsctx->hWnd = hWnd;
			SetWindowLongPtr(hWnd, DWLP_USER, (LONG_PTR)phsctx);

			SetAppIDForWindow(hWnd, TRUE);

			HashSaveDlgInit(phsctx);

			phsctx->pfnWorkerMain = (PFNWORKERMAIN)HashSaveWorkerMain;
			phsctx->hThread = CreateThreadCRT(NULL, phsctx);

			if (!phsctx->hThread)
			{
				WorkerThreadCleanup((PCOMMONCONTEXT)phsctx);
                BOOL bDeleted = phsctx->bSeparateFiles ? TRUE : HashCalcDeleteFileByHandle(phsctx->hFileOut);
				EndDialog(hWnd, bDeleted);
				return(TRUE);
			}

			{
				const DWORD dwFastDismissTimeout = 1000;
				DWORD dwStarted = GetTickCount();

				// Preserve the old auto-dismiss polish for fast save jobs, but
				// stop waiting as soon as this worker reports that it is queued.
				for (;;)
				{
					DWORD dwElapsed = GetTickCount() - dwStarted;
					DWORD dwWait;

					if (dwElapsed >= dwFastDismissTimeout)
						break;

					dwWait = WaitForSingleObject(
						phsctx->hThread,
						(dwFastDismissTimeout - dwElapsed > 50) ? 50 : dwFastDismissTimeout - dwElapsed
					);

					if (dwWait == WAIT_OBJECT_0)
					{
						WorkerThreadCleanup((PCOMMONCONTEXT)phsctx);
						EndDialog(hWnd, TRUE);
						break;
					}

					if (dwWait != WAIT_TIMEOUT ||
					    (!(phsctx->dwFlags & HCF_BYPASS_QUEUE) &&
					     InterlockedCompareExchange(&phsctx->status, 0, 0) == QUEUED))
					{
						break;
					}
				}
			}

			return(TRUE);
		}

		case WM_DESTROY:
		{
			SetAppIDForWindow(hWnd, FALSE);
			break;
		}

		case WM_ENDSESSION:
        {
            if (wParam == FALSE)  // if TRUE, fall through to WM_CLOSE
                break;
        }
		case WM_CLOSE:
		{
			phsctx = (PHASHSAVECONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			goto cleanup_and_exit;
		}

		case WM_COMMAND:
		{
			phsctx = (PHASHSAVECONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			switch (LOWORD(wParam))
			{
				case IDC_PAUSE:
				{
					if (WorkerThreadIsRunNowAvailable((PCOMMONCONTEXT)phsctx))
						WorkerThreadRequestRunNow((PCOMMONCONTEXT)phsctx);
					else
						WorkerThreadTogglePause((PCOMMONCONTEXT)phsctx);
					return(TRUE);
				}

				case IDC_CANCEL:
				{
					cleanup_and_exit:
					phsctx->dwFlags |= HCF_EXIT_PENDING;
					WorkerThreadStop((PCOMMONCONTEXT)phsctx);
					WorkerThreadCleanup((PCOMMONCONTEXT)phsctx);

                    // Don't keep partially generated checksum files
                    BOOL bDeleted = phsctx->bSeparateFiles ? TRUE : HashCalcDeleteFileByHandle(phsctx->hFileOut);
					phsctx->cFileOutErrors = 0;

					EndDialog(hWnd, bDeleted);
					break;
				}
			}

			break;
		}

		case WM_TIMER:
		{
			// Vista: Workaround to fix their buggy progress bar
			KillTimer(hWnd, TIMER_ID_PAUSE);
			phsctx = (PHASHSAVECONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phsctx->status == PAUSED || phsctx->status == QUEUED)
				SetProgressBarPause((PCOMMONCONTEXT)phsctx, PBST_PAUSED);
			return(TRUE);
		}

		case HM_WORKERTHREAD_DONE:
		{
			phsctx = (PHASHSAVECONTEXT)wParam;
			WorkerThreadCleanup((PCOMMONCONTEXT)phsctx);
			EndDialog(hWnd, TRUE);
			return(TRUE);
		}

		case HM_WORKERTHREAD_UPDATE:
		{
			phsctx = (PHASHSAVECONTEXT)wParam;
			++phsctx->cHandledMsgs;
			SendMessage(phsctx->hWndPBTotal, PBM_SETPOS, phsctx->cHandledMsgs, 0);
			return(TRUE);
		}

		case HM_WORKERTHREAD_TOGGLEPREP:
		{
			HashCalcTogglePrep((PHASHSAVECONTEXT)wParam, (BOOL)lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_QUEUESTATE:
		{
			phsctx = (PHASHSAVECONTEXT)wParam;
			WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phsctx, FALSE);
			if ((BOOL)lParam)
			{
				WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phsctx, TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HS_RUN_NOW);
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetProgressBarPause((PCOMMONCONTEXT)phsctx, PBST_PAUSED);
			}
			else if (!(phsctx->dwFlags & HCF_EXIT_PENDING) && phsctx->status != CANCEL_REQUESTED)
			{
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HS_PAUSE);
				SetProgressBarPause((PCOMMONCONTEXT)phsctx, PBST_NORMAL);
			}
			return(TRUE);
		}
	}

	return(FALSE);
}

VOID WINAPI HashSaveDlgInit( PHASHSAVECONTEXT phsctx )
{
	HWND hWnd = phsctx->hWnd;

	// Load strings
	{
		SetControlText(hWnd, IDC_PAUSE, IDS_HS_PAUSE);
		SetControlText(hWnd, IDC_CANCEL, IDS_HS_CANCEL);
	}

	// Set the window icon and title
	{
		if (phsctx->bSeparateFiles)
		{
			LoadString(g_hModThisDll, IDS_HS_MENUTEXT_SEP, phsctx->scratch.sz, countof(phsctx->scratch.sz));
			PTSTR pszSrc = phsctx->scratch.sz;
			PTSTR pszDest = phsctx->scratch.sz;
			while (*pszSrc && *pszSrc != TEXT('(') && *pszSrc != TEXT('.'))
			{
				if (*pszSrc != TEXT('&'))
					*pszDest++ = *pszSrc;
				++pszSrc;
			}
			*pszDest = 0;
		}
		else
		{
			PTSTR pszFileName = phsctx->ofn.lpstrFile + phsctx->ofn.nFileOffset;
			TCHAR szFormat[MAX_STRINGRES];
			LoadString(g_hModThisDll, IDS_HS_TITLE_FMT, szFormat, countof(szFormat));
			StringCchPrintf(phsctx->scratch.sz, countof(phsctx->scratch.sz), szFormat, pszFileName);
		}

		SendMessage(
			hWnd,
			WM_SETTEXT,
			0,
			(LPARAM)phsctx->scratch.sz
		);

		SendMessage(
			hWnd,
			WM_SETICON,
			ICON_BIG, // No need to explicitly set the small icon
			(LPARAM)LoadIcon(g_hModThisDll, MAKEINTRESOURCE(IDI_FILETYPE))
		);
	}

	// Initialize miscellaneous stuff
	{
		DWORD dwQueueFlags = phsctx->dwFlags & HCF_BYPASS_QUEUE;
		phsctx->dwFlags = dwQueueFlags;
		phsctx->cTotal = 0;
		phsctx->cFileOutErrors = 0;
        phsctx->hThread = NULL;
        phsctx->hUnpauseEvent = NULL;
    }
}
