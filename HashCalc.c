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
#include "UnicodeHelpers.h"
#include "libs/WinHash.h"
#include <Strsafe.h>

static const TCHAR SAVE_DEFAULT_NAME[] = TEXT("checksums");
#define SEP_DLG_INCLUDE_CHECKSUM_FILES 0x0100

// Due to the stupidity of the x64 compiler, the code emitted for the non-inline
// function is not as efficient as it is on x86
#ifdef _M_IX86
#undef SSChainNCpy2
#define SSChainNCpy2 SSChainNCpy2F
#endif



/*============================================================================*\
	Function declarations
\*============================================================================*/

// Path processing
VOID WINAPI HashCalcWalkDirectory( PHASHCALCCONTEXT phcctx, PTSTR pszPath, UINT cchPath );
__forceinline BOOL WINAPI IsSpecialDirectoryName( PCTSTR pszPath );
__forceinline BOOL WINAPI IsDoubleSlashPath( PCTSTR pszPath );
__forceinline UINT WINAPI HashCalcGetSharedStem( PHASHCALCCONTEXT phcctx, PCTSTR *ppszStem );
__forceinline BOOL WINAPI HasChecksumFileExtension( PCTSTR pszPath );
BOOL WINAPI HashCalcBuildSeparateOutputPath( PHASHCALCCONTEXT phcctx, PCTSTR pszPath,
                                             PTSTR pszOutputPath, UINT cchOutputPath );
BOOL WINAPI HashCalcShouldSkipSeparateInput( PHASHCALCCONTEXT phcctx, PCTSTR pszPath );

// Save helpers
INT_PTR CALLBACK HashCalcSeparateDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
__forceinline VOID WINAPI HashCalcSetSavePrefix( PHASHCALCCONTEXT phcctx, PTSTR pszSave );
__forceinline PCTSTR WINAPI HashCalcLineEnding( PHASHCALCCONTEXT phcctx );



/*============================================================================*\
	Path processing
\*============================================================================*/

BOOL WINAPI HasChecksumFileExtension( PCTSTR pszPath )
{
	PCTSTR pszExt = StrRChr(pszPath, NULL, TEXT('.'));
	if (!pszExt)
		return(FALSE);

	for (UINT i = 0; i < countof(g_szHashExtsTab); ++i)
	{
		if (StrCmpI(pszExt, g_szHashExtsTab[i]) == 0)
			return(TRUE);
	}

	return(FALSE);
}

BOOL WINAPI HashCalcBuildSeparateOutputPath( PHASHCALCCONTEXT phcctx, PCTSTR pszPath,
                                             PTSTR pszOutputPath, UINT cchOutputPath )
{
	if (!phcctx || !pszPath || !pszOutputPath ||
	    phcctx->ofn.nFilterIndex < 1 ||
	    phcctx->ofn.nFilterIndex > NUM_HASHES)
	{
		return(FALSE);
	}

	return(SUCCEEDED(StringCchCopy(pszOutputPath, cchOutputPath, pszPath)) &&
	       SUCCEEDED(StringCchCat(pszOutputPath, cchOutputPath, g_szHashExtsTab[phcctx->ofn.nFilterIndex - 1])));
}

BOOL WINAPI HashCalcShouldSkipSeparateInput( PHASHCALCCONTEXT phcctx, PCTSTR pszPath )
{
	if (!phcctx->bSeparateFiles)
		return(FALSE);

	if (!phcctx->bIncludeChecksumFiles && HasChecksumFileExtension(pszPath))
		return(TRUE);

	if (phcctx->wIfExists == IFEXISTS_KEEP)
	{
		TCHAR szOutputPath[MAX_PATH_BUFFER + 16];
		if (HashCalcBuildSeparateOutputPath(phcctx, pszPath, szOutputPath, countof(szOutputPath)) &&
		    GetFileAttributes(szOutputPath) != INVALID_FILE_ATTRIBUTES)
		{
			return(TRUE);
		}
	}

	return(FALSE);
}

