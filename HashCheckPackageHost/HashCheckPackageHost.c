#include <windows.h>
#include <shellapi.h>

typedef VOID (CALLBACK *PFNHASHCHECKRUNDLL)( HWND, HINSTANCE, PWSTR, INT );
typedef INT  (CALLBACK *PFNHASHCHECKRUNDLLRESULT)( HWND, HINSTANCE, PWSTR, INT );

#if defined(_MSC_VER)
#define HASHCHECK_NOCF __declspec(guard(nocf))
#else
#define HASHCHECK_NOCF
#endif

static const WCHAR szPathListHeader[] = L"HashCheckPathListV1";
static const WCHAR szSaveSpecHeader[] = L"HashCheckSaveSpecV1";
static const size_t cchMaxWritableString = (MAXDWORD / sizeof(WCHAR)) - 1;

#pragma function(memset)
void * __cdecl memset( void *pvDest, int iValue, size_t cbDest )
{
	PBYTE pbDest = (PBYTE)pvDest;
	while (cbDest--)
		*pbDest++ = (BYTE)iValue;

	return(pvDest);
}

enum {
	HC_HASH_CRC32 = 1,
	HC_HASH_MD5,
	HC_HASH_SHA1,
	HC_HASH_SHA256,
	HC_HASH_SHA512,
	HC_HASH_SHA3_256,
	HC_HASH_SHA3_512,
	HC_HASH_BLAKE3,
	HC_HASH_XXH3_64,
	HC_HASH_XXH3_128,
	HC_HASH_DEFAULT = HC_HASH_SHA256
};

typedef struct {
	PWSTR pszOutputPath;
	INT iHashIndex;
	INT iEncoding;
	INT iEol;
	UINT cPaths;
	BOOL bHasSilentOption;
	BOOL bBypassQueue;
	BOOL bSeparateFiles;
} CREATEOPTIONS, *PCREATEOPTIONS;

static VOID ZeroBytes( PVOID pvBuffer, size_t cbBuffer )
{
	PBYTE pbBuffer = (PBYTE)pvBuffer;

	while (cbBuffer--)
		*pbBuffer++ = 0;
}

static HRESULT StringLength( PCWSTR pszText, size_t cchMax, size_t *pcchText )
{
	if (!pszText || !pcchText)
		return(E_INVALIDARG);

	for (size_t cchText = 0; cchText < cchMax; ++cchText)
	{
		if (!pszText[cchText])
		{
			*pcchText = cchText;
			return(S_OK);
		}
	}

	return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
}

static HRESULT StringCopy( PWSTR pszDest, UINT cchDest, PCWSTR pszSrc )
{
	if (!pszDest || !pszSrc || !cchDest)
		return(E_INVALIDARG);

	for (UINT i = 0; i < cchDest; ++i)
	{
		pszDest[i] = pszSrc[i];
		if (!pszSrc[i])
			return(S_OK);
	}

	pszDest[cchDest - 1] = 0;
	return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
}

static HRESULT StringAppend( PWSTR pszDest, UINT cchDest, PCWSTR pszSrc )
{
	if (!pszDest || !pszSrc || !cchDest)
		return(E_INVALIDARG);

	UINT cchDestUsed = 0;
	while (cchDestUsed < cchDest && pszDest[cchDestUsed])
		++cchDestUsed;

	if (cchDestUsed >= cchDest)
		return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

	return(StringCopy(pszDest + cchDestUsed, cchDest - cchDestUsed, pszSrc));
}

static PCWSTR FindLastChar( PCWSTR pszText, WCHAR chFind )
{
	PCWSTR pszFound = NULL;
	if (!pszText)
		return(NULL);

	for (; *pszText; ++pszText)
	{
		if (*pszText == chFind)
			pszFound = pszText;
	}

	return(pszFound);
}

static VOID AppendHexUlong( PWSTR pszDest, UINT cchDest, ULONG ulValue )
{
	static const WCHAR szHex[] = L"0123456789ABCDEF";
	WCHAR szHexValue[9];

	for (int i = 7; i >= 0; --i)
	{
		szHexValue[i] = szHex[ulValue & 0xF];
		ulValue >>= 4;
	}
	szHexValue[8] = 0;

	StringAppend(pszDest, cchDest, szHexValue);
}

