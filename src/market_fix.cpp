// market_fix.dll -- prevents the bogus monthly market update that runs right
// after a save is loaded.
//
// Root cause (verified with market_probe v10):
//   Loading a save reads the market object (READ_bought fires, container is
//   empty at that moment), and immediately afterwards the game runs
//   FUN_1408F50A0 (MONTHLY_UPDATE). That function folds every open
//   monthly_trades order into resources_bought and bumps
//   internal_market_fluctuations -- as if the month had ticked -- but no
//   resources are transferred. Result: market prices drift on every load.
//
// Fix:
//   Remember "a load just happened" when the market reader runs, and swallow
//   the next MONTHLY_UPDATE call. Normal month ticks are unaffected because
//   the flag is only armed by the load-time reader.
//
// Implementation notes:
//   * Only call-site rel32 bytes are rewritten; no function prologue is
//     touched, so callee stack frames stay intact.
//   * The suppressed call is skipped entirely (the stub jumps straight to the
//     instruction after the call instead of into the target function).
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>

static uint64_t g_base = 0;
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static inline uint64_t T() { return GetTickCount64(); }

// armed by the market reader during load, consumed by the next monthly update
static volatile LONG g_loadArmed = 0;
static volatile LONG g_suppressed = 0;
static volatile LONG g_passed = 0;

static bool SafeReadable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uintptr_t s = (uintptr_t)p, e = s + n;
    return e <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// ---------------------------------------------------------------- stubs
// A "probe" stub logs and then tail-jumps into the real target.
// A "guard" stub logs, decides whether to run the target or skip it.
// Both preserve every caller-saved register.
typedef void (*LogFn)(void* a1, void* a2, int site);
static LogFn g_logFn = nullptr;

extern "C" void __fastcall LogProbe(void* a1, void* a2, int site) {
    if (site == 3) {                      // market reader during load
        LONG n = InterlockedIncrement(&g_loadArmed);
        Log("[%8llu] read arm=%ld container=%p", (unsigned long long)T(), n, a2);
        (void)a1;
    } else {
        Log("[%8llu] probe site=%d a1=%p a2=%p", (unsigned long long)T(), site, a1, a2);
    }
}

// returns 1 = run the real function, 0 = skip it
extern "C" int __fastcall LogGuard(void* a1, void* a2, int site) {
    LONG armed = g_loadArmed;
    if (armed > 0) {
        InterlockedDecrement(&g_loadArmed);
        LONG n = InterlockedIncrement(&g_suppressed);
        Log("[%8llu] SUPPRESS monthly-update #%ld (armed=%ld) market=%p",
            (unsigned long long)T(), n, armed, a1);
        (void)a2;
        return 0;
    }
    LONG n = InterlockedIncrement(&g_passed);
    Log("[%8llu] pass monthly-update #%ld market=%p",
        (unsigned long long)T(), n, a1);
    (void)a2;
    return 1;
}

typedef int (*GuardFn)(void*, void*, int);
static GuardFn g_guardFn = nullptr;