BOOL WINAPI HashCalcPrepare( PHASHCALCCONTEXT phcctx )
{
	PTSTR pszPrev = NULL;
	PTSTR pszCurrent, pszCurrentEnd;
	UINT cbCurrent, cchCurrent;

	SLReset(phcctx->hListRaw);

	while (pszCurrent = SLGetDataAndStepEx(phcctx->hListRaw, &cbCurrent))
	{
		pszCurrentEnd = BYTEADD(pszCurrent, cbCurrent);
		cchCurrent = cbCurrent / sizeof(TCHAR) - 1;

		// Get rid of the trailing slash if there is one
		if (cchCurrent && *(pszCurrentEnd - 1) == TEXT('\\'))
		{
			*(--pszCurrentEnd) = 0;
			--cchCurrent;
			cbCurrent -= sizeof(TCHAR);
		}

		if (pszPrev == NULL)
		{
			// Initialize the cchPrefix (and cchMax) for the first time; since
			// we have stripped away the trailing slash (if there was one), we
			// are guaranteed that cchPrefix < cchCurrent for the first run,
			// and that cchPrefix < cchPrev for all other iterations

			PTSTR pszTail = StrRChr(pszCurrent, pszCurrentEnd, TEXT('\\'));

			if (pszTail)
				phcctx->cchPrefix = (UINT)(pszTail - pszCurrent) + 1;
			else
				phcctx->cchPrefix = 0;

			// For "\\" paths, we cannot cut off any of the first two slashes
			if (phcctx->cchPrefix == 2 && IsDoubleSlashPath(pszCurrent))
				phcctx->cchPrefix = 0;

			phcctx->cchMax = cchCurrent;
		}
		else
		{
			// Or, just update cchPrefix

			UINT i, j = 0, k = 0;

			for (i = 0; i < cchCurrent && i < phcctx->cchPrefix; ++i)
			{
				if (pszCurrent[i] != pszPrev[i])
					break;

				if (pszCurrent[i] == TEXT('\\'))
				{
					j = i + 1;
					++k;
				}
			}

			// For "\\" paths, we cannot cut off any of the first two slashes
			if (cchCurrent >= 2 && IsDoubleSlashPath(pszCurrent) && k < 3)
				phcctx->cchPrefix = 0;
			else
				phcctx->cchPrefix = j;
		}

		if (cchCurrent && phcctx->hList)
		{
			// Finally, we can do the actual work that's needed!

			DWORD dwAttrs = GetFileAttributes(pszCurrent);
			if (dwAttrs != INVALID_FILE_ATTRIBUTES && (dwAttrs & FILE_ATTRIBUTE_DIRECTORY))
			{
				if (cchCurrent < MAX_PATH_BUFFER - 2)
				{
					memcpy(phcctx->scratch.sz, pszCurrent, cbCurrent);
					HashCalcWalkDirectory(phcctx, phcctx->scratch.sz, cchCurrent);
				}
			}
			else
			{
				if (HashCalcShouldSkipSeparateInput(phcctx, pszCurrent))
				{
					continue;
				}

				PHASHCALCITEM pItem = SLAddItem(phcctx->hList, NULL, sizeof(HASHCALCITEM) + cbCurrent);

				if (pItem)
				{
                    pItem->results.dwFlags = 0;
					pItem->cchPath = cchCurrent;
					memcpy(pItem->szPath, pszCurrent, cbCurrent);

					if (phcctx->cchMax < cchCurrent)
						phcctx->cchMax = cchCurrent;

					++phcctx->cTotal;
				}
			}
		}

        if (phcctx->status == PAUSED)
            WaitForSingleObject(phcctx->hUnpauseEvent, INFINITE);
        if (phcctx->status == CANCEL_REQUESTED)
			return(FALSE);

		pszPrev = pszCurrent;
	}
    return(TRUE);
}

VOID WINAPI HashCalcWalkDirectory( PHASHCALCCONTEXT phcctx, PTSTR pszPath, UINT cchPath )
{
	HANDLE hFind;
	WIN32_FIND_DATA finddata;

	PTSTR pszPathAppend = pszPath + cchPath;
	*pszPathAppend = TEXT('\\');
	SSCpy2Ch(++pszPathAppend, TEXT('*'), 0);

	if ((hFind = FindFirstFile(pszPath, &finddata)) == INVALID_HANDLE_VALUE)
		return;

	do
	{
		// Add 1 to the length since we are also going to count the slash that
		// was added at the end of the directory
		UINT cchLeaf = (UINT)SSLen(finddata.cFileName) + 1;
		UINT cchNew = cchPath + cchLeaf;

        if (phcctx->status == PAUSED)
            WaitForSingleObject(phcctx->hUnpauseEvent, INFINITE);
		if (phcctx->status == CANCEL_REQUESTED)
			break;

		if ( (!(finddata.dwFileAttributes & FILE_ATTRIBUTE_OFFLINE)) &&
		     (cchNew < MAX_PATH_BUFFER - 2) )
		{
			SSChainNCpy(pszPathAppend, finddata.cFileName, cchLeaf);

			if (finddata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				// Directory: Recurse
				if (!IsSpecialDirectoryName(finddata.cFileName))
					HashCalcWalkDirectory(phcctx, pszPath, cchNew);
			}
			else
			{
				if (HashCalcShouldSkipSeparateInput(phcctx, pszPath))
				{
					continue;
				}

				// File: Add to the list
				UINT cbPathBuffer = (cchNew + 1) * sizeof(TCHAR);
				PHASHCALCITEM pItem = SLAddItem(phcctx->hList, NULL, sizeof(HASHCALCITEM) + cbPathBuffer);

				if (pItem)
				{
                    pItem->results.dwFlags = 0;
					pItem->cchPath = cchNew;
					memcpy(pItem->szPath, pszPath, cbPathBuffer);

					if (phcctx->cchMax < cchNew)
						phcctx->cchMax = cchNew;

					++phcctx->cTotal;
				}
			}
		}

	} while (FindNextFile(hFind, &finddata));

	FindClose(hFind);
}