static VOID TrimTrailingWhitespace( PWSTR pszText )
{
	size_t cchText = 0;
	if (FAILED(StringLength(pszText ? pszText : L"", cchMaxWritableString, &cchText)))
		return;

	while (cchText && (pszText[cchText - 1] == L'\r' ||
	                   pszText[cchText - 1] == L'\n' ||
	                   pszText[cchText - 1] == L' '  ||
	                   pszText[cchText - 1] == L'\t'))
	{
		pszText[--cchText] = 0;
	}
}

static HRESULT IntToString( INT iValue, PWSTR pszBuffer, UINT cchBuffer )
{
	WCHAR szReverse[16];
	UINT cchReverse = 0;
	BOOL bNegative = (iValue < 0);
	UINT uValue = bNegative ? (UINT)(-(iValue + 1)) + 1 : (UINT)iValue;

	if (!pszBuffer || !cchBuffer)
		return(E_INVALIDARG);

	do
	{
		if (cchReverse >= ARRAYSIZE(szReverse))
			return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

		szReverse[cchReverse++] = (WCHAR)(L'0' + (uValue % 10));
		uValue /= 10;
	}
	while (uValue);

	if ((UINT)bNegative + cchReverse + 1 > cchBuffer)
		return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

	UINT iOut = 0;
	if (bNegative)
		pszBuffer[iOut++] = L'-';

	while (cchReverse)
		pszBuffer[iOut++] = szReverse[--cchReverse];

	pszBuffer[iOut] = 0;
	return(S_OK);
}

static BOOL WriteWideToStandardError( PCWSTR pszText )
{
	HANDLE hError = GetStdHandle(STD_ERROR_HANDLE);
	if (!hError || hError == INVALID_HANDLE_VALUE || !pszText)
		return(FALSE);

	size_t cchText = 0;
	if (FAILED(StringLength(pszText, cchMaxWritableString, &cchText)) || !cchText)
		return(FALSE);

	DWORD dwWritten = 0;
	if (GetFileType(hError) == FILE_TYPE_CHAR)
		return(WriteConsoleW(hError, pszText, (DWORD)cchText, &dwWritten, NULL));

	CHAR szBuffer[2048];
	INT cbBuffer = WideCharToMultiByte(
		CP_UTF8,
		0,
		pszText,
		(INT)cchText,
		szBuffer,
		(INT)sizeof(szBuffer),
		NULL,
		NULL
	);

	if (cbBuffer > 0)
		return(WriteFile(hError, szBuffer, (DWORD)cbBuffer, &dwWritten, NULL));

	return(FALSE);
}

static INT ReportErrorToStandardError( PCWSTR pszAction, HRESULT hr )
{
	WCHAR szMessage[512];
	WCHAR szErrorText[256];

	szErrorText[0] = 0;
	if (!FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		(DWORD)hr,
		0,
		szErrorText,
		ARRAYSIZE(szErrorText),
		NULL
	))
	{
		if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
		{
			FormatMessageW(
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				HRESULT_CODE(hr),
				0,
				szErrorText,
				ARRAYSIZE(szErrorText),
				NULL
			);
		}
	}

	TrimTrailingWhitespace(szErrorText);

	StringCopy(szMessage, ARRAYSIZE(szMessage), L"HashCheck: ");
	StringAppend(szMessage, ARRAYSIZE(szMessage), pszAction ? pszAction : L"operation failed");
	StringAppend(szMessage, ARRAYSIZE(szMessage), L": 0x");
	AppendHexUlong(szMessage, ARRAYSIZE(szMessage), (ULONG)hr);
	if (szErrorText[0])
	{
		StringAppend(szMessage, ARRAYSIZE(szMessage), L" ");
		StringAppend(szMessage, ARRAYSIZE(szMessage), szErrorText);
	}
	StringAppend(szMessage, ARRAYSIZE(szMessage), L"\r\n");

	WriteWideToStandardError(szMessage);
	return((INT)hr);
}

