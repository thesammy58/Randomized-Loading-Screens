// ============================================================================
//  Randomized Loading Screens by thesammy58
//
//  An ASI mod for Sims 3 that is mapped by an ASI loader. It
//  picks a random loading screen from a pool and installs it as the active
//  override before the game indexes its packages.
//
//  All work runs once, synchronously, in DllMain (before the game's entry
//  point) after which the DLL is idle for the rest of the session. It calls
//  only kernel32/advapi32 (no user32/shell32/COM), which keeps it safe under
//  the Windows loader lock.
//
//  What it explicitly touches on disk: reads a folder (pool) of .package files
//  sitting next to the .asi; copies the chosen one into the game's user
//  Mods\LoadingScreen folder; adds one high-priority block to resource.cfg
//  (backing the file up first); and writes a small log and state file.
// ============================================================================

#include "pch.h"
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Switch to 'false' to turn off logging
static const bool LOGGING = true;

static const wchar_t* MOD_VERSION = L"1.4.6";

// Mods folder override; takes precedence over every other method of detection
static const wchar_t* MODS_DIR = L"";

// Pool folder that sits next to this .asi (in Game\Bin)
static const wchar_t* POOL_SUBFOLDER = L"MyLoadingScreens";

// The Sims 3 loads this .asi into more than one process (launcher/
// bootstrapper AND the game). We only swap inside the actual game process, or
// the two randomization runs fight each other.
//
// The game's rendering executable differs by install type:
//   EA App / Origin (patch 1.69)   -> TS3.exe
//   Steam / retail  (patch 1.67)   -> TS3W.exe
// Both files exist in Bin, but only the one matching the player's install runs.
// Launchers that also load this .asi have other names (Sims3Launcher.exe,
// Sims3LauncherW.exe, EADesktop.exe), so matching either game name below is
// safe on both install types and won't cause a double loading screen swap.
static const wchar_t* GAME_EXES[] = { L"TS3.exe", L"TS3W.exe" };

// Note: most references to Tiny UI Fix below will henceforth be written as "TUIF"
// because this will be brought up a lot. Like a LOT.
// 
// Our resource.cfg block is built at runtime (see BuildCfgBlock) so its priority
// can adapt. Two slots: the layout slot (harvested TUIF-scaled LAYOs) sits one
// priority above the content slot, so when present it overrides the main loading screen
// package's own native-scale LAYO. A PackedFile line pointing at a slot file that
// doesn't exist (TUIF absent) is harmless as the game just skips it, so both lines
// can safely be present at all times. The block ends with "Priority 0" to reset
// the ambient priority, so our high number applies ONLY to our two lines and does
// not silently promote the user's own entries below us. Thank you to "Just Harry"
// (author of TUIF) for pointing this out.
static const long BASE_PRIORITY = 50000;     // content slot; layout slot = BASE_PRIORITY + 1
static const long PRIORITY_CUSHION = 10;      // how far above a conflicting mod we jump when escalating

// Marker used to detect whether our block is already present in resource.cfg.
static const char* CFG_TOKEN = "LoadingScreen/active_loadingscreen_layout.package";

static const wchar_t* DEST_SUBFOLDER = L"LoadingScreen";                // under MODS_DIR
static const wchar_t* ACTIVE_SLOT_NAME = L"active_loadingscreen.package"; // single stable slot
static const wchar_t* CFG_NAME = L"resource.cfg";
static const wchar_t* LOG_NAME = L"RandomizedLoadingScreens.log";
static const unsigned LOG_MAX_BYTES = 256u * 1024u;  // hard-reset the log past this size
static const wchar_t* LOG_SEP = L"------------------------------------------------------------";
static const wchar_t* STATE_NAME = L"last_pick.txt";          // remembers the previous pick
static const wchar_t* BACKUP_NAME = L"resource.cfg.bak";       // cfg backup (kept in dest folder)
static const wchar_t* INI_NAME = L"RandomizedLoadingScreens.ini"; // optional manual override (next to .asi)

// --- Layout harvesting (Tiny UI Fix follow-along) --------------------------
// TS3 layout resource type (LAYO). Confirmed against Tiny UI Fix's own source.
static const uint32_t LAYO_TYPE = 0x025C95B6;
// Second active slot: the harvested TUIF-scaled layout, one priority above the
// content slot (see CFG_BLOCK).
static const wchar_t* LAYOUT_SLOT_NAME = L"active_loadingscreen_layout.package";
// Tiny UI Fix always writes its generated package here, under the Mods folder.
static const wchar_t* TUIF_REL_PATH = L"\\TinyUIFix\\tiny-ui-fix.package";
// Custom marker resource the mod's included pool packages carry: its TGI instance
// is EA's native instance ID of the loading screen the LAYO was copied from. This
// lets the ASI identify the picked screen by a stable address, as pattern matching
// is virtually impossible (TUIF changes almost every unique, identifiable value
// within the scaled LAYOs). The type is an ASCII tag ("LSNI"), chosen to avoid
// potential collision with TS3's own resource types.
static const uint32_t MARKER_TYPE = 0x4C534E49;   // 'LSNI'

// Small kernel32-only helpers (safe to call this early in the process)
static std::wstring GetSelfDir(HMODULE self)
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

// The filename (no path) of the process we're running inside. GetModuleFileNameW
// with NULL asks for the MAIN executable, i.e. the host program, not this DLL.
static std::wstring GetHostExeName()
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
}

