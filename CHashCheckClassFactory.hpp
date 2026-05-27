/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#ifndef __CHASHCHECKCLASSFACTORY_HPP__
#define __CHASHCHECKCLASSFACTORY_HPP__

#include "globals.h"

enum HASHCHECK_CLASS_OBJECT {
	HCCO_LEGACY = 0,
	HCCO_EXPLORER_CREATE,
	HCCO_EXPLORER_CREATE_SEPARATE,
	HCCO_EXPLORER_VERIFY,
	HCCO_EXPLORER_OPTIONS
};

class CHashCheckClassFactory : public IClassFactory
{
	protected:
		CREF m_cRef;
		HASHCHECK_CLASS_OBJECT m_classObject;

	public:
		CHashCheckClassFactory( HASHCHECK_CLASS_OBJECT classObject = HCCO_LEGACY ) { InterlockedIncrement(&g_cRefThisDll); m_cRef = 1; m_classObject = classObject; }
		~CHashCheckClassFactory( ) { InterlockedDecrement(&g_cRefThisDll); }

		// IUnknown members
		STDMETHODIMP QueryInterface( REFIID, LPVOID * );
		STDMETHODIMP_(ULONG) AddRef( ) { return(InterlockedIncrement(&m_cRef)); }
		STDMETHODIMP_(ULONG) Release( )
		{
			// We need a non-volatile variable, hence the cRef variable
			LONG cRef = InterlockedDecrement(&m_cRef);
			if (cRef == 0) delete this;
			return(cRef);
		}

		// IClassFactory members
		STDMETHODIMP CreateInstance( LPUNKNOWN, REFIID, LPVOID * );
		STDMETHODIMP LockServer( BOOL fLock )
		{
			if (fLock)
				InterlockedIncrement(&g_cRefThisDll);
			else
				InterlockedDecrement(&g_cRefThisDll);

			return(S_OK);
		}
};

typedef CHashCheckClassFactory *LPCHASHCHECKCLASSFACTORY;

#endif
