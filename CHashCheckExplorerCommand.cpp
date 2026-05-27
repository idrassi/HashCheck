/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "CHashCheckExplorerCommand.hpp"
#include "HashCheckUI.h"
#include "HashCheckOptions.h"
#include "libs/WinHash.h"
#include <Strsafe.h>

static BOOL HasChecksumExtension( PCWSTR pszName )
{
	PCWSTR pszExt = StrRChrW(pszName, NULL, L'.');
	if (!pszExt)
		return(FALSE);

	for (UINT i = 0; i < countof(g_szHashExtsTab); ++i)
	{
		if (StrCmpIW(pszExt, g_szHashExtsTab[i]) == 0)
			return(TRUE);
	}

	return(FALSE);
}

static BOOL IsChecksumFilePath( PCWSTR pszPath, BOOL bCheckAttributes )
{
	if (!HasChecksumExtension(pszPath))
		return(FALSE);

	if (bCheckAttributes)
	{
		DWORD dwAttributes = GetFileAttributesW(pszPath);
		if (dwAttributes == INVALID_FILE_ATTRIBUTES || (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
			return(FALSE);
	}

	return(TRUE);
}

static BOOL IsMenuVisibleByOptions( )
{
	HASHCHECKOPTIONS opt;
	opt.dwFlags = HCOF_MENUDISPLAY;
	OptionsLoad(&opt);

	return(opt.dwMenuDisplay == 0);
}

static HRESULT AllocCommandTitle( UINT uStringID, LPWSTR *ppszName )
{
	WCHAR szTitle[MAX_STRINGMSG];
	LoadStringW(g_hModThisDll, uStringID, szTitle, countof(szTitle));

	WCHAR szCleanTitle[MAX_STRINGMSG];
	LPWSTR pszSrc = szTitle;
	LPWSTR pszDest = szCleanTitle;

	while (*pszSrc && pszDest < szCleanTitle + countof(szCleanTitle) - 1)
	{
		if (*pszSrc != L'&')
			*pszDest++ = *pszSrc;

		++pszSrc;
	}

	*pszDest = 0;
	return(SHStrDupW(szCleanTitle, ppszName));
}

static UINT GetCommandTitleStringID( HASHCHECK_EXPLORER_COMMAND command )
{
	switch (command)
	{
		case HCEC_CREATE_SEPARATE:
			return(IDS_HS_MENUTEXT_SEP);

		case HCEC_VERIFY:
			return(IDS_HV_MENUTEXT);

		case HCEC_OPTIONS:
			return(IDS_OPT_TITLE);

		default:
			return(IDS_HS_MENUTEXT);
	}
}

static const GUID *GetCommandCanonicalName( HASHCHECK_EXPLORER_COMMAND command )
{
	switch (command)
	{
		case HCEC_CREATE_SEPARATE:
			return(&CLSID_HashCheckExplorerCreateSeparate);

		case HCEC_VERIFY:
			return(&CLSID_HashCheckExplorerVerify);

		case HCEC_OPTIONS:
			return(&CLSID_HashCheckExplorerOptions);

		default:
			return(&CLSID_HashCheckExplorerCreate);
	}
}

static HRESULT GetShellItemPath( IShellItemArray *psia, DWORD iItem, PWSTR *ppszPath )
{
	*ppszPath = NULL;

	IShellItem *pItem = NULL;
	HRESULT hr = psia->GetItemAt(iItem, &pItem);
	if (SUCCEEDED(hr))
	{
		hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, ppszPath);
		pItem->Release();
	}

	return(hr);
}

static HRESULT GetShellItemParsingName( IShellItemArray *psia, DWORD iItem, PWSTR *ppszName )
{
	*ppszName = NULL;

	IShellItem *pItem = NULL;
	HRESULT hr = psia->GetItemAt(iItem, &pItem);
	if (SUCCEEDED(hr))
	{
		hr = pItem->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, ppszName);
		pItem->Release();
	}

	return(hr);
}

static BOOL HasSelection( IShellItemArray *psia, DWORD *pcItems )
{
	DWORD cItems = 0;
	if (!psia || FAILED(psia->GetCount(&cItems)) || !cItems)
		return(FALSE);

	if (pcItems)
		*pcItems = cItems;

	return(TRUE);
}