// The Documents folder. We read it from the registry rather than assuming
// %USERPROFILE%\Documents, because a large number of people have Documents
// redirected - the registry value reflects that, a naive
// %USERPROFILE% guess does not. Uses only advapi32 (Reg*) + kernel32, so it
// stays safe this early in boot (no shell32, no loader-lock risk). Falls back to
// %USERPROFILE%\Documents only if the registry read fails outright
static std::wstring GetDocumentsDir()
{
    std::wstring result;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
        0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        wchar_t raw[MAX_PATH] = {};
        DWORD type = 0, size = sizeof(raw);
        LONG r = RegQueryValueExW(hKey, L"Personal", nullptr, &type,
            reinterpret_cast<LPBYTE>(raw), &size);
        RegCloseKey(hKey);

        if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            // The value is often "%USERPROFILE%\Documents"; expand any env vars
            wchar_t expanded[MAX_PATH] = {};
            DWORD n = ExpandEnvironmentStringsW(raw, expanded, MAX_PATH);
            result = (n > 0 && n <= MAX_PATH) ? expanded : raw;
        }
    }

    if (result.empty()) {
        wchar_t profile[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            result = std::wstring(profile) + L"\\Documents";
    }
    return result;
}

// Read a string-table entry by ID from a module using ONLY kernel32 resource
// APIs (FindResource/LoadResource/LockResource) - deliberately avoiding user32's
// LoadStringW so we stay safe this early in boot. RT_STRING resources are packed
// in bundles of 16 length-prefixed UTF-16 strings; string N lives in bundle
// (N/16)+1 at slot N%16.
static std::wstring LoadStringResourceW(HMODULE mod, UINT id)
{
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW((id >> 4) + 1), RT_STRING);
    if (!res) return std::wstring();
    HGLOBAL glob = LoadResource(mod, res);
    if (!glob) return std::wstring();
    const wchar_t* p = static_cast<const wchar_t*>(LockResource(glob));
    if (!p) return std::wstring();
    const wchar_t* end = p + SizeofResource(mod, res) / sizeof(wchar_t);

    UINT slot = id & 15;
    for (UINT i = 0; i < 16 && p < end; ++i) {
        WORD len = *reinterpret_cast<const WORD*>(p);
        ++p;                               // step past the length word
        if (i == slot)
            return (len && p + len <= end) ? std::wstring(p, len) : std::wstring();
        p += len;                          // skip this entry's characters
    }
    return std::wstring();
}

// Read the GAME's own configured locale (e.g. "en-US") from the registry. This
// is the language the game runs in, which is what names its Documents folder -
// and it is NOT necessarily the Windows locale (someone can run, say, the Korean
// game on English Windows). Changing this value is literally how players switch
// the game's language, so it's the authoritative source. The key differs by
// install type; our 32-bit process is auto-redirected to Wow6432Node, so we
// don't spell that out. Returns L"" if none of the known keys have it, in which
// case the caller falls back to the Windows locale.
static std::wstring GetGameLocale()
{
    const wchar_t* keys[] = {
        L"SOFTWARE\\Sims\\The Sims 3",          // DVD / retail
        L"SOFTWARE\\Sims(Steam)\\The Sims 3",   // Steam
        L"SOFTWARE\\Origin Games\\The Sims 3",  // Origin / EA App
        L"SOFTWARE\\Electronic Arts\\The Sims 3",
    };
    for (const wchar_t* k : keys) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
            wchar_t val[64] = {};
            DWORD type = 0, size = sizeof(val);
            LONG r = RegQueryValueExW(h, L"Locale", nullptr, &type,
                reinterpret_cast<LPBYTE>(val), &size);
            RegCloseKey(h);
            if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && val[0])
                return val;
        }
    }
    return std::wstring();
}

// The Sims 3's user-data folder name is localized: English "The Sims 3", German
// "Die Sims 3", Korean uses its own name, etc. The game chooses it from its own
// exe string table by locale, so we read the SAME table to match exactly.
// Hardcoding "The Sims 3" will put a non-English user's files in the wrong folder.
// We drive the lookup with the GAME's locale (registry) when available, so a
// mismatched setup (e.g. Korean game on English Windows) still resolves right,
// and fall back to the Windows locale otherwise. Technique credit: sims3friend /
// Sims 3 Settings Setter; string read reimplemented kernel32-only for early boot.
// Table layout: per-locale blocks of 100 IDs; base+0 = locale code ("ko-kr"),
// base+2 = the localized folder name. outLocale/outStatus are for the log only.
static std::wstring ResolveLocalizedGameFolder(std::wstring& outLocale,
    std::wstring& outStatus)
{
    outLocale.clear();

    HMODULE exe = GetModuleHandleW(nullptr);   // the game exe
    if (!exe) { outStatus = L"default (no exe handle)"; return L"The Sims 3"; }

    // Prefer the game's own locale; fall back to the Windows locale.
    std::wstring source = L"game registry";
    std::wstring locale = GetGameLocale();
    if (locale.empty()) {
        wchar_t sys[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(sys, LOCALE_NAME_MAX_LENGTH) > 0) {
            locale = sys;
            source = L"windows locale";
        }
    }
    if (locale.empty()) { outStatus = L"default (no locale)"; return L"The Sims 3"; }

    // Normalise: lowercase, and unify the separator. EA is inconsistent (the
    // registry gives "en_us" but the exe string table stores "en-us") so we map
    // '_' to '-' on BOTH sides before comparing, or every match falls through.
    for (auto& ch : locale) {
        if (ch >= L'A' && ch <= L'Z') ch = wchar_t(ch - L'A' + L'a');
        else if (ch == L'_') ch = L'-';
    }
    outLocale = locale + L" (" + source + L")";

    std::wstring sysLang(locale);              // language prefix, e.g. "en" from "en-au"
    size_t hyphen = sysLang.find(L'-');
    if (hyphen != std::wstring::npos) sysLang.resize(hyphen);

    UINT prefixMatch = 0;
    for (UINT base = 1000; base < 3600; base += 100) {
        std::wstring loc = LoadStringResourceW(exe, base);
        if (loc.empty()) continue;
        for (auto& ch : loc) {
            if (ch >= L'A' && ch <= L'Z') ch = wchar_t(ch - L'A' + L'a');
            else if (ch == L'_') ch = L'-';    // same normalisation as above
        }

        if (loc == locale) {                   // exact locale match wins
            std::wstring name = LoadStringResourceW(exe, base + 2);
            if (!name.empty()) { outStatus = L"exact match"; return name; }
        }
        if (prefixMatch == 0 && loc.size() >= sysLang.size() &&
            loc.compare(0, sysLang.size(), sysLang) == 0)
            prefixMatch = base;                // remember first language-prefix match
    }
    if (prefixMatch != 0) {                     // fall back to same-language locale
        std::wstring name = LoadStringResourceW(exe, prefixMatch + 2);
        if (!name.empty()) { outStatus = L"language fallback"; return name; }
    }
    outStatus = L"default (no table match)";
    return L"The Sims 3";                       // last resort
}

