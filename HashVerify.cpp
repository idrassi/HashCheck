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
#include "SetAppID.h"
#include "UnicodeHelpers.h"
#include <uxtheme.h>
#include <Strsafe.h>
#include <cassert>
#include <algorithm>
#include <string>
#include <new>
#include <stdexcept>
#include <windowsx.h>
#ifdef USE_PPL
#include <ppl.h>
#include <concurrent_vector.h>
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define HV_COL_FILENAME 0
#define HV_COL_SIZE     1
#define HV_COL_STATUS   2
#define HV_COL_EXPECTED 3
#define HV_COL_ACTUAL   4
#define HV_COL_FIRST    HV_COL_FILENAME
#define HV_COL_LAST     HV_COL_ACTUAL

#define HV_STATUS_NULL       0
#define HV_STATUS_MATCH      1
#define HV_STATUS_MISMATCH   2
#define HV_STATUS_UNREADABLE 3

#define HV_PREFLIGHT_BUDGET_MS 500
#define HV_PREFLIGHT_PROBE_RUNNING   0
#define HV_PREFLIGHT_PROBE_DONE      1
#define HV_PREFLIGHT_PROBE_ABANDONED 2

#define LISTVIEW_EXSTYLES ( LVS_EX_HEADERDRAGDROP | \
                            LVS_EX_FULLROWSELECT  | \
                            LVS_EX_LABELTIP       | \
                            LVS_EX_DOUBLEBUFFER )

// Fix up missing A/W aliases
#ifdef UNICODE
#define LPNMLVDISPINFO LPNMLVDISPINFOW
#define StrCmpLogical StrCmpLogicalW
#else
#define LPNMLVDISPINFO LPNMLVDISPINFOA
#define StrCmpLogical StrCmpIA
#endif

// Due to the stupidity of the x64 compiler, the code emitted for the non-inline
// function is not as efficient as it is on x86
#ifdef _M_IX86
#undef SSChainNCpy2
#define SSChainNCpy2 SSChainNCpy2F
#endif

enum { HV_MOVE_X = 1, HV_MOVE_Y = 2, HV_SIZE_X = 4, HV_SIZE_Y = 8 };
static const struct { UINT id; UINT flags; } HashVerifyAnchors[] =
{
	{ IDC_LIST, HV_SIZE_X | HV_SIZE_Y },
	{ IDC_SUMMARY, HV_MOVE_Y | HV_SIZE_X },
	{ IDC_MATCH_LABEL, HV_MOVE_Y }, { IDC_MATCH_RESULTS, HV_MOVE_Y },
	{ IDC_MISMATCH_LABEL, HV_MOVE_Y }, { IDC_MISMATCH_RESULTS, HV_MOVE_Y },
	{ IDC_UNREADABLE_LABEL, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_UNREADABLE_RESULTS, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_PENDING_LABEL, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_PENDING_RESULTS, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_HV_COPY, HV_MOVE_Y },
	{ IDC_PROG_TOTAL, HV_MOVE_Y | HV_SIZE_X },
	{ IDC_PROG_FILE, HV_MOVE_Y | HV_SIZE_X },
	{ IDC_PAUSE, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_STOP, HV_MOVE_X | HV_MOVE_Y },
	{ IDC_EXIT, HV_MOVE_X | HV_MOVE_Y }
};

typedef struct {
	SIZE client, minimum;
	RECT controls[countof(HashVerifyAnchors)];
	UINT initialDpi, dpi;
	LOGFONT font;
	HFONT scaledFont;
	BOOL ready;
} HASHVERIFYLAYOUT;

typedef struct {
	DWORD version;
	RECT normal; // Screen coordinates, including when the window is minimized.
	UINT dpi;
	BOOL maximized;
} HASHVERIFYPLACEMENT;

typedef struct {
	UINT               cMatch;       // number of matches
	UINT               cMismatch;    // number of mismatches
	UINT               cUnreadable;  // number of unreadable files
} HASHVERIFYPREV, *PHASHVERIFYPREV;

typedef struct {
	INT                iColumn;      // column to sort
	BOOL               bReverse;     // reverse sort?
} HASHVERIFYSORT, *PHASHVERIFYSORT;

typedef struct {
	FILESIZE           filesize;
	PTSTR              pszDisplayName;
	PTSTR              pszExpected;
	INT16              cchDisplayName;
	INT                nListviewIndex;
	BOOL               bBeenSeen;    // has the listview control asked for this item's info yet?
	BOOL               bResultReported; // set by the UI after handling the result
	UINT8              uState;
	UINT8              uStatusID;
	TCHAR              szActual[MAX_DIGEST_STRING_LENGTH];
} HASHVERIFYITEM, *PHASHVERIFYITEM, *PHVITEM, **PPHVITEM;

typedef CONST HASHVERIFYITEM **PPCHVITEM;

typedef struct {
	// Common block (see COMMONCONTEXT)
	WORKERTHREADSTATUS status;       // thread status
	DWORD              dwFlags;      // misc. status flags
	MSGCOUNT           cSentMsgs;    // number update messages sent by the worker
	MSGCOUNT           cHandledMsgs; // number update messages processed by the UI
	HWND               hWnd;         // handle of the dialog window
	HWND               hWndPBTotal;  // cache of the IDC_PROG_TOTAL progress bar handle
	HWND               hWndPBFile;   // cache of the IDC_PROG_FILE progress bar handle
	HANDLE             hThread;      // handle of the worker thread
	HANDLE             hUnpauseEvent;// handle of the event which signals when unpaused
	HANDLE             hCancelEvent; // handle of the event which signals cancellation
	PFNWORKERMAIN      pfnWorkerMain;// worker function executed by the (non-GUI) thread
	DWORD              dwReadBufferSize; // size of the read buffer, in bytes
	BOOL               bOuterMultithreaded; // TRUE when files are already hashed in parallel
	// Members specific to HashVerify
	HWND               hWndList;     // handle of the list
	HSIMPLELIST        hList;        // where we store all the data
	PPHVITEM           index;        // index of the items in the list
	PTSTR              pszPath;      // raw path, set by initial input
	PTSTR              pszFileData;  // raw file data, set by initial input
	HASHVERIFYSORT     sort;         // sort information
	BOOL               bFreshStates; // is our copy of the item states fresh?
	UINT               cTotal;       // total number of files
	UINT               cMatch;       // number of matches
	UINT               cMismatch;    // number of mismatches
	UINT               cUnreadable;  // number of unreadable files
	DWORD              dwStarted;    // GetTickCount() start time
	HASHVERIFYPREV     prev;         // previous update data, used for update coalescing
	UINT               uMaxBatch;    // maximum number of updates to coalesce
    volatile DWORD     whctxFlags;   // WinHash library dwFlags (which checksums to use)
	TCHAR              szStatus[4][MAX_STRINGRES];
	HASHVERIFYLAYOUT   layout;
} HASHVERIFYCONTEXT, *PHASHVERIFYCONTEXT;

typedef struct {
	PTSTR              pszPath;
	HMODULE            hModule;
	BOOL               bReadable;
	volatile LONG      lState;
} HASHVERIFYPREFLIGHTPROBE, *PHASHVERIFYPREFLIGHTPROBE;



/*============================================================================*\
	Function declarations
\*============================================================================*/

// Data parsing functions
__forceinline PBYTE WINAPI HashVerifyLoadData( PHASHVERIFYCONTEXT phvctx );
BOOL WINAPI HashVerifyParseData( PHASHVERIFYCONTEXT phvctx );
static VOID WINAPI HashVerifyShowLoadError( PCTSTR pszPath, DWORD dwError );
BOOL WINAPI ValidateHexSequence( PTSTR psz, UINT cch );

// Worker thread
VOID CALLBACK HashVerifyRunDLLEx( HWND hWnd, HINSTANCE hInstance,
                                  PWSTR pszCmdLine, INT nCmdShow, BOOL bBypassQueue );
DWORD WINAPI HashVerifyThreadEx( PTSTR pszPath, BOOL bBypassQueue );
static UINT __stdcall HashVerifyPreflightProbeThread( PVOID pvParam );
static BOOL WINAPI HashVerifyPreflightProbeFile( PCTSTR pszPath, DWORD dwTimeout,
                                                 HANDLE hCancelEvent,
                                                 PBOOL pbReadable, PBOOL pbCanceled );
VOID __fastcall HashVerifyWorkerMain( PHASHVERIFYCONTEXT phvctx );

// Dialog general
INT_PTR CALLBACK HashVerifyDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
VOID WINAPI HashVerifyDlgInit( PHASHVERIFYCONTEXT phvctx );

// Dialog status
VOID WINAPI HashVerifyUpdateSummary( PHASHVERIFYCONTEXT phvctx, PHASHVERIFYITEM pItem );
static HBRUSH WINAPI HashVerifySummaryColor( PHASHVERIFYCONTEXT phvctx, HDC hdc, UINT uControl );

// List management
__forceinline VOID WINAPI HashVerifyListInfo( PHASHVERIFYCONTEXT phvctx, LPNMLVDISPINFO pdi );
__forceinline LONG_PTR WINAPI HashVerifySetColor( PHASHVERIFYCONTEXT phvctx, LPNMLVCUSTOMDRAW pcd );
__forceinline LONG_PTR WINAPI HashVerifyFindItem( PHASHVERIFYCONTEXT phvctx, LPNMLVFINDITEM pfi );
__forceinline VOID WINAPI HashVerifySortColumn( PHASHVERIFYCONTEXT phvctx, LPNMLISTVIEW plv );
__forceinline VOID WINAPI HashVerifyReadStates( PHASHVERIFYCONTEXT phvctx );
__forceinline VOID WINAPI HashVerifySetStates( PHASHVERIFYCONTEXT phvctx );
INT __cdecl HashVerifySortCompare( PHASHVERIFYCONTEXT phvctx, PPCHVITEM ppItemA, PPCHVITEM ppItemB );



/*============================================================================*\
	Entry points / main functions
\*============================================================================*/

VOID CALLBACK HashVerify_RunDLLW( HWND hWnd, HINSTANCE hInstance,
                                  PWSTR pszCmdLine, INT nCmdShow )
{
	HashVerifyRunDLLEx(hWnd, hInstance, pszCmdLine, nCmdShow, FALSE);
}

VOID CALLBACK HashVerifyNoQueue_RunDLLW( HWND hWnd, HINSTANCE hInstance,
                                         PWSTR pszCmdLine, INT nCmdShow )
{
	HashVerifyRunDLLEx(hWnd, hInstance, pszCmdLine, nCmdShow, TRUE);
}

VOID CALLBACK HashVerifyRunDLLEx( HWND hWnd, HINSTANCE hInstance,
                                  PWSTR pszCmdLine, INT nCmdShow, BOOL bBypassQueue )
{
	SIZE_T cchPath = SSLenW(pszCmdLine) + 1;
	PTSTR pszPath;

	// HashVerifyThread will try to free the path passed to it, as it expects
	// it to be allocated by malloc; it also expects g_cRefThisDll to be
	// incremented by the caller.

	if (pszPath = (PTSTR)malloc(cchPath * sizeof(TCHAR)))
	{
		if (WStrToTStr(pszCmdLine, pszPath, (UINT)cchPath))
		{
			InterlockedIncrement(&g_cRefThisDll);
			HashVerifyThreadEx(pszPath, bBypassQueue);
		}
		else
		{
			free(pszPath);
		}
	}
}