BOOL WINAPI IsSpecialDirectoryName( PCTSTR pszPath )
{
	// TRUE if name is "." or ".."

	#ifdef UNICODE
	return(
		(*((UPDWORD)pszPath) == WCHARS2DWORD(L'.', 0)) ||
		(*((UPDWORD)pszPath) == WCHARS2DWORD(L'.', L'.') && pszPath[2] == 0)
	);
	#else
	return(
		(*((UPWORD)pszPath) == CHARS2WORD('.', 0)) ||
		(*((UPWORD)pszPath) == CHARS2WORD('.', '.') && pszPath[2] == 0)
	);
	#endif
}

BOOL WINAPI IsDoubleSlashPath( PCTSTR pszPath )
{
	// TRUE if string starts with "\\"

	#ifdef UNICODE
	return(*((UPDWORD)pszPath) == WCHARS2DWORD(L'\\', L'\\'));
	#else
	return(*((UPWORD)pszPath) == CHARS2WORD('\\', '\\'));
	#endif
}

UINT WINAPI HashCalcGetSharedStem( PHASHCALCCONTEXT phcctx, PCTSTR *ppszStem )
{
	PTSTR pszFirstPath, pszPath;
	PCTSTR pszFirstStem, pszFirstExt;
	UINT cchFirstStem;

	SLReset(phcctx->hListRaw);
	pszFirstPath = SLGetDataAndStep(phcctx->hListRaw);

	if (!pszFirstPath)
		return(0);

	pszFirstStem = StrRChr(pszFirstPath, NULL, TEXT('\\'));
	pszFirstStem = pszFirstStem ? pszFirstStem + 1 : pszFirstPath;
	pszFirstExt = StrRChr(pszFirstStem, NULL, TEXT('.'));
	cchFirstStem = (UINT)((pszFirstExt && pszFirstExt > pszFirstStem) ?
		pszFirstExt - pszFirstStem :
		SSLen(pszFirstStem));

	if (cchFirstStem == 0)
		return(0);

	while (pszPath = SLGetDataAndStep(phcctx->hListRaw))
	{
		PCTSTR pszStem = StrRChr(pszPath, NULL, TEXT('\\'));
		PCTSTR pszExt;
		UINT cchStem;

		pszStem = pszStem ? pszStem + 1 : pszPath;
		pszExt = StrRChr(pszStem, NULL, TEXT('.'));
		cchStem = (UINT)((pszExt && pszExt > pszStem) ?
			pszExt - pszStem :
			SSLen(pszStem));

		if (cchStem != cchFirstStem || StrCmpNI(pszStem, pszFirstStem, cchFirstStem) != 0)
			return(0);
	}

	*ppszStem = pszFirstStem;
	return(cchFirstStem);
}



/*============================================================================*\
	Save dialog
\*============================================================================*/

