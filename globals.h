/**
 * HashCheck Shell Extension
 * Copyright (C) Kai Liu.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "libs/WinIntrinsics.h"

#include <windows.h>
#include <olectl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <process.h>

#include "libs/SimpleString.h"
#include "libs/SimpleList.h"
#include "libs/BitwiseIntrinsics.h"
#include "HashCheckResources.h"
#include "HashCheckTranslations.h"
#include "version.h"

// Define globals for bookkeeping this DLL instance
extern HMODULE g_hModThisDll;
extern CREF g_cRefThisDll;

// Activation context cache, to reduce the number of CreateActCtx calls
extern volatile BOOL g_bActCtxCreated;
extern HANDLE g_hActCtx;

// Major and minor Windows version
extern UINT16 g_uWinVer;

// Define the data and strings used for COM registration
static const GUID CLSID_HashCheck = { 0x705977c7, 0x86cb, 0x4743, { 0xbf, 0xaf, 0x69, 0x08, 0xbd, 0x19, 0xb7, 0xb0 } };
static const GUID CLSID_HashCheckExplorerCreate = { 0xdb56d393, 0xfc0e, 0x4f46, { 0x9c, 0x82, 0xec, 0x60, 0x1e, 0x6a, 0xb6, 0xaa } };
static const GUID CLSID_HashCheckExplorerCreateSeparate = { 0x2263623b, 0x7700, 0x4fac, { 0xae, 0x98, 0x32, 0xba, 0xa9, 0xfb, 0xc8, 0x2d } };
static const GUID CLSID_HashCheckExplorerVerify = { 0xd07a4f30, 0xd6f6, 0x4e0b, { 0x93, 0x61, 0xe3, 0x03, 0x9e, 0xdb, 0x15, 0xff } };
static const GUID CLSID_HashCheckExplorerOptions = { 0xf92dbf1d, 0x6398, 0x405c, { 0xbc, 0xb5, 0x13, 0x87, 0x08, 0x8a, 0x9e, 0x7f } };
#define CLSID_STR_HashCheck         TEXT("{705977C7-86CB-4743-BFAF-6908BD19B7B0}")
#define CLSID_STR_HashCheckExplorerCreate TEXT("{DB56D393-FC0E-4F46-9C82-EC601E6AB6AA}")
#define CLSID_STR_HashCheckExplorerCreateSeparate TEXT("{2263623B-7700-4FAC-AE98-32BAA9FBC82D}")
#define CLSID_STR_HashCheckExplorerVerify TEXT("{D07A4F30-D6F6-4E0B-9361-E3039EDB15FF}")
#define CLSID_STR_HashCheckExplorerOptions TEXT("{F92DBF1D-6398-405C-BCB5-1387088A9E7F}")
#define CLSNAME_STR_HashCheck       TEXT("HashCheck Shell Extension")
#define PROGID_STR_HashCheck        TEXT("HashCheck")
#define PACKAGE_NAME_STR_HashCheck  TEXT("IDRIX.HashCheck")
#define PACKAGE_FILE_STR_HashCheck  TEXT("HashCheckWin11.msix")

// Application ID for the NT6.1+ taskbar
#define APP_USER_MODEL_ID           L"KL.HashCheck"

// Define helper macros
#define countof(x)                  (sizeof(x)/sizeof(x[0]))
#define BYTEDIFF(a, b)              ((PBYTE)(a) - (PBYTE)(b))
#define BYTEADD(a, cb)              ((PVOID)((PBYTE)(a) + (cb)))

// Max translation string lengths; increase this as necessary
#define MAX_STRINGRES               0x20
#define MAX_STRINGMSG               0x40

#ifdef __cplusplus
}
#endif

#endif