// Optional manual override: read a folder name from RandomizedLoadingScreens.ini
// sitting next to the .asi. GetPrivateProfileStringW returns "" if the file or
// the [LoadingScreen] Language key is absent, which is hopefully the case for most,
// since the .ini ships separately and is only added when auto-detection is wrong.
// This is a READ only: we never create the file. The .ini is saved UTF-16LE, the
// encoding this API reads natively, so non-Latin folder names come back intact.
// Each .ini line reads "Label (Folder Name)" so it can show a readable language
// label; we use the text inside the parentheses (or the whole value if there are
// none, so a bare folder name also works).
static std::wstring ReadIniFolderName(const std::wstring& selfDir)
{
    std::wstring iniPath = selfDir + L"\\" + INI_NAME;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetPrivateProfileStringW(L"LoadingScreen", L"Language", L"",
        buf, MAX_PATH, iniPath.c_str());
    std::wstring val(buf, n);

    size_t open = val.rfind(L'(');
    size_t close = val.rfind(L')');
    if (open != std::wstring::npos && close != std::wstring::npos && close > open)
        val = val.substr(open + 1, close - open - 1);

    size_t a = val.find_first_not_of(L" \t\r\n");
    size_t b = val.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    return val.substr(a, b - a + 1);
}

// Mods folder detection. Priority, highest first:
//   1. MODS_DIR constant (dev/power-user override)
//   2. RandomizedLoadingScreens.ini next to the .asi (manual folder-name override)
//   3. game's registry locale -> string table
//   4. Windows locale -> string table
//   5. "The Sims 3"
// Returns L"" only if the Documents folder can't be found (essentially never).
static std::wstring ResolveModsDir(const std::wstring& selfDir, std::wstring& outLocaleInfo)
{
    std::wstring override_ = MODS_DIR;
    if (!override_.empty()) {
        outLocaleInfo = L"MODS_DIR override set - detection skipped";
        return override_;
    }

    std::wstring docs = GetDocumentsDir();
    if (docs.empty()) {
        outLocaleInfo = L"Documents folder not found";
        return L"";
    }

    // Manual .ini override beats all detection, when present.
    std::wstring iniFolder = ReadIniFolderName(selfDir);
    if (!iniFolder.empty()) {
        outLocaleInfo = L"ini override -> \"" + iniFolder + L"\"";
        return docs + L"\\Electronic Arts\\" + iniFolder + L"\\Mods";
    }

    std::wstring locale, status;
    std::wstring folder = ResolveLocalizedGameFolder(locale, status);
    outLocaleInfo = (locale.empty() ? L"(unknown)" : locale) + L" -> " + status +
        L" \"" + folder + L"\"";
    return docs + L"\\Electronic Arts\\" + folder + L"\\Mods";
}

static bool ReadWholeFileA(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(h);
    return true;
}

static bool WriteWholeFileA(const std::wstring& path, const std::string& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) != 0)
        && written == data.size();
    CloseHandle(h);
    return ok;
}