static BOOL IsCommand( PCWSTR pszArg, PCWSTR pszCommand )
{
	if (!pszArg || !pszCommand)
		return(FALSE);

	if (*pszArg != L'/' && *pszArg != L'-')
		return(FALSE);

	++pszArg;
	if (*pszArg == L'-')
		++pszArg;

	return(lstrcmpiW(pszArg, pszCommand) == 0);
}

static BOOL IsHelpCommand( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"?") ||
	       IsCommand(pszArg, L"h") ||
	       IsCommand(pszArg, L"help"));
}

static BOOL IsOutputOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"output") ||
	       IsCommand(pszArg, L"out") ||
	       IsCommand(pszArg, L"o"));
}

static BOOL IsHashOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"hash") ||
	       IsCommand(pszArg, L"algorithm") ||
	       IsCommand(pszArg, L"type"));
}

static BOOL IsEncodingOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"encoding"));
}

static BOOL IsEolOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"eol"));
}

static BOOL IsNoQueueOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"noqueue") ||
	       IsCommand(pszArg, L"no-queue"));
}

static BOOL IsSeparateOption( PCWSTR pszArg )
{
	return(IsCommand(pszArg, L"separate") ||
	       IsCommand(pszArg, L"separate-files"));
}

static BOOL HasSilentCreateOption( int argc, LPWSTR *argv, int iFirstArg )
{
	if (!argv)
		return(FALSE);

	for (int iArg = iFirstArg; iArg < argc; ++iArg)
	{
		if (IsOutputOption(argv[iArg]) || IsHashOption(argv[iArg]) ||
		    IsEncodingOption(argv[iArg]) || IsEolOption(argv[iArg]))
		{
			return(TRUE);
		}
	}

	return(FALSE);
}

static INT HashIndexFromName( PCWSTR pszName )
{
	if (!pszName || !*pszName)
		return(0);

	if (lstrcmpiW(pszName, L"crc32") == 0 ||
	    lstrcmpiW(pszName, L"crc-32") == 0 ||
	    lstrcmpiW(pszName, L"sfv") == 0)
		return(HC_HASH_CRC32);

	if (lstrcmpiW(pszName, L"md5") == 0)
		return(HC_HASH_MD5);

	if (lstrcmpiW(pszName, L"sha1") == 0 ||
	    lstrcmpiW(pszName, L"sha-1") == 0)
		return(HC_HASH_SHA1);

	if (lstrcmpiW(pszName, L"sha256") == 0 ||
	    lstrcmpiW(pszName, L"sha-256") == 0)
		return(HC_HASH_SHA256);

	if (lstrcmpiW(pszName, L"sha512") == 0 ||
	    lstrcmpiW(pszName, L"sha-512") == 0)
		return(HC_HASH_SHA512);

	if (lstrcmpiW(pszName, L"sha3-256") == 0 ||
	    lstrcmpiW(pszName, L"sha3_256") == 0 ||
	    lstrcmpiW(pszName, L"sha3256") == 0)
		return(HC_HASH_SHA3_256);

	if (lstrcmpiW(pszName, L"sha3-512") == 0 ||
	    lstrcmpiW(pszName, L"sha3_512") == 0 ||
	    lstrcmpiW(pszName, L"sha3512") == 0)
		return(HC_HASH_SHA3_512);

	if (lstrcmpiW(pszName, L"blake3") == 0)
		return(HC_HASH_BLAKE3);

	if (lstrcmpiW(pszName, L"xxh3") == 0 ||
	    lstrcmpiW(pszName, L"xxh3-64") == 0 ||
	    lstrcmpiW(pszName, L"xxh64") == 0)
		return(HC_HASH_XXH3_64);

	if (lstrcmpiW(pszName, L"xxh128") == 0 ||
	    lstrcmpiW(pszName, L"xxh3-128") == 0)
		return(HC_HASH_XXH3_128);

	return(0);
}