static BOOL IsDirectorySelectionItem( IShellItemArray *psia, DWORD iItem )
{
	if (!psia)
		return(FALSE);

	IShellItem *pItem = NULL;
	HRESULT hr = psia->GetItemAt(iItem, &pItem);
	if (FAILED(hr))
		return(FALSE);

	SFGAOF sfgao = 0;
	hr = pItem->GetAttributes(SFGAO_FOLDER, &sfgao);
	pItem->Release();

	return(SUCCEEDED(hr) && (sfgao & SFGAO_FOLDER));
}

static BOOL HasSeparateCreateSelection( IShellItemArray *psia )
{
	DWORD cItems = 0;
	if (!HasSelection(psia, &cItems))
		return(FALSE);

	return(cItems > 1 || IsDirectorySelectionItem(psia, 0));
}

static BOOL HasSingleChecksumFileSelection( IShellItemArray *psia, BOOL bCheckAttributes )
{
	if (!psia)
		return(FALSE);

	DWORD cItems = 0;
	if (FAILED(psia->GetCount(&cItems)) || cItems != 1)
		return(FALSE);

	PWSTR pszName = NULL;
	HRESULT hr = GetShellItemParsingName(psia, 0, &pszName);
	if (FAILED(hr))
		return(FALSE);

	BOOL bHasChecksumExtension = HasChecksumExtension(pszName);
	CoTaskMemFree(pszName);

	if (!bHasChecksumExtension)
		return(FALSE);

	if (!bCheckAttributes)
		return(TRUE);

	PWSTR pszPath = NULL;
	hr = GetShellItemPath(psia, 0, &pszPath);
	if (FAILED(hr))
		return(FALSE);

	BOOL bResult = IsChecksumFilePath(pszPath, bCheckAttributes);
	CoTaskMemFree(pszPath);
	return(bResult);
}

static HRESULT GetPackageHostPath( PWSTR pszHostPath, UINT cchHostPath )
{
	if (!GetModuleFileNameW(g_hModThisDll, pszHostPath, cchHostPath))
		return(HRESULT_FROM_WIN32(GetLastError()));

	if (!PathRemoveFileSpecW(pszHostPath))
		return(E_FAIL);

	if (FAILED(StringCchCatW(pszHostPath, cchHostPath, L"\\HashCheckPackageHost.exe")))
		return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

	DWORD dwAttributes = GetFileAttributesW(pszHostPath);
	if (dwAttributes == INVALID_FILE_ATTRIBUTES || (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));

	return(S_OK);
}

static HRESULT WritePathListFile( IShellItemArray *psia, PWSTR pszListPath, UINT cchListPath )
{
	static const WCHAR szPathListHeader[] = L"HashCheckPathListV1";

	if (!psia || !pszListPath || cchListPath < MAX_PATH)
		return(E_INVALIDARG);

	*pszListPath = 0;

	DWORD cItems = 0;
	HRESULT hr = psia->GetCount(&cItems);
	if (FAILED(hr))
		return(hr);

	if (!cItems)
		return(E_INVALIDARG);

	WCHAR szTempPath[MAX_PATH + 1];
	DWORD cchTempPath = GetTempPathW(countof(szTempPath), szTempPath);
	if (!cchTempPath || cchTempPath >= countof(szTempPath))
		return(HRESULT_FROM_WIN32(cchTempPath ? ERROR_INSUFFICIENT_BUFFER : GetLastError()));

	if (!GetTempFileNameW(szTempPath, L"HCK", 0, pszListPath))
		return(HRESULT_FROM_WIN32(GetLastError()));

	HANDLE hFile = CreateFileW(
		pszListPath,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
		DeleteFileW(pszListPath);
		*pszListPath = 0;
		return(hr);
	}

	DWORD cbWritten = 0;

	if (!WriteFile(hFile, szPathListHeader, sizeof(szPathListHeader), &cbWritten, NULL) ||
	    cbWritten != sizeof(szPathListHeader))
	{
		hr = HRESULT_FROM_WIN32(GetLastError());
	}

	for (DWORD iItem = 0; SUCCEEDED(hr) && iItem < cItems; ++iItem)
	{
		PWSTR pszPath = NULL;
		hr = GetShellItemPath(psia, iItem, &pszPath);
		if (SUCCEEDED(hr))
		{
			DWORD cbPath = ((DWORD)lstrlenW(pszPath) + 1) * sizeof(WCHAR);
			if (!WriteFile(hFile, pszPath, cbPath, &cbWritten, NULL) || cbWritten != cbPath)
				hr = HRESULT_FROM_WIN32(GetLastError());

			CoTaskMemFree(pszPath);
		}
	}

	if (SUCCEEDED(hr))
	{
		WCHAR chNul = 0;
		if (!WriteFile(hFile, &chNul, sizeof(chNul), &cbWritten, NULL) || cbWritten != sizeof(chNul))
			hr = HRESULT_FROM_WIN32(GetLastError());
	}

	CloseHandle(hFile);

	if (FAILED(hr))
	{
		DeleteFileW(pszListPath);
		*pszListPath = 0;
	}

	return(hr);
}

