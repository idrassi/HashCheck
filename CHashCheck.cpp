/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "CHashCheck.hpp"
#include "HashCheckUI.h"
#include "HashCheckOptions.h"
#include "libs/WinHash.h"

// Undocumented Win5-era shell flag used by the Start Menu workaround below.
#ifndef CMF_VERBSONMAN
#define CMF_VERBSONMAN 0x00020000
#endif

enum {
	HC_CMD_VERIFY = 0,
	HC_CMD_CREATE = 1
};

static BOOL IsChecksumFilePath( PCTSTR pszPath )
{
	DWORD dwAttributes = GetFileAttributes(pszPath);
	if (dwAttributes == INVALID_FILE_ATTRIBUTES || (dwAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return(FALSE);

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

static HRESULT CopyContextMenuHelpTextA( UINT uStringID, LPSTR pszName, UINT cchMax )
{
	LoadStringA(g_hModThisDll, uStringID, pszName, cchMax);

	LPSTR lpszSrcA = pszName;
	LPSTR lpszDestA = pszName;

	while (*lpszSrcA && *lpszSrcA != '(' && *lpszSrcA != '.')
	{
		if (*lpszSrcA != '&')
		{
			*lpszDestA = *lpszSrcA;
			++lpszDestA;
		}

		++lpszSrcA;
	}

	*lpszDestA = 0;
	return(S_OK);
}

static HRESULT CopyContextMenuHelpTextW( UINT uStringID, LPWSTR pszName, UINT cchMax )
{
	LoadStringW(g_hModThisDll, uStringID, pszName, cchMax);

	LPWSTR lpszSrcW = pszName;
	LPWSTR lpszDestW = pszName;

	while (*lpszSrcW && *lpszSrcW != L'(' && *lpszSrcW != L'.')
	{
		if (*lpszSrcW != L'&')
		{
			*lpszDestW = *lpszSrcW;
			++lpszDestW;
		}

		++lpszSrcW;
	}

	*lpszDestW = 0;
	return(S_OK);
}

CHashCheck::CHashCheck( )
{
    InterlockedIncrement(&g_cRefThisDll);
    m_cRef = 1;
    m_hList = NULL;
    m_bCanVerify = FALSE;
    m_hMenuBitmap = g_uWinVer >= 0x0600 ?  // Vista+
        (HBITMAP)LoadImage(g_hModThisDll, MAKEINTRESOURCE(IDI_MENUBITMAP), IMAGE_BITMAP, 0, 0, LR_DEFAULTSIZE | LR_CREATEDIBSECTION) :
        NULL;
}

STDMETHODIMP CHashCheck::QueryInterface( REFIID riid, LPVOID *ppv )
{
	if (IsEqualIID(riid, IID_IUnknown))
	{
		*ppv = this;
	}
	else if (IsEqualIID(riid, IID_IShellExtInit))
	{
		*ppv = (LPSHELLEXTINIT)this;
	}
	else if (IsEqualIID(riid, IID_IContextMenu))
	{
		*ppv = (LPCONTEXTMENU)this;
	}
	else if (IsEqualIID(riid, IID_IShellPropSheetExt))
	{
		*ppv = (LPSHELLPROPSHEETEXT)this;
	}
	else if (IsEqualIID(riid, IID_IDropTarget))
	{
		*ppv = (LPDROPTARGET)this;
	}
	else
	{
		*ppv = NULL;
		return(E_NOINTERFACE);
	}

	AddRef();
	return(S_OK);
}

STDMETHODIMP CHashCheck::Initialize( LPCITEMIDLIST pidlFolder, LPDATAOBJECT pdtobj, HKEY hkeyProgID )
{
	// We'll be needing a buffer, and let's double it just to be safe
	TCHAR szPath[MAX_PATH << 1];

	// Make sure that we are working with a fresh list
	SLRelease(m_hList);
	m_hList = SLCreate();
	m_bCanVerify = FALSE;

	// This indent exists to facilitate diffing against the CmdOpen source
	{
		FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		STGMEDIUM medium;

		if (!pdtobj || pdtobj->GetData(&format, &medium) != S_OK)
			return(E_INVALIDARG);

		if (HDROP hDrop = (HDROP)GlobalLock(medium.hGlobal))
		{
			UINT uDrops = DragQueryFile(hDrop, -1, NULL, 0);

			for (UINT uDrop = 0; uDrop < uDrops; ++uDrop)
			{
				if (DragQueryFile(hDrop, uDrop, szPath, countof(szPath)))
				{
					SLAddStringI(m_hList, szPath);
				}
			}

			if (uDrops == 1 && DragQueryFile(hDrop, 0, szPath, countof(szPath)))
				m_bCanVerify = IsChecksumFilePath(szPath);

			GlobalUnlock(medium.hGlobal);
		}

		ReleaseStgMedium(&medium);
	}


	// If there was any failure, the list would be empty...
	return((SLCheck(m_hList)) ? S_OK : E_INVALIDARG);
}

STDMETHODIMP CHashCheck::QueryContextMenu( HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags )
{
	if (uFlags & (CMF_DEFAULTONLY | CMF_NOVERBS))
		return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

	// Ugly hack: work around a bug in Windows 5.x that causes a spurious
	// separator to be added when invoking the context menu from the Start Menu
	if (g_uWinVer < 0x0600 && !(uFlags & (CMF_VERBSONMAN | CMF_EXPLORE)) && GetModuleHandleA("explorer.exe"))
		return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

	// Load the menu display settings
	HASHCHECKOPTIONS opt;
	opt.dwFlags = HCOF_MENUDISPLAY;
	OptionsLoad(&opt);

	// Do not show if the settings prohibit it
	if (opt.dwMenuDisplay == 2 || (opt.dwMenuDisplay == 1 && !(uFlags & CMF_EXTENDEDVERBS)))
		return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

	UINT cCommands = m_bCanVerify ? 2 : 1;
	if (idCmdFirst + cCommands - 1 > idCmdLast)
		return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

    if (! InsertMenu(hmenu, indexMenu, MF_SEPARATOR | MF_BYPOSITION, 0, NULL))
        return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

    MENUITEMINFO mii;
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_FTYPE | MIIM_ID | MIIM_STRING;
    if (g_uWinVer >= 0x0600)  // prior to Vista, 32-bit bitmaps w/alpha channels don't render correctly in menus
        mii.fMask |= MIIM_BITMAP;
    mii.fType      = MFT_STRING;
    mii.hbmpItem   = m_hMenuBitmap;

	UINT uMenuPos = indexMenu + 1;
	if (m_bCanVerify)
	{
		TCHAR szVerifyMenuText[MAX_STRINGMSG];
		LoadString(g_hModThisDll, IDS_HV_MENUTEXT, szVerifyMenuText, countof(szVerifyMenuText));

		mii.wID        = idCmdFirst + HC_CMD_VERIFY;
		mii.dwTypeData = szVerifyMenuText;
		if (! InsertMenuItem(hmenu, uMenuPos++, TRUE, &mii))
			return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));
	}

	TCHAR szCreateMenuText[MAX_STRINGMSG];
	LoadString(g_hModThisDll, IDS_HS_MENUTEXT, szCreateMenuText, countof(szCreateMenuText));

	mii.wID        = idCmdFirst + (m_bCanVerify ? HC_CMD_CREATE : HC_CMD_VERIFY);
	mii.dwTypeData = szCreateMenuText;
	if (! InsertMenuItem(hmenu, uMenuPos++, TRUE, &mii))
		return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0));

    InsertMenu(hmenu, uMenuPos, MF_SEPARATOR | MF_BYPOSITION, 0, NULL);

	return(MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, cCommands));
}

