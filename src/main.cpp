// KeyMacro —— Windows 11 后台键盘连发宏（修复版）
//
//   Author : 霭阳霁月 (ayjy)
//   Product: KeyMacro-ayjy
//   Copyright (c) 2026 霭阳霁月 (ayjy). All rights reserved.
//
// 相对上一版的修复：
//   P0-1 看门狗改用"最近物理钩子事件"时间戳，不再用 GetAsyncKeyState
//        （自己注入的 KEYUP 会把系统状态清成松开，误判导致连发被周期性打断）
//   P0-2 F8 / F12 加物理边沿 latch，防止系统自动重复造成疯狂切换
//   P0-3 全程加诊断输出（偏差统计 / 定时器高精度是否生效 / 实测粒度）
//   P1   隐藏窗口改为不可见顶层窗口（HWND_MESSAGE 无法置前，菜单会残留）
//        单实例互斥、回退路径补 timeBeginPeriod、退出还原 NtSetTimerResolution
//        托盘菜单后补 WM_NULL
//   P2   同时填 wVk 和 wScan、修饰键组合透传
//
// 编译（MinGW）：
//   g++ -std=c++17 -O2 -municode -mwindows src\main.cpp -o KeyMacro.exe ^
//       -luser32 -lwinmm -lshell32 -static
// 编译（MSVC）：
//   cl /EHsc /O2 /std:c++17 /DUNICODE /D_UNICODE src\main.cpp ^
//      /link /SUBSYSTEM:WINDOWS user32.lib winmm.lib shell32.lib

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>      // timeBeginPeriod / timeEndPeriod（MinGW 不会间接带出）

#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ============================================================
// 常量 / 默认配置
// ============================================================
static constexpr ULONG_PTR kMagic      = 0x4B4D4143ULL;  // 'KMAC' 自己注入的连发
static constexpr ULONG_PTR kMagicProbe = 0x4B4D5052ULL;  // 'KMPR' 探针

static constexpr UINT WM_TRAY      = WM_APP + 1;
static constexpr UINT IDM_TOGGLE   = 1001;
static constexpr UINT IDM_EXIT     = 1002;
static constexpr UINT IDM_RELOAD   = 1003;
static constexpr UINT IDM_OPENCFG  = 1004;
static constexpr UINT IDM_ABOUT    = 1005;

// 作者署名（仅用于界面展示，不影响任何逻辑）
#define KM_AUTHOR L"霭阳霁月 (ayjy)"

static constexpr int kDefaultIntervalMs = 5;
static constexpr int kDefaultPressMs    = 4;

// 默认白名单：1 2 3 4 5 + C E R G J K
static const BYTE kWhitelistVk[] = {
    '1','2','3','4','5','C','E','R','G','J','K'
};
static constexpr int kWlCount =
    (int)(sizeof(kWhitelistVk) / sizeof(kWhitelistVk[0]));

// 运行时键位表：可由配置文件 [keys] list = ... 覆盖，改键位无需重新编译。
// 未启用 [keys] 时全部沿用 kWhitelistVk，行为与之前完全一致。
static constexpr int kMaxKeys = 32;
static BYTE g_activeVk[kMaxKeys] = {
    '1','2','3','4','5','C','E','R','G','J','K'
};
static int  g_activeCount = kWlCount;

// 看门狗阈值：物理键自动重复约 30Hz，正常时时间戳不会停这么久
static constexpr uint64_t kPhysTimeoutUs = 800ULL * 1000;

// ============================================================
// 全局状态
// ============================================================
struct KeyState {
    BYTE  vk   = 0;
    BYTE  scan = 0;
    bool  extended = false;
    std::atomic<bool>     physicalDown{false};  // 物理键是否按住
    std::atomic<bool>     isDown{false};        // 我方已发 DOWN 未配 UP
    std::atomic<uint64_t> nextDownUs{0};
    std::atomic<uint64_t> nextUpUs{0};
};

static KeyState          g_keys[kMaxKeys];
static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_running{true};

