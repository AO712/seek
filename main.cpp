// Seek — a search-first, anti-File-Explorer for Windows 11.
// Native Win32 + Direct2D/DirectWrite. No frameworks, no C++ runtime, no thumbnails.
//
// Build (MinGW-w64):  see build.bat / README at the bottom of this file.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <wctype.h>
#include <winioctl.h>

#ifdef SEEK_SELFTEST   // test build only: route volume APIs to a fake NTFS volume (see selftest.inc)
static BOOL WINAPI ft_DeviceIoControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static HANDLE WINAPI ft_CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL WINAPI ft_GetVolumeInformationW(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, DWORD);
#define DeviceIoControl ft_DeviceIoControl
#define CreateFileW ft_CreateFileW
#define GetVolumeInformationW ft_GetVolumeInformationW
#endif
#define APPNAME L"Seek"
enum { PM = 1024 };                      // max path buffer (wchar_t)
#define WM_IDX     (WM_APP + 1)          // index progress
#define WM_RECENT  (WM_APP + 2)          // recents loaded (lParam = List*)
#define WM_FSCHG   (WM_APP + 3)          // watched folder changed
#define WM_SEARCHDONE (WM_APP + 4)       // background search finished (lParam = SearchOut*)

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_CAPTION_COLOR_ 35
#define DWMWA_TEXT_COLOR_ 36

// Folders/files the indexer never descends into (edit freely).
static const wchar_t* const SKIP_DIRS[] = {
    L"$Recycle.Bin", L"System Volume Information", L"node_modules", L".git",
    L"WinSxS", L"$WinREAgent", L"Recovery", 0};

// ───────────────────────────── tiny containers ─────────────────────────────
template <class T> struct Vec {
    T* p = 0; size_t n = 0, cap = 0;
    void reserve(size_t c) {
        if (c <= cap) return;
        size_t nc = cap ? cap : 16; while (nc < c) nc *= 2;
        T* q = (T*)realloc(p, nc * sizeof(T)); if (!q) ExitProcess(1);
        p = q; cap = nc;
    }
    T& push(const T& v) { reserve(n + 1); p[n] = v; return p[n++]; }
    void append(const T* a, size_t k) { reserve(n + k); memcpy(p + n, a, k * sizeof(T)); n += k; }
    void free_() { free(p); p = 0; n = cap = 0; }
};

struct Item {
    uint32_t off; uint16_t len, nameStart, hs, hl; uint8_t dir, attr;
    int64_t size; uint64_t mtime;
};
struct List {
    Vec<Item> it; Vec<wchar_t> pool;
    const wchar_t* P(int i) const { return pool.p + it.p[i].off; }
    const wchar_t* N(int i) const { return P(i) + it.p[i].nameStart; }
    int NL(int i) const { return it.p[i].len - it.p[i].nameStart; }
    void clear() { it.n = 0; pool.n = 0; }
    int add(const wchar_t* path, int len, bool dir, int64_t size = -1, uint64_t mt = 0, uint8_t attr = 0) {
        while (len > 3 && path[len - 1] == L'\\') len--;
        Item x; x.off = (uint32_t)pool.n; x.len = (uint16_t)len; x.hs = 0; x.hl = 0;
        x.dir = dir; x.attr = attr; x.size = size; x.mtime = mt;
        int ns = 0;
        if (!(len == 3 && path[1] == L':')) {
            for (int i = len - 1; i >= 0; --i) if (path[i] == L'\\') { ns = i + 1; break; }
        }
        x.nameStart = (uint16_t)ns;
        pool.append(path, len); wchar_t z = 0; pool.push(z);
        it.push(x); return (int)it.n - 1;
    }
};

static inline D2D1_COLOR_F C(uint32_t rgb, float a = 1.f) {
    D2D1_COLOR_F c = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    return c;
}
static inline COLORREF CR(uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

// ───────────────────────────── globals ─────────────────────────────
static HINSTANCE g_inst; static HWND g_hwnd, g_edit, g_ren; static UINT g_dpi = 96;
static ID2D1Factory* g_d2d; static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt; static ID2D1SolidColorBrush* g_br;
static IDWriteTextFormat *fSmallC, *fTitle, *fBody, *fBodyR, *fSmall, *fSmallR, *fLabel, *fGlyph, *fGlyphS, *fCrumb, *fCrumbB;
static IDWriteInlineObject* g_ell;
static HFONT g_fontEdit, g_fontRen; static int g_editH = 24;
static WNDPROC g_oldEdit, g_oldRen;
static HBRUSH g_brSurface;

struct Theme {
    bool dark; uint32_t bg, surface, text, text2, accent, onAccent;
    float hoverA; uint32_t hoverC, border; float borderA;
} T;

enum { HOME = 0, SEARCH = 1, FOLDER = 2 };
struct View { int sel; float scroll; int anchor; };
static View g_v[3] = {{-1, 0, -1}, {-1, 0, -1}, {-1, 0, -1}};

static List g_pins, g_recents, g_results, g_folder;
static wchar_t g_dir[PM];       // current folder ("" = home)
static wchar_t g_query[512];
static wchar_t g_note[100]; static int g_wantAct;
static bool g_pathMode; static wchar_t g_pathDir[PM]; static bool g_pathLeafEmpty;
static int g_hover = -1; static bool g_tracking;
static float g_t = 0, g_tTarget = 0;
static int g_sortKey = 0; static bool g_sortAsc = true; static bool g_showHidden = false;
static Vec<uint8_t> g_marks; static int g_nmarks;
static bool g_renaming; static int g_renIdx = -1;
static wchar_t g_profile[PM]; static int g_profLen;
static Vec<wchar_t*> g_hist;
static wchar_t g_pinFile[PM];

static int mode() { return g_query[0] ? SEARCH : (g_dir[0] ? FOLDER : HOME); }
static View& V() { return g_v[mode()]; }

// ───────────────────────────── helpers ─────────────────────────────
static wchar_t g_fold[65536];
static void initFold() {
    for (int i = 0; i < 65536; i++) g_fold[i] = (wchar_t)i;
    CharLowerBuffW(g_fold, 65536);
}
static int findFold(const wchar_t* s, int n, const wchar_t* t, int m) {
    if (m > n) return -1;
    for (int i = 0; i + m <= n; i++) {
        int j = 0; while (j < m && g_fold[(uint16_t)s[i + j]] == t[j]) j++;
        if (j == m) return i;
    }
    return -1;
}
static bool startsFold(const wchar_t* s, const wchar_t* pre, int m) {
    for (int i = 0; i < m; i++) { if (!s[i] || g_fold[(uint16_t)s[i]] != g_fold[(uint16_t)pre[i]]) return false; }
    return true;
}
static bool isSep(wchar_t c) { return c == L' ' || c == L'.' || c == L'_' || c == L'-' || c == L'(' || c == L'[' || c == L'\\'; }
static bool underOrEq(const wchar_t* path, const wchar_t* pre) {
    size_t pl = wcslen(pre);
    return _wcsnicmp(path, pre, pl) == 0 && (path[pl] == 0 || path[pl] == L'\\' || (pl && pre[pl - 1] == L'\\'));
}
static void normalizePath(const wchar_t* in, wchar_t* out) {
    size_t n = wcslen(in); if (n >= PM - 2) n = PM - 3;
    for (size_t i = 0; i < n; i++) out[i] = in[i] == L'/' ? L'\\' : in[i];
    out[n] = 0;
    if (n == 2 && out[1] == L':') { out[2] = L'\\'; out[3] = 0; n = 3; }
    while (n > 3 && out[n - 1] == L'\\') out[--n] = 0;
}
static bool parentOf(const wchar_t* p, wchar_t* out) {
    size_t len = wcslen(p);
    if (len == 3 && p[1] == L':') return false;
    const wchar_t* ls = wcsrchr(p, L'\\'); if (!ls) return false;
    size_t n = ls - p; if (n == 2 && p[1] == L':') n = 3;
    memcpy(out, p, n * 2); out[n] = 0; return true;
}
static uint64_t ft64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD regDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD def) {
    HKEY k; DWORD v = def, sz = 4;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) == 0) { RegQueryValueExW(k, name, 0, 0, (BYTE*)&v, &sz); RegCloseKey(k); }
    return v;
}
static void invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, 0, FALSE); }

// ───────────────────────────── file index (in memory) ─────────────────────────────
// Names live in one big UTF-16 pool (NUL separated); 12-byte entries point into it in pool order.
//  • NTFS volumes (when elevated): built straight from the MFT, kept live by the USN change journal,
//    cached on disk so the next start is instant and offline changes are replayed from the journal.
//  • Everything else (not admin / FAT / exFAT / ReFS): directory walker + change-watcher overlay.
enum { EF_DIR = 1, EF_DEL = 2 };
struct Entry { uint32_t off; int32_t parent; uint16_t len; uint8_t flags; uint8_t vol; };
static Vec<Entry> g_ent; static Vec<wchar_t> g_pool; static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_scanning = 1, g_count = 0;
static bool g_elevated; static int g_journalVols;
static wchar_t g_dataDir[PM];
#define FRN48(x) ((uint64_t)(x) & 0x0000FFFFFFFFFFFFull)
static volatile LONG g_progress;                       // records read so far from the volume currently being loaded
static wchar_t g_wroots[26][4];                        // roots handed to change-watcher threads (walker volumes / journal fallback)
static HANDLE g_logFile = INVALID_HANDLE_VALUE; static CRITICAL_SECTION g_logCs;
static void ilog(const char* fmt, ...) {               // %LOCALAPPDATA%\Seek\index.log : what the indexer decided and why
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char b[700]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, 600, fmt, ap); va_end(ap); if (n < 0) return; if (n > 599) n = 599;
    SYSTEMTIME st; GetLocalTime(&st); char t[760]; int m = snprintf(t, sizeof t, "%02d:%02d:%02d.%03d  %.*s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, n, b);
    EnterCriticalSection(&g_logCs); DWORD w; WriteFile(g_logFile, t, (DWORD)m, &w, NULL); LeaveCriticalSection(&g_logCs);
}

// ── open-addressing hash: 48-bit file reference → entry index ──
struct Hash { uint64_t* k; uint32_t* v; uint32_t cap, cnt; };
static inline uint32_t hSlot(uint64_t key, uint32_t cap) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1); }
static void hInit(Hash& h, uint32_t expect) {
    uint32_t c = 1024; while (c < expect * 2u) c <<= 1;
    h.k = (uint64_t*)calloc(c, 8); h.v = (uint32_t*)malloc((size_t)c * 4); h.cap = c; h.cnt = 0;
    if (!h.k || !h.v) ExitProcess(1);
}
static void hFree(Hash& h) { free(h.k); free(h.v); h.k = 0; h.v = 0; h.cap = h.cnt = 0; }
static void hRaw(Hash& h, uint64_t key, uint32_t val) {
    uint32_t i = hSlot(key, h.cap);
    while (h.k[i]) { if (h.k[i] == key) { h.v[i] = val; return; } i = (i + 1) & (h.cap - 1); }
    h.k[i] = key; h.v[i] = val; h.cnt++;
}
static bool hGet(const Hash& h, uint64_t key, uint32_t* out) {
    if (!h.cap) return false;
    uint32_t i = hSlot(key, h.cap);
    while (h.k[i]) { if (h.k[i] == key) { if (out) *out = h.v[i]; return true; } i = (i + 1) & (h.cap - 1); }
    return false;
}
static void hPut(Hash& h, uint64_t key, uint32_t val) {
    if (!h.cap) hInit(h, 1024);
    if ((uint64_t)(h.cnt + 1) * 10 > (uint64_t)h.cap * 7) {
        Hash n; n.cap = h.cap * 2; n.cnt = 0; n.k = (uint64_t*)calloc(n.cap, 8); n.v = (uint32_t*)malloc((size_t)n.cap * 4);
        if (!n.k || !n.v) ExitProcess(1);
        for (uint32_t i = 0; i < h.cap; i++) if (h.k[i]) hRaw(n, h.k[i], h.v[i]);
        hFree(h); h = n;
    }
    hRaw(h, key, val);
}
static void hDel(Hash& h, uint64_t key) {
    if (!h.cap) return;
    uint32_t m = h.cap - 1, i = hSlot(key, h.cap);
    while (h.k[i] && h.k[i] != key) i = (i + 1) & m;
    if (!h.k[i]) return;
    h.k[i] = 0; h.cnt--; uint32_t j = i;
    for (;;) {                                   // backward-shift deletion (no tombstones)
        j = (j + 1) & m; if (!h.k[j]) break;
        uint32_t k = hSlot(h.k[j], h.cap);
        bool inRange = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
        if (inRange) continue;
        h.k[i] = h.k[j]; h.v[i] = h.v[j]; h.k[j] = 0; i = j;
    }
}

static bool isSkipName(const wchar_t* nm) { for (int i = 0; SKIP_DIRS[i]; i++) if (lstrcmpiW(nm, SKIP_DIRS[i]) == 0) return true; return false; }

// ── directory walker (fallback path) ──
static int32_t addRoot(const wchar_t* path) {
    int n = (int)wcslen(path);
    AcquireSRWLockExclusive(&g_lock);
    Entry e; e.off = (uint32_t)g_pool.n; e.parent = -1; e.len = (uint16_t)n; e.flags = EF_DIR; e.vol = 0xFF;
    g_pool.append(path, n + 1); g_ent.push(e); int32_t idx = (int32_t)g_ent.n - 1;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_count);
    return idx;
}

static void scanDir(wchar_t* path, int plen, int32_t dirIdx, int depth) {
    if (plen + 3 >= PM - 2) return;
    path[plen] = L'*'; path[plen + 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    path[plen] = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    Vec<Entry> te; Vec<wchar_t> tp; Vec<uint8_t> rec;
    do {
        const wchar_t* nm = fd.cFileName;
        if (nm[0] == L'.' && (!nm[1] || (nm[1] == L'.' && !nm[2]))) continue;
        int nl = (int)wcslen(nm);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool recurse = isDir;
        if (isDir) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) recurse = false;
            if (isSkipName(nm)) recurse = false;
            if (depth == 0 && plen == 3 && lstrcmpiW(nm, L"Windows") == 0) recurse = false;
            if (plen + nl == g_profLen && _wcsnicmp(path, g_profile, plen) == 0 && _wcsnicmp(nm, g_profile + plen, nl) == 0)
                continue; // user profile is indexed first as its own root
        }
        Entry e; e.off = (uint32_t)tp.n; e.parent = dirIdx; e.len = (uint16_t)nl; e.flags = isDir ? EF_DIR : 0; e.vol = 0xFF;
        te.push(e); tp.append(nm, nl + 1); rec.push(recurse);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!te.n) { te.free_(); tp.free_(); rec.free_(); return; }

    AcquireSRWLockExclusive(&g_lock);
    uint32_t base = (uint32_t)g_ent.n, pb = (uint32_t)g_pool.n;
    g_pool.append(tp.p, tp.n); g_ent.reserve(g_ent.n + te.n);
    for (size_t k = 0; k < te.n; k++) { Entry e = te.p[k]; e.off += pb; g_ent.push(e); }
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchangeAdd(&g_count, (LONG)te.n);

    static DWORD lastPost; DWORD now = GetTickCount();
    if (now - lastPost > 120) { lastPost = now; PostMessageW(g_hwnd, WM_IDX, 0, 0); }

    for (size_t k = 0; k < te.n; k++) {
        if (!rec.p[k]) continue;
        int nl = te.p[k].len; const wchar_t* nm = tp.p + te.p[k].off;
        if (plen + nl + 2 >= PM) continue;
        memcpy(path + plen, nm, nl * 2); path[plen + nl] = L'\\'; path[plen + nl + 1] = 0;
        scanDir(path, plen + nl + 1, (int32_t)(base + k), depth + 1);
        path[plen] = 0;
    }
    te.free_(); tp.free_(); rec.free_();
}

// ── live changes for walker volumes: overlay of adds/removes on top of the scanned index ──
static SRWLOCK g_ovLock = SRWLOCK_INIT; static Vec<wchar_t*> g_ovAdd, g_ovDel;
static wchar_t g_watchDir[PM]; static SRWLOCK g_wdLock = SRWLOCK_INIT; static volatile LONG g_fsDirty;