static HRESULT AllocHostCommandLine( PCWSTR pszHostPath, PCWSTR pszVerb, PCWSTR pszArgument, PWSTR *ppszCommandLine )
{
	*ppszCommandLine = NULL;

	size_t cchHostPath = 0, cchVerb = 0, cchArgument = 0;
	HRESULT hr = StringCchLengthW(pszHostPath, STRSAFE_MAX_CCH, &cchHostPath);
	if (SUCCEEDED(hr))
		hr = StringCchLengthW(pszVerb, STRSAFE_MAX_CCH, &cchVerb);
	if (SUCCEEDED(hr))
		hr = StringCchLengthW(pszArgument, STRSAFE_MAX_CCH, &cchArgument);
	if (FAILED(hr))
		return(hr);

	size_t cchCommandLine = cchHostPath + cchVerb + cchArgument + 8;
	PWSTR pszCommandLine = (PWSTR)malloc(cchCommandLine * sizeof(WCHAR));
	if (!pszCommandLine)
		return(E_OUTOFMEMORY);

	hr = StringCchPrintfW(pszCommandLine, cchCommandLine, L"\"%s\" %s \"%s\"", pszHostPath, pszVerb, pszArgument);
	if (FAILED(hr))
	{
		free(pszCommandLine);
		return(hr);
	}

	*ppszCommandLine = pszCommandLine;
	return(S_OK);
}

static HRESULT LaunchPackageHost( PCWSTR pszVerb, PCWSTR pszArgument )
{
	WCHAR szHostPath[MAX_PATH << 1];
	HRESULT hr = GetPackageHostPath(szHostPath, countof(szHostPath));
	if (FAILED(hr))
		return(hr);

	PWSTR pszCommandLine = NULL;
	hr = AllocHostCommandLine(szHostPath, pszVerb, pszArgument, &pszCommandLine);
	if (FAILED(hr))
		return(hr);

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.cb = sizeof(si);

	if (!CreateProcessW(
		szHostPath,
		pszCommandLine,
		NULL,
		NULL,
		FALSE,
		CREATE_DEFAULT_ERROR_MODE,
		NULL,
		NULL,
		&si,
		&pi
	))
	{
		hr = HRESULT_FROM_WIN32(GetLastError());
	}
	else
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		hr = S_OK;
	}

	free(pszCommandLine);
	return(hr);
}

typedef struct {
	IStream *pstmShellItemArray;
	BOOL bSeparateFiles;
} HCEC_CREATE_THREAD_CONTEXT, *PHCEC_CREATE_THREAD_CONTEXT;

