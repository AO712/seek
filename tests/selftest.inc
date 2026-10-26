// ───────────── SELF-TEST BUILD ONLY (-DSEEK_SELFTEST): fake NTFS volume + USN journal ─────────────
// Drives the real admin-mode code (mftBuild, publishBlock, journal apply, cache, rescan, search) against
// a synthetic volume whose "real" part also exists on disk so search verification can stat the files.
#ifdef SEEK_SELFTEST
#include <stdarg.h>
#undef DeviceIoControl
#undef CreateFileW
#undef GetVolumeInformationW
#define FT_HANDLE ((HANDLE)(INT_PTR)0x7777)

struct FNode { uint64_t frn, pfrn; wchar_t name[160]; uint32_t attr; bool alive; wchar_t path[300]; };
struct FEv   { int64_t usn; uint64_t frn, pfrn; wchar_t name[160]; uint32_t attr, reason; };
static Vec<FNode> g_fn; static Vec<FEv> g_fev; static int64_t g_fusn = 5000; static uint32_t g_fctr = 0;
static CRITICAL_SECTION g_fcs; static volatile LONG g_ftInject, g_ftPaused;
static FILE* g_tlog; static int g_ftMode;   // 0 normal, 1 = file table returns scrambled parents, 2 = journal always fails
static const uint64_t FT_ROOT = (5ull << 48) | 5;