static void ovAdd(const wchar_t* p) {
    AcquireSRWLockExclusive(&g_ovLock);
    for (size_t i = 0; i < g_ovDel.n; i++) if (_wcsicmp(g_ovDel.p[i], p) == 0) { free(g_ovDel.p[i]); g_ovDel.p[i] = g_ovDel.p[--g_ovDel.n]; break; }
    bool have = false;
    for (size_t i = 0; i < g_ovAdd.n; i++) if (_wcsicmp(g_ovAdd.p[i], p) == 0) { have = true; break; }
    if (!have) {
        if (g_ovAdd.n >= 4000) { free(g_ovAdd.p[0]); memmove(g_ovAdd.p, g_ovAdd.p + 1, (g_ovAdd.n - 1) * sizeof(wchar_t*)); g_ovAdd.n--; }
        g_ovAdd.push(_wcsdup(p));
    }
    ReleaseSRWLockExclusive(&g_ovLock);
}
static void ovDel(const wchar_t* p) {
    AcquireSRWLockExclusive(&g_ovLock);
    for (size_t i = 0; i < g_ovAdd.n;) {
        if (underOrEq(g_ovAdd.p[i], p)) { free(g_ovAdd.p[i]); g_ovAdd.p[i] = g_ovAdd.p[--g_ovAdd.n]; } else i++;
    }
    if (g_ovDel.n < 5000) g_ovDel.push(_wcsdup(p));
    ReleaseSRWLockExclusive(&g_ovLock);
}
static bool noisy(const wchar_t* path) {
    wchar_t lc[PM]; size_t n = wcslen(path); if (n >= PM) n = PM - 1;
    memcpy(lc, path, n * 2); lc[n] = 0; CharLowerBuffW(lc, (DWORD)n);
    static const wchar_t* const pats[] = {L"\\appdata\\", L"\\windows\\", L"\\$recycle.bin\\", L"\\node_modules\\",
                                          L"\\.git\\", L"\\programdata\\", L"\\system volume information\\", 0};
    for (int i = 0; pats[i]; i++) if (wcsstr(lc, pats[i])) return true;
    return false;
}
static DWORD WINAPI watchThread(void* arg) {
    wchar_t root[4]; memcpy(root, arg, sizeof root);
    HANDLE h = CreateFileW(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BYTE* buf = (BYTE*)_aligned_malloc(64 * 1024, 8); DWORD ret;
    for (;;) {
        if (!ReadDirectoryChangesW(h, buf, 64 * 1024, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, &ret, NULL, NULL)) break;
        if (!ret) { InterlockedExchange(&g_fsDirty, 1); PostMessageW(g_hwnd, WM_FSCHG, 0, 0); continue; }
        FILE_NOTIFY_INFORMATION* fi = (FILE_NOTIFY_INFORMATION*)buf;
        for (;;) {
            wchar_t path[PM]; int nl = (int)(fi->FileNameLength / 2);
            if (nl > 0 && nl < PM - 4) {
                wcscpy(path, root); memcpy(path + 3, fi->FileName, nl * 2); path[3 + nl] = 0;
                bool isAdd = fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME;
                bool isDel = fi->Action == FILE_ACTION_REMOVED || fi->Action == FILE_ACTION_RENAMED_OLD_NAME;
                if (!noisy(path)) { if (isAdd) ovAdd(path); else if (isDel) ovDel(path); }
                wchar_t par[PM];
                if (parentOf(path, par)) {
                    AcquireSRWLockShared(&g_wdLock);
                    bool same = g_watchDir[0] && _wcsicmp(par, g_watchDir) == 0;
                    ReleaseSRWLockShared(&g_wdLock);
                    if (same && InterlockedExchange(&g_fsDirty, 1) == 0) PostMessageW(g_hwnd, WM_FSCHG, 0, 0);
                }
            }
            if (!fi->NextEntryOffset) break;
            fi = (FILE_NOTIFY_INFORMATION*)((BYTE*)fi + fi->NextEntryOffset);
        }
    }
    return 0;
}

static int buildPath(uint32_t idx, wchar_t* out) {  // caller holds g_lock (shared)
    uint32_t chain[512]; int n = 0; int32_t i = (int32_t)idx;
    while (i >= 0 && n < 512) { chain[n++] = i; i = g_ent.p[i].parent; }
    int pos = 0;
    for (int k = n - 1; k >= 0; --k) {
        const Entry& e = g_ent.p[chain[k]];
        if (pos > 0 && out[pos - 1] != L'\\') out[pos++] = L'\\';
        if (pos + e.len >= PM - 1) break;
        memcpy(out + pos, g_pool.p + e.off, e.len * 2); pos += e.len;
    }
    out[pos] = 0; return pos;
}

// ───────────────────────────── NTFS: MFT enumeration + USN journal ─────────────────────────────
struct USNJD    { uint64_t id; int64_t first, next, lowest, max; uint64_t maxSize, delta; };
struct MFTENUM  { uint64_t start; int64_t low, high; };
struct READUSN  { int64_t start; uint32_t mask, onlyClose; uint64_t timeout, wait, jid; };
struct USNREC   { uint32_t recLen; uint16_t major, minor; uint64_t frn, pfrn; int64_t usn, ts;
                  uint32_t reason, src, sec, attr; uint16_t nameLen, nameOff; };
struct CRTJ     { uint64_t maxSize, delta; };
enum { R_CREATE = 0x100, R_DELETE = 0x200, R_REN_OLD = 0x1000, R_REN_NEW = 0x2000 };

struct Vol {
    wchar_t letter; uint8_t id; bool journal; HANDLE h; uint64_t jid; int64_t next; uint32_t serial; int32_t rootIdx; Hash map;
};
static Vol g_vol[26];
struct Block { Vec<Entry> ent; Vec<wchar_t> pool; Vec<uint64_t> frn; };   // block-local indices
static void blockFree(Block& b) { b.ent.free_(); b.pool.free_(); b.frn.free_(); }

static volatile int g_watchVol = -1; static volatile uint64_t g_watchFrn; static volatile int32_t g_watchIdx = -1;

static bool volQuery(Vol* v, USNJD* jd) {
    DWORD ret; BYTE big[256]; memset(big, 0, sizeof big);            // newer builds may return a longer (V1/V2) struct; the first 56 bytes are the same
    if (DeviceIoControl(v->h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, big, sizeof big, &ret, NULL) && ret >= sizeof(USNJD)) { memcpy(jd, big, sizeof *jd); return true; }
    DWORD e = GetLastError(); ilog("%lc: query journal failed, error %lu", (wint_t)v->letter, e);
    if (e == ERROR_JOURNAL_NOT_ACTIVE) {
        CRTJ c = {0x2000000, 0x400000};
        if (DeviceIoControl(v->h, FSCTL_CREATE_USN_JOURNAL, &c, sizeof c, NULL, 0, &ret, NULL)) {
            if (DeviceIoControl(v->h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, big, sizeof big, &ret, NULL) && ret >= sizeof(USNJD)) { memcpy(jd, big, sizeof *jd); return true; }
        } else ilog("%lc: create journal failed, error %lu", (wint_t)v->letter, GetLastError());
    }
    return false;
}

// Enumerate every file record on the volume (fast: one sequential pass over the MFT), drop excluded trees,
// and return a self-contained block with block-local parent indices (index 0 = volume root).
static bool mftBuild(Vol* v, int64_t highUsn, Block& out) {
    struct TRec { uint64_t frn, pfrn; uint32_t off; uint16_t len; uint8_t dir; };
    Vec<TRec> tr; Vec<wchar_t> tp;
    const DWORD BUFSZ = 1 << 18; DWORD t0 = GetTickCount(), lastPost = t0; DWORD firstErr = 0;
    BYTE* buf = (BYTE*)VirtualAlloc(0, BUFSZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return false;
    MFTENUM med = {0, 0, highUsn}; DWORD err = 0;
    for (;;) {
        DWORD ret = 0;
        if (!DeviceIoControl(v->h, FSCTL_ENUM_USN_DATA, &med, sizeof med, buf, BUFSZ, &ret, NULL)) { err = GetLastError(); break; }
        if (ret < 8) break;
        med.start = *(uint64_t*)buf; InterlockedExchange(&g_progress, (LONG)tr.n);
        { DWORD nw = GetTickCount(); if (nw - lastPost > 150) { lastPost = nw; PostMessageW(g_hwnd, WM_IDX, 0, 0); } }
        BYTE* p = buf + 8; BYTE* end = buf + ret;
        while (p + 60 <= end) {
            USNREC* r = (USNREC*)p; if (!r->recLen) break;
            uint64_t f = FRN48(r->frn);
            if (r->major == 2 && f >= 24 && r->nameLen >= 2) {          // skip NTFS metafiles ($MFT, $Extend …)
                TRec t; t.frn = f; t.pfrn = FRN48(r->pfrn); t.off = (uint32_t)tp.n; t.len = (uint16_t)(r->nameLen / 2);
                t.dir = (r->attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                tp.append((wchar_t*)(p + r->nameOff), t.len); wchar_t z = 0; tp.push(z); tr.push(t);
            }
            p += r->recLen;
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    ilog("%lc: file table read: %zu records in %lu ms (ended with error %lu)", (wint_t)v->letter, tr.n, GetTickCount() - t0, err);
    if (err != ERROR_HANDLE_EOF && err != 0 && tr.n == 0) { tr.free_(); tp.free_(); return false; }

    size_t N = tr.n;
    Hash th; hInit(th, (uint32_t)N + 16);
    for (size_t t = 0; t < N; t++) hRaw(th, tr.p[t].frn, (uint32_t)t);

    // exclusion: a record is dropped if it, or any ancestor, is an excluded folder (or the parent chain is broken)
    Vec<uint8_t> st; st.reserve(N + 1); st.n = N; if (N) memset(st.p, 0, N);
    Vec<uint32_t> chain;
    for (size_t t = 0; t < N; t++) {
        if (st.p[t]) continue;
        chain.n = 0; uint32_t cur = (uint32_t)t; uint8_t res = 0; int guard = 0;
        for (;;) {
            if (st.p[cur]) { res = st.p[cur]; break; }
            const TRec& r = tr.p[cur]; const wchar_t* nm = tp.p + r.off;
            if (r.dir && (isSkipName(nm) || (r.pfrn == 5 && lstrcmpiW(nm, L"Windows") == 0))) { st.p[cur] = 1; res = 1; break; }
            chain.push(cur);
            if (r.pfrn == 5) { res = 2; break; }
            uint32_t pt; if (!hGet(th, r.pfrn, &pt)) { res = 1; break; }
            if (++guard > 2000) { res = 1; break; }
            cur = pt;
        }
        for (size_t c = 0; c < chain.n; c++) st.p[chain.p[c]] = res;
    }
    Vec<uint32_t> loc; loc.reserve(N + 1); loc.n = N;
    uint32_t k = 1; for (size_t t = 0; t < N; t++) loc.p[t] = st.p[t] == 2 ? k++ : 0xFFFFFFFFu;

    out.ent.reserve(k); out.frn.reserve(k); out.pool.reserve((size_t)k * 20);
    wchar_t rootName[4] = {v->letter, L':', L'\\', 0};
    Entry re; re.off = 0; re.parent = -1; re.len = 3; re.flags = EF_DIR; re.vol = v->id;
    out.pool.append(rootName, 4); out.ent.push(re); { uint64_t f5 = 5; out.frn.push(f5); }
    for (size_t t = 0; t < N; t++) {
        if (loc.p[t] == 0xFFFFFFFFu) continue;
        const TRec& r = tr.p[t]; Entry e; e.off = (uint32_t)out.pool.n; e.len = r.len; e.flags = r.dir ? EF_DIR : 0; e.vol = v->id;
        if (r.pfrn == 5) e.parent = 0; else { uint32_t pt = 0; hGet(th, r.pfrn, &pt); e.parent = (int32_t)loc.p[pt]; }
        out.pool.append(tp.p + r.off, r.len + 1); out.ent.push(e); out.frn.push(r.frn);
    }
    ilog("%lc: kept %zu of %zu records (%zu excluded) in %lu ms total", (wint_t)v->letter, (size_t)k, N, N - (k - 1), GetTickCount() - t0);
    hFree(th); tr.free_(); tp.free_(); st.free_(); loc.free_(); chain.free_();
    return true;
}

static bool dirFrnReal(const wchar_t* path, uint64_t* frn) {
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION bi; bool ok = GetFileInformationByHandle(h, &bi) != 0; CloseHandle(h);
    if (ok) *frn = FRN48(((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow);
    return ok;
}
#ifdef SEEK_SELFTEST
static bool ft_dirFrn(const wchar_t* path, uint64_t* frn);
#define dirFrn ft_dirFrn
#else
#define dirFrn dirFrnReal
#endif

// Make a block the live index for its volume (replacing any previous one).
static void publishBlock(Vol* v, Block& b) {
    Hash nh; hInit(nh, (uint32_t)b.ent.n + 1024);
    LONG live = 0;
    for (size_t i = 0; i < b.ent.n; i++) { if (b.frn.p[i] && !(b.ent.p[i].flags & EF_DEL)) hRaw(nh, b.frn.p[i], (uint32_t)i); if (!(b.ent.p[i].flags & EF_DEL)) live++; }
    AcquireSRWLockExclusive(&g_lock);
    if (v->rootIdx >= 0) {
        LONG dead = 0;
        for (size_t i = 0; i < g_ent.n; i++) if (g_ent.p[i].vol == v->id && !(g_ent.p[i].flags & EF_DEL)) { g_ent.p[i].flags |= EF_DEL; dead++; }
        InterlockedExchangeAdd(&g_count, -dead);
    }
    uint32_t bE = (uint32_t)g_ent.n, bP = (uint32_t)g_pool.n;
    g_pool.append(b.pool.p, b.pool.n); g_ent.reserve(g_ent.n + b.ent.n);
    for (size_t i = 0; i < b.ent.n; i++) {
        Entry e = b.ent.p[i]; e.off += bP; if (e.parent >= 0) e.parent += (int32_t)bE; e.vol = v->id; g_ent.push(e);
    }
    for (uint32_t s = 0; s < nh.cap; s++) if (nh.k[s]) nh.v[s] += bE;
    hFree(v->map); v->map = nh; v->rootIdx = (int32_t)bE;
    InterlockedExchangeAdd(&g_count, live); InterlockedExchange(&g_progress, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

// ── journal application (caller holds g_lock exclusively) ──
static void entRemove(Vol* v, uint64_t frn, uint32_t idx) {
    if (!(g_ent.p[idx].flags & EF_DEL)) { g_ent.p[idx].flags |= EF_DEL; InterlockedDecrement(&g_count); }
    hDel(v->map, frn);
}
static void entAdd(Vol* v, uint64_t frn, int32_t parent, const wchar_t* nm, int nl, bool dir) {
    Entry e; e.off = (uint32_t)g_pool.n; e.parent = parent; e.len = (uint16_t)nl; e.flags = dir ? EF_DIR : 0; e.vol = v->id;
    g_pool.append(nm, nl); wchar_t z = 0; g_pool.push(z);
    g_ent.push(e); hPut(v->map, frn, (uint32_t)g_ent.n - 1); InterlockedIncrement(&g_count);
}
static void entRename(Vol* v, uint64_t frn, uint32_t idx, int32_t parent, const wchar_t* nm, int nl) {
    size_t nextOff = (idx + 1 < g_ent.n) ? g_ent.p[idx + 1].off : g_pool.n;
    size_t slot = nextOff - g_ent.p[idx].off;                 // chars available incl. the NUL
    if ((size_t)nl + 1 <= slot) {                             // fits: rewrite in place, zero the slack
        wchar_t* d = g_pool.p + g_ent.p[idx].off; memcpy(d, nm, nl * 2); memset(d + nl, 0, (slot - nl) * 2);
        g_ent.p[idx].len = (uint16_t)nl; g_ent.p[idx].parent = parent;
    } else {                                                  // longer: new entry at the end, old one retired
        Entry ne = g_ent.p[idx]; ne.off = (uint32_t)g_pool.n; ne.len = (uint16_t)nl; ne.parent = parent;
        g_pool.append(nm, nl); wchar_t z = 0; g_pool.push(z);
        uint32_t nidx = (uint32_t)g_ent.n; g_ent.push(ne);
        g_ent.p[idx].flags |= EF_DEL; hPut(v->map, frn, nidx);
        if (ne.flags & EF_DIR) for (uint32_t i = 0; i < nidx; i++) if (g_ent.p[i].parent == (int32_t)idx) g_ent.p[i].parent = (int32_t)nidx;
    }
}
static bool applyOne(Vol* v, const USNREC* r) {   // returns true if the folder being viewed is affected
    uint64_t f = FRN48(r->frn), p = FRN48(r->pfrn); if (f < 24) return false;
    const wchar_t* nm = (const wchar_t*)((const BYTE*)r + r->nameOff); int nl = r->nameLen / 2;
    if (nl <= 0 || nl > 255) return false;
    wchar_t name[256]; memcpy(name, nm, nl * 2); name[nl] = 0;
    uint32_t idx = 0; bool have = hGet(v->map, f, &idx);
    uint32_t pidx = 0; bool pfound = (p == 5) ? (pidx = (uint32_t)v->rootIdx, true) : hGet(v->map, p, &pidx);
    bool dir = (r->attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int32_t oldParent = have ? g_ent.p[idx].parent : -1;
    if (r->reason & R_DELETE) { if (have) entRemove(v, f, idx); }
    else if (have) {
        if (!(r->reason & (R_REN_NEW | R_CREATE))) return false;           // content change only
        if (!pfound || (dir && isSkipName(name))) entRemove(v, f, idx);    // moved into an excluded place (e.g. Recycle Bin)
        else {
            const Entry& e = g_ent.p[idx];
            bool same = e.parent == (int32_t)pidx && e.len == nl && memcmp(g_pool.p + e.off, name, nl * 2) == 0;
            if (same) return false;
            entRename(v, f, idx, (int32_t)pidx, name, nl);
        }
    } else {
        if (!(r->reason & (R_CREATE | R_REN_NEW)) || !pfound || (dir && isSkipName(name))) return false;
        entAdd(v, f, (int32_t)pidx, name, nl, dir);
    }
    return (int)v->id == g_watchVol && (p == g_watchFrn || (oldParent >= 0 && oldParent == g_watchIdx));
}
static void applyBatch(Vol* v, BYTE* buf, DWORD ret) {
    bool dirty = false;
    AcquireSRWLockExclusive(&g_lock);
    BYTE* p = buf + 8; BYTE* end = buf + ret;
    while (p + 60 <= end) {
        USNREC* r = (USNREC*)p; if (!r->recLen) break;
        if (r->major == 2 && applyOne(v, r)) dirty = true;
        p += r->recLen;
    }
    v->next = *(int64_t*)buf;
    ReleaseSRWLockExclusive(&g_lock);
    if (dirty && InterlockedExchange(&g_fsDirty, 1) == 0) PostMessageW(g_hwnd, WM_FSCHG, 0, 0);
}
static bool volRescan(Vol* v) {
    ilog("%lc: rebuilding index from the file table", (wint_t)v->letter);
    USNJD jd; if (!volQuery(v, &jd)) return false;
    Block b; if (!mftBuild(v, jd.next, b)) return false;
    v->next = jd.next; v->jid = jd.id; publishBlock(v, b); blockFree(b); return true;
}
static DWORD WINAPI journalThread(void* arg) {
    Vol* v = (Vol*)arg; const DWORD SZ = 1 << 17; wint_t lt = (wint_t)v->letter;
    BYTE* buf = (BYTE*)VirtualAlloc(0, SZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); int fails = 0, rescans = 0;
    for (;;) {
        READUSN rd = {v->next, R_CREATE | R_DELETE | R_REN_NEW | R_REN_OLD, 1, 0, 1, v->jid};   // block until something closes
        DWORD ret = 0;
        if (!DeviceIoControl(v->h, FSCTL_READ_USN_JOURNAL, &rd, sizeof rd, buf, SZ, &ret, NULL)) {
            DWORD e = GetLastError(); ilog("%lc: journal read failed, error %lu", lt, e);
            if (e == ERROR_JOURNAL_ENTRY_DELETED || e == ERROR_JOURNAL_NOT_ACTIVE || e == ERROR_INVALID_PARAMETER) {
                if (rescans >= 3) { ilog("%lc: journal keeps failing; giving up on live updates", lt); break; }
                bool ok = volRescan(v); rescans++; ilog("%lc: rescan %d %s", lt, rescans, ok ? "done" : "failed");
                if (!ok && ++fails > 3) break; Sleep(500); continue;
            }
            if (++fails > 5) break; Sleep(2000); continue;
        }
        fails = 0;
        if (ret > 8) { applyBatch(v, buf, ret); Sleep(20); }       // coalesce bursts into fewer lock acquisitions
        else { v->next = *(int64_t*)buf; Sleep(50); }              // nothing to apply: just advance, no lock traffic
    }
    wchar_t root[4] = {v->letter, L':', L'\\', 0}; memcpy(g_wroots[v->id], root, sizeof root);   // keep new files visible via a plain change watcher
    HANDLE t = CreateThread(NULL, 0, watchThread, g_wroots[v->id], 0, NULL); if (t) CloseHandle(t);
    ilog("%lc: live journal stopped; change watcher started instead", lt);
    return 0;
}

// ── disk cache ──
struct CHdr { uint32_t letter, serial; uint64_t jid; int64_t next; uint32_t count, poolChars; };
struct CRec { int32_t parent; uint16_t len; uint8_t flags, pad; uint64_t frn; };
static BYTE* g_cache; static size_t g_cacheN;
static void cachePath(wchar_t* out, bool tmp) { swprintf(out, PM, L"%ls\\index.bin%ls", g_dataDir, tmp ? L".tmp" : L""); }
static void cacheRead() {
    wchar_t p[PM]; cachePath(p, false);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 16 || sz.QuadPart > (1ll << 31)) { CloseHandle(f); return; }
    BYTE* b = (BYTE*)malloc((size_t)sz.QuadPart); size_t got = 0;
    while (b && got < (size_t)sz.QuadPart) { DWORD rd = 0; if (!ReadFile(f, b + got, (DWORD)((size_t)sz.QuadPart - got > (1u << 26) ? (1u << 26) : (size_t)sz.QuadPart - got), &rd, 0) || !rd) break; got += rd; }
    CloseHandle(f);
    if (!b || got != (size_t)sz.QuadPart || memcmp(b, "SEEKIDX2", 8) != 0) { free(b); return; }
    g_cache = b; g_cacheN = got;
}
static void cacheFree() { free(g_cache); g_cache = 0; g_cacheN = 0; }
static bool cacheTake(Vol* v, const USNJD& jd, Block& out) {
    if (!g_cache) return false;
    size_t pos = 8; uint32_t nv; if (pos + 4 > g_cacheN) return false; memcpy(&nv, g_cache + pos, 4); pos += 4;
    for (uint32_t i = 0; i < nv; i++) {
        CHdr h; if (pos + sizeof h > g_cacheN) return false; memcpy(&h, g_cache + pos, sizeof h); pos += sizeof h;
        size_t recBytes = (size_t)h.count * sizeof(CRec), poolBytes = (size_t)h.poolChars * 2;
        if (pos + recBytes + poolBytes > g_cacheN) return false;
        if (h.letter == (uint32_t)v->letter && h.serial == v->serial && h.jid == jd.id && h.next >= jd.first && h.next <= jd.next && h.count) {
            out.ent.reserve(h.count); out.frn.reserve(h.count);
            const BYTE* rp = g_cache + pos; uint32_t off = 0;
            for (uint32_t k = 0; k < h.count; k++) {
                CRec r; memcpy(&r, rp + (size_t)k * sizeof(CRec), sizeof r);
                Entry e; e.off = off; e.parent = r.parent; e.len = r.len; e.flags = r.flags; e.vol = v->id; off += r.len + 1;
                out.ent.push(e); out.frn.push(r.frn);
            }
            out.pool.append((const wchar_t*)(g_cache + pos + recBytes), h.poolChars);
            v->next = h.next; return off == h.poolChars;
        }
        pos += recBytes + poolBytes;
    }
    return false;
}
struct BufW { HANDLE f; BYTE* b; size_t n; bool ok; };
static void bwFlush(BufW& w) { if (w.n && w.ok) { DWORD x; if (!WriteFile(w.f, w.b, (DWORD)w.n, &x, 0) || x != w.n) w.ok = false; } w.n = 0; }
static void bwPut(BufW& w, const void* d, size_t n) {
    const BYTE* s = (const BYTE*)d;
    while (n) { size_t c = (1u << 20) - w.n; if (c > n) c = n; memcpy(w.b + w.n, s, c); w.n += c; s += c; n -= c; if (w.n == (1u << 20)) bwFlush(w); }
}
static volatile LONG g_saving;
static void saveCache() {
    if (!g_journalVols || InterlockedExchange(&g_saving, 1)) return;
    wchar_t tmp[PM], fin[PM]; cachePath(tmp, true); cachePath(fin, false);
    BufW w = {CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL), (BYTE*)malloc(1u << 20), 0, true};
    if (w.f == INVALID_HANDLE_VALUE || !w.b) { free(w.b); InterlockedExchange(&g_saving, 0); return; }
    bwPut(w, "SEEKIDX2", 8); uint32_t nv = (uint32_t)g_journalVols; bwPut(w, &nv, 4);
    AcquireSRWLockShared(&g_lock);          // blocks journal appliers (they queue in the OS journal meanwhile)
    size_t ne = g_ent.n; Vec<uint64_t> frnOf; frnOf.reserve(ne + 1); frnOf.n = ne; if (ne) memset(frnOf.p, 0, ne * 8);
    Vec<uint32_t> remap; remap.reserve(ne + 1); remap.n = ne; Vec<uint8_t> keep; keep.reserve(ne + 1); keep.n = ne;
    for (int vi = 0; vi < 26; vi++) {
        Vol* v = &g_vol[vi]; if (!v->journal) continue;
        for (uint32_t s = 0; s < v->map.cap; s++) if (v->map.k[s] && v->map.v[s] < ne) frnOf.p[v->map.v[s]] = v->map.k[s];
    }
    for (int vi = 0; vi < 26; vi++) {
        Vol* v = &g_vol[vi]; if (!v->journal) continue;
        for (size_t i = 0; i < ne; i++) keep.p[i] = (g_ent.p[i].vol == v->id && !(g_ent.p[i].flags & EF_DEL)) ? 1 : 0;
        for (bool ch = true; ch;) {          // keep retired folders that live entries still hang from
            ch = false;
            for (size_t i = 0; i < ne; i++) if (keep.p[i]) { int32_t p = g_ent.p[i].parent; if (p >= 0 && !keep.p[p]) { keep.p[p] = 1; ch = true; } }
        }
        uint32_t cnt = 0, pc = 0;
        for (size_t i = 0; i < ne; i++) if (keep.p[i]) { remap.p[i] = cnt++; pc += g_ent.p[i].len + 1; }
        CHdr h = {(uint32_t)v->letter, v->serial, v->jid, v->next, cnt, pc}; bwPut(w, &h, sizeof h);
        for (size_t i = 0; i < ne; i++) if (keep.p[i]) {
            const Entry& e = g_ent.p[i]; CRec r; r.parent = e.parent >= 0 ? (int32_t)remap.p[e.parent] : -1; r.len = e.len; r.flags = e.flags; r.pad = 0;
            r.frn = (e.flags & EF_DEL) ? 0 : frnOf.p[i]; bwPut(w, &r, sizeof r);
        }
        for (size_t i = 0; i < ne; i++) if (keep.p[i]) bwPut(w, g_pool.p + g_ent.p[i].off, ((size_t)g_ent.p[i].len + 1) * 2);
    }
    ReleaseSRWLockShared(&g_lock);
    frnOf.free_(); remap.free_(); keep.free_();
    bwFlush(w); bool ok = w.ok; CloseHandle(w.f); free(w.b);
    if (ok) MoveFileExW(tmp, fin, MOVEFILE_REPLACE_EXISTING); else DeleteFileW(tmp);
    InterlockedExchange(&g_saving, 0);
}
static DWORD WINAPI saveThread(void*) { saveCache(); return 0; }

static void retireVol(Vol* v) {                     // drop a volume's instant-mode index (it will be walked instead)
    AcquireSRWLockExclusive(&g_lock); LONG dead = 0;
    for (size_t i = 0; i < g_ent.n; i++) if (g_ent.p[i].vol == v->id && !(g_ent.p[i].flags & EF_DEL)) { g_ent.p[i].flags |= EF_DEL; dead++; }
    InterlockedExchangeAdd(&g_count, -dead); hFree(v->map); v->rootIdx = -1; v->journal = false;
    ReleaseSRWLockExclusive(&g_lock);
}
// Trust but verify: check the freshly built index against the real disk before relying on it.
static bool volValidate(Vol* v) {
    wchar_t cand[16][PM]; int nc = 0; wchar_t L0 = v->letter;
    if (g_profLen && (wchar_t)towupper(g_profile[0]) == L0) wcscpy(cand[nc++], g_profile);
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Pictures};
    for (int i = 0; i < 4; i++) { PWSTR p = 0; if (SUCCEEDED(SHGetKnownFolderPath(*ids[i], 0, NULL, &p))) { if ((wchar_t)towupper(p[0]) == L0 && wcslen(p) < PM) wcscpy(cand[nc++], p); CoTaskMemFree(p); } }
    cand[nc][0] = L0; wcscpy(cand[nc] + 1, L":\\Users"); nc++;
    cand[nc][0] = L0; wcscpy(cand[nc] + 1, L":\\Program Files"); nc++;
#ifdef SEEK_SELFTEST
    wcscpy(cand[nc++], L"C:\\Users\\tester\\Documents");
#endif
    int tested = 0, bad = 0;
    for (int i = 0; i < nc; i++) {                 // 1) well-known folders: the file reference must resolve to exactly that path
        uint64_t f; if (!dirFrn(cand[i], &f)) continue; tested++;
        uint32_t ix = 0; wchar_t p[PM]; p[0] = 0;
        AcquireSRWLockShared(&g_lock); bool found = hGet(v->map, f, &ix); if (found) buildPath(ix, p); ReleaseSRWLockShared(&g_lock);
        if (!found || _wcsicmp(p, cand[i]) != 0) { bad++; ilog("%lc: validation: %ls -> %ls", (wint_t)L0, cand[i], found ? p : L"(not in index)"); }
    }
    int probed = 0, missing = 0;
#ifndef SEEK_SELFTEST
    {   // 2) spot-check ~48 entries spread across the volume: the paths we built must exist
        Vec<wchar_t> sp; size_t stride = v->map.cnt / 48; if (stride < 1) stride = 1; size_t seen = 0;
        AcquireSRWLockShared(&g_lock);
        for (size_t i = 0; i < g_ent.n && probed < 48; i++) if (g_ent.p[i].vol == v->id && !(g_ent.p[i].flags & EF_DEL) && g_ent.p[i].parent >= 0 && (seen++ % stride) == 0) {
            wchar_t p[PM]; int pl = buildPath((uint32_t)i, p); sp.append(p, pl + 1); probed++;
        }
        ReleaseSRWLockShared(&g_lock);
        for (const wchar_t* p = sp.p; p < sp.p + sp.n; p += wcslen(p) + 1) {
            DWORD a = GetFileAttributesW(p); DWORD e = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
            if (a == INVALID_FILE_ATTRIBUTES && e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) { missing++; if (missing <= 3) ilog("%lc: validation: built path does not exist: %ls", (wint_t)L0, p); }
        }
        sp.free_();
    }
#endif
    bool ok = bad == 0 && missing * 5 <= probed;
    ilog("%lc: validation %s (%d known folders checked, %d wrong; %d of %d sampled paths missing)", (wint_t)L0, ok ? "passed" : "FAILED", tested, bad, missing, probed);
    return ok;
}

static bool volInit(Vol* v) {
    wchar_t dev[8] = {L'\\', L'\\', L'.', L'\\', v->letter, L':', 0}; wint_t lt = (wint_t)v->letter;
    v->h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (v->h == INVALID_HANDLE_VALUE) { ilog("%lc: cannot open the volume (error %lu) -> directory walker", lt, GetLastError()); return false; }
    USNJD jd; if (!volQuery(v, &jd)) { CloseHandle(v->h); ilog("%lc: no usable change journal -> directory walker", lt); return false; }
    ilog("%lc: journal id %llx, USN %lld..%lld", lt, (unsigned long long)jd.id, (long long)jd.first, (long long)jd.next);
    Block b;
    if (cacheTake(v, jd, b)) {
        v->jid = jd.id; ilog("%lc: loaded %zu entries from cache; will replay the journal from USN %lld", lt, b.ent.n, (long long)v->next);
        publishBlock(v, b); blockFree(b);
        if (volValidate(v)) { v->journal = true; return true; }
        ilog("%lc: cache did not match the disk -> rebuilding from the file table", lt); retireVol(v);
    } else blockFree(b);
    Block nb;
    if (!mftBuild(v, jd.next, nb)) { CloseHandle(v->h); ilog("%lc: reading the file table failed -> directory walker", lt); return false; }
    v->next = jd.next; v->jid = jd.id; publishBlock(v, nb); blockFree(nb);
    if (!volValidate(v)) { retireVol(v); CloseHandle(v->h); ilog("%lc: file-table index failed verification -> directory walker", lt); return false; }
    v->journal = true; ilog("%lc: instant mode active", lt); return true;
}

// ───────────────────────────── elevation: run as admin without a UAC prompt each launch ─────────────────────────────
static bool isElevated() {
    HANDLE t; TOKEN_ELEVATION e; DWORD n; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { if (GetTokenInformation(t, TokenElevation, &e, sizeof e, &n)) r = e.TokenIsElevated != 0; CloseHandle(t); }
    return r;
}
static int runHidden(const wchar_t* cmd, DWORD waitMs) {
    wchar_t c[PM * 2]; wcsncpy(c, cmd, PM * 2 - 1); c[PM * 2 - 1] = 0;
    STARTUPINFOW si = {sizeof si}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, c, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    DWORD code = (DWORD)-1; if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return (int)code;
}
static void markerPath(wchar_t* out) { swprintf(out, PM, L"%ls\\elevated.ok", g_dataDir); }
static void taskInstall() {         // called once, from an elevated instance
    wchar_t exe[PM]; GetModuleFileNameW(NULL, exe, PM);
    wchar_t esc[PM * 2]; int k = 0;
    for (int i = 0; exe[i] && k < PM * 2 - 8; i++) { if (exe[i] == L'&') { memcpy(esc + k, L"&amp;", 10); k += 5; } else if (exe[i] == L'<') { memcpy(esc + k, L"&lt;", 8); k += 4; } else esc[k++] = exe[i]; }
    esc[k] = 0;
    static wchar_t xml[PM * 3 + 1500];
    swprintf(xml, PM * 3 + 1500,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Seek: starts with administrator rights so it can read the NTFS file table, with no UAC prompt.</Description></RegistrationInfo>"
        L"<Triggers/><Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>\"%ls\"</Command><Arguments>--elevated</Arguments></Exec></Actions></Task>", esc);
    wchar_t tmp[PM]; swprintf(tmp, PM, L"%ls\\task.xml", g_dataDir);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WORD bom = 0xFEFF; WriteFile(f, &bom, 2, &w, 0); WriteFile(f, xml, (DWORD)wcslen(xml) * 2, &w, 0); CloseHandle(f);
    wchar_t cmd[PM * 2]; swprintf(cmd, PM * 2, L"schtasks.exe /create /tn Seek /xml \"%ls\" /f", tmp);
    if (runHidden(cmd, 20000) == 0) { wchar_t m[PM]; markerPath(m); HANDLE mf = CreateFileW(m, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (mf != INVALID_HANDLE_VALUE) CloseHandle(mf); }
    DeleteFileW(tmp);
}
static void shellOpen(const wchar_t* path, const wchar_t* dir) {   // elevated → hand the file to the user's own shell so apps don't start as admin
    if (g_elevated) { wchar_t a[PM + 4]; swprintf(a, PM + 4, L"\"%ls\"", path); ShellExecuteW(NULL, L"open", L"explorer.exe", a, NULL, SW_SHOWNORMAL); return; }
    ShellExecuteW(g_hwnd, NULL, path, NULL, dir, SW_SHOWNORMAL);
}

// ───────────────────────────── frecency (how often / how recently you open things) ─────────────────────────────
struct Freq { uint64_t h; double score; uint64_t last; uint8_t dir; wchar_t* path; };
static Vec<Freq> g_freq; static SRWLOCK g_fqLock = SRWLOCK_INIT; static wchar_t g_freqFile[PM];
static uint64_t nowSec() { FILETIME f; GetSystemTimeAsFileTime(&f); return ft64(f) / 10000000ull; }
static uint64_t pathHash(const wchar_t* p) { uint64_t h = 1469598103934665603ull; for (; *p; ++p) { h ^= g_fold[(uint16_t)*p]; h *= 1099511628211ull; } return h; }
static double fqVal(const Freq& f, uint64_t now) { double dt = (double)(now > f.last ? now - f.last : 0); return f.score * pow(0.5, dt / 1209600.0); } // 14-day half-life
static int fqBoostLocked(uint64_t h, uint64_t now) {
    for (size_t i = 0; i < g_freq.n; i++) if (g_freq.p[i].h == h) { int b = (int)(260.0 * log2(1.0 + fqVal(g_freq.p[i], now))); return b > 800 ? 800 : b; }
    return 0;
}
static void freqSave() {
    HANDLE f = CreateFileW(g_freqFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return;
    AcquireSRWLockShared(&g_fqLock);
    for (size_t i = 0; i < g_freq.n; i++) {
        char u[PM * 3 + 64]; char pu[PM * 3]; if (WideCharToMultiByte(CP_UTF8, 0, g_freq.p[i].path, -1, pu, sizeof pu, 0, 0) <= 0) continue;
        int n = snprintf(u, sizeof u, "%.4f\t%llu\t%d\t%s\n", g_freq.p[i].score, (unsigned long long)g_freq.p[i].last, (int)g_freq.p[i].dir, pu);
        DWORD w; WriteFile(f, u, n, &w, 0);
    }
    ReleaseSRWLockShared(&g_fqLock); CloseHandle(f);
}
static void freqLoad() {
    HANDLE f = CreateFileW(g_freqFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(f, 0), rd; char* b = (char*)malloc(sz + 1); ReadFile(f, b, sz, &rd, 0); b[rd] = 0; CloseHandle(f);
    for (char* s = b; *s;) {
        char* e = strchr(s, '\n'); if (e) *e = 0;
        double sc = 0; unsigned long long last = 0; int dir = 0; int off = 0;
        if (sscanf(s, "%lf\t%llu\t%d\t%n", &sc, &last, &dir, &off) >= 3 && off > 0) {
            wchar_t w[PM]; if (MultiByteToWideChar(CP_UTF8, 0, s + off, -1, w, PM) && g_freq.n < 2000) { Freq q = {pathHash(w), sc, last, (uint8_t)dir, _wcsdup(w)}; g_freq.push(q); }
        }
        if (!e) break; s = e + 1;
    }
    free(b);
}
static void freqBump(const wchar_t* path, bool dir) {
    uint64_t h = pathHash(path), now = nowSec();
    AcquireSRWLockExclusive(&g_fqLock);
    size_t i = 0; for (; i < g_freq.n; i++) if (g_freq.p[i].h == h) break;
    if (i < g_freq.n) { g_freq.p[i].score = fqVal(g_freq.p[i], now) + 1.0; g_freq.p[i].last = now; }
    else {
        if (g_freq.n >= 2000) {               // forget the least valuable
            size_t lo = 0; for (size_t k = 1; k < g_freq.n; k++) if (fqVal(g_freq.p[k], now) < fqVal(g_freq.p[lo], now)) lo = k;
            free(g_freq.p[lo].path); g_freq.p[lo] = g_freq.p[--g_freq.n];
        }
        Freq q = {h, 1.0, now, (uint8_t)dir, _wcsdup(path)}; g_freq.push(q);
    }
    ReleaseSRWLockExclusive(&g_fqLock);
    freqSave();
}

// ───────────────────────────── query language ─────────────────────────────
//   words           all must appear in the name         "annual report"   phrase       -draft   exclude
//   ext:pdf,docx    extension                            type:folder|file
//   in:projects     somewhere inside a folder named …    size:>50mb  size:1mb..5mb
//   modified:today|yesterday|week|month|year|3d|2024|2024-05|2024-05-17  (also >date / <date)
struct Query {
    wchar_t inc[8][64]; int incL[8]; int ninc;
    wchar_t neg[4][64]; int negL[4]; int nneg;
    wchar_t ext[8][16]; int extL[8]; int nex;
    wchar_t inn[3][64]; int innL[3]; int nin;
    int type; bool hasSize; int64_t szA, szB; int szOp; bool hasMod; uint64_t modFrom, modTo;
    wchar_t fz[64]; int fzL; int primary;
};
static void lowerInto(wchar_t* d, const wchar_t* s, int n) { for (int i = 0; i < n; i++) d[i] = g_fold[(uint16_t)s[i]]; d[n] = 0; }
static double unitMul(const wchar_t*& e) {
    while (*e == L' ') e++;
    wchar_t u[3] = {0, 0, 0}; int n = 0; while (n < 2 && ((*e | 0x20) >= L'a' && (*e | 0x20) <= L'z')) u[n++] = (wchar_t)(*e++ | 0x20);
    if (!n || (n == 1 && u[0] == L'b')) return 1.0;
    double m = 1.0;
    switch (u[0]) { case L'k': m = 1024.0; break; case L'm': m = 1048576.0; break; case L'g': m = 1073741824.0; break; case L't': m = 1099511627776.0; break; default: return -1.0; }
    if (n == 2 && u[1] != L'b') return -1.0;
    return m;
}
static bool parseSizeTok(const wchar_t* s, Query& q) {
    int op = 3; const wchar_t* p = s;
    if (*p == L'>') { op = 1; p++; if (*p == L'=') p++; } else if (*p == L'<') { op = 2; p++; if (*p == L'=') p++; } else if (*p == L'=') p++;
    wchar_t* e; double a = wcstod(p, &e); if (e == p || a < 0) return false;
    const wchar_t* ce = e; double m = unitMul(ce); if (m < 0) return false;
    int64_t A = (int64_t)(a * m), B = 0;
    if (ce[0] == L'.' && ce[1] == L'.') {
        const wchar_t* p2 = ce + 2; wchar_t* e2; double b = wcstod(p2, &e2); if (e2 == p2) return false;
        const wchar_t* c2 = e2; double m2 = unitMul(c2); if (m2 < 0 || *c2) return false;
        B = (int64_t)(b * m2); op = 4; if (B < A) { int64_t t = A; A = B; B = t; }
    } else if (*ce) return false;
    q.hasSize = true; q.szOp = op; q.szA = A; q.szB = B; return true;
}
static uint64_t localMidnightUtc(int addDays) {
    SYSTEMTIME st; GetLocalTime(&st); st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
    FILETIME a, b; SystemTimeToFileTime(&st, &a); LocalFileTimeToFileTime(&a, &b); return ft64(b) + (int64_t)addDays * 864000000000ll;
}
static bool dateRange(const wchar_t* s, uint64_t& from, uint64_t& to) {   // Y, Y-M or Y-M-D → [from,to) in UTC
    int y = 0, mo = 0, d = 0, n = swscanf(s, L"%d-%d-%d", &y, &mo, &d);
    if (n < 1 || y < 1601 || y > 9999) return false;
    SYSTEMTIME a = {}; a.wYear = (WORD)y; a.wMonth = (WORD)(n >= 2 ? mo : 1); a.wDay = (WORD)(n >= 3 ? d : 1);
    SYSTEMTIME b = a; if (n == 1) b.wYear++; else if (n == 2) { if (b.wMonth == 12) { b.wMonth = 1; b.wYear++; } else b.wMonth++; }
    FILETIME fa, fb, la, lb; if (!SystemTimeToFileTime(&a, &fa)) return false;
    if (n == 3) { fb.dwLowDateTime = fa.dwLowDateTime; fb.dwHighDateTime = fa.dwHighDateTime; uint64_t t = ft64(fb) + 864000000000ull; fb.dwLowDateTime = (DWORD)t; fb.dwHighDateTime = (DWORD)(t >> 32); }
    else if (!SystemTimeToFileTime(&b, &fb)) return false;
    LocalFileTimeToFileTime(&fa, &la); LocalFileTimeToFileTime(&fb, &lb); from = ft64(la); to = ft64(lb); return true;
}
static bool parseModTok(const wchar_t* s, Query& q) {
    FILETIME nf; GetSystemTimeAsFileTime(&nf); uint64_t now = ft64(nf), day = 864000000000ull, inf = ~0ull;
    uint64_t from = 0, to = inf;
    if (!_wcsicmp(s, L"today")) from = localMidnightUtc(0);
    else if (!_wcsicmp(s, L"yesterday")) { from = localMidnightUtc(-1); to = localMidnightUtc(0); }
    else if (!_wcsicmp(s, L"week")) from = now - 7 * day;
    else if (!_wcsicmp(s, L"month")) from = now - 30 * day;
    else if (!_wcsicmp(s, L"year")) from = now - 365 * day;
    else {
        wchar_t* e; long n = wcstol(s, &e, 10); size_t sl = wcslen(s);
        if (e != s && e[0] && !e[1] && (e[0] | 0x20) == L'd' && n > 0 && sl <= 6) from = now - (uint64_t)n * day;
        else if (e != s && e[0] && !e[1] && (e[0] | 0x20) == L'h' && n > 0) from = now - (uint64_t)n * 36000000000ull;
        else if (e != s && e[0] && !e[1] && (e[0] | 0x20) == L'w' && n > 0) from = now - (uint64_t)n * 7 * day;
        else {
            int op = 0; const wchar_t* p = s; if (*p == L'>') { op = 1; p++; } else if (*p == L'<') { op = 2; p++; }
            uint64_t a, b; if (!dateRange(p, a, b)) return false;
            if (op == 1) from = b; else if (op == 2) to = a; else { from = a; to = b; }
        }
    }
    q.hasMod = true; q.modFrom = from; q.modTo = to; return true;
}
static void parseQuery(const wchar_t* s, Query& q) {
    memset(&q, 0, sizeof q); const wchar_t* p = s;
    while (*p) {
        while (*p == L' ') p++; if (!*p) break;
        wchar_t tok[128]; int tl = 0; bool quoted = false, neg = false;
        if (*p == L'-' && p[1] && p[1] != L' ') { neg = true; p++; }
        while (*p && (quoted || *p != L' ')) { if (*p == L'"') { quoted = !quoted; p++; continue; } if (tl < 127) tok[tl++] = *p; p++; }
        tok[tl] = 0; if (!tl) continue;
        wchar_t* colon = wcschr(tok, L':');
        if (!neg && colon && colon > tok) {
            *colon = 0; const wchar_t* key = tok; wchar_t* val = colon + 1; bool known = true;
            if (!_wcsicmp(key, L"ext")) {
                for (wchar_t* c = val; *c;) {
                    wchar_t* e = c; while (*e && *e != L',' && *e != L';' && *e != L'|') e++;
                    wchar_t sv = *e; *e = 0; wchar_t* x = c; if (*x == L'.') x++;
                    int xl = (int)wcslen(x); if (xl > 0 && xl < 16 && q.nex < 8) { lowerInto(q.ext[q.nex], x, xl); q.extL[q.nex++] = xl; }
                    if (!sv) break; c = e + 1;
                }
            } else if (!_wcsicmp(key, L"size")) { if (*val) parseSizeTok(val, q); }
            else if (!_wcsicmp(key, L"modified") || !_wcsicmp(key, L"mod") || !_wcsicmp(key, L"date")) { if (*val) parseModTok(val, q); }
            else if (!_wcsicmp(key, L"in")) { int vl = (int)wcslen(val); while (vl && val[vl - 1] == L'\\') val[--vl] = 0; if (vl && vl < 64 && q.nin < 3) { lowerInto(q.inn[q.nin], val, vl); q.innL[q.nin++] = vl; } }
            else if (!_wcsicmp(key, L"type") || !_wcsicmp(key, L"is")) {
                if (!_wcsnicmp(val, L"folder", 6) || !_wcsnicmp(val, L"dir", 3)) q.type = 2; else if (!_wcsnicmp(val, L"file", 4)) q.type = 1;
            } else known = false;
            if (known) continue;
            *colon = L':';
        }
        int l = tl > 63 ? 63 : tl;
        if (neg) { if (q.nneg < 4) { lowerInto(q.neg[q.nneg], tok, l); q.negL[q.nneg++] = l; } }
        else if (q.ninc < 8) { lowerInto(q.inc[q.ninc], tok, l); q.incL[q.ninc++] = l; }
    }
    for (int i = 1; i < q.ninc; i++) if (q.incL[i] > q.incL[q.primary]) q.primary = i;
    for (int i = 0; i < q.ninc; i++) for (int j = 0; j < q.incL[i] && q.fzL < 63; j++) if (q.inc[i][j] != L' ') q.fz[q.fzL++] = q.inc[i][j];
    q.fz[q.fzL] = 0;
}

// ───────────────────────────── search worker ─────────────────────────────
enum { MAXRES = 50, KTOP = 300, STAT_CAP = 20000, ALL_CAP = 200000 };
#define ID_OVL 0x80000000u
#define ID_FRQ 0x40000000u
struct Cand { int base; uint32_t id; uint16_t hs, hl; };
static inline bool candBetter(const Cand& a, const Cand& b) { return a.base > b.base || (a.base == b.base && a.id < b.id); }
static int cmpCand(const void* a, const void* b) { const Cand* x = (const Cand*)a; const Cand* y = (const Cand*)b; return candBetter(*x, *y) ? -1 : (candBetter(*y, *x) ? 1 : 0); }
struct Coll {
    int mode; Cand top[KTOP]; int n; Vec<Cand> vec; size_t cap; bool overflow;
    void add(const Cand& c) {
        if (mode == 0) {
            if (n == KTOP && !candBetter(c, top[KTOP - 1])) return;
            int i = n < KTOP ? n++ : KTOP - 1;
            while (i > 0 && candBetter(c, top[i - 1])) { top[i] = top[i - 1]; --i; }
            top[i] = c;
        } else { if (vec.n >= cap) { overflow = true; return; } vec.push(c); }
    }
};
struct SearchOut { List list; wchar_t note[100]; LONG gen; bool keep; };
static void searchOutFree(SearchOut* o) { o->list.it.free_(); o->list.pool.free_(); free(o); }

static CRITICAL_SECTION g_sq; static wchar_t g_sqText[512]; static bool g_sqKeep; static HANDLE g_sqEv;
static volatile LONG g_gen, g_doneGen;
static inline bool cancelled(LONG gen) { return g_gen != gen; }

static bool nameFilters(const Query& q, const wchar_t* nm, int len) {
    for (int k = 0; k < q.nneg; k++) if (findFold(nm, len, q.neg[k], q.negL[k]) >= 0) return false;
    if (q.nex) {
        int dot = -1; for (int i = len - 1; i >= 0; --i) if (nm[i] == L'.') { dot = i; break; }
        if (dot < 0) return false;
        int el = len - dot - 1; bool ok = false;
        for (int k = 0; k < q.nex && !ok; k++) {
            if (el != q.extL[k]) continue; int j = 0; while (j < el && g_fold[(uint16_t)nm[dot + 1 + j]] == q.ext[k][j]) j++; ok = j == el;
        }
        if (!ok) return false;
    }
    return true;
}
static bool entPass(const Query& q, const Entry& e, const wchar_t* nm) {      // g_lock held
    if (e.flags & EF_DEL) return false;
    bool dir = (e.flags & EF_DIR) != 0;
    if ((q.type == 1 && dir) || (q.type == 2 && !dir) || (q.nex && dir)) return false;
    if (!nameFilters(q, nm, e.len)) return false;
    for (int k = 0; k < q.nin; k++) {
        bool found = false; int32_t p = e.parent;
        for (int d = 0; d < 64 && p >= 0; d++) { const Entry& pe = g_ent.p[p]; if (findFold(g_pool.p + pe.off, pe.len, q.inn[k], q.innL[k]) >= 0) { found = true; break; } p = pe.parent; }
        if (!found) return false;
    }
    return true;
}
static int baseScore(int pos, int m, const wchar_t* nm, int len, bool dir) {
    int s = pos == 0 ? 1000 : (isSep(nm[pos - 1]) ? 700 : 400);
    int dot = -1; for (int i = len - 1; i > 0; --i) if (nm[i] == L'.') { dot = i; break; }
    if (pos == 0 && m == len) s += 400; else if (pos == 0 && dot > 0 && m == dot) s += 350;     // exact name / exact name minus extension
    s -= len > 100 ? 100 : len; if (dir) s += 20;
    return s;
}
static int fuzzyScore(const wchar_t* nm, int len, const wchar_t* q, int qn, bool needWordStart) {
    int j = 0, sum = 0, first = -1, last = -1, prev = -2;
    for (int i = 0; i < len && j < qn; i++) {
        if (g_fold[(uint16_t)nm[i]] != q[j]) continue;
        bool ws = i == 0 || isSep(nm[i - 1]);
        if (j == 0) { if (needWordStart && !ws) continue; first = i; }
        sum += ws ? 14 : (prev == i - 1 ? 9 : 2); prev = i; last = i; j++;
    }
    if (j < qn) return -1;
    int s = 60 + sum * 3 - ((last - first + 1) - qn) * 2; return s < 20 ? 20 : (s > 280 ? 280 : s);
}
struct Fin { int score; uint32_t po; uint16_t pl, hs, hl; };
static int cmpFin(const void* a, const void* b) { const Fin* x = (const Fin*)a; const Fin* y = (const Fin*)b; if (x->score != y->score) return y->score - x->score; return (int)x->pl - (int)y->pl; }

static void searchRun(const wchar_t* text, LONG gen, SearchOut* out) {
    Query q; parseQuery(text, q);
    out->note[0] = 0;
    bool needStat = q.hasSize || q.hasMod;
    bool hasFilter = q.nex || q.nin || q.type || q.nneg;
    if (!q.ninc && !hasFilter) { if (needStat) wcscpy(out->note, L"Add a name, ext: or in: to narrow a size/date search"); return; }

    Coll* C = (Coll*)calloc(1, sizeof(Coll));
    C->mode = needStat ? 1 : (q.ninc ? 0 : 1); C->cap = needStat ? STAT_CAP : ALL_CAP;
    Vec<wchar_t> xa; Vec<uint32_t> xo; Vec<uint8_t> xdir;          // extra candidates (overlay / frecency) copied locally
    int nMatch = 0;

    AcquireSRWLockShared(&g_lock);
    {
        const wchar_t* base = g_pool.p; const wchar_t* end = base + g_pool.n; const Entry* E = g_ent.p; size_t ne = g_ent.n;
        if (q.ninc && ne) {
            const wchar_t* tk = q.inc[q.primary]; int m = q.incL[q.primary]; size_t ei = 0;
            wchar_t f1 = tk[0]; wchar_t f2 = (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)f1);
            const wchar_t* s = base;
            while (s < end) {
                const wchar_t* lim = (size_t)(end - s) > 262144 ? s + 262144 : end;
                while (s < lim) {
                    wchar_t c = *s;
                    if (c != f1 && c != f2) { ++s; continue; }
                    if (end - s < m) { s = end; break; }
                    int j = 1; while (j < m && g_fold[(uint16_t)s[j]] == tk[j]) ++j;
                    if (j < m) { ++s; continue; }
                    size_t off = (size_t)(s - base);
                    while (ei + 1 < ne && E[ei + 1].off <= off) ++ei;
                    const Entry& e = E[ei]; const wchar_t* nm = base + e.off;
                    bool ok = off < (size_t)e.off + e.len && entPass(q, e, nm);
                    for (int t = 0; t < q.ninc && ok; t++) if (t != q.primary && findFold(nm, e.len, q.inc[t], q.incL[t]) < 0) ok = false;
                    if (ok) {
                        nMatch++; Cand cd; int pos = (int)(s - nm);
                        cd.base = baseScore(pos, m, nm, e.len, (e.flags & EF_DIR) != 0); cd.id = (uint32_t)ei; cd.hs = (uint16_t)pos; cd.hl = (uint16_t)m; C->add(cd);
                    }
                    s = nm + e.len + 1;
                }
                if (cancelled(gen)) { ReleaseSRWLockShared(&g_lock); free(C->vec.p); free(C); return; }
            }
            // fuzzy fallback: "rprt24" → "Report 2024" (only when plain matching finds little)
            if (nMatch < 6 && q.fzL >= 3 && !needStat) {
                bool ws = q.fzL <= 4;
                for (size_t i = 0; i < ne; i++) {
                    if ((i & 0xFFFF) == 0 && cancelled(gen)) { ReleaseSRWLockShared(&g_lock); free(C->vec.p); free(C); return; }
                    const Entry& e = E[i]; if ((e.flags & EF_DEL) || e.len < q.fzL) continue;
                    const wchar_t* nm = base + e.off; int sc = fuzzyScore(nm, e.len, q.fz, q.fzL, ws);
                    if (sc < 0 || !entPass(q, e, nm)) continue;
                    Cand cd; cd.base = sc; cd.id = (uint32_t)i; cd.hs = 0; cd.hl = 0; C->add(cd);
                }
            }
        } else if (!q.ninc && ne) {                               // filters only (e.g. "ext:pdf in:invoices")
            for (size_t i = 0; i < ne; i++) {
                if ((i & 0xFFFF) == 0 && cancelled(gen)) { ReleaseSRWLockShared(&g_lock); free(C->vec.p); free(C); return; }
                const Entry& e = E[i]; const wchar_t* nm = base + e.off;
                if (!entPass(q, e, nm)) continue;
                Cand cd; cd.base = 500 - (e.len > 100 ? 100 : e.len) + ((e.flags & EF_DIR) ? 20 : 0); cd.id = (uint32_t)i; cd.hs = 0; cd.hl = 0; C->add(cd);
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (cancelled(gen)) { free(C->vec.p); free(C); return; }

    // extras: files created since launch on walker volumes, and things you open often (even if the plain ranking buried them)
    if (q.ninc) {
        const wchar_t* tk = q.inc[q.primary]; int m = q.incL[q.primary];
        AcquireSRWLockShared(&g_ovLock);
        for (size_t k = 0; k < g_ovAdd.n; k++) {
            const wchar_t* path = g_ovAdd.p[k]; const wchar_t* nm = wcsrchr(path, L'\\'); nm = nm ? nm + 1 : path; int nl = (int)wcslen(nm);
            int pos = findFold(nm, nl, tk, m); if (pos < 0 || !nameFilters(q, nm, nl)) continue;
            bool ok = true; for (int t = 0; t < q.ninc && ok; t++) if (t != q.primary && findFold(nm, nl, q.inc[t], q.incL[t]) < 0) ok = false;
            for (int t = 0; t < q.nin && ok; t++) if (findFold(path, (int)(nm - path), q.inn[t], q.innL[t]) < 0) ok = false;
            if (!ok) continue;
            uint32_t kx = (uint32_t)xo.n; xo.push((uint32_t)xa.n); xa.append(path, wcslen(path) + 1); uint8_t d0 = 0; xdir.push(d0);
            Cand cd; cd.base = baseScore(pos, m, nm, nl, false); cd.id = ID_OVL | kx; cd.hs = (uint16_t)pos; cd.hl = (uint16_t)m; C->add(cd);
        }
        ReleaseSRWLockShared(&g_ovLock);
        AcquireSRWLockShared(&g_fqLock);
        for (size_t k = 0; k < g_freq.n; k++) {
            const wchar_t* path = g_freq.p[k].path; const wchar_t* nm = wcsrchr(path, L'\\'); nm = nm ? nm + 1 : path; int nl = (int)wcslen(nm);
            int pos = findFold(nm, nl, tk, m); if (pos < 0 || !nameFilters(q, nm, nl)) continue;
            bool ok = true; for (int t = 0; t < q.ninc && ok; t++) if (t != q.primary && findFold(nm, nl, q.inc[t], q.incL[t]) < 0) ok = false;
            for (int t = 0; t < q.nin && ok; t++) if (findFold(path, (int)(nm - path), q.inn[t], q.innL[t]) < 0) ok = false;
            if (!ok) continue;
            uint32_t kx = (uint32_t)xo.n; xo.push((uint32_t)xa.n); xa.append(path, wcslen(path) + 1); xdir.push(g_freq.p[k].dir);
            Cand cd; cd.base = baseScore(pos, m, nm, nl, g_freq.p[k].dir != 0); cd.id = ID_FRQ | kx; cd.hs = (uint16_t)pos; cd.hl = (uint16_t)m; C->add(cd);
        }
        ReleaseSRWLockShared(&g_fqLock);
    }
    if (C->overflow) wcscpy(out->note, needStat ? L"Too many matches to filter by size/date \u2014 add words, ext: or in:" : L"Showing the best of a very large match set");

    // ── final ranking: location + depth + how often you open it, then verify on disk and apply size/date filters ──
    const Cand* cs; size_t nc;
    if (C->mode == 0) { cs = C->top; nc = (size_t)C->n; } else { cs = C->vec.p; nc = C->vec.n; }
    if (C->mode == 1 && !needStat && nc > 3000) { qsort((void*)cs, nc, sizeof(Cand), cmpCand); nc = 3000; }
    Vec<Fin> fin; Vec<wchar_t> fa; uint64_t now = nowSec();
    AcquireSRWLockShared(&g_lock); AcquireSRWLockShared(&g_ovLock); AcquireSRWLockShared(&g_fqLock);
    for (size_t i = 0; i < nc; i++) {
        if ((i & 0x3FF) == 0 && cancelled(gen)) break;
        const Cand& c = cs[i]; wchar_t path[PM]; int pl;
        if (c.id & (ID_OVL | ID_FRQ)) { uint32_t kx = c.id & 0x3FFFFFFF; wcscpy(path, xa.p + xo.p[kx]); pl = (int)wcslen(path); }
        else { if (c.id >= g_ent.n) continue; pl = buildPath(c.id, path); }
        bool gone = false; for (size_t d = 0; d < g_ovDel.n && !gone; d++) if (underOrEq(path, g_ovDel.p[d])) gone = true;
        if (gone) continue;
        int score = c.base; wchar_t lc[PM]; lowerInto(lc, path, pl);
        if (g_profLen && underOrEq(path, g_profile)) score += 150;
        int depth = 0; for (int k = 0; k < pl; k++) if (path[k] == L'\\') depth++;
        score -= depth * 3 > 60 ? 60 : depth * 3;
        if (wcsstr(lc, L"\\appdata\\")) score -= 120;
        if (wcsstr(lc, L"\\program files")) score -= 40;
        score += fqBoostLocked(pathHash(path), now);
        Fin f; f.score = score; f.po = (uint32_t)fa.n; f.pl = (uint16_t)pl; f.hs = c.hs; f.hl = c.hl; fa.append(path, pl + 1); fin.push(f);
    }
    ReleaseSRWLockShared(&g_fqLock); ReleaseSRWLockShared(&g_ovLock); ReleaseSRWLockShared(&g_lock);
    if (!cancelled(gen)) {
        qsort(fin.p, fin.n, sizeof(Fin), cmpFin);
        for (size_t i = 0; i < fin.n && (int)out->list.it.n < MAXRES; i++) {
            if (cancelled(gen)) break;
            const wchar_t* path = fa.p + fin.p[i].po; bool dup = false;
            for (size_t d = 0; d < out->list.it.n && !dup; d++) if (_wcsicmp(out->list.P((int)d), path) == 0) dup = true;
            if (dup) continue;
            WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) continue;      // vanished
            bool dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((q.type == 1 && dir) || (q.type == 2 && !dir) || (q.nex && dir)) continue;
            if (q.hasSize) {
                if (dir) continue; int64_t sz = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
                bool ok = q.szOp == 1 ? sz > q.szA : q.szOp == 2 ? sz < q.szA : q.szOp == 3 ? sz == q.szA : (sz >= q.szA && sz <= q.szB);
                if (!ok) continue;
            }
            if (q.hasMod) { uint64_t t = ft64(d.ftLastWriteTime); if (t < q.modFrom || t >= q.modTo) continue; }
            int ix = out->list.add(path, fin.p[i].pl, dir, dir ? -1 : (((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow), ft64(d.ftLastWriteTime));
            if (fin.p[i].hl) { out->list.it.p[ix].hs = fin.p[i].hs; out->list.it.p[ix].hl = fin.p[i].hl; }
        }
    }
    fin.free_(); fa.free_(); xa.free_(); xo.free_(); xdir.free_(); free(C->vec.p); free(C);
}
static void requestSearch(const wchar_t* qtext, bool keep) {
    EnterCriticalSection(&g_sq); wcsncpy(g_sqText, qtext, 511); g_sqText[511] = 0; g_sqKeep = keep; LeaveCriticalSection(&g_sq);
    InterlockedIncrement(&g_gen); SetEvent(g_sqEv);
}
static DWORD WINAPI searchThread(void*) {
    for (;;) {
        WaitForSingleObject(g_sqEv, INFINITE);
        wchar_t q[512]; bool keep; LONG gen;
        EnterCriticalSection(&g_sq); wcscpy(q, g_sqText); keep = g_sqKeep; gen = g_gen; LeaveCriticalSection(&g_sq);
        SearchOut* so = (SearchOut*)calloc(1, sizeof(SearchOut)); so->gen = gen; so->keep = keep;
        searchRun(q, gen, so);
        if (gen == g_gen) PostMessageW(g_hwnd, WM_SEARCHDONE, 0, (LPARAM)so); else searchOutFree(so);
    }
}

// ───────────────────────────── index thread: pick the best strategy per volume ─────────────────────────────
static DWORD WINAPI indexThread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    bool fb[26] = {}; DWORD mask = GetLogicalDrives();
    ilog("indexing started (administrator rights: %s)", g_elevated ? "yes" : "no -> directory walker for every drive");
    if (g_elevated) cacheRead();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
#ifdef SEEK_SELFTEST
        if (i != 2) continue;
#endif
        Vol* v = &g_vol[i]; v->letter = root[0]; v->id = (uint8_t)i; v->rootIdx = -1; v->journal = false; bool ok = false;
        if (g_elevated) {
            wchar_t fs[32] = {}; DWORD ser = 0;
            if (GetVolumeInformationW(root, NULL, 0, &ser, NULL, NULL, fs, 32) && _wcsicmp(fs, L"NTFS") == 0) { v->serial = ser; ok = volInit(v); }
        }
        if (ok) g_journalVols++; else { fb[i] = true; if (g_elevated) ilog("%lc: using the directory walker", (wint_t)root[0]); }
        PostMessageW(g_hwnd, WM_IDX, 0, 0);
    }
    cacheFree();
    for (int i = 0; i < 26; i++) if (g_vol[i].journal) { HANDLE t = CreateThread(NULL, 0, journalThread, &g_vol[i], 0, NULL); if (t) CloseHandle(t); }

    for (int i = 0; i < 26; i++) if (fb[i]) {         // watch first, scan second: nothing is missed while the walk runs
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0}; memcpy(g_wroots[i], root, sizeof root);
        HANDLE t = CreateThread(NULL, 0, watchThread, g_wroots[i], 0, NULL); if (t) CloseHandle(t);
    }
    int pd = g_profLen ? (int)(towupper(g_profile[0]) - L'A') : -1;
    static wchar_t buf[PM];
    if (pd >= 0 && pd < 26 && fb[pd]) {                // user profile first → useful results in ~1s on walker volumes
        int32_t r = addRoot(g_profile); wcscpy(buf, g_profile); wcscat(buf, L"\\");
        scanDir(buf, (int)wcslen(buf), r, 0); PostMessageW(g_hwnd, WM_IDX, 0, 0);
    }
    for (int i = 0; i < 26; i++) {
        if (!fb[i]) continue;
        int32_t r = addRoot(g_wroots[i]); wcscpy(buf, g_wroots[i]); scanDir(buf, 3, r, 0);
    }
    InterlockedExchange(&g_scanning, 0);
    ilog("indexing finished: %ld items, %d volume(s) in instant mode", (long)g_count, g_journalVols);
    PostMessageW(g_hwnd, WM_IDX, 1, 0);
    if (g_journalVols) saveCache();
    return 0;
}


// ───────────────────────────── directory listing / sorting ─────────────────────────────
static List* g_sortList;
static int cmpItems(const void* a, const void* b) {
    const Item* x = (const Item*)a; const Item* y = (const Item*)b; List* L = g_sortList;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    int r = 0;
    if (g_sortKey == 1) r = x->mtime < y->mtime ? -1 : (x->mtime > y->mtime ? 1 : 0);
    else if (g_sortKey == 2) r = x->size < y->size ? -1 : (x->size > y->size ? 1 : 0);
    if (r == 0) r = StrCmpLogicalW(L->pool.p + x->off + x->nameStart, L->pool.p + y->off + y->nameStart);
    return g_sortAsc ? r : -r;
}
static void sortList(List& L) { g_sortList = &L; qsort(L.it.p, L.it.n, sizeof(Item), cmpItems); }

static void loadDir(const wchar_t* dir, const wchar_t* prefix, int plen, List& out, int limit, bool hidden) {
    wchar_t pat[PM]; size_t dl = wcslen(dir); if (dl + 3 >= PM) return;
    wcscpy(pat, dir); if (dl && pat[dl - 1] != L'\\') pat[dl++] = L'\\'; pat[dl] = L'*'; pat[dl + 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    pat[dl] = 0;
    do {
        const wchar_t* nm = fd.cFileName;
        if (nm[0] == L'.' && (!nm[1] || (nm[1] == L'.' && !nm[2]))) continue;
        if (!hidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (plen && !startsFold(nm, prefix, plen)) continue;
        wchar_t full[PM]; int nl = (int)wcslen(nm); if ((int)dl + nl >= PM - 1) continue;
        memcpy(full, pat, dl * 2); memcpy(full + dl, nm, (nl + 1) * 2);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int64_t sz = isDir ? -1 : (((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        out.add(full, (int)dl + nl, isDir, sz, ft64(fd.ftLastWriteTime), (uint8_t)(fd.dwFileAttributes & 0xFF));
        if ((int)out.it.n >= limit) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void pathSearch() {
    wchar_t q[PM], t[PM];
    if (g_query[0] == L'~') { swprintf(t, PM, L"%ls%ls", g_profile, g_query + 1); }
    else if (g_query[0] == L'%') ExpandEnvironmentStringsW(g_query, t, PM);
    else wcscpy(t, g_query);
    size_t n = wcslen(t); for (size_t i = 0; i <= n; i++) q[i] = t[i] == L'/' ? L'\\' : t[i];
    wchar_t dir[PM]; const wchar_t* leaf;
    wchar_t* ls = wcsrchr(q, L'\\');
    if (ls) { size_t dl = ls - q + 1; memcpy(dir, q, dl * 2); dir[dl] = 0; leaf = ls + 1; }
    else { wcscpy(dir, q); wcscat(dir, L"\\"); leaf = L""; }
    wcscpy(g_pathDir, dir); g_pathLeafEmpty = !leaf[0];
    loadDir(dir, leaf, (int)wcslen(leaf), g_results, 300, leaf[0] == L'.');
    { int k = g_sortKey; bool a = g_sortAsc; g_sortKey = 0; g_sortAsc = true; sortList(g_results); g_sortKey = k; g_sortAsc = a; }
    if (g_results.it.n > MAXRES * 2) g_results.it.n = MAXRES * 2;
}

// ───────────────────────────── layout ─────────────────────────────
struct Lay {
    float W, H, cx, cw, py, ph, top, bot, rowH, crumbY, hdrY;
    int cols, pinShown, recShown; float tileW, tileH, gap, pinLblY, tileY, recLblY, recY, recRowH;
};
static int pinCount() { int n = (int)g_pins.it.n; return n > 12 ? 12 : n; }
static Lay lay() {
    Lay L; RECT rc; GetClientRect(g_hwnd, &rc); float S = g_dpi / 96.f;
    L.W = rc.right / S; L.H = rc.bottom / S;
    L.cw = fminf(720.f, L.W - 48.f); if (L.cw < 240) L.cw = 240; L.cx = (L.W - L.cw) / 2; L.ph = 54;
    float homeY = fmaxf(130.f, L.H * 0.33f - 27.f);
    L.py = homeY + (20.f - homeY) * g_t;
    int m = mode(); float pb = L.py + L.ph;
    L.rowH = m == FOLDER ? 30.f : 48.f;
    L.crumbY = pb + 14; L.hdrY = L.crumbY + 32;
    L.top = m == FOLDER ? L.hdrY + 28 : pb + 12;
    L.bot = L.H - 36.f;
    L.gap = 10; L.tileH = 52; L.recRowH = 36;
    int cols = (int)((L.cw + L.gap) / (150 + L.gap)); if (cols < 2) cols = 2; if (cols > 4) cols = 4; L.cols = cols;
    L.tileW = (L.cw - L.gap * (cols - 1)) / cols;
    int np = pinCount(); L.pinShown = np < cols * 3 ? np : cols * 3;
    L.pinLblY = pb + 30; L.tileY = L.pinLblY + 26;
    int rows = (L.pinShown + cols - 1) / cols;
    float tb = L.pinShown ? L.tileY + rows * (L.tileH + L.gap) - L.gap : L.pinLblY;
    L.recLblY = L.pinShown ? tb + 28 : L.pinLblY; L.recY = L.recLblY + 26;
    int fit = (int)((L.H - 34 - L.recY) / L.recRowH); if (fit < 0) fit = 0;
    int nr = (int)g_recents.it.n; if (nr > 8) nr = 8;
    L.recShown = nr < fit ? nr : fit;
    return L;
}
struct Ref { List* l; int i; };
static int itemCount(const Lay& L) {
    switch (mode()) { case HOME: return L.pinShown + L.recShown; case SEARCH: return (int)g_results.it.n; default: return (int)g_folder.it.n; }
}
static bool refAt(int idx, const Lay& L, Ref& r) {
    if (idx < 0) return false;
    switch (mode()) {
    case HOME: if (idx < L.pinShown) { r.l = &g_pins; r.i = idx; return true; }
               idx -= L.pinShown; if (idx < L.recShown) { r.l = &g_recents; r.i = idx; return true; } return false;
    case SEARCH: if (idx < (int)g_results.it.n) { r.l = &g_results; r.i = idx; return true; } return false;
    default: if (idx < (int)g_folder.it.n) { r.l = &g_folder; r.i = idx; return true; } return false;
    }
}
static void clampScroll(const Lay& L) {
    View& v = V(); if (mode() == HOME) { v.scroll = 0; return; }
    float mx = itemCount(L) * L.rowH - (L.bot - L.top); if (mx < 0) mx = 0;
    if (v.scroll > mx) v.scroll = mx; if (v.scroll < 0) v.scroll = 0;
}
static void ensureVisible(int i, const Lay& L) {
    if (mode() == HOME || i < 0) return; View& v = V();
    float vh = L.bot - L.top;
    if (i * L.rowH < v.scroll) v.scroll = i * L.rowH;
    else if ((i + 1) * L.rowH > v.scroll + vh) v.scroll = (i + 1) * L.rowH - vh;
    clampScroll(L);
}

// ───────────────────────────── theme ─────────────────────────────
static void applyTheme() {
    bool light = regDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) != 0;
    T.dark = !light;
    if (light) { T.bg = 0xF5F5F7; T.surface = 0xFFFFFF; T.text = 0x1D1D1F; T.text2 = 0x6E6E73; T.hoverC = 0x000000; T.hoverA = 0.05f; T.border = 0x000000; T.borderA = 0.09f; }
    else       { T.bg = 0x181818; T.surface = 0x242426; T.text = 0xF5F5F7; T.text2 = 0xA1A1A6; T.hoverC = 0xFFFFFF; T.hoverA = 0.07f; T.border = 0xFFFFFF; T.borderA = 0.10f; }
    DWORD ac = regDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0);
    uint32_t acc = 0x0A84FF;                                     // Apple blue fallback
    if (ac) acc = ((ac & 255) << 16) | (((ac >> 8) & 255) << 8) | ((ac >> 16) & 255);
    float r = ((acc >> 16) & 255), g = ((acc >> 8) & 255), b = (acc & 255);
    float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.f;
    if (T.dark && lum < 0.35f) { r += (255 - r) * 0.35f; g += (255 - g) * 0.35f; b += (255 - b) * 0.35f; lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.f; }
    if (!T.dark && lum > 0.75f) { r *= 0.7f; g *= 0.7f; b *= 0.7f; lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.f; }
    T.accent = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    T.onAccent = lum > 0.62f ? 0x000000 : 0xFFFFFF;
    if (g_brSurface) DeleteObject(g_brSurface);
    g_brSurface = CreateSolidBrush(CR(T.surface));
    if (g_hwnd) {
        BOOL d = T.dark; DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof d);
        COLORREF cap = CR(T.bg), tx = CR(T.text);
        DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR_, &cap, sizeof cap);
        DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR_, &tx, sizeof tx);
        typedef int (WINAPI * SetModeFn)(int); typedef void (WINAPI * FlushFn)();
        HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
        if (ux) {   // dark context menus (undocumented ordinals 135 / 136, stable since Win10 1903)
            SetModeFn sm = (SetModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)); FlushFn ff = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
            if (sm) sm(T.dark ? 2 : 3); if (ff) ff();
        }
        InvalidateRect(g_edit, 0, TRUE); InvalidateRect(g_ren, 0, TRUE); invalidate();
    }
}

// ───────────────────────────── D2D / DWrite ─────────────────────────────
static IDWriteTextFormat* mkFmt(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT al) {
    IDWriteTextFormat* f = 0;
    g_dw->CreateTextFormat(fam, NULL, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) { f->SetTextAlignment(al); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    return f;
}
static void initText() {
    const wchar_t* tx = L"Segoe UI Variable Text"; const wchar_t* di = L"Segoe UI Variable Display"; const wchar_t* ic = L"Segoe Fluent Icons";
    fTitle = mkFmt(di, 46, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER);
    fBody = mkFmt(tx, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fBodyR = mkFmt(tx, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fSmall = mkFmt(tx, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fSmallR = mkFmt(tx, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    fLabel = mkFmt(tx, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    fSmallC = mkFmt(tx, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fGlyph = mkFmt(ic, 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fGlyphS = mkFmt(ic, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fCrumb = mkFmt(tx, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    fCrumbB = mkFmt(tx, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_dw->CreateEllipsisTrimmingSign(fBody, &g_ell);
}
// ── logos: PNG resources → WIC → Direct2D bitmap, scaled once to the exact device-pixel size (crisp at any DPI) ──
struct Logo { ID2D1Bitmap* bmp; UINT pw, ph; int res; UINT wantH; };
static Logo g_lockup, g_mark; static IWICImagingFactory* g_wic;
static void logoFree(Logo& l) { if (l.bmp) { l.bmp->Release(); l.bmp = 0; } l.res = 0; }
static void logosFree() { logoFree(g_lockup); logoFree(g_mark); }
static bool logoLoad(Logo& lg, int resId, UINT wantH) {
    if (lg.bmp && lg.res == resId && lg.wantH == wantH) return true;
    logoFree(lg); if (!g_rt) return false;
    if (!g_wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&g_wic))) return false;
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(resId), RT_RCDATA); if (!r) return false;
    HGLOBAL hg = LoadResource(g_inst, r); const void* data = hg ? LockResource(hg) : 0; DWORD n = SizeofResource(g_inst, r); if (!data || !n) return false;
    IWICStream* st = 0; IWICBitmapDecoder* dec = 0; IWICBitmapFrameDecode* fr = 0; IWICBitmapScaler* sc = 0; IWICFormatConverter* cv = 0; bool ok = false;
    if (SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)data, n)) &&
        SUCCEEDED(g_wic->CreateDecoderFromStream(st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
        UINT sw = 0, sh = 0; fr->GetSize(&sw, &sh);
        if (sw && sh) {
            UINT ph = wantH ? wantH : sh; UINT pw = (UINT)((double)sw * ph / sh + 0.5); if (pw < 1) pw = 1;
            if (SUCCEEDED(g_wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, pw, ph, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(g_rt->CreateBitmapFromWicBitmap(cv, NULL, &lg.bmp))) { lg.pw = pw; lg.ph = ph; lg.res = resId; lg.wantH = wantH; ok = true; }
        }
    }
    if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release();
    return ok;
}
static void drawLogo(const Logo& lg, float x, float y, float alpha) {
    float S = g_dpi / 96.f; D2D1_RECT_F r = {x, y, x + lg.pw / S, y + lg.ph / S};
    g_rt->DrawBitmap(lg.bmp, &r, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
}
static HICON g_icoBig, g_icoSmall;
static void setWindowIcons(HWND h) {
    UINT d = GetDpiForWindow(h);
    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, d), GetSystemMetricsForDpi(SM_CYICON, d), LR_DEFAULTCOLOR);
    HICON sm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, d), GetSystemMetricsForDpi(SM_CYSMICON, d), LR_DEFAULTCOLOR);
    if (big) { SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big); if (g_icoBig) DestroyIcon(g_icoBig); g_icoBig = big; }
    if (sm) { SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)sm); if (g_icoSmall) DestroyIcon(g_icoSmall); g_icoSmall = sm; }
}
static void discardTarget() { logosFree(); if (g_br) { g_br->Release(); g_br = 0; } if (g_rt) { g_rt->Release(); g_rt = 0; } }
static bool ensureTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = {}; rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rp.pixelFormat.format = (DXGI_FORMAT)0; rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;
    rp.dpiX = rp.dpiY = (float)g_dpi; rp.usage = D2D1_RENDER_TARGET_USAGE_NONE; rp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = {}; hp.hwnd = g_hwnd; hp.pixelSize.width = rc.right > 0 ? rc.right : 1;
    hp.pixelSize.height = rc.bottom > 0 ? rc.bottom : 1; hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    if (FAILED(g_d2d->CreateHwndRenderTarget(rp, hp, &g_rt))) return false;
    D2D1_COLOR_F c = C(0); g_rt->CreateSolidColorBrush(c, &g_br);
    return g_br != 0;
}
static void fillRR(float x, float y, float w, float h, float r, D2D1_COLOR_F c) {
    g_br->SetColor(&c); D2D1_ROUNDED_RECT rr = {{x, y, x + w, y + h}, r, r}; g_rt->FillRoundedRectangle(&rr, g_br);
}
static void strokeRR(float x, float y, float w, float h, float r, D2D1_COLOR_F c, float sw) {
    g_br->SetColor(&c); D2D1_ROUNDED_RECT rr = {{x + sw / 2, y + sw / 2, x + w - sw / 2, y + h - sw / 2}, r, r}; g_rt->DrawRoundedRectangle(&rr, g_br, sw);
}
static void TX(const wchar_t* s, int n, float x, float y, float w, float h, IDWriteTextFormat* f, D2D1_COLOR_F c, int hs = -1, int hl = 0) {
    if (n <= 0 || w <= 2 || !f) return;
    IDWriteTextLayout* tl = 0; if (FAILED(g_dw->CreateTextLayout(s, (UINT32)n, f, w, h, &tl)) || !tl) return;
    DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; tl->SetTrimming(&tr, g_ell);
    if (hs >= 0 && hl > 0 && hs + hl <= n) { DWRITE_TEXT_RANGE r = {(UINT32)hs, (UINT32)hl}; tl->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r); }
    g_br->SetColor(&c); D2D1_POINT_2F pt = {x, y}; g_rt->DrawTextLayout(pt, tl, g_br, D2D1_DRAW_TEXT_OPTIONS_NONE); tl->Release();
}
static float measure(const wchar_t* s, int n, IDWriteTextFormat* f) {
    IDWriteTextLayout* tl = 0; if (FAILED(g_dw->CreateTextLayout(s, (UINT32)n, f, 10000, 40, &tl)) || !tl) return 0;
    DWRITE_TEXT_METRICS m; tl->GetMetrics(&m); float w = m.widthIncludingTrailingWhitespace; tl->Release(); return w;
}
static void fmtCount(LONG v, wchar_t* out, int n) {              // 1203442 -> 1,203,442
    wchar_t t[32]; swprintf(t, 32, L"%ld", (long)v); NUMBERFMTW f = {0, 0, 3, (LPWSTR)L".", (LPWSTR)L",", 1};
    if (!GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, t, &f, out, n)) wcsncpy(out, t, n);
}
static void fmtDate(uint64_t ft, wchar_t* out, int n, bool dateOnly = false) {
    FILETIME f = {(DWORD)ft, (DWORD)(ft >> 32)}, l; SYSTEMTIME st; out[0] = 0;
    if (!ft || !FileTimeToLocalFileTime(&f, &l) || !FileTimeToSystemTime(&l, &st)) return;
    int k = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, out, n, NULL);
    if (!dateOnly && k > 0 && k < n - 2) { out[k - 1] = L' '; GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, out + k, n - k); }
}

// folder-view hit regions (filled while painting)
struct Seg { float x, w; int endIdx; };
static Seg g_segs[48]; static int g_nsegs;
static bool isSelIdx(int i) { return mode() == FOLDER && g_nmarks ? (i < (int)g_marks.n && g_marks.p[i]) : i == V().sel; }
static wchar_t g_status[96];

static void colGeom(const Lay& L, float& nameL, float& nameR, float& dateL, float& dateR, float& sizeL, float& sizeR) {
    sizeR = L.cx + L.cw - 14; sizeL = sizeR - 80; dateR = sizeL - 12; dateL = dateR - 140; nameL = L.cx + 46; nameR = dateL - 12;
}

static void drawRowBg(const Lay& L, float y, float h, bool sel, bool hov) {
    if (sel) fillRR(L.cx, y, L.cw, h, 9, C(T.accent));
    else if (hov) fillRR(L.cx, y, L.cw, h, 9, C(T.hoverC, T.hoverA));
}

static void paint() {
    if (!ensureTarget()) return;
    g_rt->BeginDraw(); g_rt->Clear(&(const D2D1_COLOR_F&)C(T.bg));
    Lay L = lay(); int md = mode(); View& v = V();
    D2D1_COLOR_F cText = C(T.text), cText2 = C(T.text2), cOn = C(T.onAccent), cOn2 = C(T.onAccent, 0.75f);

    // title (home only, fades while the pill glides up)
    float S = g_dpi / 96.f;
    float ta = 1.f - g_t * 1.6f;
    if (ta > 0.01f) {                                            // hero lockup (falls back to plain text if the image can't load)
        if (logoLoad(g_lockup, T.dark ? 11 : 10, (UINT)(64.f * S + 0.5f))) drawLogo(g_lockup, (L.W - g_lockup.pw / S) / 2, L.py - 100, ta);
        else TX(APPNAME, 4, 0, L.py - 104, L.W, 80, fTitle, C(T.text, ta));
    }
    float ma = g_t * 1.6f - 0.4f; if (ma > 1.f) ma = 1.f;
    if (ma > 0.01f && L.cx >= 52 && logoLoad(g_mark, T.dark ? 13 : 12, (UINT)(26.f * S + 0.5f)))   // small mark beside the field: click = home
        drawLogo(g_mark, L.cx - 22 - g_mark.pw / S, L.py + (L.ph - g_mark.ph / S) / 2, ma);

    // search pill
    if (!T.dark) for (int i = 3; i >= 1; --i) fillRR(L.cx - i * 2, L.py - i * 2 + i + 2, L.cw + i * 4, L.ph + i * 4, L.ph / 2 + i * 2, C(0x000000, 0.018f));
    fillRR(L.cx, L.py, L.cw, L.ph, L.ph / 2, C(T.surface));
    bool focus = GetFocus() == g_edit;
    if (focus) strokeRR(L.cx, L.py, L.cw, L.ph, L.ph / 2, C(T.accent, 0.85f), 2.f);
    else strokeRR(L.cx, L.py, L.cw, L.ph, L.ph / 2, C(T.border, T.borderA), 1.f);
    TX(L"\uE721", 1, L.cx + 18, L.py, 28, L.ph, fGlyph, cText2);
    if (g_query[0]) {                                            // clear (x) button
        float mx = L.cx + L.cw - 32, my = L.py + L.ph / 2; D2D1_ELLIPSE el = {{mx, my}, 9.f, 9.f}; D2D1_COLOR_F cc = C(T.text2, 0.42f);
        g_br->SetColor(&cc); g_rt->FillEllipse(&el, g_br); cc = C(T.surface); g_br->SetColor(&cc);
        D2D1_POINT_2F a1 = {mx - 3.2f, my - 3.2f}, a2 = {mx + 3.2f, my + 3.2f}, b1 = {mx - 3.2f, my + 3.2f}, b2 = {mx + 3.2f, my - 3.2f};
        g_rt->DrawLine(a1, a2, g_br, 1.6f); g_rt->DrawLine(b1, b2, g_br, 1.6f);
    }

    float contentA = 1.f;
    if (md == HOME && g_t < 0.5f) contentA = 1.f - g_t * 2;

    if (md == HOME) {
        int n = itemCount(L);
        if (L.pinShown) TX(L"Pinned", 6, L.cx + 4, L.pinLblY, 200, 20, fLabel, cText2);
        for (int i = 0; i < L.pinShown; i++) {
            int c = i % L.cols, r = i / L.cols; float x = L.cx + c * (L.tileW + L.gap), y = L.tileY + r * (L.tileH + L.gap);
            bool sel = v.sel == i, hov = g_hover == i;
            fillRR(x, y, L.tileW, L.tileH, 12, sel ? C(T.accent) : C(T.surface));
            if (!sel) strokeRR(x, y, L.tileW, L.tileH, 12, C(T.border, T.borderA), 1.f);
            if (!sel && hov) fillRR(x, y, L.tileW, L.tileH, 12, C(T.hoverC, T.hoverA));
            TX(g_pins.it.p[i].dir ? L"\uE8B7" : L"\uE8A5", 1, x + 10, y, 30, L.tileH, fGlyph, sel ? cOn : (g_pins.it.p[i].dir ? C(T.accent) : cText2));
            TX(g_pins.N(i), g_pins.NL(i), x + 46, y, L.tileW - 56, L.tileH, fBody, sel ? cOn : cText);
        }
        if (L.recShown) TX(L"Recent", 6, L.cx + 4, L.recLblY, 200, 20, fLabel, cText2);
        for (int j = 0; j < L.recShown; j++) {
            float y = L.recY + j * L.recRowH; int idx = L.pinShown + j; bool sel = v.sel == idx;
            drawRowBg(L, y, L.recRowH - 2, sel, g_hover == idx);
            const Item& it = g_recents.it.p[j];
            TX(it.dir ? L"\uE8B7" : L"\uE8A5", 1, L.cx + 10, y, 30, L.recRowH - 2, fGlyphS, sel ? cOn : (it.dir ? C(T.accent) : cText2));
            float nw = (L.cw - 80) * 0.5f;
            TX(g_recents.N(j), g_recents.NL(j), L.cx + 46, y, nw, L.recRowH - 2, fBody, sel ? cOn : cText);
            int pl = it.nameStart > 3 ? it.nameStart - 1 : it.nameStart;
            if (pl > 0) TX(g_recents.P(j), pl, L.cx + 46 + nw + 10, y, L.cw - 46 - nw - 10 - 14, L.recRowH - 2, fSmallR, sel ? cOn2 : cText2);
        }
        (void)n;
    } else {
        D2D1_RECT_F clip = {0, L.top, L.W, L.bot};
        if (md == FOLDER) {
            // breadcrumb
            g_nsegs = 0; float x = L.cx + 2;
            const wchar_t* d = g_dir; int dl = (int)wcslen(d);
            { float w = measure(L"Home", 4, fCrumb); TX(L"Home", 4, x, L.crumbY, w + 4, 28, fCrumb, cText2); g_segs[g_nsegs++] = {x, w, 0}; x += w + 8; }
            int start = 0;
            for (int i = 0; i <= dl && g_nsegs < 46; i++) {
                if (i == dl || d[i] == L'\\') {
                    int e = i; if (i < dl && start == 0 && d[1] == L':') e = i + 1;   // keep "C:\" together
                    int sl = i - start; if (sl <= 0) { start = i + 1; continue; }
                    const wchar_t* seg = d + start; int endIdx = (i == dl) ? dl : (start == 0 && d[1] == L':' ? i + 1 : i);
                    TX(L"\u203A", 1, x, L.crumbY, 14, 28, fCrumb, cText2); x += 14;
                    bool last = endIdx == dl; IDWriteTextFormat* f = last ? fCrumbB : fCrumb;
                    float w = measure(seg, sl, f); if (x + w > L.cx + L.cw - 100) w = fmaxf(40.f, L.cx + L.cw - 100 - x);
                    TX(seg, sl, x, L.crumbY, w + 4, 28, f, last ? cText : cText2);
                    g_segs[g_nsegs++] = {x, w, endIdx}; x += w + 8; start = i + 1; (void)e;
                }
            }
            wchar_t cnt[48], cn2[32]; fmtCount((LONG)g_folder.it.n, cn2, 32); swprintf(cnt, 48, L"%ls items", cn2);
            TX(cnt, (int)wcslen(cnt), L.cx + L.cw - 130, L.crumbY, 126, 28, fSmallR, cText2);
            // column headers
            float nl, nr, dlx, dr, sl, sr; colGeom(L, nl, nr, dlx, dr, sl, sr);
            const wchar_t* arrow = g_sortAsc ? L" \u2191" : L" \u2193"; wchar_t hb[24];
            swprintf(hb, 24, L"Name%ls", g_sortKey == 0 ? arrow : L""); TX(hb, (int)wcslen(hb), nl, L.hdrY, 200, 24, fLabel, cText2);
            swprintf(hb, 24, L"Modified%ls", g_sortKey == 1 ? arrow : L""); TX(hb, (int)wcslen(hb), dlx, L.hdrY, 140, 24, fLabel, cText2);
            swprintf(hb, 24, L"Size%ls", g_sortKey == 2 ? arrow : L""); TX(hb, (int)wcslen(hb), sl, L.hdrY, 80, 24, fLabel, cText2);
            g_br->SetColor(&(const D2D1_COLOR_F&)C(T.border, T.borderA));
            D2D1_RECT_F ln = {L.cx, L.hdrY + 25, L.cx + L.cw, L.hdrY + 26}; g_rt->FillRectangle(&ln, g_br);
        }
        g_rt->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
        int n = itemCount(L);
        int first = (int)(v.scroll / L.rowH); if (first < 0) first = 0;
        for (int i = first; i < n; i++) {
            float y = L.top + i * L.rowH - v.scroll; if (y > L.bot) break;
            bool sel = isSelIdx(i), hov = g_hover == i;
            List& Ls = md == SEARCH ? g_results : g_folder; const Item& it = Ls.it.p[i];
            if (md == SEARCH) {
                drawRowBg(L, y, L.rowH - 2, sel, hov);
                TX(it.dir ? L"\uE8B7" : L"\uE8A5", 1, L.cx + 12, y, 30, L.rowH - 2, fGlyph, sel ? cOn : (it.dir ? C(T.accent) : cText2));
                int hs = it.hl ? it.hs : -1; float mw = L.cw > 520 ? 112.f : 0.f;
                TX(Ls.N(i), Ls.NL(i), L.cx + 52, y + 5, L.cw - 68 - mw, 21, fBody, sel ? cOn : cText, hs, it.hl);
                int pl = it.nameStart > 3 ? it.nameStart - 1 : it.nameStart;
                if (pl > 0) TX(Ls.P(i), pl, L.cx + 52, y + 24, L.cw - 68 - mw, 17, fSmall, sel ? cOn2 : cText2);
                if (mw > 0) {
                    wchar_t mb[64];
                    if (it.mtime) { fmtDate(it.mtime, mb, 64, true); TX(mb, (int)wcslen(mb), L.cx + L.cw - 14 - mw, y + 5, mw, 21, fSmallR, sel ? cOn2 : cText2); }
                    if (!it.dir && it.size >= 0) { StrFormatByteSizeW(it.size, mb, 64); TX(mb, (int)wcslen(mb), L.cx + L.cw - 14 - mw, y + 24, mw, 17, fSmallR, sel ? cOn2 : cText2); }
                }
            } else {
                float nl, nr, dlx, dr, sl, sr; colGeom(L, nl, nr, dlx, dr, sl, sr);
                drawRowBg(L, y, L.rowH - 2, sel, hov);
                bool marked = g_nmarks && sel; (void)marked;
                TX(it.dir ? L"\uE8B7" : L"\uE8A5", 1, L.cx + 10, y, 28, L.rowH - 2, fGlyphS, sel ? cOn : (it.dir ? C(T.accent) : cText2));
                if (g_renaming && g_renIdx == i) {
                    fillRR(nl - 6, y + 1, nr - nl + 6, L.rowH - 4, 6, C(T.surface)); strokeRR(nl - 6, y + 1, nr - nl + 6, L.rowH - 4, 6, C(T.accent), 2.f);
                } else TX(Ls.N(i), Ls.NL(i), nl, y, nr - nl, L.rowH - 2, fBody, sel ? cOn : cText);
                wchar_t b[64]; fmtDate(it.mtime, b, 64); TX(b, (int)wcslen(b), dlx, y, 140, L.rowH - 2, fSmall, sel ? cOn2 : cText2);
                if (!it.dir && it.size >= 0) { StrFormatByteSizeW(it.size, b, 64); TX(b, (int)wcslen(b), sl, y, 80, L.rowH - 2, fSmallR, sel ? cOn2 : cText2); }
            }
        }
        if (n == 0 && !(md == SEARCH && g_doneGen != g_gen)) {
            const wchar_t* msg = md == SEARCH ? (g_note[0] ? g_note : (g_scanning ? L"No matches yet \u2014 still indexing\u2026" : L"No matches")) : L"This folder is empty";
            TX(msg, (int)wcslen(msg), 0, L.top + 20, L.W, 30, fBody, cText2);
        }
        g_rt->PopAxisAlignedClip();
        // overlay scrollbar
        float total = n * L.rowH, vh = L.bot - L.top;
        if (total > vh + 1) {
            float th = fmaxf(28.f, vh * vh / total), ty = L.top + (vh - th) * (v.scroll / (total - vh));
            fillRR(L.W - 9, ty, 4, th, 2, C(T.text, 0.22f));
        }
    }
    // footer: status on the right, keyboard hints on the left
    {
        wchar_t st[220]; st[0] = 0; wchar_t cs[40]; fmtCount(g_count + g_progress, cs, 40);
        if (md == SEARCH && g_note[0]) wcscpy(st, g_note);
        else if (md != FOLDER) {
            if (g_scanning) swprintf(st, 220, L"%ls  %ls items", g_progress ? L"Reading the file table\u2026" : L"Indexing\u2026", cs);
            else swprintf(st, 220, L"%ls items%ls", cs, g_journalVols ? L"  \u00B7  live" : L"");
            if (!g_elevated) wcscat(st, L"  \u00B7  Ctrl+Shift+E: instant mode (admin)");
            else if (!g_journalVols && !g_scanning) wcscat(st, L"  \u00B7  standard mode (see index.log)");
        }
        float fy = L.H - 31, sw = st[0] ? measure(st, (int)wcslen(st), fSmall) : 0, limit = L.W - 22 - sw - (sw ? 24 : 0);
        if (st[0]) TX(st, (int)wcslen(st), 0, fy, L.W - 22, 22, fSmallR, C(T.text2, 0.75f * contentA + 0.25f));
        struct Hint { const wchar_t* k; const wchar_t* t; } hs[4]; int nh = 0;
        if (md == SEARCH) { hs[nh++] = {L"Enter", L"Open"}; hs[nh++] = {L"Ctrl+Enter", L"Show in folder"}; if (g_pathMode) hs[nh++] = {L"Tab", L"Complete"}; hs[nh++] = {L"Esc", L"Clear"}; }
        else if (md == FOLDER) { hs[nh++] = {L"Enter", L"Open"}; hs[nh++] = {L"F2", L"Rename"}; hs[nh++] = {L"Del", L"Delete"}; hs[nh++] = {L"Backspace", L"Up"}; }
        float hx = L.cx;
        for (int i = 0; i < nh; i++) {
            float kw = measure(hs[i].k, (int)wcslen(hs[i].k), fSmall) + 12, lw = measure(hs[i].t, (int)wcslen(hs[i].t), fSmall);
            if (hx + kw + 6 + lw > limit) break;
            fillRR(hx, fy + 2, kw, 18, 5, C(T.surface)); strokeRR(hx, fy + 2, kw, 18, 5, C(T.border, T.borderA * 1.7f), 1.f);
            TX(hs[i].k, (int)wcslen(hs[i].k), hx + 6, fy + 2, kw, 18, fSmall, cText2);
            TX(hs[i].t, (int)wcslen(hs[i].t), hx + kw + 6, fy, lw + 4, 22, fSmall, cText2);
            hx += kw + 6 + lw + 16;
        }
        if (md == HOME) { const wchar_t* tip = L"Try   ext:pdf   size:>5mb   modified:week   in:projects   -draft"; TX(tip, (int)wcslen(tip), 0, L.H - 62, L.W, 22, fSmallC, C(T.text2, 0.6f * contentA)); }
    }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) discardTarget();
}

// ───────────────────────────── edit control plumbing ─────────────────────────────
static void makeEditFonts() {
    if (g_fontEdit) DeleteObject(g_fontEdit); if (g_fontRen) DeleteObject(g_fontRen);
    float S = g_dpi / 96.f;
    g_fontEdit = CreateFontW(-(int)(18 * S + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    g_fontRen = CreateFontW(-(int)(14 * S + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontEdit, TRUE); SendMessageW(g_ren, WM_SETFONT, (WPARAM)g_fontRen, TRUE);
    HDC dc = GetDC(g_edit); HGDIOBJ o = SelectObject(dc, g_fontEdit); TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, o); ReleaseDC(g_edit, dc); g_editH = tm.tmHeight + 2;
}
static void layoutEdit() {
    if (!g_edit) return; Lay L = lay(); float S = g_dpi / 96.f;
    int x = (int)((L.cx + 54) * S), w = (int)((L.cw - 54 - 52) * S); int y = (int)((L.py + L.ph / 2) * S) - g_editH / 2;
    SetWindowPos(g_edit, NULL, x, y, w, g_editH, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void startAnim() { SetTimer(g_hwnd, 1, 15, NULL); }

// ───────────────────────────── navigation & actions ─────────────────────────────
static void sortAndMarks() { sortList(g_folder); g_marks.n = 0; g_marks.reserve(g_folder.it.n + 1); g_marks.n = g_folder.it.n; memset(g_marks.p, 0, g_folder.it.n); g_nmarks = 0; }
static void loadFolder() { g_folder.clear(); loadDir(g_dir, NULL, 0, g_folder, INT_MAX, g_showHidden); sortAndMarks(); }
static void setWatchDir() {
    AcquireSRWLockExclusive(&g_wdLock); wcscpy(g_watchDir, g_dir); ReleaseSRWLockExclusive(&g_wdLock);
    g_watchVol = -1; g_watchFrn = 0; g_watchIdx = -1;
    if (g_dir[0] && g_dir[1] == L':') {
        int id = (int)(towupper(g_dir[0]) - L'A'); uint64_t f;
        if (id >= 0 && id < 26 && g_vol[id].journal && dirFrn(g_dir, &f)) {
            uint32_t ix = 0; AcquireSRWLockShared(&g_lock); bool ok = hGet(g_vol[id].map, f, &ix); ReleaseSRWLockShared(&g_lock);
            g_watchFrn = f; g_watchIdx = ok ? (int32_t)ix : -1; g_watchVol = id;
        }
    }
}
static void updateTitle() {
    wchar_t t[PM + 16];
    if (g_dir[0]) { const wchar_t* n = wcsrchr(g_dir, L'\\'); n = (n && n[1]) ? n + 1 : g_dir; swprintf(t, PM + 16, L"%ls \u2014 Seek", n); }
    else wcscpy(t, L"Seek");
    SetWindowTextW(g_hwnd, t);
}
static void histPush(const wchar_t* p) { if (g_hist.n >= 32) { free(g_hist.p[0]); memmove(g_hist.p, g_hist.p + 1, (g_hist.n - 1) * sizeof(wchar_t*)); g_hist.n--; } g_hist.push(_wcsdup(p)); }

static void afterNav() {
    g_tTarget = (g_dir[0] || g_query[0]) ? 1.f : 0.f; startAnim();
    updateTitle(); setWatchDir(); g_hover = -1; invalidate();
}
static void navigate(const wchar_t* path, bool push = true) {
    wchar_t np[PM]; normalizePath(path, np);
    DWORD a = GetFileAttributesW(np);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { MessageBeep(MB_ICONEXCLAMATION); return; }
    if (push) histPush(g_dir);
    wcscpy(g_dir, np); loadFolder();
    g_v[FOLDER].sel = -1; g_v[FOLDER].scroll = 0; g_v[FOLDER].anchor = -1;
    SetWindowTextW(g_edit, L"");   // → EN_CHANGE → onQueryChange
    g_query[0] = 0; afterNav();
}
static void goHome(bool push = true) {
    if (push && g_dir[0]) histPush(g_dir);
    g_dir[0] = 0; g_folder.clear(); g_nmarks = 0; SetWindowTextW(g_edit, L""); g_query[0] = 0; afterNav();
}
static void goUp() { wchar_t p[PM]; if (g_dir[0] && parentOf(g_dir, p)) navigate(p); else if (g_dir[0]) goHome(); }
static void histBack() {
    if (!g_hist.n) return; wchar_t* p = g_hist.p[--g_hist.n];
    if (p[0]) navigate(p, false); else goHome(false);
    free(p);
}
static void recentsPush(const wchar_t* path, bool dir) {
    List* nl = (List*)calloc(1, sizeof(List)); nl->add(path, (int)wcslen(path), dir);
    for (size_t i = 0; i < g_recents.it.n && nl->it.n < 24; i++) if (_wcsicmp(g_recents.P((int)i), path)) nl->add(g_recents.P((int)i), g_recents.it.p[i].len, g_recents.it.p[i].dir);
    g_recents.it.free_(); g_recents.pool.free_(); g_recents = *nl; free(nl);
}
static void openPath(const wchar_t* path, bool dir) {
    freqBump(path, dir);
    if (dir) { navigate(path); return; }
    wchar_t pd[PM]; parentOf(path, pd);
    shellOpen(path, pd);
    SHAddToRecentDocs(SHARD_PATHW, path); recentsPush(path, false);
}
static void selectByName(const wchar_t* name) {
    for (size_t i = 0; i < g_folder.it.n; i++) if (_wcsicmp(g_folder.N((int)i), name) == 0) {
        View& v = g_v[FOLDER]; v.sel = (int)i; v.anchor = (int)i; g_nmarks = 0; memset(g_marks.p, 0, g_marks.n); Lay L = lay(); if (mode() == FOLDER) ensureVisible((int)i, L); return; }
}
static void showInFolder(const wchar_t* path) {
    wchar_t pd[PM]; const wchar_t* nm = wcsrchr(path, L'\\'); wchar_t name[PM]; wcscpy(name, nm ? nm + 1 : path);
    if (!parentOf(path, pd)) { navigate(path); return; }
    navigate(pd); if (name[0]) selectByName(name); invalidate();
}
static void refreshFolder(const wchar_t* keepName) {
    if (!g_dir[0]) return; wchar_t nm[PM]; nm[0] = 0;
    if (keepName) wcscpy(nm, keepName);
    else { View& v = g_v[FOLDER]; if (v.sel >= 0 && v.sel < (int)g_folder.it.n) wcscpy(nm, g_folder.N(v.sel)); }
    float sc = g_v[FOLDER].scroll; DWORD a = GetFileAttributesW(g_dir);
    if (a == INVALID_FILE_ATTRIBUTES) { wchar_t p[PM]; if (parentOf(g_dir, p)) navigate(p, false); else goHome(false); return; }
    loadFolder(); g_v[FOLDER].sel = -1; g_v[FOLDER].scroll = sc;
    if (nm[0]) selectByName(nm);
    if (mode() == FOLDER) { Lay L = lay(); clampScroll(L); }
    invalidate();
}

// selection helpers
static void clearMarks() { if (g_marks.n) memset(g_marks.p, 0, g_marks.n); g_nmarks = 0; }
static void setSel(int i, bool extend) {
    Lay L = lay(); int n = itemCount(L); if (!n) return; if (i < 0) i = 0; if (i >= n) i = n - 1; View& v = V();
    if (mode() == FOLDER) {
        if (extend) {
            if (v.anchor < 0) v.anchor = v.sel < 0 ? i : v.sel;
            clearMarks(); int a = v.anchor < i ? v.anchor : i, b = v.anchor < i ? i : v.anchor;
            for (int k = a; k <= b; k++) g_marks.p[k] = 1; g_nmarks = b - a + 1;
        } else { clearMarks(); v.anchor = i; }
    }
    v.sel = i; ensureVisible(i, L); invalidate();
}
static void toggleMark(int i) {
    View& v = V(); if (!g_nmarks && v.sel >= 0 && v.sel != i) { g_marks.p[v.sel] = 1; g_nmarks = 1; }
    g_marks.p[i] ^= 1; g_nmarks = 0; for (size_t k = 0; k < g_marks.n; k++) g_nmarks += g_marks.p[k];
    v.sel = i; v.anchor = i; invalidate();
}
static void moveSel(int dir, bool extend) {
    Lay L = lay(); int n = itemCount(L); if (!n) return; View& v = V(); int s = v.sel;
    if (mode() == HOME) {
        int np = L.pinShown, nr = L.recShown, c = L.cols;
        if (s < 0) { setSel(0, false); return; }
        if (dir == 2) s = s > 0 ? s - 1 : s;
        else if (dir == 3) s = s < n - 1 ? s + 1 : s;
        else if (dir == 1 || dir == 5) {
            if (s < np) { if (s + c < np) s += c; else if (nr) s = np; else { int lastRowStart = ((np - 1) / c) * c; if (s < lastRowStart) s = np - 1; } }
            else if (s < n - 1) s++;
        } else if (dir == 0 || dir == 4) {
            if (s >= np) { if (s == np && np) s = np - 1; else if (s > np) s--; }
            else if (s - c >= 0) s -= c;
        }
        setSel(s, false); return;
    }
    int page = (int)((L.bot - L.top) / L.rowH) - 1; if (page < 1) page = 1;
    if (s < 0) s = (dir == 0 || dir == 4) ? n : -1;
    if (dir == 0) s--; else if (dir == 1) s++; else if (dir == 4) s -= page; else if (dir == 5) s += page;
    if (s < 0 && v.sel < 0) s = 0;
    setSel(s, extend);
}
static void activate(bool reveal) {
    if (mode() == SEARCH && !g_pathMode && g_doneGen != g_gen) { g_wantAct = reveal ? 2 : 1; return; }   // results still on their way
    Lay L = lay(); View& v = V(); Ref r; wchar_t path[PM]; bool dir;
    if (!refAt(v.sel, L, r)) {
        if (mode() == SEARCH && g_pathMode && g_pathLeafEmpty) navigate(g_pathDir);
        return;
    }
    wcscpy(path, r.l->P(r.i)); dir = r.l->it.p[r.i].dir != 0;
    if (reveal) showInFolder(path); else openPath(path, dir);
}

// ── file operations ──
static void buildPaths(Vec<wchar_t>& buf, int* count) {
    Lay L = lay(); *count = 0; buf.n = 0;
    if (mode() == FOLDER) {
        for (size_t i = 0; i < g_folder.it.n; i++) {
            bool take = g_nmarks ? g_marks.p[i] != 0 : (int)i == g_v[FOLDER].sel;
            if (take) { buf.append(g_folder.P((int)i), g_folder.it.p[i].len + 1); (*count)++; }
        }
    } else { Ref r; if (refAt(V().sel, L, r)) { buf.append(r.l->P(r.i), r.l->it.p[r.i].len + 1); *count = 1; } }
    wchar_t z = 0; buf.push(z);
}
static void copyText(const wchar_t* s) {
    if (!OpenClipboard(g_hwnd)) return; EmptyClipboard();
    size_t n = (wcslen(s) + 1) * 2; HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n); memcpy(GlobalLock(h), s, n); GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h); CloseClipboard();
}
static void clipSet(bool cut) {
    Vec<wchar_t> b; int cnt; buildPaths(b, &cnt); if (!cnt) { b.free_(); return; }
    size_t sz = sizeof(DROPFILES) + b.n * 2; HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sz);
    DROPFILES* d = (DROPFILES*)GlobalLock(h); d->pFiles = sizeof(DROPFILES); d->fWide = TRUE;
    memcpy((BYTE*)d + d->pFiles, b.p, b.n * 2); GlobalUnlock(h);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard(); SetClipboardData(CF_HDROP, h);
        HGLOBAL h2 = GlobalAlloc(GMEM_MOVEABLE, 4); *(DWORD*)GlobalLock(h2) = cut ? 2 : 1; GlobalUnlock(h2);
        SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), h2); CloseClipboard();
    } else GlobalFree(h);
    b.free_();
}
static void pasteFiles() {
    if (!g_dir[0] || !OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_HDROP); if (!h) { CloseClipboard(); return; }
    Vec<wchar_t> from; UINT n = DragQueryFileW((HDROP)h, 0xFFFFFFFF, 0, 0); wchar_t p[PM];
    for (UINT i = 0; i < n; i++) { UINT l = DragQueryFileW((HDROP)h, i, p, PM); from.append(p, l + 1); }
    wchar_t z = 0; from.push(z);
    DWORD eff = 1; HANDLE he = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    if (he) { DWORD* pe = (DWORD*)GlobalLock(he); if (pe) { eff = *pe; GlobalUnlock(he); } }
    CloseClipboard();
    wchar_t to[PM + 2]; wcscpy(to, g_dir); to[wcslen(g_dir) + 1] = 0;
    SHFILEOPSTRUCTW op = {}; op.hwnd = g_hwnd; op.wFunc = (eff & 2) ? FO_MOVE : FO_COPY; op.pFrom = from.p; op.pTo = to;
    op.fFlags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION;
    SHFileOperationW(&op); from.free_(); refreshFolder(NULL);
}
static void deleteSel() {
    Vec<wchar_t> b; int cnt; buildPaths(b, &cnt); if (!cnt) { b.free_(); return; }
    SHFILEOPSTRUCTW op = {}; op.hwnd = g_hwnd; op.wFunc = FO_DELETE; op.pFrom = b.p; op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;
    int r = SHFileOperationW(&op); b.free_();
    View& v = g_v[FOLDER]; int keep = v.sel; (void)r;
    refreshFolder(NULL); if (g_folder.it.n) { v.sel = keep < (int)g_folder.it.n ? keep : (int)g_folder.it.n - 1; if (v.sel < 0) v.sel = 0; } else v.sel = -1;
    invalidate();
}
static void placeRename() {
    if (!g_renaming) return; Lay L = lay(); float S = g_dpi / 96.f; View& v = g_v[FOLDER];
    float nl, nr, dl, dr, sl, sr; colGeom(L, nl, nr, dl, dr, sl, sr);
    float y = L.top + g_renIdx * L.rowH - v.scroll;
    if (y < L.top - 1 || y > L.bot - L.rowH + 2) { ShowWindow(g_ren, SW_HIDE); return; }
    int h = (int)((L.rowH - 10) * S); SetWindowPos(g_ren, HWND_TOP, (int)((nl - 2) * S), (int)((y + 5) * S), (int)((nr - nl - 4) * S), h, SWP_SHOWWINDOW);
}
static void beginRename(int idx) {
    if (mode() != FOLDER || idx < 0 || idx >= (int)g_folder.it.n) return;
    View& v = g_v[FOLDER]; clearMarks(); v.sel = idx; v.anchor = idx; Lay L = lay(); ensureVisible(idx, L);
    g_renIdx = idx; g_renaming = true;
    SetWindowTextW(g_ren, g_folder.N(idx)); placeRename(); SetFocus(g_ren);
    const wchar_t* nm = g_folder.N(idx); const wchar_t* dot = wcsrchr(nm, L'.');
    int sel = (!g_folder.it.p[idx].dir && dot && dot != nm) ? (int)(dot - nm) : (int)wcslen(nm);
    SendMessageW(g_ren, EM_SETSEL, 0, sel); invalidate();
}
static void endRename(bool commit) {
    if (!g_renaming) return; g_renaming = false;
    wchar_t nn[PM]; GetWindowTextW(g_ren, nn, PM); int idx = g_renIdx; g_renIdx = -1;
    ShowWindow(g_ren, SW_HIDE); SetFocus(g_edit);
    if (commit && idx >= 0 && idx < (int)g_folder.it.n && nn[0] && _wcsicmp(nn, g_folder.N(idx)) != 0 && !wcspbrk(nn, L"\\/:*?\"<>|")) {
        wchar_t from[PM], to[PM]; wcscpy(from, g_folder.P(idx)); parentOf(from, to);
        size_t tl = wcslen(to); if (to[tl - 1] != L'\\') to[tl++] = L'\\'; wcscpy(to + tl, nn);
        if (!MoveFileW(from, to)) MessageBeep(MB_ICONEXCLAMATION); else refreshFolder(nn);
    }
    invalidate();
}
static void newFolder() {
    if (mode() != FOLDER) return; wchar_t p[PM], nm[64];
    for (int i = 1; i < 1000; i++) {
        if (i == 1) wcscpy(nm, L"New folder"); else swprintf(nm, 64, L"New folder (%d)", i);
        swprintf(p, PM, L"%ls%ls%ls", g_dir, g_dir[wcslen(g_dir) - 1] == L'\\' ? L"" : L"\\", nm);
        if (CreateDirectoryW(p, NULL)) { refreshFolder(nm); for (size_t k = 0; k < g_folder.it.n; k++) if (_wcsicmp(g_folder.N((int)k), nm) == 0) { beginRename((int)k); break; } return; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    MessageBeep(MB_ICONEXCLAMATION);
}

// ── pins ──
static void savePins() {
    HANDLE f = CreateFileW(g_pinFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return;
    for (size_t i = 0; i < g_pins.it.n; i++) {
        char u[PM * 3]; int n = WideCharToMultiByte(CP_UTF8, 0, g_pins.P((int)i), -1, u, sizeof u, 0, 0); if (n <= 0) continue; u[n - 1] = '\n';
        DWORD w; WriteFile(f, u, n, &w, 0);
    }
    CloseHandle(f);
}
static void addPin(const wchar_t* p) { DWORD a = GetFileAttributesW(p); if (a == INVALID_FILE_ATTRIBUTES) return; g_pins.add(p, (int)wcslen(p), (a & FILE_ATTRIBUTE_DIRECTORY) != 0); }
static void loadPins() {
    wchar_t ad[PM]; SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, ad);
    swprintf(g_pinFile, PM, L"%ls\\Seek", ad); CreateDirectoryW(g_pinFile, NULL); wcscat(g_pinFile, L"\\pins.txt");
    HANDLE f = CreateFileW(g_pinFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(f, 0), rd; char* b = (char*)malloc(sz + 1); ReadFile(f, b, sz, &rd, 0); b[rd] = 0; CloseHandle(f);
        for (char* s = b; *s;) {
            char* e = strchr(s, '\n'); if (e) *e = 0; if (*s) { if (e && e > s && e[-1] == '\r') e[-1] = 0; wchar_t w[PM]; if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, PM)) addPin(w); }
            if (!e) break; s = e + 1;
        }
        free(b); return;
    }
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures, &FOLDERID_Videos};
    for (int i = 0; i < 5; i++) { PWSTR p = 0; if (SUCCEEDED(SHGetKnownFolderPath(*ids[i], 0, NULL, &p))) { addPin(p); CoTaskMemFree(p); } }
    int drives = 0; DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && drives < 3; i++) if (mask & (1u << i)) { wchar_t r[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0}; if (GetDriveTypeW(r) == DRIVE_FIXED) { addPin(r); drives++; } }
}
static int pinIndex(const wchar_t* p) { for (size_t i = 0; i < g_pins.it.n; i++) if (_wcsicmp(g_pins.P((int)i), p) == 0) return (int)i; return -1; }
static void togglePin(const wchar_t* path) {
    int ix = pinIndex(path);
    if (ix >= 0) { List n = {}; for (size_t i = 0; i < g_pins.it.n; i++) if ((int)i != ix) n.add(g_pins.P((int)i), g_pins.it.p[i].len, g_pins.it.p[i].dir); g_pins.it.free_(); g_pins.pool.free_(); g_pins = n; }
    else if (g_pins.it.n < 12) addPin(path);
    savePins(); View& h = g_v[HOME]; h.sel = -1; invalidate();
}
static void pinSel() {
    Ref r; Lay L = lay(); if (!refAt(V().sel, L, r)) return; wchar_t p[PM]; wcscpy(p, r.l->P(r.i)); togglePin(p);
}

// ── recents (Windows' own Recent list) ──
struct Lnk { uint64_t t; wchar_t name[MAX_PATH]; };
static int cmpLnk(const void* a, const void* b) { uint64_t x = ((const Lnk*)a)->t, y = ((const Lnk*)b)->t; return x < y ? 1 : (x > y ? -1 : 0); }
static DWORD WINAPI recentThread(void*) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wchar_t dir[PM]; SHGetFolderPathW(NULL, CSIDL_RECENT, NULL, 0, dir);
    wchar_t pat[PM]; swprintf(pat, PM, L"%ls\\*.lnk", dir);
    Vec<Lnk> ls; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) { do { Lnk l; l.t = ft64(fd.ftLastWriteTime); wcsncpy(l.name, fd.cFileName, MAX_PATH - 1); l.name[MAX_PATH - 1] = 0; ls.push(l); } while (FindNextFileW(h, &fd) && ls.n < 400); FindClose(h); }
    qsort(ls.p, ls.n, sizeof(Lnk), cmpLnk);
    List* out = (List*)calloc(1, sizeof(List));
    IShellLinkW* psl = 0; IPersistFile* ppf = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl)) && SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
        for (size_t i = 0; i < ls.n && out->it.n < 12; i++) {
            wchar_t lp[PM], tgt[PM]; swprintf(lp, PM, L"%ls\\%ls", dir, ls.p[i].name);
            if (FAILED(ppf->Load(lp, STGM_READ))) continue;
            tgt[0] = 0; if (FAILED(psl->GetPath(tgt, PM, NULL, 0)) || !tgt[0]) continue;
            DWORD a = GetFileAttributesW(tgt); if (a == INVALID_FILE_ATTRIBUTES) continue;
            bool dup = false; for (size_t k = 0; k < out->it.n; k++) if (_wcsicmp(out->P((int)k), tgt) == 0) dup = true;
            if (!dup) out->add(tgt, (int)wcslen(tgt), (a & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
    }
    if (ppf) ppf->Release(); if (psl) psl->Release(); ls.free_(); CoUninitialize();
    PostMessageW(g_hwnd, WM_RECENT, 0, (LPARAM)out); return 0;
}

// ── context menu ──
enum { M_OPEN = 1, M_SHOW, M_EXPL, M_CUT, M_COPY, M_PASTE, M_RENAME, M_DELETE, M_PIN, M_CPATH, M_NEWF, M_PROPS };
static void ctxMenu(int idx, int sx, int sy) {
    Lay L = lay(); Ref r; bool has = refAt(idx, L, r); int md = mode(); wchar_t path[PM]; bool dir = false; path[0] = 0;
    if (has) { wcscpy(path, r.l->P(r.i)); dir = r.l->it.p[r.i].dir != 0; }
    HMENU m = CreatePopupMenu(); bool canPaste = md == FOLDER && IsClipboardFormatAvailable(CF_HDROP);
    if (has) {
        AppendMenuW(m, MF_STRING, M_OPEN, L"Open");
        if (md != FOLDER) AppendMenuW(m, MF_STRING, M_SHOW, L"Show in folder\tCtrl+Enter");
        AppendMenuW(m, MF_STRING, M_EXPL, L"Reveal in File Explorer");
        AppendMenuW(m, MF_SEPARATOR, 0, 0);
        if (md == FOLDER) {
            AppendMenuW(m, MF_STRING, M_CUT, L"Cut\tCtrl+X"); AppendMenuW(m, MF_STRING, M_COPY, L"Copy\tCtrl+C");
            AppendMenuW(m, MF_STRING | (canPaste ? 0 : MF_GRAYED), M_PASTE, L"Paste\tCtrl+V");
            AppendMenuW(m, MF_SEPARATOR, 0, 0);
            AppendMenuW(m, MF_STRING, M_RENAME, L"Rename\tF2"); AppendMenuW(m, MF_STRING, M_DELETE, L"Delete\tDel");
            AppendMenuW(m, MF_SEPARATOR, 0, 0);
        }
        AppendMenuW(m, MF_STRING, M_PIN, pinIndex(path) >= 0 ? L"Unpin from home\tCtrl+P" : L"Pin to home\tCtrl+P");
        AppendMenuW(m, MF_STRING, M_CPATH, L"Copy path");
        AppendMenuW(m, MF_STRING, M_PROPS, L"Properties");
    } else if (md == FOLDER) {
        AppendMenuW(m, MF_STRING, M_NEWF, L"New folder\tCtrl+Shift+N");
        AppendMenuW(m, MF_STRING | (canPaste ? 0 : MF_GRAYED), M_PASTE, L"Paste\tCtrl+V");
    }
    if (!GetMenuItemCount(m)) { DestroyMenu(m); return; }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, g_hwnd, NULL); DestroyMenu(m);
    switch (cmd) {
    case M_OPEN: openPath(path, dir); break;
    case M_SHOW: showInFolder(path); break;
    case M_EXPL: { wchar_t a[PM + 16]; swprintf(a, PM + 16, L"/select,\"%ls\"", path); ShellExecuteW(NULL, L"open", L"explorer.exe", a, NULL, SW_SHOWNORMAL); } break;
    case M_CUT: clipSet(true); break; case M_COPY: clipSet(false); break; case M_PASTE: pasteFiles(); break;
    case M_RENAME: beginRename(idx); break; case M_DELETE: deleteSel(); break;
    case M_PIN: togglePin(path); break; case M_CPATH: copyText(path); break;
    case M_NEWF: newFolder(); break;
    case M_PROPS: { SHELLEXECUTEINFOW sei = {sizeof sei}; sei.fMask = SEE_MASK_INVOKEIDLIST; sei.hwnd = g_hwnd; sei.lpVerb = L"properties"; sei.lpFile = path; sei.nShow = SW_SHOW; ShellExecuteExW(&sei); } break;
    }
}