static DWORD WINAPI CreateCommandThread( PHCEC_CREATE_THREAD_CONTEXT pctx )
{
	HRESULT hrCoInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	BOOL bCoUninitialize = SUCCEEDED(hrCoInit);

	if (SUCCEEDED(hrCoInit))
	{
		IShellItemArray *psia = NULL;
		HRESULT hr = CoGetInterfaceAndReleaseStream(
			pctx->pstmShellItemArray,
			IID_IShellItemArray,
			(LPVOID *)&psia
		);
		pctx->pstmShellItemArray = NULL;

		if (SUCCEEDED(hr))
		{
			WCHAR szListPath[MAX_PATH + 1];
			hr = WritePathListFile(psia, szListPath, countof(szListPath));
			if (SUCCEEDED(hr))
			{
				hr = LaunchPackageHost(pctx->bSeparateFiles ? L"/hashcheck-create-separate" : L"/hashcheck-create", szListPath);
				if (FAILED(hr))
					DeleteFileW(szListPath);
			}

			psia->Release();
		}
	}

	if (pctx->pstmShellItemArray)
		pctx->pstmShellItemArray->Release();

	free(pctx);

	if (bCoUninitialize)
		CoUninitialize();

	InterlockedDecrement(&g_cRefThisDll);
	return(0);
}

static HRESULT StartCreateCommandThread( IShellItemArray *psia, BOOL bSeparateFiles )
{
	if (!psia)
		return(E_INVALIDARG);

	PHCEC_CREATE_THREAD_CONTEXT pctx = (PHCEC_CREATE_THREAD_CONTEXT)malloc(sizeof(HCEC_CREATE_THREAD_CONTEXT));
	if (!pctx)
		return(E_OUTOFMEMORY);

	pctx->pstmShellItemArray = NULL;
	pctx->bSeparateFiles = bSeparateFiles;

	HRESULT hr = CoMarshalInterThreadInterfaceInStream(
		IID_IShellItemArray,
		psia,
		&pctx->pstmShellItemArray
	);
	if (FAILED(hr))
	{
		free(pctx);
		return(hr);
	}

	InterlockedIncrement(&g_cRefThisDll);

	HANDLE hThread = CreateThreadCRT(CreateCommandThread, pctx);
	if (!hThread)
	{
		InterlockedDecrement(&g_cRefThisDll);
		pctx->pstmShellItemArray->Release();
		free(pctx);
		return(E_FAIL);
	}

	CloseHandle(hThread);
	return(S_OK);
}

CHashCheckExplorerCommand::CHashCheckExplorerCommand( HASHCHECK_EXPLORER_COMMAND command )
{
	InterlockedIncrement(&g_cRefThisDll);
	m_cRef = 1;
	m_command = command;
	m_pSite = NULL;
	m_pSelection = NULL;
	m_bClassicMenu = FALSE;
}

CHashCheckExplorerCommand::~CHashCheckExplorerCommand( )
{
	if (m_pSite)
		m_pSite->Release();

	if (m_pSelection)
		m_pSelection->Release();

	InterlockedDecrement(&g_cRefThisDll);
}