static INT HashIndexFromOutputPath( PCWSTR pszPath )
{
	PCWSTR pszFileName = FindLastChar(pszPath, L'\\');
	pszFileName = pszFileName ? pszFileName + 1 : pszPath;

	PCWSTR pszExt = FindLastChar(pszFileName, L'.');
	if (!pszExt)
		return(HC_HASH_DEFAULT);

	if (lstrcmpiW(pszExt, L".sfv") == 0)
		return(HC_HASH_CRC32);
	if (lstrcmpiW(pszExt, L".md5") == 0)
		return(HC_HASH_MD5);
	if (lstrcmpiW(pszExt, L".sha1") == 0)
		return(HC_HASH_SHA1);
	if (lstrcmpiW(pszExt, L".sha256") == 0)
		return(HC_HASH_SHA256);
	if (lstrcmpiW(pszExt, L".sha512") == 0)
		return(HC_HASH_SHA512);
	if (lstrcmpiW(pszExt, L".sha3-256") == 0)
		return(HC_HASH_SHA3_256);
	if (lstrcmpiW(pszExt, L".sha3-512") == 0)
		return(HC_HASH_SHA3_512);
	if (lstrcmpiW(pszExt, L".blake3") == 0)
		return(HC_HASH_BLAKE3);
	if (lstrcmpiW(pszExt, L".xxh3") == 0)
		return(HC_HASH_XXH3_64);
	if (lstrcmpiW(pszExt, L".xxh128") == 0)
		return(HC_HASH_XXH3_128);

	return(HC_HASH_DEFAULT);
}

static INT EncodingFromName( PCWSTR pszName )
{
	if (lstrcmpiW(pszName, L"utf8") == 0 ||
	    lstrcmpiW(pszName, L"utf-8") == 0)
		return(0);

	if (lstrcmpiW(pszName, L"utf16") == 0 ||
	    lstrcmpiW(pszName, L"utf-16") == 0 ||
	    lstrcmpiW(pszName, L"utf16le") == 0 ||
	    lstrcmpiW(pszName, L"utf-16le") == 0)
		return(1);

	if (lstrcmpiW(pszName, L"ansi") == 0)
		return(2);

	return(-2);
}

static INT EolFromName( PCWSTR pszName )
{
	if (lstrcmpiW(pszName, L"crlf") == 0 ||
	    lstrcmpiW(pszName, L"windows") == 0 ||
	    lstrcmpiW(pszName, L"win") == 0)
		return(0);

	if (lstrcmpiW(pszName, L"lf") == 0 ||
	    lstrcmpiW(pszName, L"unix") == 0)
		return(1);

	return(-2);
}

static INT ShowUsage( )
{
	MessageBoxW(
		NULL,
		L"Create a checksum file:\n"
		L"  HashCheckPackageHost.exe /create [/noqueue] <file-or-folder> [file-or-folder ...]\n\n"
		L"Create one checksum file next to each selected file:\n"
		L"  HashCheckPackageHost.exe /create /separate [/noqueue] <file-or-folder> [file-or-folder ...]\n\n"
		L"Create a checksum file without UI:\n"
		L"  HashCheckPackageHost.exe /create /output <checksum-file> [/hash sha256] [/encoding utf8|utf16|ansi] [/eol crlf|lf] [/noqueue] <file-or-folder> [...]\n\n"
		L"Verify a checksum file:\n"
		L"  HashCheckPackageHost.exe /verify [/noqueue] <checksum-file>\n\n"
		L"Show HashCheck options:\n"
		L"  HashCheckPackageHost.exe /options\n\n"
		L"When paths are supplied without a command, /create is assumed.\n"
		L"Hash jobs are queued locally; /noqueue bypasses the queue for one command-line run.",
		L"HashCheck",
		MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND
	);

	return((INT)HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER));
}

static INT ShowError( PCWSTR pszAction, HRESULT hr )
{
	WCHAR szMessage[768];

	StringCopy(szMessage, ARRAYSIZE(szMessage), pszAction ? pszAction : L"Operation");
	StringAppend(szMessage, ARRAYSIZE(szMessage), L" failed.\n\nError: 0x");
	AppendHexUlong(szMessage, ARRAYSIZE(szMessage), (ULONG)hr);

	MessageBoxW(
		NULL,
		szMessage,
		L"HashCheck",
		MB_OK | MB_ICONERROR | MB_SETFOREGROUND
	);

	return((INT)hr);
}