// ── query handling ──
static bool isPathQuery(const wchar_t* q) { return wcschr(q, L'\\') || wcschr(q, L'/') || (q[0] && q[1] == L':') || q[0] == L'%' || q[0] == L'~'; }
static void onQueryChange() {
    GetWindowTextW(g_edit, g_query, 512); g_hover = -1; g_wantAct = 0;
    View& s = g_v[SEARCH];
    if (g_query[0]) {
        g_pathMode = isPathQuery(g_query);
        if (g_pathMode) {                        // folder autocomplete is instant: do it inline
            InterlockedIncrement(&g_gen); g_results.clear(); g_note[0] = 0; pathSearch(); g_doneGen = g_gen;
            s.sel = (g_results.it.n && !g_pathLeafEmpty) ? 0 : -1; s.scroll = 0;
        } else requestSearch(g_query, false);    // worker thread; the previous list stays up until the new one lands
    } else { InterlockedIncrement(&g_gen); g_results.clear(); g_note[0] = 0; }
    g_tTarget = (g_query[0] || g_dir[0]) ? 1.f : 0.f; startAnim();
    InvalidateRect(g_edit, 0, TRUE); invalidate();
}
static void elevateNow() {
    if (g_elevated) return;
    wchar_t exe[PM]; GetModuleFileNameW(NULL, exe, PM);
    HINSTANCE r = ShellExecuteW(g_hwnd, L"runas", exe, L"--setup", NULL, SW_SHOWNORMAL);   // one UAC prompt; sets up the no-prompt launcher
    if ((INT_PTR)r > 32) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
static void escapeKey() {
    if (g_query[0]) SetWindowTextW(g_edit, L"");
    else if (g_dir[0]) goHome();
}
static void tabComplete() {
    Lay L = lay(); Ref r; if (mode() != SEARCH || !g_pathMode || !refAt(V().sel, L, r)) return;
    wchar_t p[PM]; wcscpy(p, r.l->P(r.i)); if (r.l->it.p[r.i].dir) wcscat(p, L"\\");
    SetWindowTextW(g_edit, p); SendMessageW(g_edit, EM_SETSEL, wcslen(p), wcslen(p));
}
static bool onEditKey(UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    int m = mode(); bool empty = !g_query[0];
    switch (vk) {
    case VK_UP: if (alt) { goUp(); return true; } moveSel(0, shift); return true;
    case VK_DOWN: moveSel(1, shift); return true;
    case VK_PRIOR: moveSel(4, shift); return true;
    case VK_NEXT: moveSel(5, shift); return true;
    case VK_LEFT: if (alt) { histBack(); return true; } if (empty && m == HOME) { moveSel(2, false); return true; } break;
    case VK_RIGHT: if (empty && m == HOME) { moveSel(3, false); return true; } break;
    case VK_HOME: if (m == FOLDER) { setSel(0, shift); return true; } break;
    case VK_END: if (m == FOLDER) { setSel((int)g_folder.it.n - 1, shift); return true; } break;
    case VK_RETURN: if (alt) { Ref r; Lay L = lay(); if (refAt(V().sel, L, r)) { SHELLEXECUTEINFOW sei = {sizeof sei}; sei.fMask = SEE_MASK_INVOKEIDLIST; sei.hwnd = g_hwnd; sei.lpVerb = L"properties"; sei.lpFile = r.l->P(r.i); sei.nShow = SW_SHOW; ShellExecuteExW(&sei); } return true; }
        activate(ctrl); return true;
    case VK_ESCAPE: escapeKey(); return true;
    case VK_TAB: tabComplete(); return true;
    case VK_BACK: if (m == FOLDER) { goUp(); return true; } break;
    case VK_DELETE: if (m == FOLDER) { deleteSel(); return true; } break;
    case VK_F2: if (m == FOLDER) beginRename(g_v[FOLDER].sel); return true;
    case VK_F5: if (m == FOLDER) refreshFolder(NULL); return true;
    case 'A': if (ctrl && m == FOLDER) { Lay L = lay(); clearMarks(); for (size_t i = 0; i < g_marks.n; i++) g_marks.p[i] = 1; g_nmarks = (int)g_marks.n; (void)L; invalidate(); return true; } break;
    case 'C': case 'X': if (ctrl && m == FOLDER) { clipSet(vk == 'X'); return true; } break;
    case 'V': if (ctrl && m == FOLDER && IsClipboardFormatAvailable(CF_HDROP)) { pasteFiles(); return true; } break;
    case 'N': if (ctrl && shift) { newFolder(); return true; } break;
    case 'P': if (ctrl) { pinSel(); return true; } break;
    case 'H': if (ctrl && m == FOLDER) { g_showHidden = !g_showHidden; refreshFolder(NULL); return true; } break;
    case 'E': if (ctrl && shift) { elevateNow(); return true; } break;
    case 'L': case 'K': if (ctrl) { SendMessageW(g_edit, EM_SETSEL, 0, -1); return true; } break;
    }
    return false;
}
static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: if (onEditKey((UINT)w)) return 0; break;
    case WM_CHAR:
        if (w == L'\r' || w == 27 || w == L'\t' || w == L'\n') return 0;
        if (w == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (w == 0x7F) {   // Ctrl+Backspace → delete previous word
            DWORD s, e; SendMessageW(h, EM_GETSEL, (WPARAM)&s, (LPARAM)&e); wchar_t t[512]; GetWindowTextW(h, t, 512); int c = (int)s;
            while (c > 0 && t[c - 1] == L' ') c--; while (c > 0 && t[c - 1] != L' ') c--;
            SendMessageW(h, EM_SETSEL, c, s); SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L""); return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(g_oldEdit, h, m, w, l);
        if (GetWindowTextLengthW(h) == 0) {
            HDC dc = GetDC(h); HGDIOBJ o = SelectObject(dc, g_fontEdit); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, CR(T.text2));
            const wchar_t* ph = L"Search files and folders"; TextOutW(dc, 2, 0, ph, (int)wcslen(ph)); SelectObject(dc, o); ReleaseDC(h, dc);
        }
        return r;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: { LRESULT r = CallWindowProcW(g_oldEdit, h, m, w, l); invalidate(); return r; }
    }
    return CallWindowProcW(g_oldEdit, h, m, w, l);
}
static LRESULT CALLBACK renProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { endRename(true); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { endRename(false); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == L'\n')) return 0;
    return CallWindowProcW(g_oldRen, h, m, w, l);
}

