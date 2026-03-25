/*
 * version.dll — DLL proxy chainloader
 *
 * Drop this as version.dll next to the game executable along with
 * chainloader.ini. On load it:
 *   1. Forwards all 17 version.dll exports to the real system version.dll
 *   2. Reads chainloader.ini for configuration
 *   3. Scans configured directories for .dll mod files
 *   4. Optionally reads a Mewtator manifest for managed mod DLLs
 *   5. Logs everything to mod_logs/chainloader.log
 *
 * Build (MSVC):
 *   cl /LD /O2 /GS- version.c /Fe:version.dll /link /DEF:version.def
 *
 * The .def file controls the export table so the game finds our forwarded
 * functions by name, exactly matching the real version.dll.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Game base directory (resolved once, used everywhere)
 * =================================================================== */

static char g_baseDir[MAX_PATH];  /* Ends with backslash */

static void ResolveBaseDir(void)
{
    GetModuleFileNameA(NULL, g_baseDir, MAX_PATH);
    char* slash = strrchr(g_baseDir, '\\');
    if (slash) *(slash + 1) = '\0';
    else strcpy(g_baseDir, ".\\");
}

/* ===================================================================
 *  Logging — writes to mod_logs/chainloader.log
 * =================================================================== */

static FILE*  g_logFile  = NULL;
static HANDLE g_logMutex = NULL;

static void LogOpen(void)
{
    char logDir[MAX_PATH];
    char logPath[MAX_PATH];

    snprintf(logDir, MAX_PATH, "%smod_logs", g_baseDir);
    CreateDirectoryA(logDir, NULL);

    snprintf(logPath, MAX_PATH, "%s\\chainloader.log", logDir);

    g_logFile  = fopen(logPath, "w");
    g_logMutex = CreateMutexA(NULL, FALSE, NULL);
}

static void LogClose(void)
{
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
    if (g_logMutex) { CloseHandle(g_logMutex); g_logMutex = NULL; }
}

static void CLog(const char* fmt, ...)
{
    if (!g_logFile) return;
    WaitForSingleObject(g_logMutex, INFINITE);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);

    fprintf(g_logFile, "\n");
    fflush(g_logFile);

    ReleaseMutex(g_logMutex);
}

/* ===================================================================
 *  INI configuration
 * =================================================================== */

typedef struct {
    int  enabled;            /* Master switch                            */
    int  logging;            /* Whether to log (already open by now)     */
    char scanPath[MAX_PATH]; /* Directory to scan for DLLs (relative)    */
    int  scanGameDir;        /* Also scan the exe's own directory        */
    char manifest[MAX_PATH]; /* Mewtator manifest path (abs or relative) */
} ChainloaderConfig;

static ChainloaderConfig g_config;

/* Trim leading/trailing whitespace + quotes in-place */
static void TrimInPlace(char* s)
{
    /* Leading */
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '"') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* Trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
           s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == '"'))
        s[--len] = '\0';
}