static uint8_t* AllocNear(uintptr_t hint, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t bp = hint & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000ULL; d += gran) {
        uintptr_t c[2] = { bp + d, bp - d };
        for (int i = 0; i < 2; i++) {
            if (c[i] < 0x10000) continue;
            void* p = VirtualAlloc((void*)c[i], size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

// Build a stub that either tail-calls target or (when the guard says no)
// just returns to the original caller.
//
// On entry:  [rsp] = return address, rcx/rdx/r8/r9 = args
// We spill the arg regs, call the C callback, then:
//   run  -> restore regs, jmp target
//   skip -> restore regs, ret   (returns straight to the caller of the call site)
static uint8_t* BuildStub(uint8_t* mem, void* callback, uint64_t targetVA,
                          int siteId, bool guarded) {
    uint8_t* p = mem;
    auto E = [&](uint8_t b) { *p++ = b; };

    E(0x51); E(0x52); E(0x41); E(0x50); E(0x41); E(0x51);   // push rcx,rdx,r8,r9
    E(0x48); E(0x83); E(0xEC); E(0x28);                      // sub rsp,0x28
    E(0x48); E(0x8B); E(0x4C); E(0x24); E(0x40);             // rcx = [rsp+0x40]
    E(0x48); E(0x8B); E(0x54); E(0x24); E(0x38);             // rdx = [rsp+0x38]
    E(0x41); E(0xB8); { uint32_t v = (uint32_t)siteId; memcpy(p, &v, 4); p += 4; }
    E(0x48); E(0xB8); { uint64_t f = (uint64_t)callback; memcpy(p, &f, 8); p += 8; }
    E(0xFF); E(0xD0);                                        // call rax
    E(0x89); E(0xC6);                                        // mov esi, eax  (keep decision)

    E(0x48); E(0x83); E(0xC4); E(0x28);                      // add rsp,0x28
    E(0x41); E(0x59); E(0x41); E(0x58); E(0x5A); E(0x59);    // pop r9,r8,rdx,rcx

    if (guarded) {
        E(0x85); E(0xF6);                                    // test esi,esi
        E(0x74); E(0x0C);                                    // je +0x0C  (skip -> ret)
    }
    E(0x48); E(0xB8); { uint64_t t = targetVA; memcpy(p, &t, 8); p += 8; }
    E(0xFF); E(0xE0);                                        // jmp rax
    if (guarded) {
        E(0xC3);                                             // ret (skip the call)
    }
    return p;
}

static bool HookSite(uint64_t siteRva, uint64_t targetRva, int siteId,
                     const char* name, bool guarded) {
    uint8_t* site = (uint8_t*)(g_base + siteRva);
    uint8_t* target = (uint8_t*)(g_base + targetRva);
    if (site[0] != 0xE8) {
        Log("[%s] site 0x%llX is not E8 (0x%02X)", name,
            (unsigned long long)siteRva, site[0]);
        return false;
    }
    int32_t cur = *(int32_t*)(site + 1);
    uint64_t calc = siteRva + 5 + cur;
    if (calc != targetRva) {
        Log("[%s] site 0x%llX targets 0x%llX, expected 0x%llX", name,
            (unsigned long long)siteRva, (unsigned long long)calc,
            (unsigned long long)targetRva);
        return false;
    }

    uint8_t* stub = AllocNear((uintptr_t)site, 0x1000);
    if (!stub) { Log("[%s] AllocNear failed", name); return false; }

    BuildStub(stub, guarded ? (void*)g_guardFn : (void*)g_logFn,
              (uint64_t)target, siteId, guarded);

    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[%s] VirtualProtect failed %lu", name, GetLastError());
        return false;
    }
    int64_t delta = (int64_t)((uint64_t)stub - ((uint64_t)site + 5));
    if (delta > INT32_MAX || delta < INT32_MIN) {
        VirtualProtect(site, 5, old, &old);
        Log("[%s] stub out of range", name);
        return false;
    }
    *(int32_t*)(site + 1) = (int32_t)delta;
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    Log("[%s] installed at 0x%llX", name, (unsigned long long)siteRva);
    return true;
}

static void Init() {
    g_base = (uint64_t)GetModuleHandleA(nullptr);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *s = 0;
    strncat_s(path, "\\injected_mods\\market_probe.log", _TRUNCATE);
    g_log = fopen(path, "w");
    if (!g_log) return;
    InitializeCriticalSection(&g_cs);

    Log("market_fix -- suppress bogus monthly update after save load");
    Log("base=0x%llX", (unsigned long long)g_base);

    g_logFn = (LogFn)&LogProbe;
    g_guardFn = (GuardFn)&LogGuard;

    // arm the flag whenever the market reader runs during a load
    HookSite(0x8F196CULL, 0x308990ULL, 3, "READ_BOUGHT", false);
    // and swallow the monthly update that follows
    HookSite(0x25865EULL, 0x8F50A0ULL, 1, "MONTH_1", true);
    HookSite(0x260905ULL, 0x8F50A0ULL, 2, "MONTH_2", true);

    Log("install done");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls((HMODULE)0);
        Init();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            Log("=== detach: suppressed=%ld passed=%ld ===", g_suppressed, g_passed);
            fclose(g_log); g_log = nullptr;
        }
    }
    return TRUE;
}