// ───────────────────────────── mouse ─────────────────────────────
static int hitItem(float x, float y, const Lay& L) {
    int md = mode(); View& v = V();
    if (md == HOME) {
        for (int i = 0; i < L.pinShown; i++) {
            int c = i % L.cols, r = i / L.cols; float tx = L.cx + c * (L.tileW + L.gap), ty = L.tileY + r * (L.tileH + L.gap);
            if (x >= tx && x < tx + L.tileW && y >= ty && y < ty + L.tileH) return i;
        }
        for (int j = 0; j < L.recShown; j++) { float ry = L.recY + j * L.recRowH; if (x >= L.cx && x < L.cx + L.cw && y >= ry && y < ry + L.recRowH - 2) return L.pinShown + j; }
        return -1;
    }
    if (y < L.top || y > L.bot || x < L.cx || x > L.cx + L.cw) return -1;
    int i = (int)((y - L.top + v.scroll) / L.rowH); return (i >= 0 && i < itemCount(L)) ? i : -1;
}
static bool hitFolderChrome(float x, float y, const Lay& L) {   // breadcrumb + sortable headers
    if (mode() != FOLDER) return false;
    if (y >= L.crumbY && y < L.crumbY + 28) {
        for (int i = 0; i < g_nsegs; i++) if (x >= g_segs[i].x && x <= g_segs[i].x + g_segs[i].w + 6) {
            if (i == 0) goHome(); else { wchar_t p[PM]; int e = g_segs[i].endIdx; memcpy(p, g_dir, e * 2); p[e] = 0; navigate(p); }
            return true;
        }
        return true;
    }
    if (y >= L.hdrY && y < L.hdrY + 26) {
        float nl, nr, dl, dr, sl, sr; colGeom(L, nl, nr, dl, dr, sl, sr);
        int k = x >= sl ? 2 : (x >= dl ? 1 : 0);
        if (g_sortKey == k) g_sortAsc = !g_sortAsc; else { g_sortKey = k; g_sortAsc = (k == 0); }
        refreshFolder(NULL); return true;
    }
    return false;
}