STDMETHODIMP CHashCheckExplorerCommand::QueryInterface( REFIID riid, LPVOID *ppv )
{
	if (IsEqualIID(riid, IID_IUnknown))
	{
		*ppv = static_cast<IExplorerCommand *>(this);
	}
	else if (IsEqualIID(riid, IID_IExplorerCommand))
	{
		*ppv = static_cast<IExplorerCommand *>(this);
	}
	else if (IsEqualIID(riid, IID_IObjectWithSite))
	{
		*ppv = static_cast<IObjectWithSite *>(this);
	}
	else if (IsEqualIID(riid, IID_IObjectWithSelection))
	{
		*ppv = static_cast<IObjectWithSelection *>(this);
	}
	else
	{
		*ppv = NULL;
		return(E_NOINTERFACE);
	}

	AddRef();
	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::GetTitle( IShellItemArray *, LPWSTR *ppszName )
{
	if (!ppszName)
		return(E_POINTER);

	return(AllocCommandTitle(GetCommandTitleStringID(m_command), ppszName));
}

STDMETHODIMP CHashCheckExplorerCommand::GetIcon( IShellItemArray *, LPWSTR *ppszIcon )
{
	if (!ppszIcon)
		return(E_POINTER);

	TCHAR szModuleName[MAX_PATH << 1];
	if (!GetModuleFileName(g_hModThisDll, szModuleName, countof(szModuleName)))
		return(E_FAIL);

	WCHAR szIcon[MAX_PATH << 1];
	if (FAILED(StringCchPrintfW(szIcon, countof(szIcon), L"\"%s\",-%u", szModuleName, IDI_FILETYPE)))
		return(E_FAIL);

	return(SHStrDupW(szIcon, ppszIcon));
}

STDMETHODIMP CHashCheckExplorerCommand::GetCanonicalName( GUID *pguidCommandName )
{
	if (!pguidCommandName)
		return(E_POINTER);

	*pguidCommandName = *GetCommandCanonicalName(m_command);
	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::GetState( IShellItemArray *psia, BOOL fOkToBeSlow, EXPCMDSTATE *pCmdState )
{
	if (!pCmdState)
		return(E_POINTER);

	*pCmdState = ECS_HIDDEN;

	if (m_bClassicMenu)
		return(S_OK);

	if (!IsMenuVisibleByOptions())
		return(S_OK);

	DWORD cItems = 0;
	if (!HasSelection(psia, &cItems))
		return(S_OK);

	if (m_command == HCEC_VERIFY)
	{
		if (cItems != 1)
			return(S_OK);

		if (!fOkToBeSlow)
		{
			*pCmdState = ECS_DISABLED;
			return(E_PENDING);
		}

		if (HasSingleChecksumFileSelection(psia, TRUE))
			*pCmdState = ECS_ENABLED;
	}
	else if (m_command == HCEC_CREATE_SEPARATE)
	{
		if (cItems > 1 || IsDirectorySelectionItem(psia, 0))
			*pCmdState = ECS_ENABLED;
	}
	else
	{
		*pCmdState = ECS_ENABLED;
	}

	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::Invoke( IShellItemArray *psia, IBindCtx * )
{
	if (m_command == HCEC_OPTIONS)
		return(LaunchPackageHost(L"/hashcheck-options", L""));

	if (m_command == HCEC_VERIFY)
	{
		if (!HasSingleChecksumFileSelection(psia, TRUE))
			return(E_INVALIDARG);

		PWSTR pszPath = NULL;
		HRESULT hr = GetShellItemPath(psia, 0, &pszPath);
		if (FAILED(hr))
			return(hr);

		hr = LaunchPackageHost(L"/hashcheck-verify", pszPath);
		CoTaskMemFree(pszPath);
		return(hr);
	}

	if (!HasSelection(psia, NULL))
		return(E_INVALIDARG);

	if (m_command == HCEC_CREATE_SEPARATE && !HasSeparateCreateSelection(psia))
		return(E_INVALIDARG);

	return(StartCreateCommandThread(psia, m_command == HCEC_CREATE_SEPARATE));
}

STDMETHODIMP CHashCheckExplorerCommand::GetFlags( EXPCMDFLAGS *pFlags )
{
	if (!pFlags)
		return(E_POINTER);

	*pFlags = ECF_DEFAULT;
	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::SetSite( IUnknown *pUnkSite )
{
	if (pUnkSite)
		pUnkSite->AddRef();

	if (m_pSite)
		m_pSite->Release();

	m_pSite = pUnkSite;

	if (!pUnkSite)
	{
		if (m_pSelection)
		{
			m_pSelection->Release();
			m_pSelection = NULL;
		}

		m_bClassicMenu = FALSE;
	}

	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::GetSite( REFIID riid, void **ppvSite )
{
	if (!ppvSite)
		return(E_POINTER);

	*ppvSite = NULL;

	if (!m_pSite)
		return(E_FAIL);

	return(m_pSite->QueryInterface(riid, ppvSite));
}

STDMETHODIMP CHashCheckExplorerCommand::SetSelection( IShellItemArray *psia )
{
	if (psia)
		psia->AddRef();

	if (m_pSelection)
		m_pSelection->Release();

	m_pSelection = psia;

	// The call itself is the classic-menu signal, even if psia is NULL.
	m_bClassicMenu = TRUE;
	return(S_OK);
}

STDMETHODIMP CHashCheckExplorerCommand::GetSelection( REFIID riid, void **ppv )
{
	if (!ppv)
		return(E_POINTER);

	*ppv = NULL;

	if (!m_pSelection)
		return(E_FAIL);

	return(m_pSelection->QueryInterface(riid, ppv));
}