static HRESULT WriteAll( HANDLE hFile, LPCVOID pvBuffer, DWORD cbBuffer )
{
	DWORD cbWritten = 0;
	if (!WriteFile(hFile, pvBuffer, cbBuffer, &cbWritten, NULL) ||
	    cbWritten != cbBuffer)
	{
		DWORD dwError = GetLastError();
		return(HRESULT_FROM_WIN32(dwError ? dwError : ERROR_WRITE_FAULT));
	}

	return(S_OK);
}

static HRESULT WriteWideString( HANDLE hFile, PCWSTR pszText )
{
	size_t cchText = 0;
	HRESULT hr = StringLength(pszText ? pszText : L"", cchMaxWritableString, &cchText);
	if (FAILED(hr))
		return(hr);

	if (cchText >= cchMaxWritableString)
		return(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

	return(WriteAll(hFile, pszText ? pszText : L"", (DWORD)((cchText + 1) * sizeof(WCHAR))));
}

static HRESULT ParseCreateOptions( int argc, LPWSTR *argv, int iFirstArg, PCREATEOPTIONS pOptions )
{
	if (!argv || !pOptions || iFirstArg >= argc)
		return(E_INVALIDARG);

	ZeroBytes(pOptions, sizeof(*pOptions));
	pOptions->iEncoding = -1;
	pOptions->iEol = -1;

	for (int iArg = iFirstArg; iArg < argc; ++iArg)
	{
		if (IsOutputOption(argv[iArg]))
		{
			if (++iArg >= argc || !*argv[iArg])
				return(E_INVALIDARG);

			pOptions->pszOutputPath = argv[iArg];
			pOptions->bHasSilentOption = TRUE;
		}
		else if (IsHashOption(argv[iArg]))
		{
			if (++iArg >= argc)
				return(E_INVALIDARG);

			pOptions->iHashIndex = HashIndexFromName(argv[iArg]);
			if (!pOptions->iHashIndex)
				return(E_INVALIDARG);

			pOptions->bHasSilentOption = TRUE;
		}
		else if (IsEncodingOption(argv[iArg]))
		{
			if (++iArg >= argc)
				return(E_INVALIDARG);

			pOptions->iEncoding = EncodingFromName(argv[iArg]);
			if (pOptions->iEncoding == -2)
				return(E_INVALIDARG);

			pOptions->bHasSilentOption = TRUE;
		}
		else if (IsEolOption(argv[iArg]))
		{
			if (++iArg >= argc)
				return(E_INVALIDARG);

			pOptions->iEol = EolFromName(argv[iArg]);
			if (pOptions->iEol == -2)
				return(E_INVALIDARG);

			pOptions->bHasSilentOption = TRUE;
		}
		else if (IsNoQueueOption(argv[iArg]))
		{
			pOptions->bBypassQueue = TRUE;
		}
		else if (IsSeparateOption(argv[iArg]))
		{
			pOptions->bSeparateFiles = TRUE;
		}
		else if (*argv[iArg])
		{
			++pOptions->cPaths;
		}
	}

	if (!pOptions->bHasSilentOption)
		return(S_OK);

	if (pOptions->bSeparateFiles || !pOptions->pszOutputPath || !pOptions->cPaths)
		return(E_INVALIDARG);

	if (!pOptions->iHashIndex)
		pOptions->iHashIndex = HashIndexFromOutputPath(pOptions->pszOutputPath);

	return(S_OK);
}

static HRESULT WritePathListFileFromArgs( int argc, LPWSTR *argv, int iFirstPath,
                                          PWSTR pszListPath, UINT cchListPath )
{
	if (!argv || !pszListPath || cchListPath < MAX_PATH || iFirstPath >= argc)
		return(E_INVALIDARG);

	*pszListPath = 0;

	WCHAR szTempPath[MAX_PATH + 1];
	DWORD cchTempPath = GetTempPathW(ARRAYSIZE(szTempPath), szTempPath);
	if (!cchTempPath || cchTempPath >= ARRAYSIZE(szTempPath))
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
		HRESULT hrCreate = HRESULT_FROM_WIN32(GetLastError());
		DeleteFileW(pszListPath);
		*pszListPath = 0;
		return(hrCreate);
	}

	HRESULT hr = WriteAll(hFile, szPathListHeader, sizeof(szPathListHeader));
	UINT cPaths = 0;

	for (int iArg = iFirstPath; SUCCEEDED(hr) && iArg < argc; ++iArg)
	{
		if (IsNoQueueOption(argv[iArg]) || IsSeparateOption(argv[iArg]))
			continue;

		size_t cchPath = 0;
		hr = StringLength(argv[iArg], cchMaxWritableString, &cchPath);
		if (SUCCEEDED(hr))
		{
			if (!cchPath)
				continue;

			if (cchPath >= cchMaxWritableString)
			{
				hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
			}
			else
			{
				hr = WriteAll(hFile, argv[iArg], (DWORD)((cchPath + 1) * sizeof(WCHAR)));
				if (SUCCEEDED(hr))
					++cPaths;
			}
		}
	}

	if (SUCCEEDED(hr) && !cPaths)
		hr = E_INVALIDARG;

	if (SUCCEEDED(hr))
	{
		WCHAR chNul = 0;
		hr = WriteAll(hFile, &chNul, sizeof(chNul));
	}

	CloseHandle(hFile);

	if (FAILED(hr))
	{
		DeleteFileW(pszListPath);
		*pszListPath = 0;
	}

	return(hr);
}