// ───────────────────────────── window procedure ─────────────────────────────
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h; g_dpi = GetDpiForWindow(h);
        g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 100, 24, h, (HMENU)1, g_inst, 0);
        g_ren = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 100, 24, h, (HMENU)2, g_inst, 0);
        SendMessageW(g_edit, EM_SETLIMITTEXT, 400, 0);
        g_oldEdit = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)editProc);
        g_oldRen = (WNDPROC)SetWindowLongPtrW(g_ren, GWLP_WNDPROC, (LONG_PTR)renProc);
        makeEditFonts(); applyTheme(); layoutEdit(); setWindowIcons(h); SetFocus(g_edit);
        return 0;
    }
    case WM_SIZE: if (g_rt) { D2D1_SIZE_U s = {(UINT32)LOWORD(l), (UINT32)HIWORD(l)}; if (s.width && s.height) g_rt->Resize(&s); }
        { Lay L = lay(); clampScroll(L); } layoutEdit(); placeRename(); invalidate(); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mi = (MINMAXINFO*)l; float S = g_dpi / 96.f; mi->ptMinTrackSize.x = (LONG)(520 * S); mi->ptMinTrackSize.y = (LONG)(420 * S); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); paint(); EndPaint(h, &ps); return 0; }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w); RECT* r = (RECT*)l; SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_rt) g_rt->SetDpi((float)g_dpi, (float)g_dpi); makeEditFonts(); layoutEdit(); setWindowIcons(h); invalidate(); return 0;
    }
    case WM_CTLCOLOREDIT: { HDC dc = (HDC)w; SetTextColor(dc, CR(T.text)); SetBkColor(dc, CR(T.surface)); return (LRESULT)g_brSurface; }
    case WM_COMMAND:
        if ((HWND)l == g_edit && HIWORD(w) == EN_CHANGE) onQueryChange();
        else if ((HWND)l == g_ren && HIWORD(w) == EN_KILLFOCUS) endRename(true);
        return 0;
    case WM_TIMER:
        if (w == 1) {
            float d = g_tTarget - g_t; if (fabsf(d) < 0.01f) { g_t = g_tTarget; KillTimer(h, 1); } else g_t += d * 0.30f;
            layoutEdit(); placeRename(); invalidate();
        } else if (w == 2) { KillTimer(h, 2); InterlockedExchange(&g_fsDirty, 0); if (mode() == FOLDER && !g_renaming) refreshFolder(NULL); }
        else if (w == 4) { HANDLE t = CreateThread(NULL, 0, saveThread, NULL, 0, NULL); if (t) CloseHandle(t); }
        return 0;
    case WM_FSCHG: SetTimer(h, 2, 140, NULL); return 0;
    case WM_IDX: if (mode() != FOLDER) { static DWORD lastRe; DWORD nowT = GetTickCount();
            if (g_query[0] && !g_pathMode && (w == 1 || nowT - lastRe > 400)) { lastRe = nowT; requestSearch(g_query, true); } invalidate(); } return 0;
    case WM_SEARCHDONE: {
        SearchOut* so = (SearchOut*)l;
        if (so->gen == g_gen && g_query[0] && !g_pathMode) {
            int old = g_v[SEARCH].sel; g_results.it.free_(); g_results.pool.free_(); g_results = so->list; memset(&so->list, 0, sizeof so->list);
            wcscpy(g_note, so->note); g_doneGen = so->gen; View& sv = g_v[SEARCH]; int n = (int)g_results.it.n;
            if (so->keep && old >= 0 && old < n) sv.sel = old; else { sv.sel = n ? 0 : -1; sv.scroll = 0; }
            { Lay L = lay(); clampScroll(L); } g_hover = -1; invalidate();
            if (g_wantAct) { bool rv = g_wantAct == 2; g_wantAct = 0; searchOutFree(so); activate(rv); return 0; }
        }
        searchOutFree(so); return 0;
    }
    case WM_RECENT: { List* n = (List*)l; g_recents.it.free_(); g_recents.pool.free_(); g_recents = *n; free(n); invalidate(); return 0; }
    case WM_MOUSEMOVE: {
        float S = g_dpi / 96.f; Lay L = lay(); int i = hitItem(GET_X_LPARAM(l) / S, GET_Y_LPARAM(l) / S, L);
        if (i != g_hover) { g_hover = i; invalidate(); }
        if (!g_tracking) { TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); g_tracking = true; }
        return 0;
    }
    case WM_MOUSELEAVE: g_tracking = false; if (g_hover != -1) { g_hover = -1; invalidate(); } return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        float S = g_dpi / 96.f; float x = GET_X_LPARAM(l) / S, y = GET_Y_LPARAM(l) / S; Lay L = lay(); int md = mode();
        if (g_renaming) endRename(true);
        SetFocus(g_edit);
        if (m == WM_LBUTTONDOWN) {
            float dx = x - (L.cx + L.cw - 32), dy = y - (L.py + L.ph / 2);
            if (g_query[0] && dx * dx + dy * dy <= 14 * 14) { SetWindowTextW(g_edit, L""); return 0; }        // clear
            if (md != HOME && L.cx >= 52 && x >= L.cx - 50 && x <= L.cx - 8 && y >= L.py && y <= L.py + L.ph) { goHome(); return 0; }   // mark = home
        }
        if (m == WM_LBUTTONDOWN && hitFolderChrome(x, y, L)) return 0;
        int i = hitItem(x, y, L);
        if (md == FOLDER) {
            if (i < 0) { if (y >= L.top) { clearMarks(); V().sel = -1; V().anchor = -1; invalidate(); } }
            else if (m == WM_LBUTTONDBLCLK) { setSel(i, false); activate(false); }
            else if (w & MK_CONTROL) toggleMark(i);
            else if (w & MK_SHIFT) setSel(i, true);
            else setSel(i, false);
        } else if (i >= 0 && m == WM_LBUTTONDOWN) { V().sel = i; activate(GetKeyState(VK_CONTROL) < 0); }
        return 0;
    }
    case WM_RBUTTONUP: {
        float S = g_dpi / 96.f; Lay L = lay(); int i = hitItem(GET_X_LPARAM(l) / S, GET_Y_LPARAM(l) / S, L);
        if (g_renaming) endRename(true);
        if (i >= 0) { if (!(mode() == FOLDER && g_nmarks && g_marks.p[i])) { if (mode() == FOLDER) setSel(i, false); else V().sel = i; } }
        else if (mode() == FOLDER && GET_Y_LPARAM(l) / S >= L.top) { clearMarks(); V().sel = -1; }
        invalidate(); POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &pt);
        ctxMenu(i, pt.x, pt.y); SetFocus(g_edit); return 0;
    }
    case WM_MOUSEWHEEL: {
        if (mode() == HOME) return 0; Lay L = lay(); View& v = V(); v.scroll -= (GET_WHEEL_DELTA_WPARAM(w) / 120.f) * L.rowH * 3;
        clampScroll(L); placeRename();
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt); float S = g_dpi / 96.f; g_hover = hitItem(pt.x / S, pt.y / S, L); invalidate(); return 0;
    }
    case WM_XBUTTONDOWN: if (GET_XBUTTON_WPARAM(w) == XBUTTON1) histBack(); return TRUE;
    case WM_SETTINGCHANGE: if (l && lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet") == 0) applyTheme(); return 0;
    case 0x0320 /*WM_DWMCOLORIZATIONCOLORCHANGED*/: applyTheme(); return 0;
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE) { if (!g_renaming) SetFocus(g_edit); if (mode() == FOLDER && !g_renaming) refreshFolder(NULL); } return 0;
    case WM_SETFOCUS: if (!g_renaming) SetFocus(g_edit); return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); saveCache(); DestroyWindow(h); return 0;   // persist the index, then leave
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