DWORD WINAPI HashVerifyThread( PTSTR pszPath )
{
	return(HashVerifyThreadEx(pszPath, FALSE));
}

DWORD WINAPI HashVerifyThreadEx( PTSTR pszPath, BOOL bBypassQueue )
{
	HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninitialize = SUCCEEDED(hrCoInit);

	// We will need to free the memory allocated for the data when done
	PBYTE pbRawData;

	// First, activate our manifest and AddRef our host
	ULONG_PTR uActCtxCookie = ActivateManifest(TRUE);
	ULONG_PTR uHostCookie = HostAddRef();

	// Allocate the context data that will persist across this session
	HASHVERIFYCONTEXT hvctx;

	// It's important that we zero the memory since an initial value of zero is
	// assumed for many of the elements
	ZeroMemory(&hvctx, sizeof(hvctx));

	// Prep the path
	HCNormalizeString(pszPath);
	StrTrim(pszPath, TEXT(" "));
	hvctx.pszPath = pszPath;
	if (bBypassQueue)
		hvctx.dwFlags |= HCF_BYPASS_QUEUE;
	hvctx.dwReadBufferSize = GetReadBufferSizeForPath(pszPath);
	hvctx.bOuterMultithreaded = FALSE;

	// Load the raw data
	pbRawData = HashVerifyLoadData(&hvctx);

	BOOL bLoaded = FALSE;
	DWORD dwLoadError = GetLastError();
	if (hvctx.pszFileData)
	{
		hvctx.hList = SLCreateEx(TRUE);
		if (hvctx.hList)
		{
			bLoaded = HashVerifyParseData(&hvctx);
			if (!bLoaded) dwLoadError = GetLastError();
		}
		else
			dwLoadError = ERROR_NOT_ENOUGH_MEMORY;
	}

	if (bLoaded)
	{
		DialogBoxParam(
			g_hModThisDll,
			MAKEINTRESOURCE(IDD_HASHVERF),
			NULL,
			HashVerifyDlgProc,
			(LPARAM)&hvctx
		);
	}

	// Release potentially large buffers before asking Windows to show an error.
	if (hvctx.hList) SLRelease(hvctx.hList);
	free(pbRawData);
	if (!bLoaded && *pszPath)
		HashVerifyShowLoadError(pszPath, dwLoadError);
	free(pszPath);

	// Clean up the manifest activation and release our host
	DeactivateManifest(uActCtxCookie);
	HostRelease(uHostCookie);

	InterlockedDecrement(&g_cRefThisDll);
	if (bCoUninitialize)
		CoUninitialize();
	return(0);
}



/*============================================================================*\
	Data parsing functions
\*============================================================================*/

static VOID WINAPI HashVerifyShowLoadError( PCTSTR pszPath, DWORD dwError )
{
	TCHAR szFormat[MAX_STRINGRES], szMessage[MAX_PATH_BUFFER + 0x200], szReason[0x200];
	LoadString(g_hModThisDll, IDS_HV_LOADERROR_FMT, szFormat, countof(szFormat));
	StringCchPrintf(szMessage, MAX_PATH_BUFFER, szFormat, pszPath);
	if (dwError && FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, dwError, 0, szReason, countof(szReason), NULL))
	{
		StringCchCat(szMessage, countof(szMessage), TEXT("\r\n\r\n"));
		StringCchCat(szMessage, countof(szMessage), szReason);
	}
	MessageBox(NULL, szMessage, NULL, MB_OK | MB_ICONERROR);
}

PBYTE WINAPI HashVerifyLoadData( PHASHVERIFYCONTEXT phvctx )
{
	phvctx->pszFileData = NULL;
	HANDLE hFile = OpenFileForReading(phvctx->pszPath);
	if (hFile == INVALID_HANDLE_VALUE)
		return(NULL);

	PBYTE pbRawData = NULL;
	DWORD dwError = ERROR_SUCCESS;
	do
	{
		LARGE_INTEGER cbRawData;
		DWORD cbBytesRead;
		if (!GetFileSizeEx(hFile, &cbRawData))
		{
			dwError = GetLastError();
			break;
		}
		// The text conversion APIs use signed INT buffer lengths.
		if (cbRawData.QuadPart < 0 || cbRawData.QuadPart > MAXLONG - 1)
		{
			dwError = ERROR_FILE_TOO_LARGE;
			break;
		}
		pbRawData = (PBYTE)malloc((SIZE_T)cbRawData.LowPart + sizeof(DWORD));
		if (!pbRawData)
		{
			dwError = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}
		if (!ReadFile(hFile, pbRawData, cbRawData.LowPart, &cbBytesRead, NULL))
		{
			dwError = GetLastError();
			break;
		}
		if (cbRawData.LowPart != cbBytesRead)
		{
			dwError = ERROR_HANDLE_EOF;
			break;
		}

		// Include the terminator and the extra bytes required by IsTextUTF8.
		*((UPDWORD)(pbRawData + cbRawData.LowPart)) = 0;
		phvctx->pszFileData = BufferToWStr(&pbRawData, cbRawData.LowPart);
		if (!phvctx->pszFileData)
		{
			dwError = GetLastError();
			break;
		}
		HCNormalizeString(phvctx->pszFileData);
	} while (FALSE);

	CloseHandle(hFile);
	if (dwError != ERROR_SUCCESS)
	{
		free(pbRawData);
		pbRawData = NULL;
		SetLastError(dwError);
	}
	return(pbRawData);
}