static HRESULT WriteSaveSpecFileFromArgs( int argc, LPWSTR *argv, int iFirstArg,
                                          const CREATEOPTIONS *pOptions, PWSTR pszSpecPath,
                                          UINT cchSpecPath )
{
	if (!argv || !pOptions || !pszSpecPath || cchSpecPath < MAX_PATH)
		return(E_INVALIDARG);

	*pszSpecPath = 0;

	WCHAR szTempPath[MAX_PATH + 1];
	DWORD cchTempPath = GetTempPathW(ARRAYSIZE(szTempPath), szTempPath);
	if (!cchTempPath || cchTempPath >= ARRAYSIZE(szTempPath))
		return(HRESULT_FROM_WIN32(cchTempPath ? ERROR_INSUFFICIENT_BUFFER : GetLastError()));

	if (!GetTempFileNameW(szTempPath, L"HCK", 0, pszSpecPath))
		return(HRESULT_FROM_WIN32(GetLastError()));

	HANDLE hFile = CreateFileW(
		pszSpecPath,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		HRESULT hrCreate = HRESULT_FROM_WIN32(GetLastError());
		DeleteFileW(pszSpecPath);
		*pszSpecPath = 0;
		return(hrCreate);
	}

	WCHAR szNumber[16];
	HRESULT hr = WriteWideString(hFile, szSaveSpecHeader);

	if (SUCCEEDED(hr))
		hr = WriteWideString(hFile, pOptions->pszOutputPath);

	if (SUCCEEDED(hr))
	{
		hr = IntToString(pOptions->iHashIndex, szNumber, ARRAYSIZE(szNumber));
		if (SUCCEEDED(hr))
			hr = WriteWideString(hFile, szNumber);
	}

	if (SUCCEEDED(hr))
	{
		hr = IntToString(pOptions->iEncoding, szNumber, ARRAYSIZE(szNumber));
		if (SUCCEEDED(hr))
			hr = WriteWideString(hFile, szNumber);
	}

	if (SUCCEEDED(hr))
	{
		hr = IntToString(pOptions->iEol, szNumber, ARRAYSIZE(szNumber));
		if (SUCCEEDED(hr))
			hr = WriteWideString(hFile, szNumber);
	}

	for (int iArg = iFirstArg; SUCCEEDED(hr) && iArg < argc; ++iArg)
	{
		if (IsOutputOption(argv[iArg]) || IsHashOption(argv[iArg]) ||
		    IsEncodingOption(argv[iArg]) || IsEolOption(argv[iArg]))
		{
			++iArg;
			continue;
		}

		if (IsNoQueueOption(argv[iArg]))
			continue;

		if (*argv[iArg])
			hr = WriteWideString(hFile, argv[iArg]);
	}

	if (SUCCEEDED(hr))
	{
		WCHAR chNul = 0;
		hr = WriteAll(hFile, &chNul, sizeof(chNul));
	}

	CloseHandle(hFile);

	if (FAILED(hr))
	{
		DeleteFileW(pszSpecPath);
		*pszSpecPath = 0;
	}

	return(hr);
}