VOID WINAPI HashCalcInitSave( PHASHCALCCONTEXT phcctx )
{
	HWND hWnd = phcctx->hWnd;

	// We can use the extended portion of the scratch buffer for the file name
	PTSTR pszFile = (PTSTR)phcctx->scratch.ext;

	// Default result value
	phcctx->hFileOut = INVALID_HANDLE_VALUE;

	// Load settings
	phcctx->opt.dwFlags = HCOF_FILTERINDEX | HCOF_SAVEENCODING | HCOF_SAVEEOL;
	OptionsLoad(&phcctx->opt);

	// Initialize the struct for the first time, if needed
	if (phcctx->ofn.lStructSize == 0)
	{
		phcctx->ofn.lStructSize = sizeof(phcctx->ofn);
		phcctx->ofn.hwndOwner = hWnd;
		phcctx->ofn.lpstrFilter = HASH_FILE_FILTERS;
		phcctx->ofn.nFilterIndex = phcctx->opt.dwFilterIndex;
		phcctx->ofn.lpstrFile = pszFile;
		phcctx->ofn.nMaxFile = MAX_PATH_BUFFER + 10;
		phcctx->ofn.Flags = OFN_DONTADDTORECENT | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
		phcctx->ofn.lpstrDefExt = TEXT("");

		// Set the initial file name
		{
			PTSTR pszOrigPath;

			SLReset(phcctx->hListRaw);
			pszOrigPath = SLGetDataAndStep(phcctx->hListRaw);

			if (SLCheck(phcctx->hListRaw))
			{
				PCTSTR pszSharedStem;
				UINT cchSharedStem = HashCalcGetSharedStem(phcctx, &pszSharedStem);

				// Multiple items were selected in Explorer
				if (cchSharedStem)
				{
					PTSTR pszFileEnd = SSChainNCpy2(
						pszFile,
						pszOrigPath, phcctx->cchPrefix,
						pszSharedStem, cchSharedStem
					);

					*pszFileEnd = 0;
				}
				else
				{
					SSChainNCpy2(
						pszFile,
						pszOrigPath, phcctx->cchPrefix,
						SAVE_DEFAULT_NAME, countof(SAVE_DEFAULT_NAME)
					);
				}
			}
			else
			{
				// Only one item was selected in Explorer (may be a single
				// file or a directory containing multiple files)
				SSChainCpyCat(pszFile, pszOrigPath, g_szHashExtsTab[phcctx->ofn.nFilterIndex - 1]);
			}
		}
	}

	// We should also do a sanity check to make sure that the filter index
	// is set to a valid value since we depend on that to determine the format
	if ( GetSaveFileName(&phcctx->ofn) &&
	     phcctx->ofn.nFilterIndex &&
		 phcctx->ofn.nFilterIndex <= NUM_HASHES)
	{
		// Save the filter in the user's preferences
		if (phcctx->opt.dwFilterIndex != phcctx->ofn.nFilterIndex)
		{
			phcctx->opt.dwFilterIndex = phcctx->ofn.nFilterIndex;
			phcctx->opt.dwFlags = HCOF_FILTERINDEX;
			OptionsSave(&phcctx->opt);
		}

		// Extension fixup: Correct the extension to match the selected
		// type, but only if the extension was one of the 5 in the list
		if (phcctx->ofn.nFileExtension)
		{
			PTSTR pszExt = pszFile + phcctx->ofn.nFileExtension - 1;

#define HASH_EXT_CMP_OR_op(alg) StrCmpI(pszExt, HASH_EXT_##alg) == 0 ||
			if (FOR_EACH_HASH(HASH_EXT_CMP_OR_op) FALSE)  // the FALSE is to ignore the last trailing ||
			{
				if (StrCmpI(pszExt, g_szHashExtsTab[phcctx->ofn.nFilterIndex - 1]))
					SSCpy(pszExt, g_szHashExtsTab[phcctx->ofn.nFilterIndex - 1]);
			}
		}

		// Adjust the file paths for the output path, if necessary
		HashCalcSetSavePrefix(phcctx, pszFile);

		// Open the file for output
		phcctx->hFileOut = CreateFileWithLongPathRetry(
			pszFile,
			FILE_APPEND_DATA | DELETE,
			FILE_SHARE_READ,
			NULL,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);

		if (phcctx->hFileOut != INVALID_HANDLE_VALUE)
		{
			// The actual format will be set when HashCalcWriteResult is called
			phcctx->szFormat[0] = 0;

			if (phcctx->opt.dwSaveEncoding == 1)
			{
				// Write the BOM for UTF-16LE
				WCHAR BOM = 0xFEFF;
				DWORD cbWritten;
				WriteFile(phcctx->hFileOut, &BOM, sizeof(WCHAR), &cbWritten, NULL);
			}
		}
		else
		{
			TCHAR szMessage[MAX_STRINGMSG];
			LoadString(g_hModThisDll, IDS_HC_SAVE_ERROR, szMessage, countof(szMessage));
			MessageBox(hWnd, szMessage, NULL, MB_OK | MB_ICONERROR);
		}
	}
}