BOOL WINAPI HashVerifyParseData( PHASHVERIFYCONTEXT phvctx )
{
	PTSTR pszData = phvctx->pszFileData;  // Points to the next line to process

	UINT cchChecksum;             // Expected length of the checksum in TCHARs
	BOOL bReverseFormat = FALSE;  // TRUE if using SFV's format of putting the checksum last
	BOOL bLinesRemaining = TRUE;  // TRUE if we have not reached the end of the data
	static const TCHAR szXXH3_64Prefix[] = TEXT("XXH3_");
	static const UINT cchXXH3_64Prefix = countof(szXXH3_64Prefix) - 1;

	// Try to determine the file type from the extension
	{
		PTSTR pszExt = StrRChr(phvctx->pszPath, NULL, TEXT('.'));

		if (pszExt)
		{
            do  // loops once; only here so there's something to break out of
            {
#define HASH_VERIFY_EXT_TYPE(alg)                           \
                if (StrCmpI(pszExt, HASH_EXT_##alg) == 0)   \
                {                                           \
                    phvctx->whctxFlags = WHEX_CHECK##alg;   \
                    cchChecksum = alg##_DIGEST_LENGTH * 2;  \
                    break;                                  \
                }
                FOR_EACH_HASH(HASH_VERIFY_EXT_TYPE)
            } while (FALSE);

            // Special case for CRC-32
            if (phvctx->whctxFlags == WHEX_CHECKCRC32)
				bReverseFormat = TRUE;
		}
	}

	while (bLinesRemaining)
	{
		PTSTR pszStartOfLine;  // First non-whitespace character of the line
		PTSTR pszEndOfLine;    // Last non-whitespace character of the line
		PTSTR pszChecksum = NULL, pszFileName = NULL;
		INT16 cchPath;         // This INCLUDES the NULL terminator!
		UINT cchChecksumPrefix = 0;

		// Step 1: Isolate the current line as a NULL-terminated string
		{
			pszStartOfLine = pszData;

			// Find the end of the line
			while (*pszData && *pszData != TEXT('\n'))
				++pszData;

			// Terminate it if necessary, otherwise flag the end of the data
			if (*pszData)
				*pszData = 0;
			else
				bLinesRemaining = FALSE;

			pszEndOfLine = pszData;

			// Strip spaces from the end of the line...
			while (--pszEndOfLine >= pszStartOfLine && *pszEndOfLine == TEXT(' '))
				*pszEndOfLine = 0;

			// ...and from the start of the line
			while (*pszStartOfLine == TEXT(' '))
				++pszStartOfLine;

			// Skip past this line's terminator; point at the remaining data
			++pszData;
		}

		// Step 2a: Parse the line as SFV
		if (bReverseFormat)
		{
			pszEndOfLine -= 7;

			if (pszEndOfLine > pszStartOfLine && ValidateHexSequence(pszEndOfLine, 8))
			{
				pszChecksum = pszEndOfLine;

				// Trim spaces between the checksum and the file name
				while (--pszEndOfLine >= pszStartOfLine && *pszEndOfLine == TEXT(' '))
					*pszEndOfLine = 0;

				// Lines that begin with ';' are comments in SFV
				if (*pszStartOfLine && *pszStartOfLine != TEXT(';'))
					pszFileName = pszStartOfLine;
			}
		}

		// Step 2b: All other file formats
		else
		{
			// If we do not know the type yet, make a stab at detecting it
			if (phvctx->whctxFlags == 0)
			{
				// 32-bit algorithms (8-byte)
				if (ValidateHexSequence(pszStartOfLine, 8))
				{
					cchChecksum = 8;
					phvctx->whctxFlags = WHEX_ALL32;  // WHEX_CHECKCRC32
				}
				// XXH3-64 GNU format (XXH3_ + 16-character hex digest)
				else if ( StrCmpNI(pszStartOfLine, szXXH3_64Prefix, cchXXH3_64Prefix) == 0 &&
				          ValidateHexSequence(pszStartOfLine + cchXXH3_64Prefix, 16) )
				{
					cchChecksumPrefix = cchXXH3_64Prefix;
					cchChecksum = 16;
					phvctx->whctxFlags = WHEX_CHECKXXH3_64;
				}
				// 64-bit algorithms (16-byte)
				else if (ValidateHexSequence(pszStartOfLine, 16))
				{
					cchChecksum = 16;
					phvctx->whctxFlags = WHEX_ALL64;  // WHEX_CHECKXXH3_64
				}
				// 128-bit algorithms (32-byte)
				else if (ValidateHexSequence(pszStartOfLine, 32))
				{
					cchChecksum = 32;
					phvctx->whctxFlags = WHEX_ALL128;  // WHEX_CHECKMD5 | WHEX_CHECKXXH3_128
				}
				// 160-bit algorithms (40-byte)
				else if (ValidateHexSequence(pszStartOfLine, 40))
				{
					cchChecksum = 40;
					phvctx->whctxFlags = WHEX_ALL160;  // WHEX_CHECKSHA1
				}
				// 256-bit algorithms (64-character)
				else if (ValidateHexSequence(pszStartOfLine, 64))
				{
					cchChecksum = 64;
					phvctx->whctxFlags = WHEX_ALL256;  // WHEX_CHECKSHA256 | WHEX_CHECKSHA3_256
				}
				// 512-bit algorithms (128-character)
				else if (ValidateHexSequence(pszStartOfLine, 128))
				{
					cchChecksum = 128;
					phvctx->whctxFlags = WHEX_ALL512;  // WHEX_CHECKSHA512 | WHEX_CHECKSHA3_512
				}
			}
			else if ( (phvctx->whctxFlags & WHEX_CHECKXXH3_64) &&
			          StrCmpNI(pszStartOfLine, szXXH3_64Prefix, cchXXH3_64Prefix) == 0 )
			{
				cchChecksumPrefix = cchXXH3_64Prefix;
			}

			// Parse the line
			if ( phvctx->whctxFlags && pszEndOfLine > pszStartOfLine + cchChecksumPrefix + cchChecksum &&
			     ValidateHexSequence(pszStartOfLine + cchChecksumPrefix, cchChecksum) )
			{
				pszChecksum = pszStartOfLine + cchChecksumPrefix;
				pszStartOfLine += cchChecksumPrefix + cchChecksum + 1;

				// Skip over spaces between the checksum and filename
				while (*pszStartOfLine == TEXT(' '))
					++pszStartOfLine;

				// Skip the GNU binary-mode filename marker
				if (*pszStartOfLine == TEXT('*'))
					++pszStartOfLine;

				if (*pszStartOfLine)
					pszFileName = pszStartOfLine;
			}
		}

		// Step 3: Do something useful with the results
		if (pszFileName && (cchPath = (INT16)(pszEndOfLine + 2 - pszFileName)) > 1)
		{
			// Since pszEndOfLine points to the character BEFORE the terminator,
			// cchLine == 1 + pszEnd - pszStart, and then +1 for the NULL
			// terminator means that we need to add 2 TCHARs to the length

			// By treating cchPath as INT16 and checking the sign, we ensure
			// that the path does not exceed 32K.

			// SimpleList takes a UINT byte count for the index allocation.
			if (phvctx->cTotal >= MAXDWORD / sizeof(PHVITEM))
				goto out_of_memory;

			PHASHVERIFYITEM pItem = (PHASHVERIFYITEM)SLAddItem(phvctx->hList, NULL, sizeof(HASHVERIFYITEM));
			if (!pItem) goto out_of_memory;

			pItem->filesize.ui64 = -1;
			pItem->filesize.sz[0] = 0;
			pItem->pszDisplayName = pszFileName;
			pItem->pszExpected = pszChecksum;
			pItem->cchDisplayName = cchPath;
			pItem->nListviewIndex = phvctx->cTotal;
			pItem->bBeenSeen = FALSE;
			pItem->bResultReported = FALSE;
			pItem->uStatusID = HV_STATUS_NULL;
			pItem->szActual[0] = 0;

			++phvctx->cTotal;

		} // If the current line was found to be valid

	} // Loop until there are no lines left

	// An empty file is valid, but an incomplete list must never be verified.
	if (phvctx->cTotal)
	{
		phvctx->index = (PPHVITEM)SLSetContextSize(phvctx->hList,
			(UINT)(phvctx->cTotal * sizeof(PHVITEM)));
		if (!phvctx->index) goto out_of_memory;
		SLBuildIndex(phvctx->hList, (PVOID*)phvctx->index);
	}
	return(TRUE);

out_of_memory:
	phvctx->cTotal = 0;
	phvctx->index = NULL;
	SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return(FALSE);
}

BOOL WINAPI ValidateHexSequence( PTSTR psz, UINT cch )
{
	// Check that the given hex string matches /[0-9A-Fa-f]{cch}\b/, and if it
	// does, convert to lower-case and NULL-terminate it.

	while (cch)
	{
		TCHAR ch = *psz;

		if (ch < TEXT('0'))
		{
			return(FALSE);
		}
		else if (ch > TEXT('9'))
		{
			ch |= 0x20; // Convert to lower-case

			if (ch < TEXT('a') || ch > TEXT('f'))
				return(FALSE);

			*psz = ch;
		}

		++psz;
		--cch;
	}

	if (*psz == 0 || *psz == TEXT('\n') || *psz == TEXT(' '))
	{
		*psz = 0;
		return(TRUE);
	}

	return(FALSE);
}



/*============================================================================*\
	Worker thread
\*============================================================================*/

static VOID WINAPI HashVerifyFreePreflightProbe( PHASHVERIFYPREFLIGHTPROBE pProbe )
{
	if (pProbe)
	{
		free(pProbe->pszPath);
		free(pProbe);
	}
}

static BOOL WINAPI HashVerifyShouldAbortPreflightProbeOpen( PVOID pvParam )
{
	PHASHVERIFYPREFLIGHTPROBE pProbe = (PHASHVERIFYPREFLIGHTPROBE)pvParam;
	return(InterlockedCompareExchange(&pProbe->lState, HV_PREFLIGHT_PROBE_RUNNING,
	                                  HV_PREFLIGHT_PROBE_RUNNING) != HV_PREFLIGHT_PROBE_RUNNING);
}

static UINT __stdcall HashVerifyPreflightProbeThread( PVOID pvParam )
{
	PHASHVERIFYPREFLIGHTPROBE pProbe = (PHASHVERIFYPREFLIGHTPROBE)pvParam;
	LARGE_INTEGER cbProbeFileSize;
	HANDLE hFile;
	HMODULE hModule;

	hFile = OpenFileForReadingAbortable(pProbe->pszPath,
	                                    HashVerifyShouldAbortPreflightProbeOpen,
	                                    pProbe);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		pProbe->bReadable = GetFileSizeEx(hFile, &cbProbeFileSize);
		CloseHandle(hFile);
	}

	if (InterlockedCompareExchange(&pProbe->lState, HV_PREFLIGHT_PROBE_DONE,
	                               HV_PREFLIGHT_PROBE_RUNNING) == HV_PREFLIGHT_PROBE_ABANDONED)
	{
		// The abandoned probe owns the loader reference until it exits.
		hModule = pProbe->hModule;
		HashVerifyFreePreflightProbe(pProbe);
		InterlockedDecrement(&g_cRefThisDll);
		FreeLibraryAndExitThread(hModule, 0);
	}

	InterlockedDecrement(&g_cRefThisDll);
	return(0);
}

static BOOL WINAPI HashVerifyPreflightProbeFile( PCTSTR pszPath, DWORD dwTimeout,
                                                 HANDLE hCancelEvent,
                                                 PBOOL pbReadable, PBOOL pbCanceled )
{
	PHASHVERIFYPREFLIGHTPROBE pProbe;
	HANDLE hThread;
	DWORD dwWait;
	SIZE_T cchPath;
	BOOL bCancelWait;

	*pbReadable = FALSE;
	*pbCanceled = FALSE;

	if (dwTimeout == 0)
		return(FALSE);

	cchPath = SSLen(pszPath) + 1;
	pProbe = (PHASHVERIFYPREFLIGHTPROBE)malloc(sizeof(HASHVERIFYPREFLIGHTPROBE));
	if (!pProbe)
		return(FALSE);

	ZeroMemory(pProbe, sizeof(HASHVERIFYPREFLIGHTPROBE));
	pProbe->pszPath = (PTSTR)malloc(cchPath * sizeof(TCHAR));
	if (!pProbe->pszPath)
	{
		free(pProbe);
		return(FALSE);
	}
	SSCpy(pProbe->pszPath, pszPath);

	// g_cRefThisDll protects COM unload checks; this protects direct FreeLibrary callers.
	if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
	                       (LPCTSTR)HashVerifyPreflightProbeThread,
	                       &pProbe->hModule))
	{
		HashVerifyFreePreflightProbe(pProbe);
		return(FALSE);
	}

	InterlockedIncrement(&g_cRefThisDll);
	hThread = (HANDLE)_beginthreadex(NULL, BASE_STACK_SIZE, HashVerifyPreflightProbeThread,
	                                 pProbe, 0, NULL);
	if (!hThread)
	{
		InterlockedDecrement(&g_cRefThisDll);
		FreeLibrary(pProbe->hModule);
		HashVerifyFreePreflightProbe(pProbe);
		return(FALSE);
	}

	if (hCancelEvent)
	{
		HANDLE rgHandles[2] = { hThread, hCancelEvent };
		dwWait = WaitForMultipleObjects(countof(rgHandles), rgHandles, FALSE, dwTimeout);
	}
	else
	{
		dwWait = WaitForSingleObject(hThread, dwTimeout);
	}

	if (dwWait == WAIT_OBJECT_0)
	{
		HMODULE hModule = pProbe->hModule;
		*pbReadable = pProbe->bReadable;
		CloseHandle(hThread);
		HashVerifyFreePreflightProbe(pProbe);
		FreeLibrary(hModule);
		return(TRUE);
	}

	bCancelWait = hCancelEvent && dwWait == WAIT_OBJECT_0 + 1;
	if (bCancelWait)
		*pbCanceled = TRUE;

	if (InterlockedCompareExchange(&pProbe->lState, HV_PREFLIGHT_PROBE_ABANDONED,
	                               HV_PREFLIGHT_PROBE_RUNNING) == HV_PREFLIGHT_PROBE_DONE)
	{
		HMODULE hModule = pProbe->hModule;
		WaitForSingleObject(hThread, INFINITE);
		if (!bCancelWait)
			*pbReadable = pProbe->bReadable;
		CloseHandle(hThread);
		HashVerifyFreePreflightProbe(pProbe);
		FreeLibrary(hModule);
		return(!bCancelWait);
	}

	CancelSynchronousIo(hThread);
	CloseHandle(hThread);
	return(FALSE);
}