static HRESULT GetHashCheckDllPath( PWSTR pszDllPath, UINT cchDllPath )
{
	if (!GetModuleFileNameW(NULL, pszDllPath, cchDllPath))
		return(HRESULT_FROM_WIN32(GetLastError()));

	PWSTR pszSlash = (PWSTR)FindLastChar(pszDllPath, L'\\');
	if (!pszSlash)
		return(E_FAIL);

	*(pszSlash + 1) = 0;
	return(StringAppend(pszDllPath, cchDllPath, L"HashCheck.dll"));
}

static HASHCHECK_NOCF INT RunHashCheckDllVerb( PCSTR pszExportName, PWSTR pszArgument )
{
	WCHAR szDllPath[MAX_PATH << 1];
	HRESULT hr = GetHashCheckDllPath(szDllPath, ARRAYSIZE(szDllPath));
	if (FAILED(hr))
		return((INT)hr);

	HMODULE hDll = LoadLibraryW(szDllPath);
	if (!hDll)
		return((INT)HRESULT_FROM_WIN32(GetLastError()));

	PFNHASHCHECKRUNDLL pfnVerb = (PFNHASHCHECKRUNDLL)GetProcAddress(hDll, pszExportName);
	if (!pfnVerb)
	{
		hr = HRESULT_FROM_WIN32(GetLastError());
		FreeLibrary(hDll);
		return((INT)hr);
	}

	pfnVerb(NULL, hDll, pszArgument, SW_SHOWNORMAL);
	FreeLibrary(hDll);
	return(0);
}

static HASHCHECK_NOCF INT RunHashCheckDllVerbResult( PCSTR pszExportName, PWSTR pszArgument )
{
	WCHAR szDllPath[MAX_PATH << 1];
	HRESULT hr = GetHashCheckDllPath(szDllPath, ARRAYSIZE(szDllPath));
	if (FAILED(hr))
		return((INT)hr);

	HMODULE hDll = LoadLibraryW(szDllPath);
	if (!hDll)
		return((INT)HRESULT_FROM_WIN32(GetLastError()));

	PFNHASHCHECKRUNDLLRESULT pfnVerb = (PFNHASHCHECKRUNDLLRESULT)GetProcAddress(hDll, pszExportName);
	if (!pfnVerb)
	{
		hr = HRESULT_FROM_WIN32(GetLastError());
		FreeLibrary(hDll);
		return((INT)hr);
	}

	INT iResult = pfnVerb(NULL, hDll, pszArgument, SW_SHOWNORMAL);
	FreeLibrary(hDll);
	return(iResult);
}

static INT RunCreateFromPaths( int argc, LPWSTR *argv, int iFirstPath, BOOL bSeparateFiles, BOOL bBypassQueue )
{
	WCHAR szListPath[MAX_PATH + 1];
	HRESULT hr = WritePathListFileFromArgs(argc, argv, iFirstPath, szListPath, ARRAYSIZE(szListPath));
	if (FAILED(hr))
		return(hr == E_INVALIDARG ? ShowUsage() : ShowError(L"Preparing the checksum input list", hr));

	INT iResult = RunHashCheckDllVerb(
		bSeparateFiles ?
			(bBypassQueue ? "HashSaveSeparateNoQueue_RunDLLW" : "HashSaveSeparate_RunDLLW") :
			(bBypassQueue ? "HashSaveNoQueue_RunDLLW" : "HashSave_RunDLLW"),
		szListPath
	);
	if (iResult)
		DeleteFileW(szListPath);

	return(iResult);
}