// 配置（调度线程读，主线程写后发布；此处均为小整数，用 atomic 足够）
static std::atomic<int>  g_intervalMs{kDefaultIntervalMs};
static std::atomic<int>  g_pressMs{kDefaultPressMs};

static HINSTANCE g_hInst = nullptr;
static HWND      g_hWnd  = nullptr;
static HHOOK     g_hook  = nullptr;
static HANDLE    g_stateEvent  = nullptr;
static HANDLE    g_schedThread = nullptr;
static HANDLE    g_mutexSingle = nullptr;
static HANDLE    g_schedDone   = nullptr;
static NOTIFYICONDATAW g_nid = {};

static std::atomic<bool> g_hiResTimerOk{false};
static std::atomic<uint64_t> g_lastPhysEventUs{0};

// 热键 latch：只在钩子回调（主线程）里访问，无需 atomic
static bool g_f8Down  = false;
static bool g_f12Down = false;

// ============================================================
// 诊断
// ============================================================
static void TraceA(const char* s) { OutputDebugStringA(s); }

static void TraceF(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// ============================================================
// 时间
// ============================================================
static uint64_t NowUs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000000ULL) / freq.QuadPart);
}

static void SafeCopy(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (i + 1 < cap && src && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

// ============================================================
// 发送
// ============================================================
static void SendKeyEvt(BYTE vk, BYTE scan, bool extended, bool up) {
    INPUT in = {};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwExtraInfo = kMagic;

    // 扫描码可用时优先用扫描码（部分游戏只认扫描码）；
    // MapVirtualKey 对 F13-F24 等可能返回 0，此时退回纯虚拟键模式。
    if (scan != 0) {
        in.ki.wScan   = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (up) in.ki.dwFlags |= KEYEVENTF_KEYUP;

    SendInput(1, &in, sizeof(INPUT));
}

static void SendKeyDown(const KeyState& ks) {
    SendKeyEvt(ks.vk, ks.scan, ks.extended, false);
}
static void SendKeyUp(const KeyState& ks) {
    SendKeyEvt(ks.vk, ks.scan, ks.extended, true);
}

// ============================================================
// 定时器：创建 + 实测验证 + 回退
// ============================================================
static bool VerifyHighResTimer(HANDLE hTimer) {
    LARGE_INTEGER freq, t0, t1, due;
    QueryPerformanceFrequency(&freq);
    long long total = 0;
    const int N = 5;
    for (int i = 0; i < N; ++i) {
        due.QuadPart = -(3LL * 10000);     // 3ms
        if (!SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE))
            return false;
        QueryPerformanceCounter(&t0);
        if (WaitForSingleObject(hTimer, 500) != WAIT_OBJECT_0)
            return false;
        QueryPerformanceCounter(&t1);
        total += (t1.QuadPart - t0.QuadPart) * 1000000LL / freq.QuadPart;
    }
    const long long avg = total / N;
    TraceF("[KeyMacro] 高精度定时器实测：请求 3ms，实际 avg=%lldus\n", avg);
    return avg < 5000;
}

typedef LONG (NTAPI *PFN_NtSetTimerResolution)(ULONG, BOOLEAN, PULONG);
static PFN_NtSetTimerResolution g_pfnNtSet = nullptr;
static bool g_ntSetApplied = false;

static void TryNtSetTimerResolution(ULONG desired100ns) {
    if (!g_pfnNtSet) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
            g_pfnNtSet = (PFN_NtSetTimerResolution)
                GetProcAddress(ntdll, "NtSetTimerResolution");
    }
    if (!g_pfnNtSet) return;
    ULONG cur = 0;
    if (g_pfnNtSet(desired100ns, TRUE, &cur) >= 0) {
        g_ntSetApplied = true;
        TraceA("[KeyMacro] NtSetTimerResolution(0.5ms) 已应用\n");
    }
}

static void RestoreTimerResolution() {
    if (g_ntSetApplied && g_pfnNtSet) {
        ULONG cur = 0;
        g_pfnNtSet(0, FALSE, &cur);
        g_ntSetApplied = false;
    }
}

// ============================================================
// 调度核心
// ============================================================
static uint64_t ComputeNextWake() {
    uint64_t best = UINT64_MAX;
    for (auto& ks : g_keys) {
        const bool physDown = ks.physicalDown.load(std::memory_order_acquire);
        const bool isDown   = ks.isDown.load(std::memory_order_acquire);
        // 物理已松但还按着 → 立刻补 UP
        if (!physDown && isDown) return 0;
        const uint64_t d = ks.nextDownUs.load(std::memory_order_acquire);
        const uint64_t u = ks.nextUpUs.load(std::memory_order_acquire);
        if (physDown && d > 0 && d < best) best = d;
        if (isDown   && u > 0 && u < best) best = u;
    }
    return best;
}

static void ProcessDueKeys(uint64_t now) {
    const uint64_t intervalUs = (uint64_t)g_intervalMs.load(std::memory_order_relaxed) * 1000ULL;
    const uint64_t pressUs    = (uint64_t)g_pressMs.load(std::memory_order_relaxed)    * 1000ULL;

    for (auto& ks : g_keys) {
        const bool physDown = ks.physicalDown.load(std::memory_order_acquire);
        const bool isDown   = ks.isDown.load(std::memory_order_acquire);
        const uint64_t d    = ks.nextDownUs.load(std::memory_order_acquire);
        const uint64_t u    = ks.nextUpUs.load(std::memory_order_acquire);

        // 1) 松手急停：物理已松但还处于 DOWN → 立即补 UP
        if (!physDown && isDown) {
            SendKeyUp(ks);
            ks.isDown.store(false, std::memory_order_release);
            ks.nextUpUs.store(0, std::memory_order_release);
            ks.nextDownUs.store(0, std::memory_order_release);
            continue;
        }

        // 2) 到期 UP
        if (isDown && u > 0 && u <= now) {
            SendKeyUp(ks);
            ks.isDown.store(false, std::memory_order_release);
            ks.nextUpUs.store(0, std::memory_order_release);
        }

        // 3) 到期 DOWN
        if (physDown && !ks.isDown.load(std::memory_order_acquire)
            && d > 0 && d <= now) {
            SendKeyDown(ks);
            ks.isDown.store(true, std::memory_order_release);
            ks.nextUpUs.store(now + pressUs,       std::memory_order_release);
            ks.nextDownUs.store(now + intervalUs,  std::memory_order_release);

            // ---- 偏差统计（P0-3）----
            static uint64_t s_last = 0, s_cnt = 0, s_sum = 0, s_max = 0;
            if (s_last) {
                int64_t diff = (int64_t)now - (int64_t)(s_last + intervalUs);
                if (diff < 0) diff = -diff;
                s_sum += (uint64_t)diff;
                if ((uint64_t)diff > s_max) s_max = (uint64_t)diff;
                if (++s_cnt % 100 == 0) {
                    TraceF("[KeyMacro] 偏差 avg=%lluus max=%lluus | 间隔 %dms 保持 %dms | 高精度定时器=%d\n",
                           (unsigned long long)(s_sum / s_cnt),
                           (unsigned long long)s_max,
                           (int)g_intervalMs.load(std::memory_order_relaxed),
                           (int)g_pressMs.load(std::memory_order_relaxed),
                           (int)g_hiResTimerOk.load(std::memory_order_relaxed));
                }
            }
            s_last = now;
        }
    }
}

// P0-1 修复：看门狗只看"最近一次物理钩子事件"是否停止刷新。
// 绝不能用 GetAsyncKeyState —— 我们自己注入的 KEYUP 会把它清成"松开"，
// 而按住时"松开"窗口占每个周期的 (interval-press)/interval，
// 看门狗撞进该窗口就会误判清掉连发，造成周期性卡顿。
static void CheckWatchdog() {
    const uint64_t now = NowUs();
    const uint64_t last = g_lastPhysEventUs.load(std::memory_order_relaxed);
    if (last == 0) return;
    if (now - last <= kPhysTimeoutUs) return;

    // 钩子大概率已被系统卸载 → 才真正清状态
    TraceA("[KeyMacro] 看门狗触发：物理事件超时，清空所有通道\n");
    for (auto& ks : g_keys)
        ks.physicalDown.store(false, std::memory_order_release);
    g_lastPhysEventUs.store(now, std::memory_order_relaxed);
}

static void ReleaseAllKeys() {
    for (auto& ks : g_keys) {
        if (ks.isDown.load(std::memory_order_acquire)) {
            SendKeyUp(ks);
            ks.isDown.store(false, std::memory_order_release);
        }
        ks.physicalDown.store(false, std::memory_order_release);
        ks.nextDownUs.store(0, std::memory_order_release);
        ks.nextUpUs.store(0, std::memory_order_release);
    }
}

// ============================================================
// 调度线程
// ============================================================
static DWORD WINAPI SchedulerThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 先做最基础的：提升系统时钟分辨率（大概率无效，但零成本）
    timeBeginPeriod(1);

    // ---- 创建并实测高精度定时器 ----
    HANDLE hTimer = nullptr;
    for (int attempt = 0; attempt < 2 && !hTimer; ++attempt) {
        HANDLE h = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (h && VerifyHighResTimer(h)) {
            hTimer = h;
            g_hiResTimerOk.store(true, std::memory_order_relaxed);
        } else {
            if (h) CloseHandle(h);
            if (attempt == 0) TryNtSetTimerResolution(5000);  // 0.5ms
        }
    }
    if (!hTimer) {
        TraceA("[KeyMacro] 高精度定时器不可用，回退普通定时器（可能受 15.6ms 粒度影响）\n");
        hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    DWORD lastWatchdog = GetTickCount();

    while (g_running.load(std::memory_order_acquire)) {
        const uint64_t now  = NowUs();
        const uint64_t next = ComputeNextWake();

        const DWORD tickNow = GetTickCount();
        if (tickNow - lastWatchdog > 1000) {
            CheckWatchdog();
            lastWatchdog = tickNow;
        }

        if (next == UINT64_MAX) {
            // 无任务：等事件即可，不占用定时器
            WaitForSingleObject(g_stateEvent, 500);
            continue;
        }

        const int64_t waitUs = (int64_t)next - (int64_t)now;
        if (waitUs <= 0) {
            ProcessDueKeys(now);
            continue;
        }

        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(waitUs * 10);   // us → 100ns
        if (!SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE)) {
            Sleep(1);
            continue;
        }

        // 定时器为主，事件仅作 500ms 兜底（防止状态悬挂）
        HANDLE handles[2] = { hTimer, g_stateEvent };
        const DWORD wr = WaitForMultipleObjects(2, handles, FALSE, 500);

        if (wr == WAIT_OBJECT_0) {
            ProcessDueKeys(NowUs());
        }
        // WAIT_OBJECT_0+1 / WAIT_TIMEOUT → 下一轮重算
    }

    ReleaseAllKeys();
    if (hTimer) CloseHandle(hTimer);
    timeEndPeriod(1);
    RestoreTimerResolution();
    SetEvent(g_schedDone);
    return 0;
}