BOOL WINAPI HashCalcInitSaveToFile( PHASHCALCCONTEXT phcctx, PTSTR pszSaveFile,
                                    UINT uFilterIndex, INT iSaveEncoding,
                                    INT iSaveEol )
{
	if (!pszSaveFile || !*pszSaveFile || !uFilterIndex || uFilterIndex > NUM_HASHES)
		return(FALSE);

	phcctx->hFileOut = INVALID_HANDLE_VALUE;

	phcctx->opt.dwFlags = HCOF_SAVEENCODING | HCOF_SAVEEOL;
	OptionsLoad(&phcctx->opt);

	if (iSaveEncoding >= 0 && iSaveEncoding < 3)
		phcctx->opt.dwSaveEncoding = (DWORD)iSaveEncoding;

	if (iSaveEol >= 0 && iSaveEol < 2)
		phcctx->opt.dwSaveEol = (DWORD)iSaveEol;

	phcctx->ofn.lpstrFile = pszSaveFile;
	phcctx->ofn.nFilterIndex = uFilterIndex;

	PTSTR pszFileName = StrRChr(pszSaveFile, NULL, TEXT('\\'));
	phcctx->ofn.nFileOffset = pszFileName ? (WORD)(pszFileName - pszSaveFile + 1) : 0;

	PTSTR pszExt = StrRChr(pszSaveFile + phcctx->ofn.nFileOffset, NULL, TEXT('.'));
	phcctx->ofn.nFileExtension = pszExt ? (WORD)(pszExt - pszSaveFile + 1) : 0;

	HashCalcSetSavePrefix(phcctx, pszSaveFile);

	phcctx->hFileOut = CreateFileWithLongPathRetry(
		pszSaveFile,
		FILE_APPEND_DATA | DELETE,
		FILE_SHARE_READ,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (phcctx->hFileOut == INVALID_HANDLE_VALUE)
		return(FALSE);

	phcctx->szFormat[0] = 0;

	if (phcctx->opt.dwSaveEncoding == 1)
	{
		WCHAR BOM = 0xFEFF;
		DWORD cbWritten;
		if (!WriteFile(phcctx->hFileOut, &BOM, sizeof(WCHAR), &cbWritten, NULL) ||
		    cbWritten != sizeof(WCHAR))
		{
			CloseHandle(phcctx->hFileOut);
			phcctx->hFileOut = INVALID_HANDLE_VALUE;
			DeleteFile(pszSaveFile);
			return(FALSE);
		}
	}

	return(TRUE);
}

INT_PTR CALLBACK HashCalcSeparateDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			TCHAR szTitle[MAX_STRINGMSG];
			if (LoadString(g_hModThisDll, IDS_HS_MENUTEXT_SEP, szTitle, countof(szTitle)))
			{
				PTSTR pszSrc = szTitle;
				PTSTR pszDest = szTitle;

				while (*pszSrc && *pszSrc != TEXT('(') && *pszSrc != TEXT('.'))
				{
					if (*pszSrc != TEXT('&'))
						*pszDest++ = *pszSrc;
					++pszSrc;
				}
				*pszDest = 0;
				SetWindowText(hWnd, szTitle);
			}

			SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(g_hModThisDll, MAKEINTRESOURCE(IDI_FILETYPE)));

			SetControlText(hWnd, IDC_SEP_CHK, IDS_HS_SEP_CHK);
			SetControlText(hWnd, IDC_SEP_EX, IDS_HS_SEP_EX);
			SetControlText(hWnd, IDC_SEP_EX_KEEP, IDS_HS_SEP_EX_KEEP);
			SetControlText(hWnd, IDC_SEP_EX_OVERWRITE, IDS_HS_SEP_EX_OVERWRITE);
			SetControlText(hWnd, IDC_SEP_INCLUDE_CHECKSUMS, IDS_HS_SEP_INCLUDE_CHECKSUMS);
			SetControlText(hWnd, IDC_OK, IDS_HC_OK);
			SetControlText(hWnd, IDC_CANCEL, IDS_HC_CANCEL);

			if (lParam < 1 || lParam > NUM_HASHES)
				lParam = DEFAULT_HASH_ALGORITHM;

			SendDlgItemMessage(hWnd, IDC_SEP_CHK_FIRSTID + (int)lParam - 1, BM_SETCHECK, BST_CHECKED, 0);
			SendDlgItemMessage(hWnd, IDC_SEP_EX_KEEP, BM_SETCHECK, BST_CHECKED, 0);
			return(TRUE);
		}

		case WM_ENDSESSION:
		{
			if (wParam == FALSE)
				break;
		}
		case WM_CLOSE:
		{
			EndDialog(hWnd, 0);
			return(TRUE);
		}

		case WM_COMMAND:
		{
			switch (LOWORD(wParam))
			{
				case IDC_OK:
				{
					WORD wHashSelected = 0;
					WORD wExistsSelected = IFEXISTS_KEEP;
					WORD wDialogFlags;

					for (WORD i = 0; i < NUM_HASHES; ++i)
					{
						if (SendDlgItemMessage(hWnd, IDC_SEP_CHK_FIRSTID + i, BM_GETCHECK, 0, 0) == BST_CHECKED)
						{
							wHashSelected = i + 1;
							break;
						}
					}

					if (!wHashSelected)
						wHashSelected = DEFAULT_HASH_ALGORITHM;

					for (WORD i = 0; i < IDC_SEP_EX_COUNT; ++i)
					{
						if (SendDlgItemMessage(hWnd, IDC_SEP_EX_FIRSTID + i, BM_GETCHECK, 0, 0) == BST_CHECKED)
						{
							wExistsSelected = i;
							break;
						}
					}

					wDialogFlags = wExistsSelected;
					if (SendDlgItemMessage(hWnd, IDC_SEP_INCLUDE_CHECKSUMS, BM_GETCHECK, 0, 0) == BST_CHECKED)
						wDialogFlags |= SEP_DLG_INCLUDE_CHECKSUM_FILES;

					EndDialog(hWnd, MAKELONG(wHashSelected, wDialogFlags));
					return(TRUE);
				}

				case IDC_CANCEL:
				{
					EndDialog(hWnd, 0);
					return(TRUE);
				}
			}
			break;
		}
	}

	return(FALSE);
}

