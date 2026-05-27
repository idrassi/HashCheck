/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#ifndef __CHASHCHECKEXPLORERCOMMAND_HPP__
#define __CHASHCHECKEXPLORERCOMMAND_HPP__

#include "globals.h"

enum HASHCHECK_EXPLORER_COMMAND {
	HCEC_CREATE = 0,
	HCEC_CREATE_SEPARATE = 1,
	HCEC_VERIFY = 2,
	HCEC_OPTIONS = 3
};

class CHashCheckExplorerCommand : public IExplorerCommand, public IObjectWithSite, public IObjectWithSelection
{
	protected:
		CREF m_cRef;
		HASHCHECK_EXPLORER_COMMAND m_command;
		IUnknown *m_pSite;
		IShellItemArray *m_pSelection;
		// Empirical Windows 11 signal: the classic menu calls SetSelection.
		BOOL m_bClassicMenu;

	public:
		CHashCheckExplorerCommand( HASHCHECK_EXPLORER_COMMAND );
		~CHashCheckExplorerCommand( );

		// IUnknown members
		STDMETHODIMP QueryInterface( REFIID, LPVOID * );
		STDMETHODIMP_(ULONG) AddRef( ) { return(InterlockedIncrement(&m_cRef)); }
		STDMETHODIMP_(ULONG) Release( )
		{
			LONG cRef = InterlockedDecrement(&m_cRef);
			if (cRef == 0) delete this;
			return(cRef);
		}

		// IExplorerCommand members
		STDMETHODIMP GetTitle( IShellItemArray *, LPWSTR * );
		STDMETHODIMP GetIcon( IShellItemArray *, LPWSTR * );
		STDMETHODIMP GetToolTip( IShellItemArray *, LPWSTR * ) { return(E_NOTIMPL); }
		STDMETHODIMP GetCanonicalName( GUID * );
		STDMETHODIMP GetState( IShellItemArray *, BOOL, EXPCMDSTATE * );
		STDMETHODIMP Invoke( IShellItemArray *, IBindCtx * );
		STDMETHODIMP GetFlags( EXPCMDFLAGS * );
		STDMETHODIMP EnumSubCommands( IEnumExplorerCommand ** ) { return(E_NOTIMPL); }

		// IObjectWithSite members
		STDMETHODIMP SetSite( IUnknown * );
		STDMETHODIMP GetSite( REFIID, void ** );

		// IObjectWithSelection members
		STDMETHODIMP SetSelection( IShellItemArray * );
		STDMETHODIMP GetSelection( REFIID, void ** );
};

typedef CHashCheckExplorerCommand *LPCHASHCHECKEXPLORERCOMMAND;

#endif