// ============================================================
// 低级键盘钩子
// ============================================================
static int WlIndex(DWORD vk) {
    for (int i = 0; i < g_activeCount; ++i)
        if (g_activeVk[i] == (BYTE)vk) return i;
    return -1;
}

static void TrayRefresh();
static void InitKeys();

static bool ModifierHeld() {
    return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
           ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0) ||
           ((GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0) ||
           ((GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0);
}

static LRESULT CALLBACK LowLevelKeyboardProc(
        int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    auto* kb = (KBDLLHOOKSTRUCT*)lParam;

    if (kb->dwExtraInfo == kMagicProbe) return 1;
    if (kb->dwExtraInfo == kMagic)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    if (kb->flags & LLKHF_INJECTED)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    // ★ 只有物理事件才刷新看门狗时间戳（注入事件会自我满足）
    g_lastPhysEventUs.store(NowUs(), std::memory_order_relaxed);

    // ---- F8 切换（P0-2：物理边沿 latch）----
    if (kb->vkCode == VK_F8) {
        if (isUp) { g_f8Down = false; return 1; }
        if (g_f8Down) return 1;
        g_f8Down = true;
        if (!ModifierHeld()) {
            const bool next = !g_enabled.load(std::memory_order_acquire);
            g_enabled.store(next, std::memory_order_release);
            if (!next) {
                for (auto& ks : g_keys)
                    ks.physicalDown.store(false, std::memory_order_release);
                SetEvent(g_stateEvent);
            }
            TrayRefresh();
            TraceF("[KeyMacro] 连发已%s\n", next ? "启用" : "禁用");
        }
        return 1;
    }

    // ---- Ctrl+Alt+F12 退出（P0-2：latch）----
    if (kb->vkCode == VK_F12) {
        if (isUp) { g_f12Down = false; return 1; }
        if (g_f12Down) return 1;
        g_f12Down = true;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            (GetAsyncKeyState(VK_MENU)    & 0x8000) &&
            !((GetAsyncKeyState(VK_LWIN)  & 0x8000) ||
              (GetAsyncKeyState(VK_RWIN)  & 0x8000))) {
            PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
            return 1;
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const int idx = WlIndex(kb->vkCode);
    if (idx < 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    if (!g_enabled.load(std::memory_order_acquire))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    // 修饰键组合一律透传（Ctrl+1 之类不该连发）
    if (ModifierHeld() || (kb->flags & LLKHF_ALTDOWN))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    KeyState& ks = g_keys[idx];

    if (isDown) {
        // 系统自动重复：physicalDown 已为 true → 忽略
        if (ks.physicalDown.exchange(true, std::memory_order_acq_rel))
            return 1;

        // 首发：钩子内直发，与物理 keydown 同一时刻到达
        ks.scan     = (BYTE)(kb->scanCode & 0xFF);
        ks.extended = (kb->flags & LLKHF_EXTENDED) != 0;

        SendKeyDown(ks);

        const uint64_t now = NowUs();
        const uint64_t pressUs    = (uint64_t)g_pressMs.load(std::memory_order_relaxed)    * 1000ULL;
        const uint64_t intervalUs = (uint64_t)g_intervalMs.load(std::memory_order_relaxed) * 1000ULL;
        ks.isDown.store(true, std::memory_order_release);
        ks.nextUpUs.store(now + pressUs,     std::memory_order_release);
        ks.nextDownUs.store(now + intervalUs, std::memory_order_release);
        SetEvent(g_stateEvent);
        return 1;          // 吞掉原始按下，避免双触发
    }

    if (isUp) {
        ks.physicalDown.store(false, std::memory_order_release);
        SetEvent(g_stateEvent);
        return 1;          // 吞掉原始抬起
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ============================================================
// 配置（interval_ms / press_ms / 键位表）
// ============================================================

// 键名 -> 虚拟键码。支持 0-9 A-Z、F1-F24、方向键、小键盘、常用命名键。
// 无法识别返回 0，调用方跳过该键。
static BYTE VkFromToken(const std::wstring& raw) {
    std::wstring s;
    for (wchar_t c : raw) {
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        s += c;
    }
    if (s.empty()) return 0;

    // F1 - F24
    if (s.size() >= 2 && s[0] == L'F') {
        int  n  = 0;
        bool ok = true;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] < L'0' || s[i] > L'9') { ok = false; break; }
            n = n * 10 + (int)(s[i] - L'0');
        }
        if (ok && n >= 1 && n <= 24) return (BYTE)(VK_F1 + n - 1);
    }

    static const struct { const wchar_t* n; BYTE vk; } kNamed[] = {
        { L"SPACE", VK_SPACE },       { L"TAB", VK_TAB },
        { L"ENTER", VK_RETURN },      { L"RETURN", VK_RETURN },
        { L"ESC", VK_ESCAPE },        { L"ESCAPE", VK_ESCAPE },
        { L"BACKSPACE", VK_BACK },    { L"BS", VK_BACK },
        { L"UP", VK_UP },             { L"DOWN", VK_DOWN },
        { L"LEFT", VK_LEFT },         { L"RIGHT", VK_RIGHT },
        { L"HOME", VK_HOME },         { L"END", VK_END },
        { L"PGUP", VK_PRIOR },        { L"PGDN", VK_NEXT },
        { L"PAGEUP", VK_PRIOR },      { L"PAGEDOWN", VK_NEXT },
        { L"INSERT", VK_INSERT },     { L"INS", VK_INSERT },
        { L"DELETE", VK_DELETE },     { L"DEL", VK_DELETE },
        { L"CAPSLOCK", VK_CAPITAL },  { L"CAPS", VK_CAPITAL },
        { L"SHIFT", VK_SHIFT },       { L"LSHIFT", VK_LSHIFT },
        { L"RSHIFT", VK_RSHIFT },     { L"CTRL", VK_CONTROL },
        { L"LCTRL", VK_LCONTROL },    { L"RCTRL", VK_RCONTROL },
        { L"ALT", VK_MENU },          { L"LALT", VK_LMENU },
        { L"RALT", VK_RMENU },        { L"WIN", VK_LWIN },
        { L"LWIN", VK_LWIN },         { L"RWIN", VK_RWIN },
        { L"LMB", VK_LBUTTON },       { L"RMB", VK_RBUTTON },
        { L"MMB", VK_MBUTTON },       { L"X1", VK_XBUTTON1 },
        { L"X2", VK_XBUTTON2 }
    };
    for (const auto& e : kNamed)
        if (s == e.n) return e.vk;

    if (s.size() == 7 && s.compare(0, 6, L"NUMPAD") == 0) {
        const wchar_t d = s[6];
        if (d >= L'0' && d <= L'9') return (BYTE)(VK_NUMPAD0 + (d - L'0'));
    }

    if (s.size() == 1) {
        const SHORT r = VkKeyScanW(s[0]);
        if (r != -1) return (BYTE)(r & 0xFF);
    }
    return 0;
}

static std::wstring GetConfigPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring s(buf, n);
    s += L"\\KeyMacro";
    CreateDirectoryW(s.c_str(), nullptr);
    return s + L"\\KeyMacro.ini";
}

static void LoadConfig() {
    const std::wstring path = GetConfigPath();
    if (path.empty()) return;

    const wchar_t* kTemplate =
        L"; KeyMacro 配置\r\n"
        L"; Author: 霭阳霁月 (ayjy)\r\n"
        L"; 改完用托盘右键「重新加载配置」生效，不用重启程序\r\n"
        L"\r\n"
        L"[keys]\r\n"
        L"; 连发键位，空格或逗号分隔（改这个不用重装、不用重编译）\r\n"
        L"; 支持：0-9 A-Z、F1-F24、SPACE TAB ENTER ESC、\r\n"
        L";       UP DOWN LEFT RIGHT、NUMPAD0-9、SHIFT CTRL ALT 等\r\n"
        L"; 整段删掉或留空 = 使用默认键位\r\n"
        L"list = 1 2 3 4 5 C E R G J K\r\n"
        L"\r\n"
        L"[timing]\r\n"
        L"; 连发间隔（毫秒），默认 5\r\n"
        L"interval_ms = 5\r\n"
        L"; 单次按下保持时长（毫秒），默认 4，必须小于 interval_ms\r\n"
        L"; 太小会被轮询式输入的游戏整次漏掉\r\n"
        L"press_ms = 4\r\n";

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        f = _wfopen(path.c_str(), L"wb");
        if (f) {
            int nb = WideCharToMultiByte(CP_UTF8, 0, kTemplate, -1,
                                         nullptr, 0, nullptr, nullptr);
            std::string out((size_t)(nb > 0 ? nb : 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, kTemplate, -1,
                                &out[0], nb, nullptr, nullptr);
            fwrite("\xEF\xBB\xBF", 1, 3, f);
            fwrite(out.c_str(), 1, out.size() - 1, f);
            fclose(f);
        }
        return;
    }
    fclose(f);

    // ---- 键位表（可选：解析失败或整段缺失则沿用编译期默认值）----
    wchar_t keysBuf[512] = {0};
    GetPrivateProfileStringW(L"keys", L"list", L"",
                             keysBuf, 512, path.c_str());
    if (keysBuf[0] != L'\0') {
        BYTE         tmp[kMaxKeys];
        int          n   = 0;
        std::wstring cur;
        for (const wchar_t* p = keysBuf; ; ++p) {
            const wchar_t c = *p;
            if (c == L' ' || c == L',' || c == L'\t' || c == L';' || c == L'\0') {
                if (!cur.empty() && n < kMaxKeys) {
                    const BYTE vk  = VkFromToken(cur);
                    bool       dup = (vk == 0);
                    for (int j = 0; j < n && !dup; ++j)
                        if (tmp[j] == vk) dup = true;
                    if (!dup) tmp[n++] = vk;
                }
                cur.clear();
                if (c == L'\0') break;
            } else {
                cur += c;
            }
        }
        if (n > 0) {
            for (int i = 0; i < n; ++i) g_activeVk[i] = tmp[i];
            g_activeCount = n;
            TraceF("[KeyMacro] 载入键位：%d 个（来自配置）\n", n);
        } else {
            TraceF("[KeyMacro] 键位配置无法识别，沿用默认 %d 个\n", kWlCount);
        }
    }

    const int interval = GetPrivateProfileIntW(L"timing", L"interval_ms",
                                               kDefaultIntervalMs, path.c_str());
    const int press    = GetPrivateProfileIntW(L"timing", L"press_ms",
                                               kDefaultPressMs, path.c_str());

    int iv = interval, ps = press;
    if (iv < 1 || iv > 500) iv = kDefaultIntervalMs;
    if (ps < 3 || ps > 50)  ps = kDefaultPressMs;
    if (ps >= iv)           ps = iv - 1 >= 3 ? iv - 1 : 3;

    g_intervalMs.store(iv, std::memory_order_release);
    g_pressMs.store(ps, std::memory_order_release);
    TraceF("[KeyMacro] 载入配置：interval=%dms press=%dms\n", iv, ps);
}

// ============================================================
// 托盘
// ============================================================
static void TrayAdd(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    SafeCopy(g_nid.szTip, 128, L"KeyMacro  by " KM_AUTHOR);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void TrayRefresh() {
    const bool en = g_enabled.load(std::memory_order_acquire);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    SafeCopy(g_nid.szTip, 128,
             en ? L"KeyMacro [启用]  F8 切换  by 霭阳霁月 (ayjy)"
                 : L"KeyMacro [禁用]  F8 切换  by 霭阳霁月 (ayjy)");
    g_nid.hIcon = LoadIconW(nullptr, en ? IDI_APPLICATION : IDI_WARNING);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TOGGLE,
                g_enabled.load() ? L"禁用 (F8)" : L"启用 (F8)");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"重新加载配置");
    AppendMenuW(m, MF_STRING, IDM_OPENCFG, L"打开配置文件");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"关于 KeyMacro");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出 (Ctrl+Alt+F12)");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    // TPM_RETURNCMD 会把选中项的 ID 通过返回值给出；TPM_NONOTIFY 已禁止
    // 自动通知消息，所以必须接住返回值再转发成 WM_COMMAND，
    // 否则点菜单没有任何反应。
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd, WM_NULL, 0, 0);   // 防菜单残留
    if (cmd) PostMessageW(hwnd, WM_COMMAND, cmd, 0);
}