VOID __fastcall HashVerifyWorkerMain( PHASHVERIFYCONTEXT phvctx )
{
	// Note that ALL message communication to and from the main window MUST
	// be asynchronous, or else there may be a deadlock

	HANDLE hJobSlot = WorkerThreadAcquireJobSlot((PCOMMONCONTEXT)phvctx);
	if (!hJobSlot)
		return;

	JobSlotGuard jobSlotGuard(hJobSlot);

	// Initialize the path prefix length; used for building the full path
	PTSTR pszPathTail = StrRChr(phvctx->pszPath, NULL, TEXT('\\'));
	SIZE_T cchPathPrefix = (pszPathTail) ? pszPathTail + 1 - phvctx->pszPath : 0;

    PCTSTR pszReadBufferPath = phvctx->pszPath;
    if (phvctx->cTotal > 0 && phvctx->index)
    {
        PCTSTR pszFirstPath = phvctx->index[0]->pszDisplayName;
        if (pszFirstPath[0] == TEXT('\\') || pszFirstPath[1] == TEXT(':'))
            pszReadBufferPath = pszFirstPath;
    }
    phvctx->dwReadBufferSize = GetReadBufferSizeForPath(pszReadBufferPath);

#ifdef USE_PPL
    const bool bMultithreaded = phvctx->cTotal > 1 && phvctx->dwReadBufferSize == READ_BUFFER_SIZE_SSD;
#else
    constexpr bool bMultithreaded = false;
#endif
    phvctx->bOuterMultithreaded = bMultithreaded ? TRUE : FALSE;

    PBYTE pbTheBuffer = (PBYTE)malloc(phvctx->dwReadBufferSize);  // filename/read buffer
    if (pbTheBuffer == NULL)
        return;

#ifdef USE_PPL
    concurrency::concurrent_vector<void*> vecBuffers;  // a vector of all allocated read buffers (one per thread)
    DWORD dwBufferTlsIndex = TLS_OUT_OF_INDEXES;       // TLS index of the current thread's read buffer
    if (bMultithreaded)
    {
        dwBufferTlsIndex = TlsAlloc();
        if (dwBufferTlsIndex == TLS_OUT_OF_INDEXES)
        {
            free(pbTheBuffer);
            return;
        }
    }
#endif

    // Initialize the progress bar update synchronization vars
    CRITICAL_SECTION updateCritSec;
    volatile ULONGLONG cbCurrentMaxSize = 0;
    if (bMultithreaded)
        InitializeCriticalSection(&updateCritSec);

	// We need to keep track of the thread's execution time so that we can do a
	// sound notification of completion when appropriate
	phvctx->dwStarted = GetTickCount();

    class CanceledException {};

    auto check_pause_cancel = [&]()
    {
        if (phvctx->status == PAUSED)
            WaitForSingleObject(phvctx->hUnpauseEvent, INFINITE);
        if (phvctx->status == CANCEL_REQUESTED)
            throw CanceledException();
    };

    auto throttle_updates = [&](DWORD dwMaxWait)
    {
        DWORD dwThrottleStarted = GetTickCount();

        while (phvctx->cSentMsgs > phvctx->cHandledMsgs + MSG_THROTTLE_THRESHOLD)
        {
            DWORD dwWaited = GetTickCount() - dwThrottleStarted;
            if (dwWaited >= dwMaxWait)
                break;

            Sleep(std::min<DWORD>(50, dwMaxWait - dwWaited));
            check_pause_cancel();
        }
    };

    auto post_item_update = [&](PHASHVERIFYITEM pItem)
    {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&phvctx->cSentMsgs));
        PostMessage(phvctx->hWnd, HM_WORKERTHREAD_UPDATE, (WPARAM)phvctx, (LPARAM)pItem);
    };

    auto build_item_path = [&](PHASHVERIFYITEM pItem, PTSTR pszPath)
    {
        SIZE_T cchPrefix = cchPathPrefix;

        // Do not use the prefix if pszDisplayName is an absolute path
        if ( pItem->pszDisplayName[0] == TEXT('\\') ||
             pItem->pszDisplayName[1] == TEXT(':') )
        {
            cchPrefix = 0;
        }

        SSChainNCpy2(
            pszPath,
            phvctx->pszPath, cchPrefix,
            pItem->pszDisplayName, pItem->cchDisplayName
        );
    };

    auto preflight_unreadable_files = [&]()
    {
        DWORD dwPreflightStarted = GetTickCount();

        for (PPHVITEM ppItem = phvctx->index; ppItem < phvctx->index + phvctx->cTotal; ++ppItem)
        {
            PHASHVERIFYITEM pItem = *ppItem;
            BOOL bReadable, bCanceled;
            DWORD dwElapsed;

            check_pause_cancel();

            // Keep preflight from becoming an unbounded startup delay.
            dwElapsed = GetTickCount() - dwPreflightStarted;
            if (dwElapsed >= HV_PREFLIGHT_BUDGET_MS)
                break;

            throttle_updates(HV_PREFLIGHT_BUDGET_MS - dwElapsed);

            dwElapsed = GetTickCount() - dwPreflightStarted;
            if (dwElapsed >= HV_PREFLIGHT_BUDGET_MS)
                break;

            build_item_path(pItem, (PTSTR)pbTheBuffer);

            if (!HashVerifyPreflightProbeFile((PTSTR)pbTheBuffer, HV_PREFLIGHT_BUDGET_MS - dwElapsed,
                                              phvctx->hCancelEvent, &bReadable, &bCanceled))
            {
                if (bCanceled)
                    throw CanceledException();
                break;
            }

            if (bReadable)
                continue;

            pItem->uStatusID = HV_STATUS_UNREADABLE;
            post_item_update(pItem);
        }
    };

    // concurrency::parallel_for_each(phvctx->index, phvctx->index + phvctx->cTotal, ...
    auto per_file_worker = [&](PHASHVERIFYITEM pItem)
	{
        if (pItem->uStatusID != HV_STATUS_NULL)
            return;

        PBYTE pbBuffer;
#ifdef USE_PPL
        if (bMultithreaded)
        {
            // Allocate or retrieve the already-allocated read buffer for the current thread
            pbBuffer = (PBYTE)TlsGetValue(dwBufferTlsIndex);
            if (pbBuffer == NULL)
            {
                pbBuffer = (PBYTE)malloc(phvctx->dwReadBufferSize);
                if (pbBuffer == NULL)
                    throw CanceledException();
                // Cache the read buffer for the current thread
                vecBuffers.push_back(pbBuffer);
                TlsSetValue(dwBufferTlsIndex, pbBuffer);
            }
        }
        else
#endif
            pbBuffer = pbTheBuffer;

		// Part 1: Build the path
        build_item_path(pItem, (PTSTR)pbBuffer);

		// Part 2: Calculate the checksum(s)
        WHCTXEX whctx;
        WHRESULTEX whres;
        whctx.dwFlags = phvctx->whctxFlags;
        whres.dwFlags = 0;
		WorkerThreadHashFile(
			(PCOMMONCONTEXT)phvctx,
            (PTSTR)pbBuffer,
			&whctx,
			&whres,
            pbBuffer,
			&pItem->filesize,
            pItem->nListviewIndex,
            bMultithreaded ? &updateCritSec : NULL, &cbCurrentMaxSize
#ifdef _TIMED
          , NULL
#endif
        );

        check_pause_cancel();

		// Part 3: Do something with the results
		if (whres.dwFlags)
		{
            UINT cHashes = 0;
            DWORD dwMatched = 0;
            PTSTR pszActual = NULL;

#define HASH_VERIFY_ONE_HASH_op(alg)                                  \
            if (whres.dwFlags & WHEX_CHECK##alg)                      \
            {                                                         \
                cHashes++;                                            \
                if (! dwMatched)                                      \
                {                                                     \
                    pszActual = whres.szHex##alg;                     \
                    if (StrCmpI(pItem->pszExpected, pszActual) == 0)  \
                        dwMatched = WHEX_CHECK##alg;                  \
                }                                                     \
            }
            FOR_EACH_HASH(HASH_VERIFY_ONE_HASH_op)

            assert(cHashes > 0);  // should always be true since whres.dwFlags > 0
            assert(pszActual);
            if (dwMatched)
            {
                pItem->uStatusID = HV_STATUS_MATCH;

                StringCbCopy(pItem->szActual, sizeof(pItem->szActual), pszActual);
                if (cHashes > 1 && phvctx->whctxFlags != dwMatched)
                    phvctx->whctxFlags = dwMatched;
            }
            else
            {
                pItem->uStatusID = HV_STATUS_MISMATCH;
                if (cHashes == 1)
                    StringCbCopy(pItem->szActual, sizeof(pItem->szActual), pszActual);
            }
		}
		else
		{
			pItem->uStatusID = HV_STATUS_UNREADABLE;
		}

		// Part 4: Update the UI
		post_item_update(pItem);
    };

    try
    {
        // Surface readily found missing/unopenable files before expensive hashing starts.
        preflight_unreadable_files();

#ifdef USE_PPL
        if (bMultithreaded)
        {
            free(pbTheBuffer);
            pbTheBuffer = NULL;
            concurrency::parallel_for_each(phvctx->index, phvctx->index + phvctx->cTotal, per_file_worker);
        }
        else
#endif
            std::for_each(phvctx->index, phvctx->index + phvctx->cTotal, per_file_worker);
    }
    catch (CanceledException) {}  // ignore cancellation requests

#ifdef USE_PPL
    if (bMultithreaded)
    {
        for (void* pBuffer : vecBuffers)
            free(pBuffer);
        if (dwBufferTlsIndex != TLS_OUT_OF_INDEXES)
            TlsFree(dwBufferTlsIndex);
        DeleteCriticalSection(&updateCritSec);
    }
#endif
    free(pbTheBuffer);

	// Play a sound to signal the normal, successful termination of operations,
	// but exempt operations that were nearly instantaneous
	if (phvctx->cTotal && GetTickCount() - phvctx->dwStarted >= 2000)
		MessageBeep(MB_ICONASTERISK);
}



/*============================================================================*\
	Dialog general
\*============================================================================*/

static UINT WINAPI HashVerifyWindowDpi(HWND hWnd)
{
	typedef UINT (WINAPI *GETDPI)(HWND);
	static GETDPI getDpi = (GETDPI)GetProcAddress(GetModuleHandle(TEXT("user32.dll")), "GetDpiForWindow");
	if (getDpi)
	{
		UINT dpi = getDpi(hWnd);
		if (dpi) return dpi;
	}
	HDC dc = GetDC(hWnd);
	UINT dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
	if (dc) ReleaseDC(hWnd, dc);
	return dpi ? dpi : 96;
}

static BOOL WINAPI HashVerifyScalesForDpi(HWND hWnd)
{
	typedef HANDLE (WINAPI *GETCONTEXT)(HWND);
	typedef int (WINAPI *GETAWARENESS)(HANDLE);
	static GETCONTEXT getContext = (GETCONTEXT)GetProcAddress(
		GetModuleHandle(TEXT("user32.dll")), "GetWindowDpiAwarenessContext");
	static GETAWARENESS getAwareness = (GETAWARENESS)GetProcAddress(
		GetModuleHandle(TEXT("user32.dll")), "GetAwarenessFromDpiAwarenessContext");
	// Windows handles bitmap scaling for hosts that aren't aware of each monitor's DPI.
	return !getContext || !getAwareness || getAwareness(getContext(hWnd)) == 2;
}

static SIZE WINAPI HashVerifyMinimumSize(PHASHVERIFYCONTEXT ctx)
{
	SIZE size = {
		MulDiv(ctx->layout.minimum.cx, ctx->layout.dpi, ctx->layout.initialDpi),
		MulDiv(ctx->layout.minimum.cy, ctx->layout.dpi, ctx->layout.initialDpi)
	};
	return size;
}

static VOID WINAPI HashVerifyLayoutInit(PHASHVERIFYCONTEXT ctx)
{
	HASHVERIFYLAYOUT *layout = &ctx->layout;
	RECT client, window;
	GetClientRect(ctx->hWnd, &client);
	GetWindowRect(ctx->hWnd, &window);
	layout->client.cx = client.right;
	layout->client.cy = client.bottom;
	layout->minimum.cx = window.right - window.left;
	layout->minimum.cy = window.bottom - window.top;
	layout->initialDpi = layout->dpi = HashVerifyWindowDpi(ctx->hWnd);
	GetObject((HFONT)SendMessage(ctx->hWnd, WM_GETFONT, 0, 0), sizeof(LOGFONT), &layout->font);
	for (UINT i = 0; i < countof(HashVerifyAnchors); ++i)
	{
		GetWindowRect(GetDlgItem(ctx->hWnd, HashVerifyAnchors[i].id), &layout->controls[i]);
		MapWindowPoints(NULL, ctx->hWnd, (POINT*)&layout->controls[i], 2);
	}
	// This dialog handles font scaling and anchoring itself on a DPI change.
	// Load the newer API dynamically so older Windows versions still work.
	typedef BOOL (WINAPI *SETDPIBEHAVIOR)(HWND, DWORD, DWORD);
	SETDPIBEHAVIOR setBehavior = (SETDPIBEHAVIOR)GetProcAddress(
		GetModuleHandle(TEXT("user32.dll")), "SetDialogDpiChangeBehavior");
	if (setBehavior) setBehavior(ctx->hWnd, 1, 1); // DDC_DISABLE_ALL
	layout->ready = TRUE;
}

