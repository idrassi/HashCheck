/**
 * HashCheck uninstall registration
 * Copyright (C) 2026 Mounir IDRASSI. All rights reserved.
 * Please refer to license.txt for details about distribution and modification.
 **/

#ifndef __HASHCHECKINSTALL_H__
#define __HASHCHECKINSTALL_H__

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL WINAPI RegisterUninstallEntry( LPCTSTR lpszDllPath );
BOOL WINAPI UnregisterUninstallEntry( VOID );

#ifdef __cplusplus
}
#endif

#endif