// ============================================================
// 隐藏窗口（P1：不可见顶层窗口，不能用 HWND_MESSAGE）
// ============================================================
static LRESULT CALLBACK HiddenWndProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAY: {
        const UINT evt = LOWORD(lParam);
        if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU ||
            evt == NIN_SELECT   || evt == NIN_KEYSELECT) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE: {
            const bool next = !g_enabled.load(std::memory_order_acquire);
            g_enabled.store(next, std::memory_order_release);
            if (!next) {
                for (auto& ks : g_keys)
                    ks.physicalDown.store(false, std::memory_order_release);
                SetEvent(g_stateEvent);
            }
            TrayRefresh();
            return 0;
        }
        case IDM_RELOAD:
            ReleaseAllKeys();   // 先补发释放，改键位时不会留下卡住的键
            LoadConfig();
            InitKeys();
            TrayRefresh();
            return 0;
        case IDM_OPENCFG: {
            const std::wstring p = GetConfigPath();
            if (!p.empty())
                ShellExecuteW(nullptr, L"open", p.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"KeyMacro —— Windows 后台键盘连发宏\n\n"
                L"作者：霭阳霁月 (ayjy)\n\n"
                L"F8             启用 / 禁用连发\n"
                L"Ctrl+Alt+F12   退出程序\n\n"
                L"托盘右键：打开配置文件 / 重新加载配置",
                L"关于 KeyMacro", MB_ICONINFORMATION);
            return 0;
        case IDM_EXIT:
            PostQuitMessage(0);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool CreateHiddenWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HiddenWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"KeyMacroHiddenWnd";
    if (!RegisterClassExW(&wc)) return false;

    // 不可见顶层窗口：HWND_MESSAGE 无法 SetForegroundWindow，
    // 会导致托盘菜单弹出后不消失。
    g_hWnd = CreateWindowExW(0, L"KeyMacroHiddenWnd", L"KeyMacro",
                             WS_OVERLAPPED, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);
    return g_hWnd != nullptr;
}