static void LoadConfig(void)
{
    char iniPath[MAX_PATH];
    char buf[MAX_PATH];

    /* Defaults — these apply if no ini exists */
    g_config.enabled     = 1;
    g_config.logging     = 1;
    g_config.scanGameDir = 0;
    strcpy(g_config.scanPath, "mods");
    g_config.manifest[0] = '\0';

    snprintf(iniPath, MAX_PATH, "%schainloader.ini", g_baseDir);

    /* Check if ini exists */
    DWORD attr = GetFileAttributesA(iniPath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        CLog("No chainloader.ini found — using defaults");
        CLog("  ScanPath=%s, ScanGameDir=%d",
             g_config.scanPath, g_config.scanGameDir);
        return;
    }

    CLog("Loading config from %s", iniPath);

    /* Enabled */
    GetPrivateProfileStringA("Chainloader", "Enabled", "1",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.enabled = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* Logging */
    GetPrivateProfileStringA("Chainloader", "Logging", "1",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.logging = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* ScanPath */
    GetPrivateProfileStringA("Chainloader", "ScanPath", "mods",
                             g_config.scanPath, sizeof(g_config.scanPath),
                             iniPath);
    TrimInPlace(g_config.scanPath);

    /* ScanGameDir */
    GetPrivateProfileStringA("Chainloader", "ScanGameDir", "0",
                             buf, sizeof(buf), iniPath);
    TrimInPlace(buf);
    g_config.scanGameDir = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    /* MewtatorManifest */
    GetPrivateProfileStringA("Chainloader", "MewtatorManifest", "",
                             g_config.manifest, sizeof(g_config.manifest),
                             iniPath);
    TrimInPlace(g_config.manifest);

    CLog("  Enabled=%d, Logging=%d, ScanPath=%s, ScanGameDir=%d",
         g_config.enabled, g_config.logging,
         g_config.scanPath, g_config.scanGameDir);
    if (g_config.manifest[0])
        CLog("  MewtatorManifest=%s", g_config.manifest);
    else
        CLog("  MewtatorManifest=(none)");
}

/* ===================================================================
 *  Real version.dll forwarding
 * =================================================================== */

static HMODULE g_realVersionDll = NULL;

/* Function pointers for all 17 exports */
static FARPROC pfn_GetFileVersionInfoA        = NULL;
static FARPROC pfn_GetFileVersionInfoByHandle  = NULL;
static FARPROC pfn_GetFileVersionInfoExA       = NULL;
static FARPROC pfn_GetFileVersionInfoExW       = NULL;
static FARPROC pfn_GetFileVersionInfoSizeA     = NULL;
static FARPROC pfn_GetFileVersionInfoSizeExA   = NULL;
static FARPROC pfn_GetFileVersionInfoSizeExW   = NULL;
static FARPROC pfn_GetFileVersionInfoSizeW     = NULL;
static FARPROC pfn_GetFileVersionInfoW         = NULL;
static FARPROC pfn_VerFindFileA                = NULL;
static FARPROC pfn_VerFindFileW                = NULL;
static FARPROC pfn_VerInstallFileA             = NULL;
static FARPROC pfn_VerInstallFileW             = NULL;
static FARPROC pfn_VerLanguageNameA            = NULL;
static FARPROC pfn_VerLanguageNameW            = NULL;
static FARPROC pfn_VerQueryValueA              = NULL;
static FARPROC pfn_VerQueryValueW              = NULL;

static int LoadRealVersionDll(void)
{
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    strcat(sysDir, "\\version.dll");

    g_realVersionDll = LoadLibraryA(sysDir);
    if (!g_realVersionDll) {
        CLog("FATAL: Could not load real version.dll from %s (error %lu)",
             sysDir, GetLastError());
        return 0;
    }

    CLog("Loaded real version.dll from %s", sysDir);

    #define RESOLVE(name) \
        pfn_##name = GetProcAddress(g_realVersionDll, #name); \
        if (!pfn_##name) CLog("  WARNING: Could not resolve " #name);

    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoByHandle);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerLanguageNameA);
    RESOLVE(VerLanguageNameW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);

    #undef RESOLVE

    return 1;
}

/* ===================================================================
 *  Exported forwarding stubs
 *
 *  Each is a thin wrapper that tail-calls the real function.
 *  The .def file maps the real export names to these Proxy_ functions.
 * =================================================================== */

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoA(
    LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(LPCSTR, DWORD, DWORD, LPVOID);
    return ((fn_t)pfn_GetFileVersionInfoA)(lptstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) int WINAPI Proxy_GetFileVersionInfoByHandle(
    int hMem, LPCWSTR lpFileName, HANDLE handle, LPVOID lpData, DWORD cbData)
{
    typedef int (WINAPI *fn_t)(int, LPCWSTR, HANDLE, LPVOID, DWORD);
    if (!pfn_GetFileVersionInfoByHandle) return 0;
    return ((fn_t)pfn_GetFileVersionInfoByHandle)(hMem, lpFileName, handle, lpData, cbData);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoExA(
    DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    if (!pfn_GetFileVersionInfoExA) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoExA)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoExW(
    DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    if (!pfn_GetFileVersionInfoExW) return FALSE;
    return ((fn_t)pfn_GetFileVersionInfoExW)(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeA(
    LPCSTR lptstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(LPCSTR, LPDWORD);
    return ((fn_t)pfn_GetFileVersionInfoSizeA)(lptstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(
    DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPDWORD);
    if (!pfn_GetFileVersionInfoSizeExA) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeExA)(dwFlags, lpwstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(
    DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPDWORD);
    if (!pfn_GetFileVersionInfoSizeExW) return 0;
    return ((fn_t)pfn_GetFileVersionInfoSizeExW)(dwFlags, lpwstrFilename, lpdwHandle);
}

__declspec(dllexport) DWORD WINAPI Proxy_GetFileVersionInfoSizeW(
    LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
{
    typedef DWORD (WINAPI *fn_t)(LPCWSTR, LPDWORD);
    return ((fn_t)pfn_GetFileVersionInfoSizeW)(lptstrFilename, lpdwHandle);
}

__declspec(dllexport) BOOL WINAPI Proxy_GetFileVersionInfoW(
    LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    typedef BOOL (WINAPI *fn_t)(LPCWSTR, DWORD, DWORD, LPVOID);
    return ((fn_t)pfn_GetFileVersionInfoW)(lptstrFilename, dwHandle, dwLen, lpData);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerFindFileA(
    DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir,
    LPSTR szCurDir, PUINT puCurDirLen, LPSTR szDestDir, PUINT puDestDirLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    return ((fn_t)pfn_VerFindFileA)(uFlags, szFileName, szWinDir, szAppDir,
                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerFindFileW(
    DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir,
    LPWSTR szCurDir, PUINT puCurDirLen, LPWSTR szDestDir, PUINT puDestDirLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    return ((fn_t)pfn_VerFindFileW)(uFlags, szFileName, szWinDir, szAppDir,
                                     szCurDir, puCurDirLen, szDestDir, puDestDirLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerInstallFileA(
    DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir,
    LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT puTmpFileLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    return ((fn_t)pfn_VerInstallFileA)(uFlags, szSrcFileName, szDestFileName, szSrcDir,
                                        szDestDir, szCurDir, szTmpFile, puTmpFileLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerInstallFileW(
    DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir,
    LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT puTmpFileLen)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    return ((fn_t)pfn_VerInstallFileW)(uFlags, szSrcFileName, szDestFileName, szSrcDir,
                                        szDestDir, szCurDir, szTmpFile, puTmpFileLen);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPSTR, DWORD);
    return ((fn_t)pfn_VerLanguageNameA)(wLang, szLang, cchLang);
}

__declspec(dllexport) DWORD WINAPI Proxy_VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang)
{
    typedef DWORD (WINAPI *fn_t)(DWORD, LPWSTR, DWORD);
    return ((fn_t)pfn_VerLanguageNameW)(wLang, szLang, cchLang);
}

__declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueA(
    LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    typedef BOOL (WINAPI *fn_t)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    return ((fn_t)pfn_VerQueryValueA)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

__declspec(dllexport) BOOL WINAPI Proxy_VerQueryValueW(
    LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    typedef BOOL (WINAPI *fn_t)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    return ((fn_t)pfn_VerQueryValueW)(pBlock, lpSubBlock, lplpBuffer, puLen);
}

/* ===================================================================
 *  Mod DLL scanning and loading
 * =================================================================== */

/* DLLs to skip — case-insensitive comparison */
static const char* g_excludeList[] = {
    "version.dll",            /* ourselves                          */
    "steam_api.dll",          /* Steam                              */
    "steam_api64.dll",        /* Steam 64-bit                       */
    "steamclient.dll",        /* Steam client                       */
    "steamclient64.dll",      /* Steam client 64-bit                */
    "tier0_s.dll",            /* Source/Steam tier                   */
    "tier0_s64.dll",
    "vstdlib_s.dll",          /* Valve stdlib                       */
    "vstdlib_s64.dll",
    "gameoverlayrenderer.dll",
    "gameoverlayrenderer64.dll",
    "d3dcompiler_47.dll",     /* DirectX runtime                    */
    "xaudio2_9.dll",
    "xinput1_3.dll",
    "xinput1_4.dll",
    "xinput9_1_0.dll",
    NULL
};

static int IsExcluded(const char* filename)
{
    for (int i = 0; g_excludeList[i] != NULL; i++) {
        if (_stricmp(filename, g_excludeList[i]) == 0)
            return 1;
    }
    return 0;
}

/* Track loaded DLLs to avoid double-loading from multiple sources */
#define MAX_LOADED 256
static char g_loadedDlls[MAX_LOADED][MAX_PATH];
static int  g_loadedCount = 0;

static int AlreadyLoaded(const char* fullPath)
{
    for (int i = 0; i < g_loadedCount; i++) {
        if (_stricmp(g_loadedDlls[i], fullPath) == 0)
            return 1;
    }
    return 0;
}

static void RecordLoaded(const char* fullPath)
{
    if (g_loadedCount < MAX_LOADED) {
        strncpy(g_loadedDlls[g_loadedCount], fullPath, MAX_PATH - 1);
        g_loadedDlls[g_loadedCount][MAX_PATH - 1] = '\0';
        g_loadedCount++;
    }
}

/* Scan a directory for .dll files and load them */
static void ScanAndLoadDir(const char* dirPath, const char* label)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*.dll", dirPath);

    CLog("[%s] Scanning: %s", label, dirPath);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            CLog("[%s]   No .dll files found", label);
        else if (err == ERROR_PATH_NOT_FOUND)
            CLog("[%s]   Directory does not exist", label);
        else
            CLog("[%s]   FindFirstFile error %lu", label, err);
        return;
    }

    int loaded = 0, skipped = 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (IsExcluded(fd.cFileName)) {
            CLog("[%s]   SKIP (excluded): %s", label, fd.cFileName);
            skipped++;
            continue;
        }

        char fullPath[MAX_PATH];
        snprintf(fullPath, MAX_PATH, "%s\\%s", dirPath, fd.cFileName);

        if (AlreadyLoaded(fullPath)) {
            CLog("[%s]   SKIP (already loaded): %s", label, fd.cFileName);
            skipped++;
            continue;
        }

        CLog("[%s]   Loading: %s", label, fd.cFileName);

        HMODULE hMod = LoadLibraryA(fullPath);
        if (hMod) {
            CLog("[%s]     OK at 0x%p", label, (void*)hMod);
            RecordLoaded(fullPath);
            loaded++;
        } else {
            DWORD err = GetLastError();
            CLog("[%s]     FAILED — error %lu (0x%08lX)", label, err, err);
        }

    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    CLog("[%s]   Done: %d loaded, %d skipped", label, loaded, skipped);
}

/* ===================================================================
 *  Mewtator manifest loading
 * =================================================================== */

static void LoadMewtatorManifest(const char* manifestPath)
{
    char resolvedPath[MAX_PATH];

    /* Resolve relative paths against game directory */
    if (manifestPath[0] != '\\' && !(manifestPath[1] == ':')) {
        snprintf(resolvedPath, MAX_PATH, "%s%s", g_baseDir, manifestPath);
    } else {
        strncpy(resolvedPath, manifestPath, MAX_PATH - 1);
        resolvedPath[MAX_PATH - 1] = '\0';
    }

    CLog("[Mewtator] Reading manifest: %s", resolvedPath);

    FILE* f = fopen(resolvedPath, "r");
    if (!f) {
        CLog("[Mewtator]   File not found or unreadable — skipping");
        return;
    }

    char line[MAX_PATH];
    int loaded = 0, skipped = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#')
            continue;

        TrimInPlace(line);
        len = strlen(line);

        /* Validate: must end in .dll */
        if (len < 5 || _stricmp(line + len - 4, ".dll") != 0) {
            CLog("[Mewtator]   SKIP (not .dll): %s", line);
            skipped++;
            continue;
        }

        /* Check file exists */
        DWORD attr = GetFileAttributesA(line);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            CLog("[Mewtator]   SKIP (not found): %s", line);
            skipped++;
            continue;
        }

        if (AlreadyLoaded(line)) {
            CLog("[Mewtator]   SKIP (already loaded): %s", line);
            skipped++;
            continue;
        }

        CLog("[Mewtator]   Loading: %s", line);

        HMODULE hMod = LoadLibraryA(line);
        if (hMod) {
            CLog("[Mewtator]     OK at 0x%p", (void*)hMod);
            RecordLoaded(line);
            loaded++;
        } else {
            DWORD err = GetLastError();
            CLog("[Mewtator]     FAILED — error %lu (0x%08lX)", err, err);
        }
    }

    fclose(f);
    CLog("[Mewtator]   Done: %d loaded, %d skipped", loaded, skipped);
}

/* ===================================================================
 *  DLL entry point
 * =================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        ResolveBaseDir();
        LogOpen();

        CLog("=== Chainloader v2.0 ===");
        CLog("Process: PID %lu", GetCurrentProcessId());
        CLog("Game directory: %s", g_baseDir);
        CLog("");

        if (!LoadRealVersionDll()) {
            CLog("Cannot continue without real version.dll. Aborting mod load.");
            LogClose();
            return TRUE;
        }
        CLog("");

        LoadConfig();
        CLog("");

        if (!g_config.enabled) {
            CLog("Chainloader DISABLED via config. No mods will be loaded.");
            CLog("=== Chainloader init complete (disabled) ===");
            return TRUE;
        }

        /* Phase 1: Scan configured mod directory */
        if (g_config.scanPath[0]) {
            char scanDir[MAX_PATH];
            /* Resolve relative paths against game directory */
            if (g_config.scanPath[0] != '\\' &&
                !(g_config.scanPath[1] == ':')) {
                snprintf(scanDir, MAX_PATH, "%s%s", g_baseDir, g_config.scanPath);
            } else {
                strncpy(scanDir, g_config.scanPath, MAX_PATH - 1);
                scanDir[MAX_PATH - 1] = '\0';
            }
            ScanAndLoadDir(scanDir, "ScanPath");
            CLog("");
        }

        /* Phase 2: Optionally scan game exe directory */
        if (g_config.scanGameDir) {
            /* Strip trailing backslash for the scan */
            char gameDir[MAX_PATH];
            strncpy(gameDir, g_baseDir, MAX_PATH - 1);
            gameDir[MAX_PATH - 1] = '\0';
            size_t gdLen = strlen(gameDir);
            if (gdLen > 0 && gameDir[gdLen-1] == '\\')
                gameDir[gdLen-1] = '\0';
            ScanAndLoadDir(gameDir, "GameDir");
            CLog("");
        }

        /* Phase 3: Mewtator manifest */
        if (g_config.manifest[0]) {
            LoadMewtatorManifest(g_config.manifest);
            CLog("");
        }

        CLog("=== Chainloader init complete: %d DLL(s) loaded ===",
             g_loadedCount);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        CLog("=== Chainloader detaching ===");
        LogClose();

        if (g_realVersionDll) {
            FreeLibrary(g_realVersionDll);
            g_realVersionDll = NULL;
        }
    }

    return TRUE;
}