// Create a directory and any missing parents (like "mkdir -p"). CreateDirectoryW
// only makes the leaf, so we walk each path separator and create each level in
// turn. Attempts on existing levels (e.g. "C:", "C:\Users") fail harmlessly.
// kernel32-only, so it's safe this early - no SHCreateDirectoryEx (shell32)
static void EnsureDirTree(const std::wstring& path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        if ((path[i] == L'\\' || path[i] == L'/') && i > 0)
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

static bool ContainsNoCase(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    auto low = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        };
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j)
            if (low(hay[i + j]) != low(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

// ASCII case-insensitive equality (package filenames are ASCII)
static bool EqualsNoCaseW(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = wchar_t(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = wchar_t(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return true;
}

// UTF-8 <-> UTF-16 conversion via kernel32 (safe this early - no CRT, shell, or
// loader-lock concerns). Uses CP_UTF8 explicitly so non-Latin usernames and paths
// survive intact rather than being mangled by the locale-dependent default
// codepage. Technique credit: sims3fiend / Sims 3 Settings Setter.
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// True if the host process is the actual game (either install's executable),
// as opposed to a launcher/bootstrapper that also happened to load this .asi
static bool IsGameProcess(const std::wstring& host)
{
    for (const wchar_t* name : GAME_EXES)
        if (EqualsNoCaseW(host, name)) return true;
    return false;
}

// Enumerate *.package in the pool folder and return one at random, avoiding
// avoidName (the previous pick) whenever excluding it still leaves a choice.
// poolCount is set to the total number of packages found (for logging)
static bool PickRandomPackage(const std::wstring& poolDir,
    const std::wstring& avoidName,
    std::wstring& chosen,
    size_t& poolCount)
{
    std::vector<std::wstring> paths;   // full paths
    std::vector<std::wstring> names;   // matching filenames (parallel to paths)
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = poolDir + L"\\*.package";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    poolCount = 0;
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            names.push_back(fd.cFileName);
            paths.push_back(poolDir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    poolCount = paths.size();
    if (paths.empty()) return false;

    // Build the list of allowed indices, dropping the previous pick
    std::vector<size_t> candidates;
    for (size_t i = 0; i < names.size(); ++i)
        if (avoidName.empty() || !EqualsNoCaseW(names[i], avoidName))
            candidates.push_back(i);

    // If dropping the previous pick emptied the list (e.g. a pool of one),
    // fall back to all files so we still pick *something*
    if (candidates.empty())
        for (size_t i = 0; i < names.size(); ++i)
            candidates.push_back(i);

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    size_t pick = candidates[(size_t)(c.QuadPart % (LONGLONG)candidates.size())];
    chosen = paths[pick];
    return true;
}

static void AppendLog(const std::wstring& logPath, const std::wstring& message)
{
    if (!LOGGING) return;

    // Cap the log: if it has grown past LOG_MAX_BYTES, start it fresh. Recent runs
    // are what matter for troubleshooting, so a hard reset (not tail-keeping) is fine.
    bool trimmed = false;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
        ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size > LOG_MAX_BYTES) trimmed = true;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE h = CreateFileW(logPath.c_str(),
        trimmed ? GENERIC_WRITE : FILE_APPEND_DATA,   // reset truncates; otherwise append
        FILE_SHARE_READ, nullptr,
        trimmed ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // A fresh file (brand new, or just reset) leads with a UTF-8 BOM so editors
    // render non-Latin paths/usernames correctly instead of as mojibake, followed
    // by a one-line attribution banner. Both are written ONLY on a fresh file, so
    // they appear once at the very top and never repeat between runs.
    bool freshFile = trimmed || (GetLastError() != ERROR_ALREADY_EXISTS);
    if (freshFile) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD bw = 0;
        WriteFile(h, bom, 3, &bw, nullptr);

        std::string banner = "Randomized Loading Screens by thesammy58\r\n";
        DWORD nb = 0;
        WriteFile(h, banner.data(), (DWORD)banner.size(), &nb, nullptr);
    }
    if (trimmed) {
        std::string note = "(log trimmed - exceeded " +
            std::to_string(LOG_MAX_BYTES / 1024) + " KB; this is expected behavior.)\r\n";
        DWORD nw = 0;
        WriteFile(h, note.data(), (DWORD)note.size(), &nw, nullptr);
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    auto pad2 = [](int v) {
        std::string s = std::to_string(v);
        return s.size() < 2 ? ("0" + s) : s;
        };
    std::string line =
        std::to_string(st.wYear) + "-" + pad2(st.wMonth) + "-" + pad2(st.wDay) + " " +
        pad2(st.wHour) + ":" + pad2(st.wMinute) + ":" + pad2(st.wSecond) + "  ";
    line += WideToUtf8(message);        // real Unicode, not '?' placeholders
    line += "\r\n";

    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

// --- resource.cfg helpers --------------------------------------------------
// True if s (after any leading whitespace) begins with pfx.
static bool LineStartsWith(const std::string& s, const char* pfx)
{
    size_t i = s.find_first_not_of(" \t");
    if (i == std::string::npos) return false;
    for (size_t k = 0; pfx[k]; ++k)
        if (i + k >= s.size() || s[i + k] != pfx[k]) return false;
    return true;
}

// Parse the integer of a "Priority N" line. Returns -1 if it isn't one / malformed.
static long ParsePriorityValue(const std::string& s)
{
    if (!LineStartsWith(s, "Priority")) return -1;
    size_t i = s.find_first_not_of(" \t");
    i += 8;                                        // past "Priority"
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    long v = 0; bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
    if (!any) return -1;
    return neg ? -v : v;
}

// Split into lines, dropping the line endings (we rejoin with \r\n).
static std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) { out.push_back(s.substr(start)); break; }
        std::string ln = s.substr(start, nl - start);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        out.push_back(ln);
        start = nl + 1;
    }
    return out;
}

// Build our two-slot block at a given content priority. The layout slot sits one
// above, and a trailing "Priority 0" resets the ambient priority so our high number
// never leaks onto the user's own entries below us.
static std::string BuildCfgBlock(long contentPri)
{
    std::string b;
    b += "Priority " + std::to_string(contentPri + 1) + "\r\n";
    b += "PackedFile LoadingScreen/active_loadingscreen_layout.package\r\n";
    b += "Priority " + std::to_string(contentPri) + "\r\n";
    b += "PackedFile LoadingScreen/active_loadingscreen.package\r\n";
    b += "Priority 0\r\n";
    b += "\r\n";
    return b;
}

// Ensure resource.cfg carries our block AND keeps it above everything else (e.g.
// if Tiny UI Fix's configurator has been told to outrank us). Behavior:
//   - block absent          -> add it (at BASE_PRIORITY, or above a higher mod)
//   - present and on top     -> no change (idempotent; the usual boot)
//   - present but outranked  -> rewrite our block just above the offender
// Any write backs up first and swaps in atomically, and only happens when a
// change is actually needed, so the normal boot leaves the file untouched.
static void EnsureCfg(const std::wstring& modsDir, const std::wstring& destDir,
    const std::wstring& logPath)
{
    std::wstring cfg = modsDir + L"\\" + CFG_NAME;

    std::string content;
    if (!ReadWholeFileA(cfg, content)) {
        // No resource.cfg found - do NOT fabricate one (not our place to)
        AppendLog(logPath, L"warning: resource.cfg not found (" + cfg + L") - check MODS_DIR");
        return;
    }

    std::vector<std::string> lines = SplitLines(content);

    // Locate our block by its token (the layout PackedFile line) and excise ONLY
    // our own recognizable lines around it - never a neighbor's. Structure:
    //   [layout Priority] [layout PackedFile=token] [content Priority]
    //   [content PackedFile] [optional "Priority 0" reset] [optional blank]
    int lo = -1, endExcl = -1;
    int tokenAt = -1;
    for (int i = 0; i < (int)lines.size(); ++i)
        if (lines[i].find(CFG_TOKEN) != std::string::npos) { tokenAt = i; break; }
    bool present = (tokenAt >= 0);
    if (present) {
        int n = (int)lines.size();
        lo = tokenAt;
        if (lo > 0 && LineStartsWith(lines[lo - 1], "Priority")) --lo;   // layout Priority above token
        int j = tokenAt + 1;
        if (j < n && LineStartsWith(lines[j], "Priority")) ++j;          // content Priority
        if (j < n && lines[j].find("active_loadingscreen.package") != std::string::npos &&
            lines[j].find("_layout") == std::string::npos) ++j;          // content PackedFile
        if (j < n && ParsePriorityValue(lines[j]) == 0) ++j;             // optional "Priority 0" reset
        endExcl = j;
        if (endExcl < n && lines[endExcl].empty()) ++endExcl;            // trailing blank
    }

    // Highest priority currently in use that ISN'T ours.
    long maxOther = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (present && i >= lo && i < endExcl) continue;
        long v = ParsePriorityValue(lines[i]);
        if (v > maxOther) maxOther = v;
    }

    // Our target content priority: our base, or just above a higher-priority mod.
    long target = BASE_PRIORITY;
    if (maxOther + PRIORITY_CUSHION > target) target = maxOther + PRIORITY_CUSHION;

    // If our block is present, correct, and on top, there is nothing to do.
    if (present) {
        long ourPri = -1; bool hasReset = false;
        for (int i = lo; i < endExcl; ++i) {
            long v = ParsePriorityValue(lines[i]);
            if (v == 0) hasReset = true;
            else if (v > 0 && (ourPri < 0 || v < ourPri)) ourPri = v;
        }
        bool onTop = (ourPri > maxOther);
        if (onTop && hasReset && ourPri == target) {
            AppendLog(logPath, L"cfg: resource.cfg block present and on top (priority " +
                std::to_wstring(target) + L") - no change");
            return;
        }
    }

    // A change is needed. Back up the current (clean, user-owned) cfg first.
    std::wstring bak = destDir + L"\\" + BACKUP_NAME;
    if (CopyFileW(cfg.c_str(), bak.c_str(), FALSE))
        AppendLog(logPath, L"cfg: backed up resource.cfg -> LoadingScreen\\" + std::wstring(BACKUP_NAME));
    else
        AppendLog(logPath, L"warning: could not back up resource.cfg (continuing anyway)");

    // Rebuild without our old block (if any), then prepend a fresh one.
    std::string rest;
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (present && i >= lo && i < endExcl) continue;
        rest += lines[i];
        rest += "\r\n";
    }
    while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
    if (!rest.empty()) rest += "\r\n";             // single clean terminator, no blank drift

    std::string updated = BuildCfgBlock(target) + rest;

    std::wstring tmp = cfg + L".tmp";
    if (WriteWholeFileA(tmp, updated) &&
        MoveFileExW(tmp.c_str(), cfg.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (!present)
            AppendLog(logPath, L"cfg: added to resource.cfg at priority " + std::to_wstring(target));
        else if (maxOther >= BASE_PRIORITY)
            AppendLog(logPath, L"cfg: another mod at priority " + std::to_wstring(maxOther) +
                L" is at/above ours - raised resource.cfg block to " + std::to_wstring(target));
        else
            AppendLog(logPath, L"cfg: normalized resource.cfg block (priority " + std::to_wstring(target) + L")");
    }
    else {
        AppendLog(logPath, L"warning: failed to update resource.cfg");
    }
}
// ---------------------------------------------------------------------------
//  Layout harvesting: make the randomized loading screen follow Tiny UI Fix's
//  scaling.
//
//  Each pool package stamps the same EA screen LAYO onto all 21 loading instance
//  IDs, so whichever pack the game thinks is newest, that one screen shows. To
//  make it follow TUIF's scale we must replace that LAYO with TUIF's scaled
//  version of THE SAME screen. TUIF scales every EA screen's LAYO at that
//  screen's own native instance - but it rewrites the contents (repoints images
//  to freshly upscaled bitmaps, adds/renames controls), and the parts it leaves
//  alone (control IDs, layout template) are shared across every screen. So, as
//  mentioned earlier, there is no field that is both stable under TUIF and unique
//  to the screen: content matching cannot work here.
//
//  Instead we identify the screen by an immutable address. Each pool package
//  carries a marker resource (type MARKER_TYPE) whose TGI instance is the EA
//  native instance ID of its screen (the instance we copied the LAYO from at build
//  time). The ASI reads that instance, grabs TUIF's LAYO at exactly that instance,
//  and stamps it verbatim onto all 21 slots.
// ---------------------------------------------------------------------------

// One DBPF v2 index entry (the on-disk format the build scripts read/write).
struct DbpfEntry {
    uint32_t type = 0, group = 0, instHi = 0, instLo = 0;
    uint32_t offset = 0, fileSize = 0, memSize = 0;
    uint16_t compression = 0, flags = 0;
};

// A slot to write in the layout package: one of our picked EA stock loading screen's LAYO TGIs.
struct SlotTarget { uint32_t group = 0, instHi = 0, instLo = 0; };

// Parse a DBPF v2 index out of raw package bytes.
static bool ParseDbpfIndex(const std::string& d, std::vector<DbpfEntry>& out)
{
    out.clear();
    if (d.size() < 0x60 ||
        d[0] != 'D' || d[1] != 'B' || d[2] != 'P' || d[3] != 'F')
        return false;

    auto u32 = [&](size_t p) -> uint32_t {
        if (p + 4 > d.size()) return 0;
        return  (uint8_t)d[p] | ((uint8_t)d[p + 1] << 8) |
            ((uint8_t)d[p + 2] << 16) | ((uint32_t)(uint8_t)d[p + 3] << 24);
        };
    auto u16 = [&](size_t p) -> uint16_t {
        if (p + 2 > d.size()) return 0;
        return (uint16_t)((uint8_t)d[p] | ((uint8_t)d[p + 1] << 8));
        };

    uint32_t idxCount = u32(0x24);
    uint32_t idxOff = u32(0x40);
    if (idxOff == 0 || (size_t)idxOff + 4 > d.size()) return false;

    uint32_t flags = u32(idxOff);
    size_t p = (size_t)idxOff + 4;

    // Fields flagged constant are stored ONCE, right after the flags word,
    // instead of per-entry. (bit0=type, bit1=group, bit2=instance-high.)
    uint32_t cType = 0, cGroup = 0, cInstHi = 0;
    if (flags & 1) { cType = u32(p); p += 4; }
    if (flags & 2) { cGroup = u32(p); p += 4; }
    if (flags & 4) { cInstHi = u32(p); p += 4; }

    for (uint32_t i = 0; i < idxCount; ++i) {
        DbpfEntry e{};
        if (flags & 1) e.type = cType;     else { e.type = u32(p); p += 4; }
        if (flags & 2) e.group = cGroup;   else { e.group = u32(p); p += 4; }
        if (flags & 4) e.instHi = cInstHi; else { e.instHi = u32(p); p += 4; }
        e.instLo = u32(p); p += 4;
        e.offset = u32(p); p += 4;
        e.fileSize = u32(p) & 0x7FFFFFFF; p += 4;   // strip the size high-bit flag
        e.memSize = u32(p); p += 4;
        e.compression = u16(p); p += 2;
        e.flags = u16(p); p += 2;
        if ((size_t)e.offset + e.fileSize > d.size()) return false;   // sanity
        out.push_back(e);
    }
    return true;
}

static std::wstring HexW32(uint32_t v)
{
    wchar_t b[9]; const wchar_t* d = L"0123456789ABCDEF";
    for (int i = 0; i < 8; ++i) b[7 - i] = d[(v >> (i * 4)) & 0xF];
    b[8] = 0; return std::wstring(b);
}

// Build the layout slot: ONE blob (the matched scaled LAYO, verbatim/compressed)
// with one index entry per target slot, all sharing the same data offset (flat
// index, size high-bit set, index version 3).
static bool BuildSharedLayoutSlot(const std::wstring& outPath,
    const std::string& blob, uint32_t memSize, uint16_t compression, uint16_t flags,
    const std::vector<SlotTarget>& targets)
{
    if (blob.empty() || targets.empty()) return false;

    std::string out(96, '\0');
    out[0] = 'D'; out[1] = 'B'; out[2] = 'P'; out[3] = 'F';
    auto putU32 = [&](size_t pos, uint32_t v) {
        out[pos] = (char)(v & 0xFF); out[pos + 1] = (char)((v >> 8) & 0xFF);
        out[pos + 2] = (char)((v >> 16) & 0xFF); out[pos + 3] = (char)((v >> 24) & 0xFF);
        };
    putU32(0x04, 2);   // major version
    putU32(0x08, 0);   // minor version

    uint32_t blobOff = (uint32_t)out.size();
    out.append(blob);                        // the single shared blob

    uint32_t idxOff = (uint32_t)out.size();
    std::string idx;
    auto pU32 = [&](uint32_t v) {
        char b[4] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF),
                      (char)((v >> 16) & 0xFF), (char)((v >> 24) & 0xFF) };
        idx.append(b, 4);
        };
    auto pU16 = [&](uint16_t v) {
        char b[2] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF) };
        idx.append(b, 2);
        };

    pU32(0);   // index flags = 0 -> every field stored per-entry
    for (const auto& t : targets) {
        pU32(LAYO_TYPE);
        pU32(t.group);
        pU32(t.instHi);
        pU32(t.instLo);
        pU32(blobOff);                       // SHARED offset - all entries point here
        pU32((uint32_t)blob.size() | 0x80000000);
        pU32(memSize);
        pU16(compression);
        pU16(flags);
    }
    out.append(idx);

    putU32(0x24, (uint32_t)targets.size());  // index entry count
    putU32(0x2C, (uint32_t)idx.size());      // index size (flags word + entries)
    putU32(0x3C, 3);                          // index version
    putU32(0x40, idxOff);                     // index offset

    std::wstring tmp = outPath + L".tmp";
    if (!WriteWholeFileA(tmp, out)) return false;
    return MoveFileExW(tmp.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Harvest the scaled twin of the PICKED screen and stamp it onto every slot the
// picked package overrides. Called right after the active-slot swap succeeds.
static void HarvestScaledLayouts(const std::wstring& modsDir,
    const std::wstring& destDir,
    const std::wstring& pickedPackagePath,
    const std::wstring& logPath)
{
    std::wstring layoutSlot = destDir + L"\\" + LAYOUT_SLOT_NAME;
    DeleteFileW(layoutSlot.c_str());   // clear any previous harvest first

    // 1. TUIF's generated package.
    std::wstring tuifPath = modsDir + TUIF_REL_PATH;
    std::string tuifRaw;
    std::vector<DbpfEntry> tuifIndex;
    if (!ReadWholeFileA(tuifPath, tuifRaw) || !ParseDbpfIndex(tuifRaw, tuifIndex)) {
        AppendLog(logPath, L"layout: Tiny UI Fix package not present/readable - keeping native LAYO");
        return;
    }

    // 2. The picked content package: collect its LAYO slots, and read the native
    //    instance from our marker resource (its TGI instance IS the native inst).
    std::string pickedRaw;
    std::vector<DbpfEntry> pickedIndex;
    if (!ReadWholeFileA(pickedPackagePath, pickedRaw) ||
        !ParseDbpfIndex(pickedRaw, pickedIndex)) {
        AppendLog(logPath, L"layout: could not read picked package index - keeping native LAYO");
        return;
    }
    std::vector<SlotTarget> targets;
    bool haveNative = false;
    uint32_t natHi = 0, natLo = 0;
    for (const auto& pe : pickedIndex) {
        if (pe.type == LAYO_TYPE)
            targets.push_back({ pe.group, pe.instHi, pe.instLo });
        else if (pe.type == MARKER_TYPE) {
            natHi = pe.instHi; natLo = pe.instLo; haveNative = true;
        }
    }
    if (targets.empty()) {
        AppendLog(logPath, L"layout: picked package carries no LAYO - keeping native LAYO");
        return;
    }
    if (!haveNative) {
        AppendLog(logPath, L"layout: no native-instance marker in package - keeping native "
            L"LAYO (may be due to a custom or 3rd party loading screen)");
        return;
    }

    // 3. TUIF's scaled LAYO for that screen lives at the native instance. Match by
    //    instance only - group may differ, and there is one LAYO per instance.
    const DbpfEntry* match = nullptr;
    for (const auto& te : tuifIndex) {
        if (te.type == LAYO_TYPE && te.instHi == natHi && te.instLo == natLo) {
            match = &te; break;
        }
    }
    if (!match) {
        AppendLog(logPath, L"layout: Tiny UI Fix has no LAYO at native inst 0x" +
            HexW32(natHi) + HexW32(natLo) + L" - keeping native LAYO");
        return;
    }

    // 3b. Self-poisoning guard. If TUIF's configurator was run while a loading screen
    //     override was installed in Mods, it scaled that ONE screen across ALL 21 loading
    //     instances, so every TUIF loading LAYO is identical - and harvesting any
    //     of them yields the wrong screen. Detect that by sampling a few sibling
    //     loading LAYOs (the other target instances): if our match is byte-for-
    //     byte identical to all sampled siblings, TUIF's loading LAYOs are uniform
    //     -> skip the harvest and keep the native LAYO (correct screen, unscaled).
    auto sameBlob = [&](const DbpfEntry* a, const DbpfEntry* b) -> bool {
        if (a->fileSize != b->fileSize) return false;
        if ((size_t)a->offset + a->fileSize > tuifRaw.size()) return false;
        if ((size_t)b->offset + b->fileSize > tuifRaw.size()) return false;
        const char* pa = tuifRaw.data() + a->offset;
        const char* pb = tuifRaw.data() + b->offset;
        for (uint32_t i = 0; i < a->fileSize; ++i)
            if (pa[i] != pb[i]) return false;
        return true;
        };
    int checked = 0, identical = 0;
    for (const auto& t : targets) {
        if (t.instHi == natHi && t.instLo == natLo) continue;   // skip the target itself
        const DbpfEntry* sib = nullptr;
        for (const auto& te : tuifIndex)
            if (te.type == LAYO_TYPE && te.instHi == t.instHi && te.instLo == t.instLo) {
                sib = &te; break;
            }
        if (!sib) continue;
        ++checked;
        if (sameBlob(match, sib)) ++identical;
        else break;                              // a difference -> not uniform, stop early
        if (checked >= 4) break;                 // enough samples to be confident
    }
    if (checked >= 2 && identical == checked) {
        AppendLog(logPath, L"layout: TUIF's loading LAYOs are all identical (TUIF was "
            L"configured with a loading screen present in the Mods folder) - keeping native LAYO. Re-run the "
            L"Tiny UI Fix configurator with no loading screen installed to enable scaling.");
        return;
    }

    // 4. Stamp the matched (still-compressed) blob onto all our slots.
    std::string blob = tuifRaw.substr(match->offset, match->fileSize);
    if (BuildSharedLayoutSlot(layoutSlot, blob, match->memSize,
        match->compression, match->flags, targets))
        AppendLog(logPath, L"layout: harvested scaled screen from Tiny UI Fix (native 0x" +
            HexW32(natHi) + HexW32(natLo) + L") onto " +
            std::to_wstring(targets.size()) + L" slot(s)");
    else
        AppendLog(logPath, L"warning: failed to write layout slot - keeping native LAYO");
}

static void Worker(HMODULE self)
{
    try {
        // Gate to the game process FIRST, before resolving any paths. Two reasons:
        // (1) only the game process should swap, or the launcher and game fight
        //     and (with a small pool) lock onto one screen;
        // (2) ResolveLocalizedGameFolder reads THIS process's exe resources, and
        //     only the game exe has the locale table - a launcher would fall back
        //     to the English name and create the wrong folder. So the launcher
        //     must do nothing at all here.
        std::wstring host = GetHostExeName();
        if (!IsGameProcess(host))
            return;

        std::wstring selfDir = GetSelfDir(self);
        if (selfDir.empty()) return;

        std::wstring poolDir = selfDir + L"\\" + POOL_SUBFOLDER;
        std::wstring localeInfo;
        std::wstring modsDir = ResolveModsDir(selfDir, localeInfo);
        if (modsDir.empty())
            return; // Couldn't locate the Mods folder at all - nothing safe to do
        std::wstring destDir = modsDir + L"\\" + DEST_SUBFOLDER;
        EnsureDirTree(destDir); // create Mods\LoadingScreen and any missing parents

        std::wstring logPath = destDir + L"\\" + LOG_NAME;
        std::wstring statePath = destDir + L"\\" + STATE_NAME;

        AppendLog(logPath, L"v" + std::wstring(MOD_VERSION) + L" [" + host +
            L"] run start (MODS_DIR=" + modsDir + L")");
        AppendLog(logPath, L"locale: " + localeInfo);

        // Read the previous pick (if any) so we can avoid repeating it this boot
        std::wstring lastPick;
        {
            std::string raw;
            if (ReadWholeFileA(statePath, raw)) {
                while (!raw.empty() &&
                    (raw.back() == '\r' || raw.back() == '\n' ||
                        raw.back() == ' ' || raw.back() == '\t'))
                    raw.pop_back();
                lastPick = Utf8ToWide(raw);
            }
        }

        std::wstring chosen;
        size_t poolCount = 0;
        if (!PickRandomPackage(poolDir, lastPick, chosen, poolCount)) {
            AppendLog(logPath, L"warning: no packages found in pool (" + poolDir + L")");
            AppendLog(logPath, LOG_SEP);        // session end marker
            return; // Nothing to swap; boot normally
        }

        AppendLog(logPath, L"pool: " + std::to_wstring(poolCount) +
            L" package(s) found in " + poolDir);
        if (poolCount == 1)
            AppendLog(logPath, L"warning: only one package in pool - the screen "
                L"will not change between boots until you add more");

        std::wstring pickedName = chosen.substr(chosen.find_last_of(L"\\/") + 1);
        {
            std::wstring pickLine = L"picked: " + pickedName;
            if (!lastPick.empty())
                pickLine += L" (previous was: " + lastPick + L")";
            AppendLog(logPath, pickLine);
        }

        // Copy the chosen package OVER the single stable active slot, atomically:
        // copy to a temp file, then replace-move it into place so the game
        // never reads a half-written package
        std::wstring slot = destDir + L"\\" + ACTIVE_SLOT_NAME;
        std::wstring tmp = slot + L".tmp";
        bool swapped = CopyFileW(chosen.c_str(), tmp.c_str(), FALSE) &&
            MoveFileExW(tmp.c_str(), slot.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (swapped) {
            AppendLog(logPath, L"swap: installed active slot (LoadingScreen\\" +
                std::wstring(ACTIVE_SLOT_NAME) + L")");
            // Remember this pick so next boot can avoid it (only if it took)
            WriteWholeFileA(statePath, WideToUtf8(pickedName));
            // Follow Tiny UI Fix's scaling: harvest the scaled LAYO(s) for this
            // screen into the layout slot. Reads the pool source (chosen) rather
            // than the just-written slot copy. No-op when TUIF isn't installed.
            HarvestScaledLayouts(modsDir, destDir, chosen, logPath);
        }
        else {
            AppendLog(logPath, L"warning: swap failed - could not install active "
                L"loading screen (is the package readable / destination writable?)");
        }

        // Make sure the high-priority resource.cfg entry exists
        EnsureCfg(modsDir, destDir, logPath);

        AppendLog(logPath, L"run done");
        AppendLog(logPath, LOG_SEP);            // session end marker
    }
    catch (...) {
    }
}
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        __try {
            Worker(hModule);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Absolute last-resort guard. Worst case: last active screen is kept.
        }
    }
    return TRUE;
}