STDMETHODIMP CHashCheck::InvokeCommand( LPCMINVOKECOMMANDINFO pici )
{
	// Ignore string verbs
	if (HIWORD(pici->lpVerb))
		return(E_INVALIDARG);

	UINT idCmd = LOWORD(pici->lpVerb);

	if (m_bCanVerify && idCmd == HC_CMD_VERIFY)
	{
		PCTSTR pszPath = (PCTSTR)SLGetData(m_hList);
		if (!pszPath)
			return(E_INVALIDARG);

		UINT cchPath = (UINT)SSLen(pszPath) + 1;
		LPTSTR pszPathCopy = (LPTSTR)malloc(cchPath * sizeof(TCHAR));
		if (!pszPathCopy)
			return(E_OUTOFMEMORY);

		SSCpy(pszPathCopy, pszPath);
		InterlockedIncrement(&g_cRefThisDll);

		HANDLE hThread = CreateThreadCRT(HashVerifyThread, pszPathCopy);
		if (!hThread)
		{
			InterlockedDecrement(&g_cRefThisDll);
			free(pszPathCopy);
			return(E_FAIL);
		}

		CloseHandle(hThread);

		SLRelease(m_hList);
		m_hList = NULL;

		return(S_OK);
	}

	if (idCmd != (m_bCanVerify ? HC_CMD_CREATE : HC_CMD_VERIFY))
		return(E_INVALIDARG);

	// Hand things over to HashSave, where all the work is done...
	HashSaveStart(pici->hwnd, m_hList);

	// HashSave has AddRef'ed and now owns our list
	SLRelease(m_hList);
	m_hList = NULL;

	return(S_OK);
}