VOID WINAPI HashCalcInitSaveSeparate( PHASHCALCCONTEXT phcctx )
{
	phcctx->ofn.nFilterIndex = 0;
	phcctx->wIfExists = IFEXISTS_KEEP;
	phcctx->bIncludeChecksumFiles = FALSE;

	phcctx->opt.dwFlags = HCOF_FILTERINDEX | HCOF_SAVEENCODING | HCOF_SAVEEOL;
	OptionsLoad(&phcctx->opt);

	DWORD dwOrigFilterIndex = phcctx->opt.dwFilterIndex;
	INT_PTR nDialogRet = DialogBoxParam(
		g_hModThisDll,
		MAKEINTRESOURCE(IDD_HASHSAVE_SEP),
		phcctx->hWnd,
		HashCalcSeparateDlgProc,
		(LPARAM)dwOrigFilterIndex
	);

	if (nDialogRet <= 0)
		return;

	phcctx->ofn.nFilterIndex = LOWORD(nDialogRet);
	{
		WORD wDialogFlags = HIWORD(nDialogRet);
		phcctx->wIfExists = wDialogFlags & 0xFF;
		if (phcctx->wIfExists > IFEXISTS_OVERWRITE)
			phcctx->wIfExists = IFEXISTS_KEEP;
		phcctx->bIncludeChecksumFiles = (wDialogFlags & SEP_DLG_INCLUDE_CHECKSUM_FILES) != 0;
	}

	if (phcctx->ofn.nFilterIndex &&
	    phcctx->ofn.nFilterIndex != dwOrigFilterIndex)
	{
		phcctx->opt.dwFilterIndex = phcctx->ofn.nFilterIndex;
		phcctx->opt.dwFlags = HCOF_FILTERINDEX;
		OptionsSave(&phcctx->opt);
	}

	phcctx->szFormat[0] = 0;
}

PCTSTR WINAPI HashCalcLineEnding( PHASHCALCCONTEXT phcctx )
{
	return(phcctx->opt.dwSaveEol == 1 ? TEXT("\n") : TEXT("\r\n"));
}

VOID WINAPI HashCalcSetSaveFormat( PHASHCALCCONTEXT phcctx )
{
	// Set szFormat if necessary
	if (phcctx->szFormat[0] == 0)
	{
		// Did I ever mention that I hate SFV?
		// The reason we tracked cchMax was because of this idiotic format
		if (phcctx->ofn.nFilterIndex == 1)
		{
			if (phcctx->bSeparateFiles)
			{
				StringCchPrintf(
					phcctx->szFormat,
					countof(phcctx->szFormat),
					TEXT("%%s %%s%s"),
					HashCalcLineEnding(phcctx)
				);
			}
			else
			{
				StringCchPrintf(
					phcctx->szFormat,
					countof(phcctx->szFormat),
					TEXT("%%-%ds %%s%s"),
					phcctx->cchMax - phcctx->cchAdjusted,
					HashCalcLineEnding(phcctx)
				);
			}
		}
		else if (phcctx->ofn.nFilterIndex == XXH3_64)
		{
			StringCchPrintf(
				phcctx->szFormat,
				countof(phcctx->szFormat),
				TEXT("XXH3_%%s *%%s%s"),
				HashCalcLineEnding(phcctx)
			);
		}
		else
		{
			StringCchPrintf(
				phcctx->szFormat,
				countof(phcctx->szFormat),
				TEXT("%%s *%%s%s"),
				HashCalcLineEnding(phcctx)
			);
		}
	}
}

BOOL WINAPI HashCalcWriteResult( PHASHCALCCONTEXT phcctx, PHASHCALCITEM pItem )
{
	BOOL bHashValid = TRUE;
	return(HashCalcWriteResultToFile(phcctx, phcctx->hFileOut, pItem, &bHashValid) && bHashValid);
}