static INT RunCreateSilent( int argc, LPWSTR *argv, int iFirstArg, const CREATEOPTIONS *pOptions )
{
	WCHAR szSpecPath[MAX_PATH + 1];
	HRESULT hr = WriteSaveSpecFileFromArgs(argc, argv, iFirstArg, pOptions, szSpecPath, ARRAYSIZE(szSpecPath));
	if (FAILED(hr))
		return(ReportErrorToStandardError(L"preparing checksum creation failed", hr));

	INT iResult = RunHashCheckDllVerbResult(
		pOptions->bBypassQueue ? "HashSaveSilentNoQueue_RunDLLW" : "HashSaveSilent_RunDLLW",
		szSpecPath
	);
	if (iResult)
	{
		DeleteFileW(szSpecPath);
		return(ReportErrorToStandardError(L"creating checksum file failed", (HRESULT)iResult));
	}

	return(0);
}

static INT RunCreateCommand( int argc, LPWSTR *argv, int iFirstArg )
{
	CREATEOPTIONS options;
	BOOL bSilentRequested = HasSilentCreateOption(argc, argv, iFirstArg);
	HRESULT hr = ParseCreateOptions(argc, argv, iFirstArg, &options);
	if (FAILED(hr))
	{
		return(bSilentRequested ?
		       ReportErrorToStandardError(L"invalid command-line parameters", hr) :
		       ShowUsage());
	}

	if (options.bHasSilentOption)
		return(RunCreateSilent(argc, argv, iFirstArg, &options));

	return(RunCreateFromPaths(argc, argv, iFirstArg, options.bSeparateFiles, options.bBypassQueue));
}

static INT RunVerifyCommand( int argc, LPWSTR *argv, int iFirstArg )
{
	BOOL bBypassQueue = FALSE;
	PWSTR pszPath = NULL;

	for (int iArg = iFirstArg; iArg < argc; ++iArg)
	{
		if (IsNoQueueOption(argv[iArg]))
		{
			bBypassQueue = TRUE;
		}
		else if (*argv[iArg] && !pszPath)
		{
			pszPath = argv[iArg];
		}
		else if (*argv[iArg])
		{
			return(ShowUsage());
		}
	}

	return(pszPath ?
	       RunHashCheckDllVerb(bBypassQueue ? "HashVerifyNoQueue_RunDLLW" : "HashVerify_RunDLLW", pszPath) :
	       ShowUsage());
}

static INT HashCheckPackageHostMain( )
{
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return((INT)HRESULT_FROM_WIN32(GetLastError()));

	INT iResult = 0;

	if (argc < 2 || IsHelpCommand(argv[1]))
	{
		iResult = ShowUsage();
	}
	else if (argc == 3 && IsCommand(argv[1], L"hashcheck-create"))
	{
		iResult = RunHashCheckDllVerb("HashSave_RunDLLW", argv[2]);
	}
	else if (argc == 3 && IsCommand(argv[1], L"hashcheck-create-separate"))
	{
		iResult = RunHashCheckDllVerb("HashSaveSeparate_RunDLLW", argv[2]);
	}
	else if (argc == 3 && IsCommand(argv[1], L"hashcheck-verify"))
	{
		iResult = RunHashCheckDllVerb("HashVerify_RunDLLW", argv[2]);
	}
	else if (argc == 3 && IsCommand(argv[1], L"hashcheck-options"))
	{
		iResult = RunHashCheckDllVerb("ShowOptions_RunDLLW", argv[2]);
	}
	else if (IsCommand(argv[1], L"create"))
	{
		iResult = RunCreateCommand(argc, argv, 2);
	}
	else if (IsCommand(argv[1], L"verify"))
	{
		iResult = RunVerifyCommand(argc, argv, 2);
	}
	else if (IsCommand(argv[1], L"options"))
	{
		iResult = (argc == 2) ? RunHashCheckDllVerb("ShowOptions_RunDLLW", L"") : ShowUsage();
	}
	else
	{
		iResult = RunCreateCommand(argc, argv, 1);
	}

	LocalFree(argv);
	return(iResult);
}

void __cdecl wWinMainCRTStartup( void )
{
	ExitProcess((UINT)HashCheckPackageHostMain());
}