STDMETHODIMP CHashCheck::GetCommandString( UINT_PTR idCmd, UINT uFlags, UINT *pwReserved, LPSTR pszName, UINT cchMax )
{
	static const  CHAR szVerbA[] =  "cksum";
	static const WCHAR szVerbW[] = L"cksum";
	static const  CHAR szVerifyVerbA[] =  "verifychecksum";
	static const WCHAR szVerifyVerbW[] = L"verifychecksum";

	BOOL bVerifyCommand = m_bCanVerify && idCmd == HC_CMD_VERIFY;
	BOOL bCreateCommand = idCmd == (m_bCanVerify ? HC_CMD_CREATE : HC_CMD_VERIFY);
	if (!bVerifyCommand && !bCreateCommand)
		return(E_INVALIDARG);

	UINT uStringID = bVerifyCommand ? IDS_HV_MENUTEXT : IDS_HS_MENUTEXT;

	switch (uFlags)
	{
		// The help text (status bar text) should not contain any of the
		// characters added for the menu access keys.

		case GCS_HELPTEXTA:
		{
			return(CopyContextMenuHelpTextA(uStringID, (LPSTR)pszName, cchMax));
		}

		case GCS_HELPTEXTW:
		{
			return(CopyContextMenuHelpTextW(uStringID, (LPWSTR)pszName, cchMax));
		}

		case GCS_VERBA:
		{
			if (bVerifyCommand)
			{
				if (cchMax < countof(szVerifyVerbA))
					return(E_INVALIDARG);
				SSStaticCpyA((LPSTR)pszName, szVerifyVerbA);
			}
			else
			{
				if (cchMax < countof(szVerbA))
					return(E_INVALIDARG);
				SSStaticCpyA((LPSTR)pszName, szVerbA);
			}
			return(S_OK);
		}

		case GCS_VERBW:
		{
			if (bVerifyCommand)
			{
				if (cchMax < countof(szVerifyVerbW))
					return(E_INVALIDARG);
				SSStaticCpyW((LPWSTR)pszName, szVerifyVerbW);
			}
			else
			{
				if (cchMax < countof(szVerbW))
					return(E_INVALIDARG);
				SSStaticCpyW((LPWSTR)pszName, szVerbW);
			}
			return(S_OK);
		}
	}

	return(E_INVALIDARG);
}

STDMETHODIMP CHashCheck::AddPages( LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam )
{
	PROPSHEETPAGE psp;
	psp.dwSize = sizeof(psp);
	psp.dwFlags = PSP_USECALLBACK | PSP_USEREFPARENT | PSP_USETITLE;
	psp.hInstance = g_hModThisDll;
	psp.pszTemplate = MAKEINTRESOURCE(IDD_HASHPROP);
	psp.pszTitle = MAKEINTRESOURCE(IDS_HP_TITLE);
	psp.pfnDlgProc = HashPropDlgProc;
	psp.lParam = (LPARAM)m_hList;
	psp.pfnCallback = HashPropCallback;
	psp.pcRefParent = (PUINT)&g_cRefThisDll;

	if (ActivateManifest(FALSE))
	{
		psp.dwFlags |= PSP_USEFUSIONCONTEXT;
		psp.hActCtx = g_hActCtx;
	}

	HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);

	if (hPage && !pfnAddPage(hPage, lParam))
		DestroyPropertySheetPage(hPage);

	// HashProp has AddRef'ed and now owns our list
	SLRelease(m_hList);
	m_hList = NULL;

	return(S_OK);
}

STDMETHODIMP CHashCheck::Drop( LPDATAOBJECT pdtobj, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect )
{
	FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium;

	UINT uThreads = 0;

	if (pdtobj && pdtobj->GetData(&format, &medium) == S_OK)
	{
		if (HDROP hDrop = (HDROP)GlobalLock(medium.hGlobal))
		{
			UINT uDrops = DragQueryFile(hDrop, -1, NULL, 0);
			UINT cchPath;
			LPTSTR lpszPath;

			// Reduce the likelihood of a race condition when trying to create
			// an activation context by creating it before creating threads
			ActivateManifest(FALSE);

			for (UINT uDrop = 0; uDrop < uDrops; ++uDrop)
			{
				if ( (cchPath = DragQueryFile(hDrop, uDrop, NULL, 0)) &&
				     (lpszPath = (LPTSTR)malloc((cchPath + 1) * sizeof(TCHAR))) )
				{
					InterlockedIncrement(&g_cRefThisDll);

					HANDLE hThread;

					if ( (DragQueryFile(hDrop, uDrop, lpszPath, cchPath + 1) == cchPath) &&
					     (!(GetFileAttributes(lpszPath) & FILE_ATTRIBUTE_DIRECTORY)) &&
					     (hThread = CreateThreadCRT(HashVerifyThread, lpszPath)) )
					{
						// The thread should free lpszPath, not us
						CloseHandle(hThread);
						++uThreads;
					}
					else
					{
						free(lpszPath);
						InterlockedDecrement(&g_cRefThisDll);
					}
				}
			}

			GlobalUnlock(medium.hGlobal);
		}

		ReleaseStgMedium(&medium);
	}

	if (uThreads)
	{
		// DROPEFFECT_LINK would work here as well; it really doesn't matter
		*pdwEffect = DROPEFFECT_COPY;
		return(S_OK);
	}
	else
	{
		// We shouldn't ever be hitting this case
		*pdwEffect = DROPEFFECT_NONE;
		return(E_INVALIDARG);
	}
}