#ifdef SEEK_SELFTEST
#include "tests/selftest.inc"
#endif
static DWORD WINAPI taskThread(void*) { taskInstall(); return 0; }
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nShow) {
    g_inst = hi; CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); initFold();
    { wchar_t la[PM]; SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la); swprintf(g_dataDir, PM, L"%ls\\Seek", la); CreateDirectoryW(g_dataDir, NULL); }
    g_elevated = isElevated();
#ifdef SEEK_SELFTEST
    g_elevated = true; selfTestSetup();
#endif
    InitializeCriticalSection(&g_logCs);
    { wchar_t lp[PM]; swprintf(lp, PM, L"%ls\\index.log", g_dataDir); g_logFile = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); }
    ilog("Seek starting (elevated=%d)", (int)g_elevated);
    const wchar_t* cl = GetCommandLineW(); bool setupMode = wcsstr(cl, L"--setup") != 0, noHand = wcsstr(cl, L"--elevated") || wcsstr(cl, L"--local");
    if (!g_elevated && !noHand) {        // launcher task exists → start the elevated copy (no UAC prompt) and step aside
        wchar_t mk[PM]; markerPath(mk);
        if (GetFileAttributesW(mk) != INVALID_FILE_ATTRIBUTES && runHidden(L"schtasks.exe /run /tn Seek", 8000) == 0) return 0;
    }
    InitializeCriticalSection(&g_sq); g_sqEv = CreateEventW(NULL, FALSE, FALSE, NULL);
    swprintf(g_freqFile, PM, L"%ls\\freq.txt", g_dataDir); freqLoad();
    g_ovAdd.reserve(64); g_ovDel.reserve(64); g_pool.reserve(8u << 20); g_ent.reserve(1u << 19);
    DWORD pl = GetEnvironmentVariableW(L"USERPROFILE", g_profile, PM); g_profLen = (pl > 0 && pl < PM) ? (int)pl : 0;
    while (g_profLen > 3 && g_profile[g_profLen - 1] == L'\\') g_profile[--g_profLen] = 0;
    loadPins();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory, NULL, (void**)&g_d2d))) return 1;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) return 1;
    initText();

    WNDCLASSEXW wc = {sizeof wc}; wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"SeekWnd"; wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN); int w = 1000, hgt = 700; if (w > sw - 80) w = sw - 80; if (hgt > sh - 80) hgt = sh - 80;
    HWND hw = CreateWindowExW(0, wc.lpszClassName, APPNAME, WS_OVERLAPPEDWINDOW, (sw - w) / 2, (sh - hgt) / 2, w, hgt, NULL, NULL, hi, NULL);
    if (!hw) return 1;
    // DPI-correct initial size
    { UINT d = GetDpiForWindow(hw); if (d != 96) { RECT r; GetWindowRect(hw, &r); SetWindowPos(hw, NULL, 0, 0, MulDiv(w, d, 96), MulDiv(hgt, d, 96), SWP_NOMOVE | SWP_NOZORDER); } }
    ShowWindow(hw, nShow); UpdateWindow(hw);

    CreateThread(NULL, 0, recentThread, NULL, 0, NULL);
    { HANDLE t = CreateThread(NULL, 0, searchThread, NULL, 0, NULL); if (t) CloseHandle(t); }
    HANDLE st = CreateThread(NULL, 8 << 20, indexThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL); if (st) CloseHandle(st);
#ifdef SEEK_SELFTEST
    { HANDLE t = CreateThread(NULL, 0, selfTestThread, NULL, 0, NULL); if (t) CloseHandle(t); }
#endif
    SetTimer(hw, 4, 15 * 60 * 1000, NULL);                       // periodic index checkpoint
    if (setupMode && g_elevated) { HANDLE t = CreateThread(NULL, 0, taskThread, NULL, 0, NULL); if (t) CloseHandle(t); }

    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}

/* BUILD
   MinGW-w64:
     windres seek.rc -O coff -o seek.res
     g++ -std=c++17 -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -s -static -mwindows ^
         main.cpp seek.res -o Seek.exe -ld2d1 -ldwrite -ldwmapi -lshlwapi -lole32 -luuid -lshell32 -luser32 -lgdi32 -ladvapi32
*/