// ============================================================
// 初始化
// ============================================================
static void InitKeys() {
    // 先整体复位，键位数量减少时旧槽位不会残留
    for (int i = 0; i < kMaxKeys; ++i) {
        KeyState& k = g_keys[i];
        k.physicalDown.store(false, std::memory_order_release);
        k.isDown.store(false,       std::memory_order_release);
        k.nextDownUs.store(0,       std::memory_order_release);
        k.nextUpUs.store(0,         std::memory_order_release);
        k.vk       = 0;
        k.scan     = 0;
        k.extended = false;
    }
    for (int i = 0; i < g_activeCount; ++i) {
        const BYTE vk = g_activeVk[i];
        const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        g_keys[i].vk       = vk;
        g_keys[i].scan     = (BYTE)(sc & 0xFF);
        g_keys[i].extended = (sc & 0x100) != 0;
    }
}

// ============================================================
// 入口
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    // 作者署名（仅输出一行日志，不影响任何逻辑）
    OutputDebugStringW(L"[KeyMacro] " KM_AUTHOR L" - starting\r\n");

    // 单实例
    g_mutexSingle = CreateMutexW(nullptr, TRUE, L"Global\\KeyMacro_SingleInstance");
    if (!g_mutexSingle || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutexSingle) CloseHandle(g_mutexSingle);
        MessageBoxW(nullptr, L"KeyMacro 已在运行。",
                    L"KeyMacro  by " KM_AUTHOR, MB_ICONINFORMATION);
        return 1;
    }

    LoadConfig();   // 先确定键位（可能来自 ini）
    InitKeys();     // 再按最终键位初始化键表

    g_stateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_schedDone  = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!g_stateEvent || !g_schedDone) return 1;

    if (!CreateHiddenWindow(hInst)) return 1;

    TrayAdd(g_hWnd);
    TrayRefresh();

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"安装键盘钩子失败。\n请尝试以管理员身份运行。",
                    L"KeyMacro  by " KM_AUTHOR, MB_ICONERROR);
        return 1;
    }

    g_schedThread = CreateThread(nullptr, 0, SchedulerThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 退出：先卸钩子 → 停线程（超时不无限等）→ 补发 KEYUP ----
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }

    g_running.store(false, std::memory_order_release);
    SetEvent(g_stateEvent);

    bool detached = false;
    if (g_schedThread) {
        if (WaitForSingleObject(g_schedDone, 2000) != WAIT_OBJECT_0)
            detached = true;      // 绝不无限等
        CloseHandle(g_schedThread);
    }

    // 兜底：确保没有残留"按住"
    if (detached) ReleaseAllKeys();

    TrayRemove();
    CloseHandle(g_schedDone);
    CloseHandle(g_stateEvent);
    if (g_mutexSingle) CloseHandle(g_mutexSingle);
    OutputDebugStringW(L"[KeyMacro] " KM_AUTHOR L" - exited\r\n");
    return 0;
}