static VOID WINAPI HashVerifyLayoutResize(PHASHVERIFYCONTEXT ctx)
{
	if (!ctx || !ctx->layout.ready || IsIconic(ctx->hWnd)) return;
	HASHVERIFYLAYOUT *layout = &ctx->layout;
	RECT client;
	GetClientRect(ctx->hWnd, &client);
	LONG dx = client.right - MulDiv(layout->client.cx, layout->dpi, layout->initialDpi);
	LONG dy = client.bottom - MulDiv(layout->client.cy, layout->dpi, layout->initialDpi);
	RECT rectangles[countof(HashVerifyAnchors)];
	for (UINT i = 0; i < countof(HashVerifyAnchors); ++i)
	{
		RECT rect = layout->controls[i];
		rect.left = MulDiv(rect.left, layout->dpi, layout->initialDpi);
		rect.top = MulDiv(rect.top, layout->dpi, layout->initialDpi);
		rect.right = MulDiv(rect.right, layout->dpi, layout->initialDpi);
		rect.bottom = MulDiv(rect.bottom, layout->dpi, layout->initialDpi);
		UINT flags = HashVerifyAnchors[i].flags;
		if (flags & HV_MOVE_X) OffsetRect(&rect, dx, 0);
		if (flags & HV_MOVE_Y) OffsetRect(&rect, 0, dy);
		if (flags & HV_SIZE_X) rect.right += dx;
		if (flags & HV_SIZE_Y) rect.bottom += dy;
		rectangles[i] = rect;
	}
	HDWP batch = BeginDeferWindowPos(countof(HashVerifyAnchors));
	for (UINT i = 0; batch && i < countof(HashVerifyAnchors); ++i)
	{
		const RECT *rect = &rectangles[i];
		batch = DeferWindowPos(batch, GetDlgItem(ctx->hWnd, HashVerifyAnchors[i].id), NULL,
			rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (!batch || !EndDeferWindowPos(batch))
	{
		for (UINT i = 0; i < countof(HashVerifyAnchors); ++i)
		{
			const RECT *rect = &rectangles[i];
			SetWindowPos(GetDlgItem(ctx->hWnd, HashVerifyAnchors[i].id), NULL,
				rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
	RedrawWindow(ctx->hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static VOID WINAPI HashVerifyChangeDpi(PHASHVERIFYCONTEXT ctx, UINT dpi, const RECT *suggested)
{
	if (!ctx || !ctx->layout.ready || !dpi) return;
	HASHVERIFYLAYOUT *layout = &ctx->layout;
	LOGFONT font = layout->font;
	font.lfHeight = MulDiv(font.lfHeight, dpi, layout->initialDpi);
	font.lfWidth = MulDiv(font.lfWidth, dpi, layout->initialDpi);
	HFONT scaled = CreateFontIndirect(&font);
	if (scaled)
	{
		SendMessage(ctx->hWnd, WM_SETFONT, (WPARAM)scaled, FALSE);
		for (UINT i = 0; i < countof(HashVerifyAnchors); ++i)
			SendDlgItemMessage(ctx->hWnd, HashVerifyAnchors[i].id, WM_SETFONT, (WPARAM)scaled, FALSE);
		if (layout->scaledFont) DeleteObject(layout->scaledFont);
		layout->scaledFont = scaled;
	}
	for (int column = HV_COL_FIRST; column <= HV_COL_LAST; ++column)
		ListView_SetColumnWidth(ctx->hWndList, column,
			MulDiv(ListView_GetColumnWidth(ctx->hWndList, column), dpi, layout->dpi));
	layout->dpi = dpi;
	SetWindowPos(ctx->hWnd, NULL, suggested->left, suggested->top,
		suggested->right - suggested->left, suggested->bottom - suggested->top,
		SWP_NOZORDER | SWP_NOACTIVATE);
	HashVerifyLayoutResize(ctx);
}

static BOOL WINAPI HashVerifyValidPlacement(const HASHVERIFYPLACEMENT *saved)
{
	LONGLONG width = (LONGLONG)saved->normal.right - saved->normal.left;
	LONGLONG height = (LONGLONG)saved->normal.bottom - saved->normal.top;
	return saved->version == 1 && saved->dpi >= 48 && saved->dpi <= 960 &&
		width > 0 && width <= 65535 && height > 0 && height <= 65535;
}

static RECT WINAPI HashVerifyFitWindow(RECT rect, const RECT *work, SIZE minimum)
{
	LONG width = std::min<LONG>(std::max<LONG>(rect.right - rect.left, minimum.cx), work->right - work->left);
	LONG height = std::min<LONG>(std::max<LONG>(rect.bottom - rect.top, minimum.cy), work->bottom - work->top);
	rect.left = std::max<LONG>(work->left, std::min<LONG>(rect.left, work->right - width));
	rect.top = std::max<LONG>(work->top, std::min<LONG>(rect.top, work->bottom - height));
	rect.right = rect.left + width;
	rect.bottom = rect.top + height;
	return rect;
}

static VOID WINAPI HashVerifyRestoreWindow(PHASHVERIFYCONTEXT ctx)
{
	HKEY key;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\HashCheck"), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return;
	HASHVERIFYPLACEMENT saved = {};
	DWORD type = 0, size = sizeof(saved);
	LSTATUS error = RegQueryValueEx(key, TEXT("VerifyWindow"), NULL, &type, (PBYTE)&saved, &size);
	RegCloseKey(key);
	if (error != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(saved) || !HashVerifyValidPlacement(&saved))
		return;
	MONITORINFO monitor = { sizeof(monitor) };
	if (!GetMonitorInfo(MonitorFromRect(&saved.normal, MONITOR_DEFAULTTONEAREST), &monitor)) return;
	RECT rect = HashVerifyFitWindow(saved.normal, &monitor.rcWork, HashVerifyMinimumSize(ctx));
	// Moving the hidden dialog first lets its host apply the target monitor's DPI.
	SetWindowPos(ctx->hWnd, NULL, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	rect.right = rect.left + MulDiv(saved.normal.right - saved.normal.left, ctx->layout.dpi, saved.dpi);
	rect.bottom = rect.top + MulDiv(saved.normal.bottom - saved.normal.top, ctx->layout.dpi, saved.dpi);
	rect = HashVerifyFitWindow(rect, &monitor.rcWork, HashVerifyMinimumSize(ctx));
	WINDOWPLACEMENT placement = { sizeof(placement) };
	placement.showCmd = saved.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
	placement.rcNormalPosition = rect;
	// WINDOWPLACEMENT uses workspace coordinates; our saved rectangle uses screen coordinates.
	OffsetRect(&placement.rcNormalPosition, monitor.rcMonitor.left - monitor.rcWork.left,
		monitor.rcMonitor.top - monitor.rcWork.top);
	SetWindowPlacement(ctx->hWnd, &placement);
}

static VOID WINAPI HashVerifySaveWindow(PHASHVERIFYCONTEXT ctx)
{
	if (!ctx->layout.ready) return;
	WINDOWPLACEMENT placement = { sizeof(placement) };
	MONITORINFO monitor = { sizeof(monitor) };
	if (!GetWindowPlacement(ctx->hWnd, &placement) ||
		!GetMonitorInfo(MonitorFromWindow(ctx->hWnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
	HASHVERIFYPLACEMENT saved = {};
	saved.version = 1;
	saved.normal = placement.rcNormalPosition;
	OffsetRect(&saved.normal, monitor.rcWork.left - monitor.rcMonitor.left,
		monitor.rcWork.top - monitor.rcMonitor.top);
	saved.dpi = ctx->layout.dpi;
	saved.maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
		(placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
	HKEY key;
	if (HashVerifyValidPlacement(&saved) && RegCreateKeyEx(HKEY_CURRENT_USER,
		TEXT("Software\\HashCheck"), 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS)
	{
		RegSetValueEx(key, TEXT("VerifyWindow"), 0, REG_BINARY, (PCBYTE)&saved, sizeof(saved));
		RegCloseKey(key);
	}
}

static VOID WINAPI HashVerifyBuildCopyText(PHASHVERIFYCONTEXT ctx, UINT command, std::basic_string<TCHAR>& text)
{
	if (command != IDM_HV_COPY_SELECTED)
	{
		text += ctx->pszPath;
		text += TEXT("\r\n");
		static const UINT labels[] = { IDC_MATCH_LABEL, IDC_MISMATCH_LABEL, IDC_UNREADABLE_LABEL, IDC_PENDING_LABEL };
		for (UINT i = 0; i < countof(labels); ++i)
		{
			TCHAR label[MAX_STRINGRES], value[MAX_STRINGMSG];
			GetDlgItemText(ctx->hWnd, labels[i], label, countof(label));
			GetDlgItemText(ctx->hWnd, labels[i] + 1, value, countof(value));
			text += label;
			text += TEXT("\t");
			text += value;
			text += TEXT("\r\n");
		}
		if (command == IDM_HV_COPY_SUMMARY) return;
		text += TEXT("\r\n");
	}
	static const UINT headers[] = { IDS_HV_COL_FILENAME, IDS_HV_COL_SIZE, IDS_HV_COL_STATUS, IDS_HV_COL_EXPECTED, IDS_HV_COL_ACTUAL };
	int order[] = { 0, 1, 2, 3, 4 };
	ListView_GetColumnOrderArray(ctx->hWndList, countof(order), order);
	for (UINT column = 0; column < countof(headers); ++column)
	{
		TCHAR label[MAX_STRINGRES];
		LoadString(g_hModThisDll, headers[order[column]], label, countof(label));
		if (column) text += TEXT("\t");
		text += label;
	}
	text += TEXT("\r\n");
	TCHAR pending[MAX_STRINGRES];
	LoadString(g_hModThisDll, IDS_HV_STATUS_PENDING, pending, countof(pending));
	BOOL selected = command == IDM_HV_COPY_SELECTED;
	for (int row = selected ? ListView_GetNextItem(ctx->hWndList, -1, LVNI_SELECTED) : 0;
		row >= 0 && (UINT)row < ctx->cTotal;
		row = selected ? ListView_GetNextItem(ctx->hWndList, row, LVNI_SELECTED) : row + 1)
	{
		const HASHVERIFYITEM *item = ctx->index[row];
		// Workers may already be writing the next result while its UI message is queued.
		// Only copy mutable result fields after the UI has received that item's update.
		BOOL reported = item->bResultReported;
		PCTSTR values[] = { item->pszDisplayName, reported ? item->filesize.sz : TEXT(""),
			reported ? ctx->szStatus[item->uStatusID] : pending,
			item->pszExpected, reported ? item->szActual : TEXT("") };
		for (UINT column = 0; column < countof(values); ++column)
		{
			if (column) text += TEXT("\t");
			text += values[order[column]];
		}
		text += TEXT("\r\n");
	}
}

static BOOL WINAPI HashVerifyCopyResults(PHASHVERIFYCONTEXT ctx, UINT command)
{
	std::basic_string<TCHAR> text;
	try
	{
		HashVerifyUpdateSummary(ctx, NULL);
		HashVerifyBuildCopyText(ctx, command, text);
	}
	catch (const std::bad_alloc&) { return FALSE; }
	catch (const std::length_error&) { return FALSE; }
	if (text.size() >= (SIZE_T)-1 / sizeof(TCHAR)) return FALSE;
	SIZE_T bytes = (text.size() + 1) * sizeof(TCHAR);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!memory) return FALSE;
	PVOID data = GlobalLock(memory);
	if (!data) { GlobalFree(memory); return FALSE; }
	memcpy(data, text.c_str(), bytes);
	GlobalUnlock(memory);
	BOOL copied = FALSE;
	if (OpenClipboard(ctx->hWnd))
	{
#ifdef UNICODE
		const UINT format = CF_UNICODETEXT;
#else
		const UINT format = CF_TEXT;
#endif
		if (EmptyClipboard() && SetClipboardData(format, memory)) copied = TRUE;
		CloseClipboard();
	}
	if (!copied) GlobalFree(memory);
	return copied;
}

static VOID WINAPI HashVerifyCopyCommand(PHASHVERIFYCONTEXT ctx, UINT command)
{
	if (command == IDM_HV_COPY_SELECTED && !ListView_GetSelectedCount(ctx->hWndList)) return;
	if (!HashVerifyCopyResults(ctx, command))
	{
		TCHAR message[MAX_STRINGMSG];
		LoadString(g_hModThisDll, IDS_HV_COPY_ERROR, message, countof(message));
		MessageBox(ctx->hWnd, message, NULL, MB_OK | MB_ICONERROR);
	}
}

static VOID WINAPI HashVerifyCopyMenu(PHASHVERIFYCONTEXT ctx, POINT point)
{
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	static const struct { UINT command, string; } entries[] = {
		{ IDM_HV_COPY_SELECTED, IDS_HV_COPY_SELECTED },
		{ IDM_HV_COPY_ALL, IDS_HV_COPY_ALL },
		{ IDM_HV_COPY_SUMMARY, IDS_HV_COPY_SUMMARY }
	};
	for (UINT i = 0; i < countof(entries); ++i)
	{
		TCHAR label[MAX_STRINGRES];
		LoadString(g_hModThisDll, entries[i].string, label, countof(label));
		UINT flags = MF_STRING;
		if ((entries[i].command == IDM_HV_COPY_SELECTED && !ListView_GetSelectedCount(ctx->hWndList)) ||
			(entries[i].command == IDM_HV_COPY_ALL && !ctx->cTotal)) flags |= MF_GRAYED;
		AppendMenu(menu, flags, entries[i].command, label);
	}
	UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
		point.x, point.y, 0, ctx->hWnd, NULL);
	DestroyMenu(menu);
	if (command) HashVerifyCopyCommand(ctx, command);
}

INT_PTR CALLBACK HashVerifyDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHVERIFYCONTEXT phvctx;

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			phvctx = (PHASHVERIFYCONTEXT)lParam;

			// Associate the window with the context and vice-versa
			phvctx->hWnd = hWnd;
			SetWindowLongPtr(hWnd, DWLP_USER, (LONG_PTR)phvctx);

			SetAppIDForWindow(hWnd, TRUE);

			HashVerifyDlgInit(phvctx);

			phvctx->pfnWorkerMain = (PFNWORKERMAIN)HashVerifyWorkerMain;
			phvctx->hThread = CreateThreadCRT(NULL, phvctx);

			if (!phvctx->hThread)
				WorkerThreadCleanup((PCOMMONCONTEXT)phvctx);

			// Initialize the summary
			SendMessage(phvctx->hWndPBTotal, PBM_SETRANGE32, 0, phvctx->cTotal);
			HashVerifyUpdateSummary(phvctx, NULL);
			HashVerifyRestoreWindow(phvctx);

			return(TRUE);
		}

		case WM_DESTROY:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phvctx && phvctx->layout.scaledFont)
			{
				DeleteObject(phvctx->layout.scaledFont);
				phvctx->layout.scaledFont = NULL;
			}
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
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			goto cleanup_and_exit;
		}

		case WM_COMMAND:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			switch (LOWORD(wParam))
			{
				case IDC_HV_COPY:
				{
					RECT button;
					GetWindowRect(GetDlgItem(hWnd, IDC_HV_COPY), &button);
					POINT point = { button.left, button.bottom };
					HashVerifyCopyMenu(phvctx, point);
					return(TRUE);
				}
				case IDM_HV_COPY_SELECTED:
				case IDM_HV_COPY_ALL:
				case IDM_HV_COPY_SUMMARY:
					HashVerifyCopyCommand(phvctx, LOWORD(wParam));
					return(TRUE);

				case IDC_PAUSE:
				{
					if (WorkerThreadIsRunNowAvailable((PCOMMONCONTEXT)phvctx))
						WorkerThreadRequestRunNow((PCOMMONCONTEXT)phvctx);
					else
						WorkerThreadTogglePause((PCOMMONCONTEXT)phvctx);
					return(TRUE);
				}

				case IDC_STOP:
				{
					WorkerThreadStop((PCOMMONCONTEXT)phvctx);
					return(TRUE);
				}

				case IDC_EXIT:
				{
					cleanup_and_exit:
					phvctx->dwFlags |= HCF_EXIT_PENDING;
					WorkerThreadStop((PCOMMONCONTEXT)phvctx);
					WorkerThreadCleanup((PCOMMONCONTEXT)phvctx);
					HashVerifySaveWindow(phvctx);
					EndDialog(hWnd, 0);
					break;
				}
			}

			break;
		}

		case WM_SIZE:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (wParam != SIZE_MINIMIZED) HashVerifyLayoutResize(phvctx);
			return(TRUE);
		}
		case WM_GETMINMAXINFO:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phvctx && phvctx->layout.ready)
			{
				SIZE minimum = HashVerifyMinimumSize(phvctx);
				((LPMINMAXINFO)lParam)->ptMinTrackSize.x = minimum.cx;
				((LPMINMAXINFO)lParam)->ptMinTrackSize.y = minimum.cy;
				return(TRUE);
			}
			break;
		}
		case WM_DPICHANGED:
		{
			if (!HashVerifyScalesForDpi(hWnd)) break;
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			HashVerifyChangeDpi(phvctx, LOWORD(wParam), (const RECT*)lParam);
			return(TRUE);
		}
		case WM_CONTEXTMENU:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (!phvctx) break;
			POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			if (point.x == -1 && point.y == -1)
			{
				RECT item;
				int row = ListView_GetNextItem(phvctx->hWndList, -1, LVNI_FOCUSED);
				if (row >= 0 && ListView_GetItemRect(phvctx->hWndList, row, &item, LVIR_BOUNDS))
				{
					point.x = item.left;
					point.y = item.bottom;
					ClientToScreen(phvctx->hWndList, &point);
				}
				else
				{
					GetWindowRect(phvctx->hWndList, &item);
					point.x = item.left;
					point.y = item.top;
				}
			}
			HashVerifyCopyMenu(phvctx, point);
			return(TRUE);
		}

		case WM_NOTIFY:
		{
			LPNMHDR pnm = (LPNMHDR)lParam;

			if (pnm && pnm->idFrom == IDC_LIST)
			{
				phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

				switch (pnm->code)
				{
					case LVN_KEYDOWN:
					{
						if (GetKeyState(VK_CONTROL) & 0x8000)
						{
							WORD key = ((LPNMLVKEYDOWN)lParam)->wVKey;
							if (key == 'C') HashVerifyCopyCommand(phvctx, IDM_HV_COPY_SELECTED);
							else if (key == 'A') ListView_SetItemState(phvctx->hWndList, -1, LVIS_SELECTED, LVIS_SELECTED);
						}
						break;
					}
					case LVN_GETDISPINFO:
					{
						HashVerifyListInfo(phvctx, (LPNMLVDISPINFO)lParam);
						return(TRUE);
					}
					case NM_CUSTOMDRAW:
					{
						SetWindowLongPtr(hWnd, DWLP_MSGRESULT, HashVerifySetColor(phvctx, (LPNMLVCUSTOMDRAW)lParam));
						return(TRUE);
					}
					case LVN_ODFINDITEM:
					{
						SetWindowLongPtr(hWnd, DWLP_MSGRESULT, HashVerifyFindItem(phvctx, (LPNMLVFINDITEM)lParam));
						return(TRUE);
					}
					case LVN_COLUMNCLICK:
					{
						HashVerifySortColumn(phvctx, (LPNMLISTVIEW)lParam);
						return(TRUE);
					}
					case LVN_ITEMCHANGED:
					{
						if (((LPNMLISTVIEW)lParam)->uChanged & LVIF_STATE)
							phvctx->bFreshStates = FALSE;
						break;
					}
					case LVN_ODSTATECHANGED:
					{
						phvctx->bFreshStates = FALSE;
						break;
					}
				}
			}

			break;
		}

		case WM_CTLCOLORSTATIC:
		{
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phvctx)
			{
				HBRUSH hBrush = HashVerifySummaryColor(phvctx, (HDC)wParam, GetDlgCtrlID((HWND)lParam));
				if (hBrush) return((INT_PTR)hBrush);
			}
			break;
		}

		case WM_SETTINGCHANGE:
		case WM_SYSCOLORCHANGE:
		case WM_THEMECHANGED:
		{
			RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
			break;
		}

		case WM_TIMER:
		{
			// Vista: Workaround to fix their buggy progress bar
			KillTimer(hWnd, TIMER_ID_PAUSE);
			phvctx = (PHASHVERIFYCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phvctx->status == PAUSED || phvctx->status == QUEUED)
				SetProgressBarPause((PCOMMONCONTEXT)phvctx, PBST_PAUSED);
			return(TRUE);
		}

		case HM_WORKERTHREAD_DONE:
		{
			phvctx = (PHASHVERIFYCONTEXT)wParam;
			WorkerThreadCleanup((PCOMMONCONTEXT)phvctx);
			return(TRUE);
		}

		case HM_WORKERTHREAD_UPDATE:
		{
			phvctx = (PHASHVERIFYCONTEXT)wParam;
			++phvctx->cHandledMsgs;
			HashVerifyUpdateSummary(phvctx, (PHASHVERIFYITEM)lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_SETSIZE:
		{
			phvctx = (PHASHVERIFYCONTEXT)wParam;
			assert(lParam >= 0 && (UINT)lParam < phvctx->cTotal);
			if (phvctx->index[lParam]->bBeenSeen)
				ListView_RedrawItems(phvctx->hWndList, lParam, lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_QUEUESTATE:
		{
			phvctx = (PHASHVERIFYCONTEXT)wParam;
			WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phvctx, FALSE);
			if ((BOOL)lParam)
			{
				WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phvctx, TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HV_RUN_NOW);
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetProgressBarPause((PCOMMONCONTEXT)phvctx, PBST_PAUSED);
			}
			else if (!(phvctx->dwFlags & HCF_EXIT_PENDING) && phvctx->status != CANCEL_REQUESTED)
			{
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HV_PAUSE);
				SetProgressBarPause((PCOMMONCONTEXT)phvctx, PBST_NORMAL);
			}
			return(TRUE);
		}
	}

	return(FALSE);
}

VOID WINAPI HashVerifyDlgInit( PHASHVERIFYCONTEXT phvctx )
{
	HWND hWnd = phvctx->hWnd;
	UINT i;

	// Load strings
	{
		static const UINT16 arStrMap[][2] =
		{
			{ IDC_SUMMARY,          IDS_HV_SUMMARY    },
			{ IDC_HV_COPY,          IDS_HV_COPY       },
			{ IDC_MATCH_LABEL,      IDS_HV_MATCH      },
			{ IDC_MISMATCH_LABEL,   IDS_HV_MISMATCH   },
			{ IDC_UNREADABLE_LABEL, IDS_HV_UNREADABLE },
			{ IDC_PENDING_LABEL,    IDS_HV_PENDING    },
			{ IDC_PAUSE,            IDS_HV_PAUSE      },
			{ IDC_STOP,             IDS_HV_STOP       },
			{ IDC_EXIT,             IDS_HV_EXIT       }
		};

		for (i = 0; i < countof(arStrMap); ++i)
			SetControlText(hWnd, arStrMap[i][0], arStrMap[i][1]);
	}

	// Set the window icon and title
	{
		PTSTR pszFileName = StrRChr(phvctx->pszPath, NULL, TEXT('\\'));

		if (!(pszFileName && *++pszFileName))
			pszFileName = phvctx->pszPath;

		SendMessage(
			hWnd,
			WM_SETTEXT,
			0,
			(LPARAM)pszFileName
		);

		SendMessage(
			hWnd,
			WM_SETICON,
			ICON_BIG, // No need to explicitly set the small icon
			(LPARAM)LoadIcon(g_hModThisDll, MAKEINTRESOURCE(IDI_FILETYPE))
		);
	}

	// Initialize the list box
	{
		typedef struct {
			UINT16 iStringID;
			UINT16 iAlign;
			UINT16 iWidth;
		} COLINFO, *PCOLINFO;

		static const COLINFO arCols[] =
		{
			{ IDS_HV_COL_FILENAME, LVCFMT_LEFT,  245 },
			{ IDS_HV_COL_SIZE,     LVCFMT_RIGHT,  64 },
			{ IDS_HV_COL_STATUS,   LVCFMT_CENTER, 64 },
			{ IDS_HV_COL_EXPECTED, LVCFMT_CENTER,  0 },
			{ IDS_HV_COL_ACTUAL,   LVCFMT_CENTER,  0 },
		};

		// We will be using the list window handle a lot throughout HashVerify,
		// so we should cache it to reduce the number of lookups
		phvctx->hWndList = GetDlgItem(hWnd, IDC_LIST);

		for (i = 0; i < countof(arCols); ++i)
		{
			TCHAR szBuffer[MAX_STRINGRES];
			LVCOLUMN lvc;
			RECT rc;

			LoadString(g_hModThisDll, arCols[i].iStringID, szBuffer, countof(szBuffer));

			rc.left = arCols[i].iWidth;

			if (rc.left == 0)
			{
                if (phvctx->whctxFlags & WHEX_ALL512)
                    rc.left = 512 + 20;
                else if (phvctx->whctxFlags & WHEX_ALL256)
                    rc.left = 256 + 20;
                else if (phvctx->whctxFlags & WHEX_ALL160)
                    rc.left = 160 + 20;
                else if (phvctx->whctxFlags & WHEX_ALL128)
                    rc.left = 128 + 20;
                else if (phvctx->whctxFlags & WHEX_ALL64)
                    rc.left =  64 + 20;
                else if (phvctx->whctxFlags & WHEX_ALL32)
                    rc.left =  32 + 20 + 40;  // extra size to accommodate the header labels
			}

			MapDialogRect(hWnd, &rc);

			lvc.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
			lvc.fmt = arCols[i].iAlign;
			lvc.cx = rc.left;
			lvc.pszText = szBuffer;

			ListView_InsertColumn(phvctx->hWndList, i, &lvc);
		}

		ListView_SetExtendedListViewStyle(phvctx->hWndList, LISTVIEW_EXSTYLES);
		ListView_SetItemCount(phvctx->hWndList, phvctx->cTotal);

		// Use the new-fangled list view style for Vista
		if (g_uWinVer >= 0x0600)
			SetWindowTheme(phvctx->hWndList, L"Explorer", NULL);

		phvctx->sort.iColumn = -1;
	}

	// Initialize the status strings
	{
		UINT i;

		for (i = 1; i <= 3; ++i)
		{
			LoadString(
				g_hModThisDll,
				i + (IDS_HV_STATUS_MATCH - 1),
				phvctx->szStatus[i],
				countof(phvctx->szStatus[i])
			);
		}
	}

	// Initialize miscellaneous stuff
	{
		phvctx->uMaxBatch = (phvctx->cTotal < (0x20 << 8)) ? 0x20 : phvctx->cTotal >> 8;
		phvctx->dwStarted = 0;
        phvctx->hThread = NULL;
        phvctx->hUnpauseEvent = NULL;
	}
	HashVerifyLayoutInit(phvctx);
}



/*============================================================================*\
	Dialog status
\*============================================================================*/

static HBRUSH WINAPI HashVerifySummaryColor( PHASHVERIFYCONTEXT phvctx, HDC hdc, UINT uControl )
{
	COLORREF colorText, colorBackground;
	switch (uControl)
	{
		case IDC_MATCH_LABEL:
		case IDC_MATCH_RESULTS:
			if (!phvctx->cTotal || phvctx->cMatch != phvctx->cTotal ||
				phvctx->cHandledMsgs != phvctx->cTotal)
				return(NULL);
			colorText = RGB(0x00, 0x00, 0x00);
			colorBackground = RGB(0x00, 0xE0, 0x00);
			break;

		case IDC_MISMATCH_LABEL:
		case IDC_MISMATCH_RESULTS:
			if (!phvctx->cMismatch) return(NULL);
			colorText = RGB(0xFF, 0xFF, 0xFF);
			colorBackground = RGB(0xC0, 0x00, 0x00);
			break;

		case IDC_UNREADABLE_LABEL:
		case IDC_UNREADABLE_RESULTS:
			if (!phvctx->cUnreadable) return(NULL);
			colorText = RGB(0x00, 0x00, 0x00);
			colorBackground = RGB(0xFF, 0xE0, 0x00);
			break;

		default:
			return(NULL);
	}

	HIGHCONTRAST contrast = { sizeof(contrast) };
	if (!SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
		(contrast.dwFlags & HCF_HIGHCONTRASTON))
		return(NULL);

	SetTextColor(hdc, colorText);
	SetBkColor(hdc, colorBackground);
	SetBkMode(hdc, OPAQUE);
	SetDCBrushColor(hdc, colorBackground);
	return((HBRUSH)GetStockObject(DC_BRUSH));
}

VOID WINAPI HashVerifyUpdateSummary( PHASHVERIFYCONTEXT phvctx, PHASHVERIFYITEM pItem )
{
	HWND hWnd = phvctx->hWnd;
	TCHAR szFormat[MAX_STRINGRES], szBuffer[MAX_STRINGMSG];

	// If this is not the initial update and we are lagging, and our update
	// drought is not TOO long, then we should skip the update...
    UINT cUnhandledMsgs = phvctx->cSentMsgs - phvctx->cHandledMsgs;
    BOOL bUpdateUI = pItem == NULL || cUnhandledMsgs == 0 || cUnhandledMsgs > phvctx->uMaxBatch;

	// Update the list
	if (pItem)
	{
		pItem->bResultReported = TRUE;
		switch (pItem->uStatusID)
		{
			case HV_STATUS_MATCH:
				++phvctx->cMatch;
				break;
			case HV_STATUS_MISMATCH:
				++phvctx->cMismatch;
				break;
			default:
				++phvctx->cUnreadable;
		}

		if (pItem->bBeenSeen)
		{
			ListView_RedrawItems(
				phvctx->hWndList,
				pItem->nListviewIndex,
				pItem->nListviewIndex
			);
		}
	}

	// Update the counts and progress bar
	if (bUpdateUI)
	{
		// FormatFractionalResults expects an empty format buffer on the first call
		szFormat[0] = 0;

		if (!pItem || phvctx->prev.cMatch != phvctx->cMatch)
		{
			FormatFractionalResults(szFormat, szBuffer, phvctx->cMatch, phvctx->cTotal);
			SetDlgItemText(hWnd, IDC_MATCH_RESULTS, szBuffer);
			InvalidateRect(GetDlgItem(hWnd, IDC_MATCH_LABEL), NULL, TRUE);
		}

		if (!pItem || phvctx->prev.cMismatch != phvctx->cMismatch)
		{
			FormatFractionalResults(szFormat, szBuffer, phvctx->cMismatch, phvctx->cTotal);
			SetDlgItemText(hWnd, IDC_MISMATCH_RESULTS, szBuffer);
			InvalidateRect(GetDlgItem(hWnd, IDC_MISMATCH_LABEL), NULL, TRUE);
		}

		if (!pItem || phvctx->prev.cUnreadable != phvctx->cUnreadable)
		{
			FormatFractionalResults(szFormat, szBuffer, phvctx->cUnreadable, phvctx->cTotal);
			SetDlgItemText(hWnd, IDC_UNREADABLE_RESULTS, szBuffer);
			InvalidateRect(GetDlgItem(hWnd, IDC_UNREADABLE_LABEL), NULL, TRUE);
		}

		FormatFractionalResults(szFormat, szBuffer, phvctx->cTotal - phvctx->cHandledMsgs, phvctx->cTotal);
		SetDlgItemText(hWnd, IDC_PENDING_RESULTS, szBuffer);

		SendMessage(phvctx->hWndPBTotal, PBM_SETPOS, phvctx->cHandledMsgs, 0);

		// Now that we've updated the UI, update the prev structure
		phvctx->prev.cMatch = phvctx->cMatch;
		phvctx->prev.cMismatch = phvctx->cMismatch;
		phvctx->prev.cUnreadable = phvctx->cUnreadable;
	}

	// Update the header
	if (!(phvctx->dwFlags & HVF_HAS_SET_TYPE))
	{
		PCTSTR pszSubtitle = NULL;

		switch (phvctx->whctxFlags)
		{
#define HASH_VERIFY_TITLE_op(alg)  \
			case WHEX_CHECK##alg:  pszSubtitle = HASH_NAME_##alg;  break;
            FOR_EACH_HASH(HASH_VERIFY_TITLE_op)
		}

		if (pszSubtitle)
		{
			LoadString(g_hModThisDll, IDS_HV_SUMMARY, szFormat, countof(szFormat));
#ifndef _TIMED
			StringCchPrintf(szBuffer, countof(szBuffer), TEXT("%s (%s)"), szFormat, pszSubtitle);
			phvctx->dwFlags |= HVF_HAS_SET_TYPE;
#else
            StringCchPrintf(szBuffer, countof(szBuffer), TEXT("%s (%s) - %d ms"), szFormat, pszSubtitle,
                            phvctx->dwStarted ? GetTickCount() - phvctx->dwStarted : 0);
#endif
			SetDlgItemText(hWnd, IDC_SUMMARY, szBuffer);
		}
	}
}



/*============================================================================*\
	List management
\*============================================================================*/

VOID WINAPI HashVerifyListInfo( PHASHVERIFYCONTEXT phvctx, LPNMLVDISPINFO pdi )
{
	if ((UINT)pdi->item.iItem >= phvctx->cTotal)
		return;  // Invalid index; by casting to unsigned, we also catch negatives

	if (pdi->item.mask & LVIF_TEXT)
	{
		PHASHVERIFYITEM pItem = phvctx->index[pdi->item.iItem];

		switch (pdi->item.iSubItem)
		{
			case HV_COL_FILENAME: pdi->item.pszText = pItem->pszDisplayName;              break;
			case HV_COL_SIZE:     pdi->item.pszText = pItem->filesize.sz;                 break;
			case HV_COL_STATUS:   pdi->item.pszText = phvctx->szStatus[pItem->uStatusID]; break;
			case HV_COL_EXPECTED: pdi->item.pszText = pItem->pszExpected;                 break;
			case HV_COL_ACTUAL:   pdi->item.pszText = pItem->szActual;                    break;
			default:              pdi->item.pszText = TEXT("");                           break;
		}
        if (! pItem->bBeenSeen)
            pItem->bBeenSeen = TRUE;
	}

	if (pdi->item.mask & LVIF_IMAGE)
		pdi->item.iImage = I_IMAGENONE;

	// We can (and should) ignore LVIF_STATE
}

LONG_PTR WINAPI HashVerifySetColor( PHASHVERIFYCONTEXT phvctx, LPNMLVCUSTOMDRAW pcd )
{
	switch (pcd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
			return(CDRF_NOTIFYITEMDRAW);

		case CDDS_ITEMPREPAINT:
		{
			// We need to determine the highlight state during the item stage
			// because this information becomes subitem-specific if we try to
			// retrieve it when we actually need it in the subitem stage

			if (g_uWinVer >= 0x0600 && IsAppThemed())
			{
				// Clear the highlight bit...
				phvctx->dwFlags &= ~HVF_ITEM_HILITE;

				// uItemState is buggy; if LVS_SHOWSELALWAYS is set, uItemState
				// will ALWAYS have the CDIS_SELECTED bit set, regardless of
				// whether the item is actually selected, so a more expensive
				// test for the LVIS_SELECTED bit is needed...
				if ( pcd->nmcd.uItemState & CDIS_HOT ||
				     ListView_GetItemState(pcd->nmcd.hdr.hwndFrom, pcd->nmcd.dwItemSpec, LVIS_SELECTED) )
				{
					phvctx->dwFlags |= HVF_ITEM_HILITE;
				}
			}

			return(CDRF_NOTIFYSUBITEMDRAW);
		}

		case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
		{
			PHASHVERIFYITEM pItem;

			if (pcd->nmcd.dwItemSpec >= phvctx->cTotal)
				break;  // Invalid index

			pItem = phvctx->index[pcd->nmcd.dwItemSpec];

			// By default, we use the default foreground and background colors
			// except when the item is a mismatch or is unreadable, in which
			// case, we change the foreground color
			switch (pItem->uStatusID)
			{
				case HV_STATUS_MISMATCH:
					pcd->clrText = RGB(0xC0, 0x00, 0x00);
					break;

				case HV_STATUS_UNREADABLE:
					pcd->clrText = RGB(0x80, 0x80, 0x80);
					break;

				default:
					pcd->clrText = CLR_DEFAULT;
			}

			pcd->clrTextBk = CLR_DEFAULT;

			// The status column, however, deserves special treatment
			if (pcd->iSubItem == HV_COL_STATUS)
			{
				if (phvctx->dwFlags & HVF_ITEM_HILITE)
				{
					// Vista-style highlighting means that the foreground
					// color can show through, but not the background color
					if (pItem->uStatusID == HV_STATUS_MATCH)
						pcd->clrText = RGB(0x00, 0x80, 0x00);
				}
				else
				{
					switch (pItem->uStatusID)
					{
						case HV_STATUS_MATCH:
							pcd->clrText = RGB(0x00, 0x00, 0x00);
							pcd->clrTextBk = RGB(0x00, 0xE0, 0x00);
							break;

						case HV_STATUS_MISMATCH:
							pcd->clrText = RGB(0xFF, 0xFF, 0xFF);
							pcd->clrTextBk = RGB(0xC0, 0x00, 0x00);
							break;

						case HV_STATUS_UNREADABLE:
							pcd->clrText = RGB(0x00, 0x00, 0x00);
							pcd->clrTextBk = RGB(0xFF, 0xE0, 0x00);
							break;
					}
				}
			}

			break;
		}
	}

	return(CDRF_DODEFAULT);
}

LONG_PTR WINAPI HashVerifyFindItem( PHASHVERIFYCONTEXT phvctx, LPNMLVFINDITEM pfi )
{
	PHASHVERIFYITEM pItem;
	INT cchCompare, iStart = pfi->iStart;
	LONG_PTR i;

	if (pfi->lvfi.flags & (LVFI_PARAM | LVFI_NEARESTXY))
		goto not_found;  // Unsupported search types

	if (!(pfi->lvfi.flags & (LVFI_PARTIAL | LVFI_STRING)))
		goto not_found;  // No valid search type specified

	// According to the documentation, LVFI_STRING without a corresponding
	// LVFI_PARTIAL should match the FULL string, but when the user sends
	// keyboard input (which uses a partial match), the notification does not
	// have the LVFI_PARTIAL flag, so we should just always assume LVFI_PARTIAL
	// INT cchCompare = (pfi->lvfi.flags & LVFI_PARTIAL) ? 0 : 1;
	// cchCompare += SSLen(pfi->lvfi.psz);
	// The above code should have been correct, but it is not...
	cchCompare = (INT)SSLen(pfi->lvfi.psz);

	// Fix out-of-range indices; by casting to unsigned, we also catch negatives
	if ((UINT)iStart > phvctx->cTotal)
		iStart = phvctx->cTotal;

	for (i = iStart; i < (INT)phvctx->cTotal; ++i)
	{
		pItem = phvctx->index[i];
		if (StrCmpNI(pItem->pszDisplayName, pfi->lvfi.psz, cchCompare) == 0)
			return(i);
	}

	if (pfi->lvfi.flags & LVFI_WRAP)
	{
		for (i = 0; i < iStart; ++i)
		{
			pItem = phvctx->index[i];
			if (StrCmpNI(pItem->pszDisplayName, pfi->lvfi.psz, cchCompare) == 0)
				return(i);
		}
	}

	not_found: return(-1);
}

VOID WINAPI HashVerifySortColumn( PHASHVERIFYCONTEXT phvctx, LPNMLISTVIEW plv )
{
	if (phvctx->status != CLEANUP_COMPLETED)
		return;  // Sorting is available only after the worker is done

	// Capture the current selection/focus state
	HashVerifyReadStates(phvctx);

	if (phvctx->sort.iColumn != plv->iSubItem)
	{
		// Change to a new column
		phvctx->sort.iColumn = plv->iSubItem;
		phvctx->sort.bReverse = FALSE;
		qsort_s(phvctx->index, phvctx->cTotal, sizeof(PHVITEM), (int(__cdecl*)(void*, const void*, const void*))HashVerifySortCompare, phvctx);
	}
	else if (phvctx->sort.bReverse)
	{
		// Clicking a column thrice in a row reverts to the original file order
		phvctx->sort.iColumn = -1;
		phvctx->sort.bReverse = FALSE;

		// We do need to validate phvctx->index to handle the edge case where
		// the list is really non-empty, but we are treating it as empty because
		// we could not allocate an index (qsort_s uses the given length while
		// SLBuildIndex uses the actual length); this is, admittedly, a very
		// extreme edge case, as it crops up only in an OOM situation where the
		// user tries to click-sort an empty list view!
		if (phvctx->index)
			SLBuildIndex(phvctx->hList, (PVOID*)phvctx->index);
	}
	else
	{
		// Clicking a column twice in a row reverses the order; since we are
		// just reversing the order of an already-sorted column, we can just
		// naively flip the index

		if (phvctx->index)
		{
			PHVITEM pItemTemp;
			PPHVITEM ppItemLow = phvctx->index;
			PPHVITEM ppItemHigh = phvctx->index + phvctx->cTotal - 1;

			while (ppItemHigh > ppItemLow)
			{
				pItemTemp = *ppItemLow;
				*ppItemLow = *ppItemHigh;
				*ppItemHigh = pItemTemp;
				++ppItemLow;
				--ppItemHigh;
			}
		}

		phvctx->sort.bReverse = TRUE;
	}

	// Restore the selection/focus state
	HashVerifySetStates(phvctx);

	// Update the UI
	{
		HWND hWndHeader = ListView_GetHeader(phvctx->hWndList);
		INT i;

		HDITEM hdi;
		hdi.mask = HDI_FORMAT;

		for (i = HV_COL_FIRST; i <= HV_COL_LAST; ++i)
		{
			Header_GetItem(hWndHeader, i, &hdi);
			hdi.fmt &= ~(HDF_SORTDOWN | HDF_SORTUP);
			if (phvctx->sort.iColumn == i)
				hdi.fmt |= (phvctx->sort.bReverse) ? HDF_SORTDOWN : HDF_SORTUP;
			Header_SetItem(hWndHeader, i, &hdi);
		}

		// Invalidate all items
		ListView_RedrawItems(phvctx->hWndList, 0, phvctx->cTotal);

		// Set a light gray background on the sorted column
		ListView_SetSelectedColumn(
			phvctx->hWndList,
			(phvctx->sort.iColumn != HV_COL_STATUS) ? phvctx->sort.iColumn : -1
		);

		// Unfortunately, the list does not automatically repaint all of the
		// areas affected by SetSelectedColumn, so it is necessary to force a
		// repaint of the list view's visible areas in order to avoid artifacts
		InvalidateRect(phvctx->hWndList, NULL, FALSE);
	}
}

VOID WINAPI HashVerifyReadStates( PHASHVERIFYCONTEXT phvctx )
{
	if (!phvctx->bFreshStates)
	{
		UINT i;

		for (i = 0; i < phvctx->cTotal; ++i)
		{
			phvctx->index[i]->uState = ListView_GetItemState(
				phvctx->hWndList,
				i,
				LVIS_FOCUSED | LVIS_SELECTED
			);
		}
	}
}

VOID WINAPI HashVerifySetStates( PHASHVERIFYCONTEXT phvctx )
{
	UINT i;

	// Optimize for the case where most items are unselected
	ListView_SetItemState(phvctx->hWndList, -1, 0, LVIS_FOCUSED | LVIS_SELECTED);

	for (i = 0; i < phvctx->cTotal; ++i)
	{
		if (phvctx->index[i]->uState)
		{
			ListView_SetItemState(
				phvctx->hWndList,
				i,
				phvctx->index[i]->uState,
				LVIS_FOCUSED | LVIS_SELECTED
			);
		}
	}

	phvctx->bFreshStates = TRUE;
}

INT __cdecl HashVerifySortCompare( PHASHVERIFYCONTEXT phvctx, PPCHVITEM ppItemA, PPCHVITEM ppItemB )
{
	PHASHVERIFYITEM pItemA = *(PPHVITEM)ppItemA;
	PHASHVERIFYITEM pItemB = *(PPHVITEM)ppItemB;

	switch (phvctx->sort.iColumn)
	{
		case HV_COL_FILENAME:
			return(StrCmpLogical(pItemA->pszDisplayName, pItemB->pszDisplayName));

		case HV_COL_SIZE:
			return(pItemA->filesize.ui64 < pItemB->filesize.ui64 ? -1 : (pItemA->filesize.ui64 == pItemB->filesize.ui64 ? 0 : 1));

		case HV_COL_STATUS:
		{
			static const INT8 s_iStatusSortOrder[] = {
				3, // HV_STATUS_NULL
				2, // HV_STATUS_MATCH
				0, // HV_STATUS_MISMATCH
				1  // HV_STATUS_UNREADABLE
			};

			return(s_iStatusSortOrder[pItemA->uStatusID] - s_iStatusSortOrder[pItemB->uStatusID]);
		}

		case HV_COL_EXPECTED:
			return(StrCmpI(pItemA->pszExpected, pItemB->pszExpected));

		case HV_COL_ACTUAL:
			return(StrCmpI(pItemA->szActual, pItemB->szActual));
	}

	return(0);
}