BOOL WINAPI HashCalcWriteResultToFile( PHASHCALCCONTEXT phcctx, HANDLE hFileOut,
                                       PHASHCALCITEM pItem, PBOOL pbHashValid )
{
	PCTSTR pszHash;                     // will be pointed to the hash name
    WCHAR szWbuffer[MAX_PATH_BUFFER];   // wide-char buffer
    CHAR  szAbuffer[MAX_PATH_BUFFER];   // narrow-char buffer
#ifdef UNICODE
#   define szTbuffer szWbuffer
#else
#   define szTbuffer szAbuffer
#endif
    PTSTR szTbufferAppend = szTbuffer;  // current end of the buffer used to build output
    size_t cchLine = MAX_PATH_BUFFER;   // starts off as count of remaining TCHARS in the buffer
    PVOID pvLine;                       // will be pointed to the buffer to write out
    size_t cbLine;                      // will be line length in bytes, EXCLUDING nul terminator
    BOOL bHashValid = TRUE;

	// If the checksum to save isn't present in the results
    if (! ((1 << (phcctx->ofn.nFilterIndex - 1)) & pItem->results.dwFlags))
    {
        // Start with a commented-out error message - "; UNREADABLE:"
        WCHAR szUnreadable[MAX_STRINGRES];
        LoadString(g_hModThisDll, IDS_HV_STATUS_UNREADABLE, szUnreadable, MAX_STRINGRES);
        StringCchPrintfEx(szTbufferAppend, cchLine, &szTbufferAppend, &cchLine, 0, TEXT("; %s:%s"), szUnreadable, HashCalcLineEnding(phcctx));

        // We'll still output a hash, but it will be all 0's, that way Verify will indicate an mismatch
        HashCalcClearInvalid(&pItem->results, TEXT('0'));
        bHashValid = FALSE;
    }

	// Translate the filter index to a hash
	switch (phcctx->ofn.nFilterIndex)
	{
#define HASH_INDEX_TO_RESULTS_op(alg) \
        case alg:  pszHash = pItem->results.szHex##alg;  break;
        FOR_EACH_HASH(HASH_INDEX_TO_RESULTS_op)
		default: return(FALSE);
	}

	// Format the line
	PCTSTR pszPathAdjusted;
	if (!phcctx->bSeparateFiles)
	{
		pszPathAdjusted = pItem->szPath + phcctx->cchAdjusted;
	}
	else
	{
		pszPathAdjusted = pItem->szPath + pItem->cchPath;
		while (pszPathAdjusted > pItem->szPath)
		{
			--pszPathAdjusted;
			if (*pszPathAdjusted == TEXT('\\') || *pszPathAdjusted == TEXT('/'))
			{
				++pszPathAdjusted;
				break;
			}
		}
	}

	#define HashCalcFormat(a, b) StringCchPrintfEx(szTbufferAppend, cchLine, &szTbufferAppend, &cchLine, 0, phcctx->szFormat, a, b)
	(phcctx->ofn.nFilterIndex == 1) ?
		HashCalcFormat(pszPathAdjusted, pszHash) : // SFV
		HashCalcFormat(pszHash, pszPathAdjusted);  // everything else
	#undef HashCalcFormat

#ifdef _TIMED
    StringCchPrintfEx(szTbufferAppend, cchLine, NULL, &cchLine, 0,
                      _T("; Elapsed: %d ms%s"), pItem->dwElapsed, HashCalcLineEnding(phcctx));
#endif

	cchLine = MAX_PATH_BUFFER - cchLine;  // from now on cchLine is the line length in bytes, EXCLUDING nul terminator
	if (cchLine > 0)
	{
		// Convert to the correct encoding
		switch (phcctx->opt.dwSaveEncoding)
		{
			case 0:
			{
				// UTF-8
				#ifdef UNICODE
				cbLine = WStrToUTF8(szWbuffer, szAbuffer, MAX_PATH_BUFFER) - 1;
				#else
				         AStrToWStr(szAbuffer, szWbuffer, MAX_PATH_BUFFER));
				cbLine = WStrToUTF8(szWbuffer, szAbuffer, MAX_PATH_BUFFER)) - 1;
				#endif

				pvLine = szAbuffer;
				break;
			}

			case 1:
			{
				// UTF-16
				#ifndef UNICODE
				cchLine = AStrToWStr(szAbuffer, szWbuffer, MAX_PATH_BUFFER) - 1;
				#endif

				cbLine = cchLine * sizeof(WCHAR);
				pvLine = szWbuffer;
				break;
			}

			case 2:
			{
				// ANSI
				#ifdef UNICODE
				cbLine = WStrToAStr(szWbuffer, szAbuffer, MAX_PATH_BUFFER) - 1;
				#else
				cbLine = cchLine;
				#endif

				pvLine = szAbuffer;
				break;
			}

			default: return(FALSE);
		}

		if (cbLine > 0)
		{
			DWORD cbWritten = 0;
			WriteFile(hFileOut, pvLine, (DWORD)cbLine, &cbWritten, NULL);
			if (cbLine != cbWritten) return(FALSE);
		}
		else return(FALSE);
	}
	else return(FALSE);

	if (pbHashValid)
		*pbHashValid = bHashValid;

	return(TRUE);
}