static int ftCmp(const void* a, const void* b) { uint64_t x = FRN48(((const FNode*)a)->frn), y = FRN48(((const FNode*)b)->frn); return x < y ? -1 : (x > y ? 1 : 0); }
static FNode* ftFind(uint64_t frn) { for (size_t i = 0; i < g_fn.n; i++) if (g_fn.p[i].alive && FRN48(g_fn.p[i].frn) == FRN48(frn)) return &g_fn.p[i]; return 0; }
static uint64_t ftNew(uint64_t pfrn, const wchar_t* name, bool dir, const wchar_t* path) {
    FNode n; memset(&n, 0, sizeof n); uint32_t idx = 100 + (uint32_t)((uint64_t)(g_fctr++) * 40503ull % 1000003ull);       // scrambled: children often precede parents
    n.frn = ((uint64_t)(1 + idx % 7) << 48) | idx; n.pfrn = pfrn; wcsncpy(n.name, name, 159); n.attr = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE; n.alive = true;
    if (path) wcsncpy(n.path, path, 299);
    g_fn.push(n); return n.frn;
}
static uint32_t ftRecord(BYTE* out, uint64_t frn, uint64_t pfrn, const wchar_t* name, uint32_t attr, uint32_t reason, int64_t usn) {
    int nl = (int)wcslen(name); uint32_t len = (60 + nl * 2 + 7) & ~7u; memset(out, 0, len);
    USNREC* r = (USNREC*)out; r->recLen = len; r->major = 2; r->frn = frn; r->pfrn = pfrn; r->usn = usn; r->reason = reason; r->attr = attr;
    r->nameLen = (uint16_t)(nl * 2); r->nameOff = 60; memcpy(out + 60, name, nl * 2); return len;
}
static void ftEvent(uint64_t frn, uint64_t pfrn, const wchar_t* name, uint32_t attr, uint32_t reason) {
    FEv e; memset(&e, 0, sizeof e); e.usn = g_fusn; g_fusn += 100; e.frn = frn; e.pfrn = pfrn; wcsncpy(e.name, name, 159); e.attr = attr; e.reason = reason | 0x80000000u; g_fev.push(e);
}
static void ftTouch(const wchar_t* p) { HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

// operations: change the "real" disk, the fake MFT, and append the matching journal record
static uint64_t ftOpCreate(uint64_t pfrn, const wchar_t* name, bool dir) {
    EnterCriticalSection(&g_fcs); FNode* pn = ftFind(pfrn); wchar_t path[300] = {};
    if (pn && pn->path[0]) { swprintf(path, 300, L"%ls\\%ls", pn->path, name); if (dir) CreateDirectoryW(path, NULL); else ftTouch(path); }
    uint64_t f = ftNew(pfrn, name, dir, path[0] ? path : NULL); ftEvent(f, pfrn, name, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE, R_CREATE);
    LeaveCriticalSection(&g_fcs); return f;
}
static void ftFixPaths(FNode* n, const wchar_t* oldp, const wchar_t* newp) {            // re-root recorded real paths under a renamed/moved dir
    size_t ol = wcslen(oldp);
    for (size_t i = 0; i < g_fn.n; i++) { FNode& c = g_fn.p[i]; if (c.alive && c.path[0] && &c != n && _wcsnicmp(c.path, oldp, ol) == 0 && c.path[ol] == L'\\') { wchar_t t[300]; swprintf(t, 300, L"%ls%ls", newp, c.path + ol); wcsncpy(c.path, t, 299); } }
}
static void ftOpRename(uint64_t frn, uint64_t newParent, const wchar_t* newName) {
    EnterCriticalSection(&g_fcs); FNode* n = ftFind(frn); FNode* np = ftFind(newParent);
    if (n && n->path[0]) {
        wchar_t dst[300];
        if (np && np->path[0]) swprintf(dst, 300, L"%ls\\%ls", np->path, newName); else swprintf(dst, 300, L"C:\\Users\\tester\\_trash_%ls", newName);
        BOOL mv = MoveFileW(n->path, dst); DWORD mvErr = GetLastError();
        if (g_tlog) { fprintf(g_tlog, "DISK  move %ls -> %ls : %s (err %lu)\n", n->path, dst, mv ? "ok" : "FAILED", mv ? 0 : mvErr); fflush(g_tlog); }
        if (np && np->path[0]) { if (mv) { ftFixPaths(n, n->path, dst); wcsncpy(n->path, dst, 299); } } else { n->path[0] = 0; }
    }
    if (n) { n->pfrn = newParent; wcsncpy(n->name, newName, 159); ftEvent(n->frn, newParent, newName, n->attr, R_REN_OLD | R_REN_NEW); }
    LeaveCriticalSection(&g_fcs);
}
static void ftOpDelete(uint64_t frn) {
    EnterCriticalSection(&g_fcs); FNode* n = ftFind(frn);
    if (n) { if (n->path[0]) { if (n->attr & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(n->path); else DeleteFileW(n->path); } n->alive = false; ftEvent(n->frn, n->pfrn, n->name, n->attr, R_DELETE); }
    LeaveCriticalSection(&g_fcs);
}

static BOOL WINAPI ft_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inN, LPVOID out, DWORD outN, LPDWORD ret, LPOVERLAPPED ov) {
    if (h != FT_HANDLE) return DeviceIoControl(h, code, in, inN, out, outN, ret, ov);
    if (code == FSCTL_QUERY_USN_JOURNAL) {
        if (outN < sizeof(USNJD)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        USNJD* j = (USNJD*)out; memset(j, 0, sizeof *j); j->id = 0xABCDEF123ull; j->first = 0; EnterCriticalSection(&g_fcs); j->next = g_fusn; LeaveCriticalSection(&g_fcs); *ret = sizeof *j; return TRUE;
    }
    if (code == FSCTL_CREATE_USN_JOURNAL) return TRUE;
    if (code == FSCTL_ENUM_USN_DATA) {
        MFTENUM* m = (MFTENUM*)in; uint64_t start = FRN48(m->start); BYTE* o = (BYTE*)out; size_t pos = 8; uint64_t last = 0; bool any = false;
        EnterCriticalSection(&g_fcs); qsort(g_fn.p, g_fn.n, sizeof(FNode), ftCmp);
        for (size_t i = 0; i < g_fn.n; i++) {
            FNode& n = g_fn.p[i]; if (!n.alive || FRN48(n.frn) < start) continue;
            uint32_t need = (60 + (uint32_t)wcslen(n.name) * 2 + 7) & ~7u; if (pos + need > outN) break;
            pos += ftRecord(o + pos, n.frn, (g_ftMode == 1 && FRN48(n.pfrn) != 5) ? n.pfrn + 0x1000 : n.pfrn, n.name, n.attr, 0, 1); last = FRN48(n.frn); any = true;
        }
        LeaveCriticalSection(&g_fcs);
        if (!any) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        *(uint64_t*)o = last + 1; *ret = (DWORD)pos; return TRUE;
    }
    if (code == FSCTL_READ_USN_JOURNAL) {
        READUSN* r = (READUSN*)in;
        for (;;) {
            if (g_ftMode == 2) { Sleep(30); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
            if (InterlockedExchange(&g_ftInject, 0)) { SetLastError(ERROR_JOURNAL_ENTRY_DELETED); return FALSE; }
            EnterCriticalSection(&g_fcs); size_t pos = 8; bool any = false; int64_t nextUsn = g_fusn;
            if (!g_ftPaused) {
                for (size_t i = 0; i < g_fev.n; i++) {
                    FEv& e = g_fev.p[i]; if (e.usn < r->start) continue;
                    uint32_t need = (60 + (uint32_t)wcslen(e.name) * 2 + 7) & ~7u; if (pos + need > outN) { nextUsn = e.usn; break; }
                    pos += ftRecord((BYTE*)out + pos, e.frn, e.pfrn, e.name, e.attr, e.reason, e.usn); any = true;
                }
            }
            LeaveCriticalSection(&g_fcs);
            if (any) { *(int64_t*)out = nextUsn; *ret = (DWORD)pos; return TRUE; }
            Sleep(20);
        }
    }
    SetLastError(ERROR_INVALID_FUNCTION); return FALSE;
}
static HANDLE WINAPI ft_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
    if (n && n[0] == L'\\' && n[1] == L'\\' && n[2] == L'.' && (n[4] == L'C' || n[4] == L'c')) return FT_HANDLE;
    return CreateFileW(n, a, s, sa, d, f, t);
}
static BOOL WINAPI ft_GetVolumeInformationW(LPCWSTR root, LPWSTR vn, DWORD vnN, LPDWORD ser, LPDWORD mcl, LPDWORD fl, LPWSTR fs, DWORD fsN) {
    if (root && (root[0] == L'C' || root[0] == L'c')) { if (ser) *ser = 0x5EEC; if (fs) wcsncpy(fs, L"NTFS", fsN); return TRUE; }
    if (fs) wcsncpy(fs, L"FAT32", fsN); return TRUE;
}

// ── test driver ──
static int g_tfails;
static void stLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(g_tlog, fmt, ap); va_end(ap); fputc('\n', g_tlog); fflush(g_tlog); }
#define CK(c, msg) do { if (c) stLog("PASS  %s", msg); else { stLog("FAIL  %s", msg); g_tfails++; } } while (0)
static SearchOut* stSearch(const wchar_t* q) { SearchOut* so = (SearchOut*)calloc(1, sizeof *so); searchRun(q, g_gen, so); return so; }
static bool stHas(const wchar_t* q, const wchar_t* path) {
    SearchOut* so = stSearch(q); bool f = false; for (size_t i = 0; i < so->list.it.n; i++) if (_wcsicmp(so->list.P((int)i), path) == 0) f = true; searchOutFree(so); return f;
}
static int stCount(const wchar_t* q) { SearchOut* so = stSearch(q); int n = (int)so->list.it.n; searchOutFree(so); return n; }
static void stLive(int vol, size_t* cnt, uint64_t* hash) {                      // count + order-independent hash of every live path on a volume
    *cnt = 0; *hash = 0; AcquireSRWLockShared(&g_lock); wchar_t p[PM];
    for (size_t i = 0; i < g_ent.n; i++) if (g_ent.p[i].vol == vol && !(g_ent.p[i].flags & EF_DEL)) { buildPath((uint32_t)i, p); *hash += pathHash(p); (*cnt)++; }
    ReleaseSRWLockShared(&g_lock);
}
static FNode* ftBsearch(uint64_t frn) {                                   // g_fn must be sorted by FRN index
    size_t lo = 0, hi = g_fn.n; uint64_t k = FRN48(frn);
    while (lo < hi) { size_t m = (lo + hi) / 2; uint64_t v = FRN48(g_fn.p[m].frn); if (v < k) lo = m + 1; else if (v > k) hi = m; else return g_fn.p[m].alive ? &g_fn.p[m] : 0; }
    return 0;
}
static bool stExcluded(const FNode* n) {                                        // independent re-implementation of the exclusion rule
    for (int guard = 0; guard < 100 && n; guard++) {
        if ((n->attr & FILE_ATTRIBUTE_DIRECTORY) && (isSkipName(n->name) || (FRN48(n->pfrn) == 5 && !lstrcmpiW(n->name, L"Windows")))) return true;
        if (FRN48(n->pfrn) == 5) return false;
        if (FRN48(n->pfrn) < 24) return true;
        n = ftBsearch(n->pfrn);
    }
    return true;
}
static size_t stExpectedLive() {
    EnterCriticalSection(&g_fcs); qsort(g_fn.p, g_fn.n, sizeof(FNode), ftCmp);
    size_t c = 1; for (size_t i = 0; i < g_fn.n; i++) if (g_fn.p[i].alive && FRN48(g_fn.p[i].frn) >= 24 && !stExcluded(&g_fn.p[i])) c++;
    LeaveCriticalSection(&g_fcs); return c;
}

static bool stLogHas(const char* needle) {                                    // does index.log mention this?
    HANDLE f = CreateFileW(L"C:\\seekdata\\index.log", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) return false;
    static char b[65536]; DWORD n = 0; ReadFile(f, b, sizeof b - 1, &n, 0); b[n] = 0; CloseHandle(f); return strstr(b, needle) != 0;
}
static DWORD WINAPI selfTestThread(void*) {
    g_tlog = fopen("C:\\selftest.log", "w");
    wchar_t mode[16] = {}; GetEnvironmentVariableW(L"SEEK_FT_MODE", mode, 16);
    if (!wcscmp(mode, L"break")) {            // scenario: real Windows returns something the parser misreads → must fall back, not index nothing
        for (int i = 0; i < 900 && g_scanning; i++) Sleep(100);
        CK(!g_scanning, "index finished");
        CK(g_journalVols == 0, "bad file table was rejected by verification");
        CK(stLogHas("failed verification"), "log explains the fallback");
        CK(stHas(L"report", L"C:\\Users\\tester\\Documents\\Report 2024.xlsx"), "walker fallback still indexes: file found");
        CK(stHas(L"notes", L"C:\\Users\\tester\\Documents\\notes.txt"), "walker fallback still indexes: second file found");
        CK(g_count > 5, "item counter reflects the walker index");
        stLog(g_tfails ? "RESULT: %d FAILURES" : "RESULT: ALL PASSED", g_tfails); fclose(g_tlog); ExitProcess(g_tfails ? 1 : 0);
    }
    if (!wcscmp(mode, L"journalfail")) {       // scenario: journal reads always fail → bounded retries, then plain change watcher
        for (int i = 0; i < 900 && g_scanning; i++) Sleep(100);
        CK(g_journalVols == 1, "index built from file table");
        size_t n0 = g_ent.n; Sleep(12000); size_t n1 = g_ent.n;
        stLog("INFO  entries at 0s: %zu, after 12s of failing journal: %zu", n0, n1);
        CK(n1 <= n0 + 3 * 200300, "rescans are capped (at most 3 extra copies)");
        Sleep(6000); CK(g_ent.n == n1, "no further rescans after giving up");
        CK(stLogHas("giving up on live updates"), "log says the journal was abandoned");
        CK(stHas(L"report", L"C:\\Users\\tester\\Documents\\Report 2024.xlsx"), "search still works");
        stLog(g_tfails ? "RESULT: %d FAILURES" : "RESULT: ALL PASSED", g_tfails); fclose(g_tlog); ExitProcess(g_tfails ? 1 : 0);
    }
    for (int i = 0; i < 600 && g_scanning; i++) Sleep(100);
    CK(!g_scanning, "index finished");
    CK(g_journalVols == 1, "NTFS volume took the MFT path (not the walker fallback)");
    size_t cnt; uint64_t hh; stLive(2, &cnt, &hh);
    stLog("INFO  live entries after MFT build: %zu (expected %zu), total entries %zu, pool chars %zu", cnt, stExpectedLive(), g_ent.n, g_pool.n);
    CK(cnt == stExpectedLive(), "MFT build kept exactly the non-excluded records");
    {   EnterCriticalSection(&g_fcs); qsort(g_fn.p, g_fn.n, sizeof(FNode), ftCmp); size_t dup = 0, sys = 0, alive = 0, exc = 0, inc = 0;
        for (size_t i = 0; i < g_fn.n; i++) { FNode& n = g_fn.p[i]; if (!n.alive) continue; alive++; if (FRN48(n.frn) < 24) { sys++; continue; } if (i && FRN48(g_fn.p[i-1].frn) == FRN48(n.frn)) dup++; if (stExcluded(&n)) exc++; else inc++; }
        LeaveCriticalSection(&g_fcs);
        stLog("INFO  fake nodes: total=%zu alive=%zu system(<24)=%zu duplicateFRN=%zu excluded=%zu included=%zu", g_fn.n, alive, sys, dup, exc, inc); }
    {   int shown = 0; EnterCriticalSection(&g_fcs); AcquireSRWLockShared(&g_lock);
        for (size_t i = 0; i < g_fn.n && shown < 12; i++) { FNode& n = g_fn.p[i]; if (!n.alive || FRN48(n.frn) < 24 || stExcluded(&n)) continue;
            if (!hGet(g_vol[2].map, FRN48(n.frn), NULL)) { stLog("INFO  MISSING from index: frn=%llx idx=%llu parent=%llx name=%ls", (unsigned long long)n.frn, (unsigned long long)FRN48(n.frn), (unsigned long long)n.pfrn, n.name); shown++; } }
        ReleaseSRWLockShared(&g_lock); LeaveCriticalSection(&g_fcs); }
    CK(g_count == (LONG)cnt, "status counter matches live entries");

    CK(stHas(L"report", L"C:\\Users\\tester\\Documents\\Report 2024.xlsx"), "search: plain word finds file");
    SearchOut* so = stSearch(L"report"); CK(so->list.it.n > 0 && _wcsicmp(so->list.P(0), L"C:\\Users\\tester\\Documents\\Report 2024.xlsx") == 0, "search: best hit first"); searchOutFree(so);
    CK(stCount(L"winlogon") == 0, "excluded: C:\\Windows is not indexed");
    CK(stCount(L"left-pad") == 0 && stCount(L"index.js") == 0, "excluded: node_modules not indexed");
    CK(stCount(L"recycle") == 0, "excluded: $Recycle.Bin not indexed");
    CK(stCount(L"$MFT") == 0 && stCount(L"UsnJrnl") == 0, "NTFS metafiles not indexed");
    CK(stHas(L"ext:txt in:documents", L"C:\\Users\\tester\\Documents\\notes.txt"), "operators: ext + in");
    CK(stHas(L"rprt24", L"C:\\Users\\tester\\Documents\\Report 2024.xlsx"), "fuzzy: rprt24 -> Report 2024");
    CK(stHas(L"\"2023 return\"", L"C:\\Users\\tester\\Documents\\Taxes\\2023 return.pdf"), "phrase search in nested folder");
    CK(stHas(L"holiday -draft", L"C:\\Users\\tester\\Pictures\\holiday photo.jpg"), "negation keeps non-matching");
    CK(stCount(L"holiday -photo") == 0, "negation excludes");

    // ── live journal: create / rename (grow + shrink) / delete / move-to-excluded / dir renames ──
    uint64_t fDocs = 0, fDown = 0, fRep = 0, fNotes = 0, fSetup = 0, fTester = 0, fWin = 0;
    EnterCriticalSection(&g_fcs);
    for (size_t i = 0; i < g_fn.n; i++) { FNode& n = g_fn.p[i]; if (!n.alive) continue;
        if (!wcscmp(n.name, L"Documents")) fDocs = n.frn; else if (!wcscmp(n.name, L"Downloads")) fDown = n.frn; else if (!wcscmp(n.name, L"Report 2024.xlsx")) fRep = n.frn;
        else if (!wcscmp(n.name, L"notes.txt")) fNotes = n.frn; else if (!wcscmp(n.name, L"setup.exe")) fSetup = n.frn; else if (!wcscmp(n.name, L"tester")) fTester = n.frn;
        else if (!wcscmp(n.name, L"Windows")) fWin = n.frn; }
    LeaveCriticalSection(&g_fcs);
    ftOpCreate(fDocs, L"created_after.txt", false);
    ftOpRename(fRep, fDocs, L"Report 2025 final version.xlsx");        // longer name → new entry
    ftOpRename(fNotes, fDocs, L"n.txt");                                // shorter → in place
    ftOpDelete(fSetup);
    Sleep(1500);
    CK(stHas(L"created_after", L"C:\\Users\\tester\\Documents\\created_after.txt"), "journal: new file appears");
    CK(stHas(L"final version", L"C:\\Users\\tester\\Documents\\Report 2025 final version.xlsx"), "journal: rename to longer name");
    CK(stCount(L"report 2024") == 0 || !stHas(L"report 2024", L"C:\\Users\\tester\\Documents\\Report 2024.xlsx"), "journal: old name gone");
    CK(stHas(L"n.txt", L"C:\\Users\\tester\\Documents\\n.txt"), "journal: rename to shorter name (in place)");
    CK(!stHas(L"notes", L"C:\\Users\\tester\\Documents\\notes.txt"), "journal: old short-rename name gone");
    CK(stCount(L"setup.exe") == 0, "journal: delete removes entry");
    ftOpRename(fDocs, fTester, L"Docs");                                // shrink dir name: children follow
    ftOpRename(fDown, fTester, L"Downloads archive 2024");              // grow dir name: children re-parented
    Sleep(1500);
    {   SearchOut* d = stSearch(L"created_after"); stLog("INFO  search 'created_after' -> %zu results", d->list.it.n);
        for (size_t i = 0; i < d->list.it.n; i++) stLog("INFO    result: %ls", d->list.P((int)i)); searchOutFree(d);
        stLog("INFO  on disk: Docs\\created_after.txt=%d  Documents\\created_after.txt=%d  Downloads archive...=%d",
              GetFileAttributesW(L"C:\\Users\\tester\\Docs\\created_after.txt") != INVALID_FILE_ATTRIBUTES, GetFileAttributesW(L"C:\\Users\\tester\\Documents\\created_after.txt") != INVALID_FILE_ATTRIBUTES,
              GetFileAttributesW(L"C:\\Users\\tester\\Downloads archive 2024") != INVALID_FILE_ATTRIBUTES); }
    CK(stHas(L"created_after", L"C:\\Users\\tester\\Docs\\created_after.txt"), "journal: dir renamed (shorter) -> children paths follow");
    CK(stHas(L"final version", L"C:\\Users\\tester\\Docs\\Report 2025 final version.xlsx"), "journal: dir renamed -> renamed child follows");
    CK(stCount(L"photo") > 0, "unrelated entries intact after renames");
    stLive(2, &cnt, &hh);
    CK(cnt == stExpectedLive(), "live count equals truth after journal ops");

    // ── cache round-trip + replay of changes made while "closed" ──
    saveCache();
    cacheRead(); CK(g_cache != 0, "cache file written and read back");
    Vol vtmp = g_vol[2]; USNJD jd; Block b; bool took = false; int64_t savedNext = 0;
    if (g_cache && volQuery(&vtmp, &jd)) { took = cacheTake(&vtmp, jd, b); savedNext = vtmp.next; }
    CK(took, "cache accepted (serial / journal id / USN range valid)");
    size_t bc = 0; uint64_t bh = 0;
    for (size_t i = 0; i < b.ent.n; i++) {                              // path hash of the cached block
        if (b.ent.p[i].flags & EF_DEL) continue; wchar_t tmp[PM]; int stk[400], n = 0; for (int32_t k = (int32_t)i; k >= 0 && n < 400; k = b.ent.p[k].parent) stk[n++] = k;
        int pos = 0; for (int k = n - 1; k >= 0; --k) { const Entry& e = b.ent.p[stk[k]]; if (pos > 0 && tmp[pos - 1] != L'\\') tmp[pos++] = L'\\'; memcpy(tmp + pos, b.pool.p + e.off, e.len * 2); pos += e.len; }
        tmp[pos] = 0; bh += pathHash(tmp); bc++;
    }
    stLive(2, &cnt, &hh);
    CK(bc == cnt && bh == hh, "cached block == live index (every path identical)");
    cacheFree();
    // offline period: changes happen, journal records pile up, then a "restart" loads the cache and replays
    uint64_t fPics = 0, fCreated = 0; EnterCriticalSection(&g_fcs);
    for (size_t i = 0; i < g_fn.n; i++) { FNode& n = g_fn.p[i]; if (!n.alive) continue; if (!wcscmp(n.name, L"Pictures")) fPics = n.frn; else if (!wcscmp(n.name, L"created_after.txt")) fCreated = n.frn; }
    LeaveCriticalSection(&g_fcs);
    InterlockedExchange(&g_ftPaused, 1);                                // "Seek is closed": journal piles up unseen by the live index
    ftOpCreate(fDocs, L"offline_made.txt", false);
    ftOpRename(fPics, fTester, L"Pictures and more");
    ftOpDelete(fNotes);
    ftOpRename(fCreated, fWin, L"created_after.txt");                   // moved into an excluded folder
    Sleep(200);
    Vol v2; memset(&v2, 0, sizeof v2); v2.letter = L'D'; v2.id = 3; v2.rootIdx = -1; v2.journal = true;
    publishBlock(&v2, b); blockFree(b);
    {   // replay everything newer than the cache's USN into the second copy
        BYTE* buf = (BYTE*)malloc(1 << 20); size_t pos = 8; EnterCriticalSection(&g_fcs);
        for (size_t i = 0; i < g_fev.n; i++) if (g_fev.p[i].usn >= savedNext) { FEv& e = g_fev.p[i]; pos += ftRecord(buf + pos, e.frn, e.pfrn, e.name, e.attr, e.reason, e.usn); }
        *(int64_t*)buf = g_fusn; LeaveCriticalSection(&g_fcs);
        stLog("INFO  replaying %zu bytes of journal newer than cache USN %lld", pos - 8, (long long)savedNext);
        applyBatch(&v2, buf, (DWORD)pos); free(buf);
    }
    InterlockedExchange(&g_ftPaused, 0); Sleep(1500);                   // live copy catches up now
    size_t c2; uint64_t h2; stLive(3, &c2, &h2); stLive(2, &cnt, &hh);
    stLog("INFO  live copy: %zu entries, cache+replay copy: %zu entries", cnt, c2);
    CK(c2 == cnt && h2 == hh, "cache + journal replay reproduces the live index exactly");
    CK(stHas(L"offline_made", L"C:\\Users\\tester\\Docs\\offline_made.txt"), "replay: file created while closed is found");
    CK(stCount(L"created_after") == 0, "replay: file moved into excluded folder disappears");

    // ── journal wrap: forces a full rescan; must not duplicate or lose anything ──
    size_t before = g_ent.n; stLive(2, &cnt, &hh); uint64_t hh0 = hh; size_t cnt0 = cnt;
    InterlockedExchange(&g_ftInject, 1); ftOpCreate(fTester, L"poke.txt", false); Sleep(4000);
    size_t cnt1; uint64_t hh1; stLive(2, &cnt1, &hh1);
    stLog("INFO  rescan: entries %zu -> %zu (live %zu -> %zu)", before, g_ent.n, cnt0, cnt1);
    CK(cnt1 == stExpectedLive(), "after rescan live count equals truth");
    CK(g_ent.n < before + cnt0 + 50, "rescan appended one fresh copy only (no runaway growth)");
    CK(stHas(L"poke", L"C:\\Users\\tester\\Poke.txt") || stHas(L"poke", L"C:\\Users\\tester\\poke.txt"), "search works after rescan");
    Sleep(3000);
    stLog("INFO  entries after idle: %zu (must not keep growing)", g_ent.n);
    CK(g_ent.n < before + cnt0 + 100, "no repeated rescans while idle");

    stLog(g_tfails ? "RESULT: %d FAILURES" : "RESULT: ALL PASSED", g_tfails); fclose(g_tlog); ExitProcess(g_tfails ? 1 : 0);
    return 0;
}
static void selfTestSetup() {
    { wchar_t m[16] = {}; GetEnvironmentVariableW(L"SEEK_FT_MODE", m, 16); g_ftMode = !wcscmp(m, L"break") ? 1 : (!wcscmp(m, L"journalfail") ? 2 : 0); }                                           // build the synthetic volume (+ the real files it mirrors)
    InitializeCriticalSection(&g_fcs);
    CreateDirectoryW(L"C:\\Users", NULL); CreateDirectoryW(L"C:\\Users\\tester", NULL); CreateDirectoryW(L"C:\\seekdata", NULL);
    wcscpy(g_dataDir, L"C:\\seekdata"); DeleteFileW(L"C:\\seekdata\\index.bin");
    uint64_t users = ftNew(FT_ROOT, L"Users", true, L"C:\\Users"), me = ftNew(users, L"tester", true, L"C:\\Users\\tester");
    struct { const wchar_t* d; uint64_t* f; } dirs[] = {{L"Documents", 0}, {L"Downloads", 0}, {L"Pictures", 0}, {L"proj", 0}};
    uint64_t docs = 0, down = 0, pics = 0, proj = 0;
    for (int i = 0; i < 4; i++) { wchar_t p[300]; swprintf(p, 300, L"C:\\Users\\tester\\%ls", dirs[i].d); CreateDirectoryW(p, NULL); uint64_t f = ftNew(me, dirs[i].d, true, p); if (i == 0) docs = f; else if (i == 1) down = f; else if (i == 2) pics = f; else proj = f; }
    ftOpCreate(docs, L"Report 2024.xlsx", false); ftOpCreate(docs, L"notes.txt", false);
    uint64_t taxes = ftOpCreate(docs, L"Taxes", true); ftOpCreate(taxes, L"2023 return.pdf", false);
    ftOpCreate(down, L"setup.exe", false); ftOpCreate(pics, L"holiday photo.jpg", false);
    uint64_t src = ftOpCreate(proj, L"src", true); ftOpCreate(src, L"main.c", false);
    uint64_t nm = ftNew(proj, L"node_modules", true, NULL), lp = ftNew(nm, L"left-pad", true, NULL); ftNew(lp, L"index.js", false, NULL);
    uint64_t win = ftNew(FT_ROOT, L"Windows", true, NULL), s32 = ftNew(win, L"System32", true, NULL); ftNew(s32, L"winlogon.exe", false, NULL);
    uint64_t rb = ftNew(FT_ROOT, L"$Recycle.Bin", true, NULL); ftNew(rb, L"recycled thing.txt", false, NULL);
    ftNew(FT_ROOT, L"Program Files", true, NULL);
    // NTFS metafiles (FRN < 24) and children of $Extend (parent never enumerated)
    { FNode n; memset(&n, 0, sizeof n); n.frn = (1ull << 48) | 0; n.pfrn = FT_ROOT; wcscpy(n.name, L"$MFT"); n.alive = true; g_fn.push(n);
      FNode e; memset(&e, 0, sizeof e); e.frn = (11ull << 48) | 11; e.pfrn = FT_ROOT; wcscpy(e.name, L"$Extend"); e.attr = FILE_ATTRIBUTE_DIRECTORY; e.alive = true; g_fn.push(e);
      FNode u; memset(&u, 0, sizeof u); u.frn = (3ull << 48) | 27; u.pfrn = e.frn; wcscpy(u.name, L"$UsnJrnl"); u.alive = true; g_fn.push(u);
      FNode r; memset(&r, 0, sizeof r); r.frn = FT_ROOT; r.pfrn = FT_ROOT; wcscpy(r.name, L"."); r.attr = FILE_ATTRIBUTE_DIRECTORY; r.alive = true; g_fn.push(r); }
    // bulk: 200 dirs x 1000 files (not on disk) to exercise paging and speed
    uint64_t bulk = ftNew(me, L"bulk", true, NULL);
    for (int d = 0; d < 200; d++) { wchar_t dn[32]; swprintf(dn, 32, L"dir%03d", d); uint64_t df = ftNew(bulk, dn, true, NULL); for (int f = 0; f < 1000; f++) { wchar_t fnm[48]; swprintf(fnm, 48, L"data_%03d_%04d.dat", d, f); ftNew(df, fnm, false, NULL); } }
    g_fev.n = 0;   // setup operations are part of the initial "disk state", not journal events
}
#endif
#ifdef SEEK_SELFTEST
static bool ft_dirFrn(const wchar_t* path, uint64_t* frn) {                     // resolve a directory path to its fake file reference
    bool ok = false; EnterCriticalSection(&g_fcs);
    for (size_t i = 0; i < g_fn.n; i++) if (g_fn.p[i].alive && g_fn.p[i].path[0] && _wcsicmp(g_fn.p[i].path, path) == 0) { *frn = FRN48(g_fn.p[i].frn); ok = true; break; }
    LeaveCriticalSection(&g_fcs); return ok;
}
#endif
