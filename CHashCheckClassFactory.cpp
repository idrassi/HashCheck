/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "CHashCheckClassFactory.hpp"
#include "CHashCheck.hpp"
#include "CHashCheckExplorerCommand.hpp"
#include <new>

STDMETHODIMP CHashCheckClassFactory::QueryInterface( REFIID riid, LPVOID *ppv )
{
	if (IsEqualIID(riid, IID_IUnknown))
	{
		*ppv = this;
	}
	else if (IsEqualIID(riid, IID_IClassFactory))
	{
		*ppv = (LPCLASSFACTORY)this;
	}
	else
	{
		*ppv = NULL;
		return(E_NOINTERFACE);
	}

	AddRef();
	return(S_OK);
}

STDMETHODIMP CHashCheckClassFactory::CreateInstance( LPUNKNOWN pUnkOuter, REFIID riid, LPVOID *ppv )
{
	*ppv = NULL;

	if (pUnkOuter) return(CLASS_E_NOAGGREGATION);

	IUnknown *pUnknown = NULL;

	switch (m_classObject)
	{
		case HCCO_EXPLORER_CREATE:
			pUnknown = static_cast<IExplorerCommand *>(new(std::nothrow) CHashCheckExplorerCommand(HCEC_CREATE));
			break;

		case HCCO_EXPLORER_CREATE_SEPARATE:
			pUnknown = static_cast<IExplorerCommand *>(new(std::nothrow) CHashCheckExplorerCommand(HCEC_CREATE_SEPARATE));
			break;

		case HCCO_EXPLORER_VERIFY:
			pUnknown = static_cast<IExplorerCommand *>(new(std::nothrow) CHashCheckExplorerCommand(HCEC_VERIFY));
			break;

		case HCCO_EXPLORER_OPTIONS:
			pUnknown = static_cast<IExplorerCommand *>(new(std::nothrow) CHashCheckExplorerCommand(HCEC_OPTIONS));
			break;

		default:
			pUnknown = static_cast<IShellExtInit *>(new(std::nothrow) CHashCheck);
			break;
	}

	if (pUnknown == NULL) return(E_OUTOFMEMORY);

	HRESULT hr = pUnknown->QueryInterface(riid, ppv);
	pUnknown->Release();
	return(hr);
}