VOID WINAPI HashCalcClearInvalid( PWHRESULTEX pwhres, WCHAR cInvalid )
{
#ifdef UNICODE
#   define _tmemset wmemset
#else
#   define _tmemset memset
#endif

#define HASH_CLEAR_INVALID_op(alg)                                                \
    if (! (pwhres->dwFlags & WHEX_CHECK##alg))                                    \
    {                                                                             \
        _tmemset(pwhres->szHex##alg, cInvalid, countof(pwhres->szHex##alg) - 1);  \
        pwhres->szHex##alg[countof(pwhres->szHex##alg) - 1] = L'\0';              \
    }
    FOR_EACH_HASH(HASH_CLEAR_INVALID_op)
}

// This can only succeed on Windows Vista and later;
// returns FALSE on failure
BOOL WINAPI HashCalcDeleteFileByHandle(HANDLE hFile)
{
    if (hFile == INVALID_HANDLE_VALUE)
        return(FALSE);

    HMODULE hKernel32 = GetModuleHandle(TEXT("kernel32.dll"));
    if (hKernel32 == NULL)
        return(FALSE);

    typedef BOOL(WINAPI* PFN_SFIBH)(_In_ HANDLE, _In_ FILE_INFO_BY_HANDLE_CLASS, _In_ LPVOID, _In_ DWORD);
    PFN_SFIBH pfnSetFileInformationByHandle = (PFN_SFIBH)GetProcAddress(hKernel32, "SetFileInformationByHandle");
    if (pfnSetFileInformationByHandle == NULL)
        return(FALSE);

    FILE_DISPOSITION_INFO fdi;
    fdi.DeleteFile = TRUE;
    return(pfnSetFileInformationByHandle(hFile, FileDispositionInfo, &fdi, sizeof(fdi)));
}

VOID WINAPI HashCalcSetSavePrefix( PHASHCALCCONTEXT phcctx, PTSTR pszSave )
{
	// We have to be careful here about case sensitivity since we are now
	// working with a user-provided path instead of a system-provided path...

	// We want to build new paths without resorting to using "..", as that is
	// ugly, fragile (often more so than absolute paths), and not to mention,
	// complicated to calculate.  This means that relative paths will be used
	// only for paths within the same line of ancestry.

	BOOL bMultiSel;
	PTSTR pszOrig;
	PTSTR pszTail;

	// First, grab one of the original paths to work with
	SLReset(phcctx->hListRaw);
	pszOrig = SLGetDataAndStep(phcctx->hListRaw);
	bMultiSel = SLCheck(phcctx->hListRaw);

	// Unfortunately, we also have to contend with the possibility that one of
	// these paths may be in short name format (e.g., if the user navigates to
	// %TEMP% on a NT 5.x system)
	{
		// The scratch buffer's sz members are large enough for us
		PTSTR pszOrigLong = (PTSTR)phcctx->scratch.szW;
		PTSTR pszSaveLong = (PTSTR)phcctx->scratch.szA;

		// Copy original path to scratch and terminate
		pszTail = SSChainNCpy(pszOrigLong, pszOrig, phcctx->cchPrefix);
		pszTail[0] = 0;

		// Copy output path to scratch and terminate
		pszTail = SSChainNCpy(pszSaveLong, pszSave, phcctx->ofn.nFileOffset);
		pszTail[0] = 0;

		// Normalize both paths to LFN
		GetLongPathName(pszOrigLong, pszOrigLong, MAX_PATH_BUFFER);
		GetLongPathName(pszSaveLong, pszSaveLong, MAX_PATH_BUFFER);

		// We will only handle the case where they are the same, to prevent our
		// re-prefixing from messing up the base behavior; it is not worth the
		// trouble to account for LFN for all cases--just let it fall through
		// to an absolute path.
		if (StrCmpNI(pszOrigLong, pszSaveLong, MAX_PATH_BUFFER) == 0)
		{
			phcctx->cchAdjusted = phcctx->cchPrefix;
			return;
		}
	}

	if (pszTail = StrRChr(pszSave, NULL, TEXT('\\')))
	{
		phcctx->cchAdjusted = (UINT)(pszTail - pszSave) + 1;

		if (phcctx->cchAdjusted <= phcctx->cchPrefix)
		{
			if (StrCmpNI(pszOrig, pszSave, phcctx->cchAdjusted) == 0)
			{
				// If the ouput prefix is the same as or a parent of the input
				// prefix...

				if (!(IsDoubleSlashPath(pszSave) && phcctx->cchAdjusted < 3))
					return;
			}
		}
		else if (!bMultiSel)
		{
			// We will make an exception for the case where the user selects
			// a single directory from the Shell and then saves the output in
			// that directory...

			BOOL bEqual;

			*pszTail = 0;
			bEqual = StrCmpNI(pszOrig, pszSave, phcctx->cchAdjusted) == 0;
			*pszTail = TEXT('\\');

			if (bEqual) return;
		}
	}

	// If we have reached this point, we need to use an absolute path

	phcctx->cchAdjusted = 0;
}



/*============================================================================*\
	Progress bar
\*============================================================================*/

VOID WINAPI HashCalcTogglePrep( PHASHCALCCONTEXT phcctx, BOOL bState )
{
	DWORD dwStyle = (DWORD)GetWindowLongPtr(phcctx->hWndPBTotal, GWL_STYLE);

	if (bState)
	{
		dwStyle &= ~PBS_SMOOTH;
		dwStyle |= PBS_MARQUEE;
		phcctx->dwFlags |= HCF_MARQUEE;
	}
	else
	{
		dwStyle |= PBS_SMOOTH;
		dwStyle &= ~PBS_MARQUEE;
		phcctx->dwFlags &= ~HCF_MARQUEE;
	}

	SetWindowLongPtr(phcctx->hWndPBTotal, GWL_STYLE, dwStyle);
	SendMessage(phcctx->hWndPBTotal, PBM_SETMARQUEE, bState, MARQUEE_INTERVAL);

	if (!bState)
		SendMessage(phcctx->hWndPBTotal, PBM_SETRANGE32, 0, phcctx->cTotal);
}
