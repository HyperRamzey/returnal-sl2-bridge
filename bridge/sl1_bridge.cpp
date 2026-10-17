// ============================================================================
//  sl1_bridge  -  Streamline 1.3.3 -> Streamline 2.14.16 API bridge
//
//  Drop-in replacement for the Streamline 1.x `sl.interposer.dll`.
//  Exports the exact SL1 export surface (see sl1_bridge.def) so the host game
//  resolves its entry points unchanged, then translates every SL1 call onto
//  the genuine SL 2.x API which is loaded from a sub-directory.
//
//  SL1 side (this file's ABI, recovered by static RE of the shipping
//  Streamline 1.3.3 binaries and headers):
//    - sl::Constants        : 432 bytes, 1-byte Boolean at 412..419, ext at 424
//    - sl::Resource         : {type u32 @0, native @8, memory @16, view u32 @24, state u32 @28}
//    - sl::ReflexConstants  : {mode u32, frameLimitUs u32, useMarkersToOptimize u8, virtualKey u16 @0xA, ext @0x10}
//    - sl::ReflexSettings   : {lowLatencyAvailable u8 @0, latencyReportAvailable u8 @1, statsWindowMessage u32 @4, ...}
//    - sl::DLSSGOptions     : {mode u32 @0, numFramesToGenerate u32 @4, ...}
//    - sl::Feature          : DLSS = 0, Reflex = 3, DLSS_G = 1000 (identical to SL2)
//    - sl::BufferType       : Depth=0, MotionVectors=1, HUDLessColor=2, UIColorAndAlpha=23 (identical to SL2)
//    - sl::ReflexMarker     : 0..5 same as SL2 PCLMarker, 6=InputSample (gone in SL2), 7=TriggerFlash, 8=PCLatencyPing, 0x1000=Sleep
//
//  Build: see build.bat (MSVC x64, no import libs: SL2 is resolved dynamically)
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>          // D3D11CreateDevice[AndSwapChain] signatures (pulls in dxgi.h)
#include <stdint.h>
#include <stdio.h>
#include <share.h>          // _SH_DENYNO: log stays readable while the game runs
#include <stdarg.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>

// ---- SL 2.14.16 SDK (ABI target for the second half of the bridge) ----
#include "sl_struct.h"
#include "sl_result.h"
#include "sl_core_types.h"
#include "sl_consts.h"
#include "sl_core_api.h"
#include "sl_version.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_pcl.h"

// ============================================================================
//  logging
// ============================================================================
static std::recursive_mutex g_logMutex;
static FILE* g_log = nullptr;
static int   g_logLevel = 1;          // 0 errors, 1 state changes, 2 every call
static unsigned g_frameCounter = 0;

static void LogF(int level, const char* fmt, ...)
{
    if (level > g_logLevel || !g_log) return;
    char body[900];
    va_list ap; va_start(ap, fmt);
    vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st; GetLocalTime(&st);
    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%s] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                level == 0 ? "ERR " : (level == 1 ? "info" : "dbg "), body);

    std::lock_guard<std::recursive_mutex> lk(g_logMutex);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
}
#define LOGE(...) LogF(0, __VA_ARGS__)
#define LOGI(...) LogF(1, __VA_ARGS__)
#define LOGD(...) LogF(2, __VA_ARGS__)

// Per-frame APIs arrive hundreds of times per second; logging every call produced
// a 302 MB sl_bridge.log in ten minutes.  Throttle(key) logs the first `first`
// occurrences and then every `every`th one: startup is fully logged, steady state
// is sampled.  Distinct keys keep unrelated call sites independent.
static std::map<uint32_t, uint32_t> g_thrCount;
static bool Throttle(uint32_t key, uint32_t first, uint32_t every)
{
    uint32_t c = ++g_thrCount[key];
    return c <= first || (every && ((c - first) % every) == 0);
}

// ============================================================================
//  SL1 ABI (input side)
// ============================================================================
#pragma pack(push, 4)
struct Sl1Extent { int32_t left, top, width, height; };

struct Sl1Resource
{
    uint32_t type;      // 0 = eTex2d
    uint32_t pad0;
    void*    native;    // ID3D12Resource*
    void*    memory;
    uint32_t view;
    uint32_t state;
};

struct Sl1Constants
{
    float cameraViewToClip[16];      // 0
    float clipToCameraView[16];      // 64
    float clipToLensClip[16];        // 128
    float clipToPrevClip[16];        // 192
    float prevClipToClip[16];        // 256
    float jitterOffset[2];           // 320
    float mvecScale[2];              // 328
    float cameraPinholeOffset[2];    // 336
    float cameraPos[3];              // 344
    float cameraUp[3];               // 356
    float cameraRight[3];            // 368
    float cameraFwd[3];              // 380
    float cameraNear;                // 392
    float cameraFar;                 // 396
    float cameraFOV;                 // 400
    float cameraAspectRatio;         // 404
    float motionVectorsInvalidValue; // 408
    uint8_t depthInverted;           // 412
    uint8_t cameraMotionIncluded;    // 413
    uint8_t motionVectors3D;         // 414 (never written by the game)
    uint8_t reset;                   // 415
    uint8_t notRenderingGameFrames;  // 416
    uint8_t orthographicProjection;  // 417
    uint8_t motionVectorsDilated;    // 418
    uint8_t motionVectorsJittered;   // 419
    uint8_t pad1[4];                 // 420
    void*   ext;                     // 424
};

struct Sl1ReflexConstants
{
    uint32_t mode;                    // 0
    uint32_t frameLimitUs;            // 4
    uint8_t  useMarkersToOptimize;    // 8
    uint8_t  pad0;
    uint16_t virtualKey;              // 0xA
    uint32_t pad1;
    void*    ext;                     // 0x10
};

struct Sl1DlssgOptions
{
    uint32_t mode;                    // 0
    uint32_t numFramesToGenerate;     // 4
    uint32_t flags;                   // 8
    uint32_t dynamicResWidth;         // 12
    uint32_t dynamicResHeight;        // 16
    uint32_t numBackBuffers;          // 20
    uint32_t mvecDepthWidth;          // 24
    uint32_t mvecDepthHeight;         // 28
    uint32_t colorWidth;              // 32
    uint32_t colorHeight;             // 36
    uint32_t colorFormat;             // 40
    uint32_t mvecFormat;              // 44
    uint32_t depthFormat;             // 48
    uint32_t hudlessFormat;           // 52
    uint32_t uiFormat;                // 56
};
#pragma pack(pop)

static_assert(sizeof(Sl1Constants) == 432, "SL1 Constants must be 432 bytes");
static_assert(sizeof(Sl1Resource) == 32, "SL1 Resource must be 32 bytes");
static_assert(sizeof(Sl1ReflexConstants) == 24, "SL1 ReflexConstants must be 24 bytes");

// ============================================================================
//  SL2 dynamic bindings (no import lib)
// ============================================================================
typedef sl::Result (__cdecl *pfn_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (__cdecl *pfn_slShutdown)();
typedef sl::Result (__cdecl *pfn_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (__cdecl *pfn_slSetTagForFrame)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (__cdecl *pfn_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
typedef sl::Result (__cdecl *pfn_slAllocateResources)(sl::CommandBuffer*, sl::Feature, const sl::ViewportHandle&);
typedef sl::Result (__cdecl *pfn_slFreeResources)(sl::Feature, const sl::ViewportHandle&);
typedef sl::Result (__cdecl *pfn_slIsFeatureSupported)(sl::Feature, const sl::AdapterInfo&);
typedef sl::Result (__cdecl *pfn_slIsFeatureLoaded)(sl::Feature, bool&);
typedef sl::Result (__cdecl *pfn_slSetFeatureLoaded)(sl::Feature, bool);
typedef sl::Result (__cdecl *pfn_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (__cdecl *pfn_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (__cdecl *pfn_slSetD3DDevice)(void*);

typedef sl::Result (__cdecl *pfn_slDLSSGSetOptions)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
typedef sl::Result (__cdecl *pfn_slDLSSGGetState)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
typedef sl::Result (__cdecl *pfn_slReflexSetOptions)(const sl::ReflexOptions&);
typedef sl::Result (__cdecl *pfn_slReflexGetState)(sl::ReflexState&);
typedef sl::Result (__cdecl *pfn_slReflexSleep)(const sl::FrameToken&);
typedef sl::Result (__cdecl *pfn_slReflexSetMarker)(sl::PCLMarker, const sl::FrameToken&);

struct Sl2Bindings
{
    HMODULE mod = nullptr;
    pfn_slInit             init = nullptr;
    pfn_slShutdown         shutdown = nullptr;
    pfn_slSetConstants     setConstants = nullptr;
    pfn_slSetTagForFrame   setTagForFrame = nullptr;
    pfn_slEvaluateFeature  evaluateFeature = nullptr;
    pfn_slAllocateResources allocateResources = nullptr;
    pfn_slFreeResources    freeResources = nullptr;
    pfn_slIsFeatureSupported isFeatureSupported = nullptr;
    pfn_slIsFeatureLoaded  isFeatureLoaded = nullptr;
    pfn_slSetFeatureLoaded setFeatureLoaded = nullptr;
    pfn_slGetFeatureFunction getFeatureFunction = nullptr;
    pfn_slGetNewFrameToken getNewFrameToken = nullptr;
    pfn_slSetD3DDevice     setD3DDevice = nullptr;

    pfn_slDLSSGSetOptions  dlssgSetOptions = nullptr;
    pfn_slDLSSGGetState    dlssgGetState = nullptr;
    pfn_slReflexSetOptions reflexSetOptions = nullptr;
    pfn_slReflexGetState   reflexGetState = nullptr;
    pfn_slReflexSleep      reflexSleep = nullptr;
    pfn_slReflexSetMarker  reflexSetMarker = nullptr;
};
static Sl2Bindings g_sl2;

// ============================================================================
//  bridge state
// ============================================================================
static HMODULE      g_self = nullptr;
static bool         g_started = false;          // one-time init done
static bool         g_initOk = false;
static std::wstring g_dir;                      // directory holding this DLL

struct BridgeConfig
{
    int logLevel = 2;
    std::wstring pluginDir = L"sl2";
    std::wstring logDir = L"sl2logs";
    int forceMode = -1;              // -1 passthrough, 0 eOff, 1 eOn, 2 eAuto, 3 eDynamic
    int forceNumFrames = -1;         // -1 passthrough, else 1..5
    int forceDynamicFps = 0;
    int forceFlags = -1;             // -1 default (fullscreen menu detection)
    int enableUIRecomposition = 1;
    int forwardReflex = 1;
    int forceReflexMode = -1;        // -1 passthrough, else 0 off / 1 low latency / 2 boost
    int autoReflexWithFG = 1;        // DLSS-G requires Reflex: enable it if the game did not
    int bypassOSCheck = 0;
    int canonicalViewport = 1;
    int enableReflexPickupLog = 1;
    // SL2 patches the DXGI v-table by default, which collides with injection-based
    // overlay proxies that wrap the same interfaces (ReShade as dxgi.dll). The SDK
    // flag below makes SL create a factory proxy object instead - documented for
    // exactly this case.
    int dxgiFactoryProxy = 1;
    // Host SDK version reported to SL2 - see ParseHostSDK() for why 1.5.0.
    std::wstring hostSDK = L"1.5.0";
    // SL2 keeps its sl.log open for the lifetime of the process and cannot share it
    // between two processes (the direct-launch path runs the RHI pre-init pass, then
    // relaunches through Steam).  Per-process sub-directory keeps both logs intact.
    int logPerProcess = 1;
    // --- NGX application identity -------------------------------------------------
    // SL2 production builds replace a 0 application id with kTemporaryAppId and then
    // refuse to initialise NGX ("Please provide correct application id when calling
    // slInit - NGX based features will be disabled").  Stock SL1 passed the game's id
    // straight to NGX.  AppId: -1 relays the id the game passed to slInit, >= 0
    // forces a value; an effective 0 is replaced with a non-zero stand-in.
    int appIdOverride = -1;
    // ProjectId: non-empty => report it as projectId; SL2 then uses NGX's
    // Init_with_ProjectID path and skips the application-id check entirely.
    // "0" or empty => use the application-id path instead.
    std::wstring projectId = L"0";
    std::string  projectIdA;                 // UTF-8 copy handed to SL2 (must outlive slInit)
};
static BridgeConfig g_cfg;

struct StoredTag
{
    sl::Resource res;        // stable storage: tags point here
    sl::Extent   extent;
    bool         hasExtent = false;
    bool         used = false;
};
// viewport -> bufferType -> stored tag
static std::map<uint32_t, std::map<uint32_t, StoredTag>> g_tags;
static std::map<uint32_t, bool> g_featureEnabled;     // Feature -> app enabled state
static std::map<uint32_t, sl::FrameToken*> g_frameTokens; // frameIndex -> token (small cache)
static sl::FrameToken* g_lastFrame = nullptr;
static uint32_t g_lastFrameIndex = 0;
static bool g_haveFrame = false;
static std::map<uint64_t, uint64_t> g_loggedOnce;     // (feature<<32|id) -> count
static uint32_t g_seenFeatures = 0;
static sl::DLSSGStatus g_lastFgStatus = (sl::DLSSGStatus)0xFFFFFFFF;
static int g_reflexModeSeen = -1;                     // last reflex mode the app asked for (-1 = never)

// ============================================================================
//  helpers
// ============================================================================
static std::string W2A(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static void ComputeSelfDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(g_self, buf, MAX_PATH * 2);
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    g_dir = (p == std::wstring::npos) ? s : s.substr(0, p);
}

static uint32_t CanonicalViewport(uint32_t id)
{
    if (g_cfg.canonicalViewport) return 0;
    return id;
}

static void LoadConfig()
{
    std::wstring ini = g_dir + L"\\sl_bridge.ini";
    auto gi = [&](const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"bridge", key, def, ini.c_str()); };
    auto gs = [&](const wchar_t* key, const wchar_t* def) {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(L"bridge", key, def, buf, 511, ini.c_str());
        return std::wstring(buf);
    };

    g_cfg.logLevel = gi(L"LogLevel", 2);
    g_cfg.pluginDir = gs(L"PluginDir", L"sl2");
    g_cfg.logDir = gs(L"LogDir", L"sl2logs");
    g_cfg.forceMode = gi(L"ForceMode", -1);
    g_cfg.forceNumFrames = gi(L"ForceNumFrames", -1);
    g_cfg.forceDynamicFps = gi(L"ForceDynamicFPS", 0);
    g_cfg.forceFlags = gi(L"ForceFlags", -1);
    g_cfg.enableUIRecomposition = gi(L"EnableUIRecomposition", 1);
    g_cfg.forwardReflex = gi(L"ForwardReflex", 1);
    g_cfg.forceReflexMode = gi(L"ForceReflexMode", -1);
    g_cfg.autoReflexWithFG = gi(L"AutoReflexWithFG", 1);
    g_cfg.bypassOSCheck = gi(L"BypassOSVersionCheck", 0);
    g_cfg.canonicalViewport = gi(L"CanonicalViewport", 1);
    g_cfg.dxgiFactoryProxy = gi(L"DXGIFactoryProxy", 1);
    g_cfg.hostSDK = gs(L"HostSDK", L"1.5.0");
    g_cfg.logPerProcess = gi(L"LogPerProcess", 1);
    g_cfg.appIdOverride = gi(L"AppId", -1);
    g_cfg.projectId = gs(L"ProjectId", L"0");
    g_cfg.projectIdA = W2A(g_cfg.projectId);
    g_logLevel = g_cfg.logLevel;
}

static void OpenLog()
{
    CreateDirectoryW((g_dir + L"\\" + g_cfg.logDir).c_str(), nullptr);
    CreateDirectoryW((g_dir + L"\\" + g_cfg.pluginDir).c_str(), nullptr);
    std::wstring path = g_dir + L"\\sl_bridge.log";
    // Append + full sharing: the log stays readable (tail-able) while the game
    // runs, and a repeated slInit can never truncate earlier evidence.
    if (!g_log) g_log = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
}

// Look up (or create) the SL2 frame token for a given game frame index.
static sl::FrameToken* GetFrameToken(uint32_t frameIndex, bool logit)
{
    if (!g_sl2.getNewFrameToken) return nullptr;
    auto it = g_frameTokens.find(frameIndex);
    if (it != g_frameTokens.end()) return it->second;

    sl::FrameToken* tok = nullptr;
    uint32_t idx = frameIndex;
    sl::Result r = g_sl2.getNewFrameToken(tok, &idx);
    if (r != sl::Result::eOk || !tok)
    {
        LOGE("slGetNewFrameToken(index=%u) failed: %d", frameIndex, (int)r);
        return nullptr;
    }
    g_frameTokens[frameIndex] = tok;
    if (logit) LOGD("frame token for index %u created (%zu cached)", frameIndex, g_frameTokens.size());

    // prune old tokens (SL pools them; we only keep a small window)
    while (g_frameTokens.size() > 8)
    {
        auto oldest = g_frameTokens.begin();
        if (oldest->first == frameIndex) ++oldest;
        if (oldest == g_frameTokens.end()) break;
        g_frameTokens.erase(oldest);
    }
    return tok;
}

static void ReissueTags(sl::FrameToken* token, uint32_t viewportId, bool logit)
{
    if (!token || !g_sl2.setTagForFrame) return;
    auto vpIt = g_tags.find(viewportId);
    if (vpIt == g_tags.end()) return;
    sl::ViewportHandle vp(CanonicalViewport(viewportId));
    for (auto& kv : vpIt->second)
    {
        StoredTag& st = kv.second;
        if (!st.used) continue;
        sl::ResourceTag tag(&st.res, (sl::BufferType)kv.first, sl::ResourceLifecycle::eValidUntilPresent,
                            st.hasExtent ? &st.extent : nullptr);
        sl::Result r = g_sl2.setTagForFrame(*token, vp, &tag, 1, nullptr);
        if (logit && r != sl::Result::eOk)
            LOGE("slSetTagForFrame(reissue type=%u) failed: %d", kv.first, (int)r);
    }
}

static sl::Boolean BoolToSL2(uint8_t v)
{
    return v ? sl::Boolean::eTrue : sl::Boolean::eFalse;
}

// ============================================================================
//  SL2 bootstrap
// ============================================================================
static FARPROC Resolve(HMODULE m, const char* name)
{
    FARPROC p = GetProcAddress(m, name);
    if (!p) LOGD("sl2 export not found: %s", name);
    return p;
}

static uint64_t InterposerFileSDKVersion()
{
    // kSDKVersion = major<<48 | minor<<32 | patch<<16 | 0xfedc
    std::wstring path = g_dir + L"\\" + g_cfg.pluginDir + L"\\sl.interposer.dll";
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return 0;
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return 0;
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) || !ffi) return 0;
    uint64_t major = HIWORD(ffi->dwFileVersionMS), minor = LOWORD(ffi->dwFileVersionMS);
    uint64_t patch = HIWORD(ffi->dwFileVersionLS);
    return (major << 48) | (minor << 32) | (patch << 16) | 0xfedcULL;
}

// Host SDK version the bridge reports to slInit (ini: HostSDK).
//
// "1.5.0" (default) is EXACTLY the value NVIDIA's own sl.interposer redirection
// reports for legacy SL1 titles, and SL 2.x branches on it:
//   * version < 2.0.0  - the D3D12/DXGI proxy exports register the device with the
//     plugin manager automatically (the SL1-era contract), so the engine's custom
//     D3D12 functions path works without a slSetD3DDevice call that SL1 games cannot
//     make.  Plugin initialization then happens lazily on the first hooked call.
//   * version == 1.5.0 - slGetFeatureFunction skips the "feature has not been
//     initialized yet" gate, so slDLSSGSetOptions/slReflexSetOptions can be resolved
//     before the device exists.
// Reporting the SL2 SDK version instead disables both behaviours (SL2 then expects
// the host to call slSetD3DDevice explicitly, as a native SL2 integration would).
static constexpr uint64_t kLegacyHostSDK = (uint64_t(1) << 48) | (uint64_t(5) << 32) | (uint64_t(0) << 16) | 0xfedcULL;

// "file" -> 0 (caller resolves via InterposerFileSDKVersion), "sdk"/"2" -> SL2 SDK
// version of the build headers, "x.y.z" -> that literal version.
static uint64_t ParseHostSDK(const std::wstring& v)
{
    if (!_wcsicmp(v.c_str(), L"sdk") || !_wcsicmp(v.c_str(), L"latest") || v == L"2") return sl::kSDKVersion;
    if (!_wcsicmp(v.c_str(), L"file")) return 0;
    unsigned a = 0, b = 0, c = 0;
    if (swscanf_s(v.c_str(), L"%u.%u.%u", &a, &b, &c) >= 2)
        return (uint64_t(a) << 48) | (uint64_t(b) << 32) | (uint64_t(c) << 16) | 0xfedcULL;
    return kLegacyHostSDK;      // unknown -> the legacy value this bridge requires
}

static bool LoadSl2()
{
    std::wstring path = g_dir + L"\\" + g_cfg.pluginDir + L"\\sl.interposer.dll";
    LOGI("loading SL2 interposer: %ls", path.c_str());
    g_sl2.mod = LoadLibraryW(path.c_str());
    if (!g_sl2.mod)
    {
        LOGE("LoadLibraryW failed (%lu) for %ls", GetLastError(), path.c_str());
        return false;
    }
    g_sl2.init              = (pfn_slInit)             Resolve(g_sl2.mod, "slInit");
    g_sl2.shutdown          = (pfn_slShutdown)         Resolve(g_sl2.mod, "slShutdown");
    g_sl2.setConstants      = (pfn_slSetConstants)     Resolve(g_sl2.mod, "slSetConstants");
    g_sl2.setTagForFrame    = (pfn_slSetTagForFrame)   Resolve(g_sl2.mod, "slSetTagForFrame");
    g_sl2.evaluateFeature   = (pfn_slEvaluateFeature)  Resolve(g_sl2.mod, "slEvaluateFeature");
    g_sl2.allocateResources = (pfn_slAllocateResources)Resolve(g_sl2.mod, "slAllocateResources");
    g_sl2.freeResources     = (pfn_slFreeResources)    Resolve(g_sl2.mod, "slFreeResources");
    g_sl2.isFeatureSupported= (pfn_slIsFeatureSupported)Resolve(g_sl2.mod, "slIsFeatureSupported");
    g_sl2.isFeatureLoaded   = (pfn_slIsFeatureLoaded)  Resolve(g_sl2.mod, "slIsFeatureLoaded");
    g_sl2.setFeatureLoaded  = (pfn_slSetFeatureLoaded) Resolve(g_sl2.mod, "slSetFeatureLoaded");
    g_sl2.getFeatureFunction= (pfn_slGetFeatureFunction)Resolve(g_sl2.mod, "slGetFeatureFunction");
    g_sl2.getNewFrameToken  = (pfn_slGetNewFrameToken) Resolve(g_sl2.mod, "slGetNewFrameToken");
    g_sl2.setD3DDevice      = (pfn_slSetD3DDevice)     Resolve(g_sl2.mod, "slSetD3DDevice");
    if (!g_sl2.init || !g_sl2.setConstants || !g_sl2.setTagForFrame)
    {
        LOGE("essential SL2 exports missing - aborting init");
        return false;
    }
    return true;
}

// A feature function resolved through SL2 can be stale: SL2 hands out pointers for
// plugins that are not (or are no longer) mapped, and calling one jumps into unmapped
// memory (verified: EXECUTE AV into a non-image hole on the first slDLSSGSetOptions
// call). Accept only pointers that are executable code inside a live module image
// right now; every call site below re-checks before invoking.
static bool IsValidCodePtr(const void* p)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    if ((mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    HMODULE mod = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)p, &mod) != 0 && mod != nullptr;
}
static inline bool PtrUsable(const void* p) { return IsValidCodePtr(p); }

static void ResolveFeatureFunctions()
{
    if (!g_sl2.getFeatureFunction) return;

    static const void* g_badFn[6] = {};
    auto resolveOne = [](sl::Feature feat, const char* name, void** slot, int idx)
    {
        void* f = nullptr;
        sl::Result r = g_sl2.getFeatureFunction(feat, name, f);
        if (r != sl::Result::eOk || !f) return false;       // no answer yet - keep any previous value
        if (!IsValidCodePtr(f))                             // pointer we must never call - drop it
        {
            if (g_badFn[idx] != f)
            {
                g_badFn[idx] = f;
                LOGE("feature fn %s rejected: %p is not executable code in a live module (stale pointer from SL2)", name, f);
            }
            *slot = nullptr;
            return false;
        }
        g_badFn[idx] = nullptr;
        *slot = f;
        return true;
    };

    resolveOne(sl::kFeatureDLSS_G, "slDLSSGSetOptions",  (void**)&g_sl2.dlssgSetOptions,  0);
    resolveOne(sl::kFeatureDLSS_G, "slDLSSGGetState",    (void**)&g_sl2.dlssgGetState,    1);
    resolveOne(sl::kFeatureReflex, "slReflexSetOptions", (void**)&g_sl2.reflexSetOptions, 2);
    resolveOne(sl::kFeatureReflex, "slReflexGetState",   (void**)&g_sl2.reflexGetState,   3);
    resolveOne(sl::kFeatureReflex, "slReflexSleep",      (void**)&g_sl2.reflexSleep,      4);
    resolveOne(sl::kFeatureReflex, "slReflexSetMarker",  (void**)&g_sl2.reflexSetMarker,  5);

    LOGD("feature functions resolved: dlssgSet=%d dlssgGet=%d reflexSet=%d reflexGet=%d reflexSleep=%d reflexSetMarker=%d",
         g_sl2.dlssgSetOptions ? 1 : 0, g_sl2.dlssgGetState ? 1 : 0, g_sl2.reflexSetOptions ? 1 : 0,
         g_sl2.reflexGetState ? 1 : 0, g_sl2.reflexSleep ? 1 : 0, g_sl2.reflexSetMarker ? 1 : 0);
}

// SL2 only hands out feature functions once a device/swap-chain exists, so this is
// retried on first use instead of once during slInit. Pointers are re-validated
// periodically because a resolved pointer dies when its plugin module is unloaded.
static int g_featureFnAttempts = 0;
static bool g_featureFnsLogged = false;
static bool g_fnsReady = false;
static DWORD g_fnsCheckTick = 0;
static DWORD g_lastFnAttemptTick = 0;

static bool FnsAllReady()
{
    return g_sl2.dlssgSetOptions && IsValidCodePtr((const void*)g_sl2.dlssgSetOptions)
        && g_sl2.dlssgGetState && IsValidCodePtr((const void*)g_sl2.dlssgGetState)
        && g_sl2.reflexSetOptions && IsValidCodePtr((const void*)g_sl2.reflexSetOptions)
        && g_sl2.reflexGetState && IsValidCodePtr((const void*)g_sl2.reflexGetState)
        && g_sl2.reflexSleep && IsValidCodePtr((const void*)g_sl2.reflexSleep);
}

static void EnsureFeatureFns()
{
    DWORD now = GetTickCount();
    if (g_fnsReady && (now - g_fnsCheckTick) < 250) return;     // cheap fast path
    g_fnsCheckTick = now;

    if (FnsAllReady())
    {
        g_fnsReady = true;
        if (!g_featureFnsLogged)
        {
            g_featureFnsLogged = true;
            LOGI("feature functions ready after %d attempt(s)", g_featureFnAttempts);
        }
        return;
    }
    g_fnsReady = false;

    if (g_lastFnAttemptTick != 0 && (now - g_lastFnAttemptTick) < 500) return;
    g_lastFnAttemptTick = now ? now : 1;
    g_featureFnAttempts++;
    ResolveFeatureFunctions();
}

// ============================================================================
//  translation: constants
// ============================================================================
static bool TranslateConstants(const Sl1Constants& s1, sl::Constants& out)
{
    memcpy(&out.cameraViewToClip,   s1.cameraViewToClip,   64);
    memcpy(&out.clipToCameraView,   s1.clipToCameraView,   64);
    memcpy(&out.clipToLensClip,     s1.clipToLensClip,     64);
    memcpy(&out.clipToPrevClip,     s1.clipToPrevClip,     64);
    memcpy(&out.prevClipToClip,     s1.prevClipToClip,     64);

    out.jitterOffset.x = s1.jitterOffset[0];
    out.jitterOffset.y = s1.jitterOffset[1];
    out.mvecScale.x    = s1.mvecScale[0];
    out.mvecScale.y    = s1.mvecScale[1];
    out.cameraPinholeOffset.x = s1.cameraPinholeOffset[0];
    out.cameraPinholeOffset.y = s1.cameraPinholeOffset[1];
    out.cameraPos.x = s1.cameraPos[0]; out.cameraPos.y = s1.cameraPos[1]; out.cameraPos.z = s1.cameraPos[2];
    out.cameraUp.x  = s1.cameraUp[0];  out.cameraUp.y  = s1.cameraUp[1];  out.cameraUp.z  = s1.cameraUp[2];
    out.cameraRight.x = s1.cameraRight[0]; out.cameraRight.y = s1.cameraRight[1]; out.cameraRight.z = s1.cameraRight[2];
    out.cameraFwd.x = s1.cameraFwd[0]; out.cameraFwd.y = s1.cameraFwd[1]; out.cameraFwd.z = s1.cameraFwd[2];

    out.cameraNear        = s1.cameraNear;
    out.cameraFar         = s1.cameraFar;
    out.cameraFOV         = s1.cameraFOV;
    out.cameraAspectRatio = s1.cameraAspectRatio;
    out.motionVectorsInvalidValue = s1.motionVectorsInvalidValue;

    out.depthInverted         = BoolToSL2(s1.depthInverted);
    out.cameraMotionIncluded  = BoolToSL2(s1.cameraMotionIncluded);
    out.motionVectors3D       = sl::Boolean::eFalse; // SL1 left this uninitialised; game MVs are 2D
    out.reset                 = BoolToSL2(s1.reset);
    out.orthographicProjection= BoolToSL2(s1.orthographicProjection);
    out.motionVectorsDilated  = BoolToSL2(s1.motionVectorsDilated);
    out.motionVectorsJittered = BoolToSL2(s1.motionVectorsJittered);
    // SL2 v2 member: leave engine default (40.0f, set by the header default init)
    return true;
}

// ============================================================================
//  translation: tags
// ============================================================================
static void StoreTag(uint32_t viewportId, uint32_t type, const Sl1Resource* r, const Sl1Extent* e)
{
    auto& slot = g_tags[viewportId][type];
    if (!r)
    {
        slot.used = false;
        return;
    }
    slot.used = true;
    slot.res = sl::Resource((sl::ResourceType)r->type, r->native, r->memory, (void*)(uintptr_t)r->view, r->state);
    // fill optional description (helps SL with format/size, harmless if unknown)
    if (r->native)
    {
        ID3D12Resource* nres = (ID3D12Resource*)r->native;
        D3D12_RESOURCE_DESC d = nres->GetDesc();
        slot.res.width = (uint32_t)d.Width;
        slot.res.height = d.Height;
        slot.res.nativeFormat = (uint32_t)d.Format;
        slot.res.mipLevels = d.MipLevels;
        slot.res.arrayLayers = d.DepthOrArraySize;
        if (type == 0 /*depth*/ || type == 1 /*mvecs*/)
        {
            if (Throttle(4000 + type, 8, 1200))
                LOGD("tag type=%u native=%p %ux%u fmt=%u", type, r->native, slot.res.width, slot.res.height, slot.res.nativeFormat);
        }
    }
    if (e)
    {
        slot.extent.left   = (decltype(slot.extent.left))e->left;
        slot.extent.top    = (decltype(slot.extent.top))e->top;
        slot.extent.width  = (decltype(slot.extent.width))e->width;
        slot.extent.height = (decltype(slot.extent.height))e->height;
        slot.hasExtent = true;
    }
    else
    {
        slot.hasExtent = false;
    }
}

// ============================================================================
//  translation: DLSS-G options
// ============================================================================
static void FillDlssgFromTags(sl::DLSSGOptions& o, uint32_t viewportId)
{
    auto vpIt = g_tags.find(viewportId);
    if (vpIt == g_tags.end()) return;
    for (auto& kv : vpIt->second)
    {
        const StoredTag& st = kv.second;
        if (!st.used || !st.res.native) continue;
        switch (kv.first)
        {
        case 0: // depth
            o.depthBufferFormat = st.res.nativeFormat;
            o.mvecDepthWidth = st.res.width; o.mvecDepthHeight = st.res.height;
            break;
        case 1: // motion vectors
            o.mvecBufferFormat = st.res.nativeFormat;
            if (!o.mvecDepthWidth) { o.mvecDepthWidth = st.res.width; o.mvecDepthHeight = st.res.height; }
            break;
        case 2: // hudless color
            o.hudLessBufferFormat = st.res.nativeFormat;
            if (!o.colorWidth) { o.colorWidth = st.res.width; o.colorHeight = st.res.height; }
            break;
        case 23: // ui color and alpha
            o.uiBufferFormat = st.res.nativeFormat;
            if (g_cfg.enableUIRecomposition) o.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
            break;
        default: break;
        }
    }
}

static void ApplyReflexOptions(const Sl1ReflexConstants& rc, const char* why);
static void EnableReflexDefault(const char* why);

static void ApplyDlssgOptions(uint32_t mode, uint32_t numFrames, uint32_t viewportId, const char* why)
{
    EnsureFeatureFns();
    if (!PtrUsable((const void*)g_sl2.dlssgSetOptions)) { LOGE("dlssgSetOptions unavailable/stale (%s)", why); return; }

    sl::DLSSGOptions o{};                       // structVersion 5 by default
    o.mode = (sl::DLSSGMode)mode;
    o.numFramesToGenerate = numFrames ? numFrames : 1;
    o.flags = (sl::DLSSGFlags)(g_cfg.forceFlags >= 0 ? g_cfg.forceFlags
                                                     : (int)sl::DLSSGFlags::eEnableFullscreenMenuDetection);
    if (g_cfg.forceNumFrames > 0) o.numFramesToGenerate = (uint32_t)g_cfg.forceNumFrames;
    if (g_cfg.forceMode >= 0) o.mode = (sl::DLSSGMode)g_cfg.forceMode;
    if (g_cfg.forceDynamicFps > 0) o.dynamicTargetFrameRate = (float)g_cfg.forceDynamicFps;
    FillDlssgFromTags(o, viewportId);

    sl::ViewportHandle vp(CanonicalViewport(viewportId));
    sl::Result r = g_sl2.dlssgSetOptions(vp, o);
    static uint32_t lastSig = 0xFFFFFFFFu;
    uint32_t sig = (uint32_t)o.mode | (o.numFramesToGenerate << 8) | ((uint32_t)o.flags << 16) |
                   ((uint32_t)(r != sl::Result::eOk) << 31);
    bool changed = (sig != lastSig); lastSig = sig;
    if (r != sl::Result::eOk)
    {
        if (changed || Throttle(3000, 6, 900)) LOGE("slDLSSGSetOptions failed: %d (%s)", (int)r, why);
    }
    else if (changed || Throttle(3001, 6, 900))
    {
        LOGI("DLSS-G options: mode=%u(%s) frames=%u uir=%d flags=0x%X mvecDepth=%ux%u color=%ux%u (%s)",
              (uint32_t)o.mode, why, o.numFramesToGenerate,
              o.enableUserInterfaceRecomposition == sl::Boolean::eTrue ? 1 : 0,
              (uint32_t)o.flags, o.mvecDepthWidth, o.mvecDepthHeight, o.colorWidth, o.colorHeight, why);
    }

    // DLSS-G cannot run without Reflex: satisfy the requirement automatically
    if (o.mode != sl::DLSSGMode::eOff && g_cfg.forwardReflex)
    {
        if (g_cfg.forceReflexMode >= 0 && g_reflexModeSeen != g_cfg.forceReflexMode)
        {
            Sl1ReflexConstants rc{};
            rc.mode = (uint32_t)g_cfg.forceReflexMode;
            rc.useMarkersToOptimize = 1;
            ApplyReflexOptions(rc, "ini force");
        }
        else if (g_cfg.autoReflexWithFG && g_reflexModeSeen <= 0)
        {
            EnableReflexDefault("auto: DLSS-G requires Reflex");
        }
    }
    if (o.mode != sl::DLSSGMode::eOff && g_cfg.forwardReflex)
    {
        if (PtrUsable((const void*)g_sl2.reflexGetState))
        {
            sl::ReflexState st{};
            if (g_sl2.reflexGetState(st) == sl::Result::eOk && !st.lowLatencyAvailable &&
                Throttle(3100, 6, 900))
                LOGE("DLSS-G requested but Reflex reports lowLatencyAvailable=false - FG will fail");
        }
    }
}

static void ApplyDlssgOptionsFromApp(const Sl1DlssgOptions& appOpts, uint32_t viewportId, const char* why)
{
    ApplyDlssgOptions(appOpts.mode, appOpts.numFramesToGenerate, viewportId, why);
}

// ============================================================================
//  translation: Reflex
// ============================================================================
static void ApplyReflexOptions(const Sl1ReflexConstants& rc, const char* why)
{
    if (!g_cfg.forwardReflex) { LOGI("reflex options dropped (ForwardReflex=0)"); return; }
    EnsureFeatureFns();
    if (!PtrUsable((const void*)g_sl2.reflexSetOptions)) { LOGE("reflexSetOptions unavailable/stale (%s)", why); return; }
    sl::ReflexOptions ro{};
    ro.mode = (sl::ReflexMode)(g_cfg.forceReflexMode >= 0 ? g_cfg.forceReflexMode : rc.mode); // 0/1/2 identical in SL1 and SL2
    ro.frameLimitUs = rc.frameLimitUs;
    ro.useMarkersToOptimize = rc.useMarkersToOptimize != 0;
    ro.virtualKey = rc.virtualKey;
    ro.idThread = 0;
    sl::Result r = g_sl2.reflexSetOptions(ro);
    static uint32_t lastSig = 0xFFFFFFFFu;
    uint32_t sig = (uint32_t)ro.mode | (ro.frameLimitUs << 4) | ((uint32_t)ro.useMarkersToOptimize << 2) |
                   ((uint32_t)ro.virtualKey << 24) | ((uint32_t)(r != sl::Result::eOk) << 30);
    bool changed = (sig != lastSig); lastSig = sig;
    if (r != sl::Result::eOk)
    {
        if (changed || Throttle(3200, 6, 900)) LOGE("slReflexSetOptions failed: %d (%s)", (int)r, why);
    }
    else
    {
        g_reflexModeSeen = (int)ro.mode;
        if (changed || Throttle(3201, 6, 900))
            LOGI("reflex options: mode=%u frameLimitUs=%u markers=%d vk=0x%X (%s)",
                 (uint32_t)ro.mode, ro.frameLimitUs, ro.useMarkersToOptimize ? 1 : 0, ro.virtualKey, why);
    }
}

static void EnableReflexDefault(const char* why)
{
    Sl1ReflexConstants rc{};
    rc.mode = 1;                    // eLowLatency
    rc.frameLimitUs = 0;
    rc.useMarkersToOptimize = 1;
    rc.virtualKey = 0;
    ApplyReflexOptions(rc, why);
}

// ============================================================================
//  D3D / DXGI proxy exports
//
//  Returnal's StreamlineD3D12RHIPreInit resolves exactly these names from
//  sl.interposer.dll and installs them as the engine's custom device/factory
//  creation functions (D3D12Bridge / D3D11Bridge / DXGIBridge):
//      CreateDXGIFactory, CreateDXGIFactory1, CreateDXGIFactory2,
//      DXGIGetDebugInterface1, D3D12CreateDevice, D3D11CreateDevice
//  The SL 1.x interposer implemented them as real proxies.  SL 2.14 exports
//  the same set, so forward to it: it performs the real creation *and*
//  registers the object with Streamline.  System32 is only a last resort for
//  an export SL2 does not provide.
// ============================================================================
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

static void* ProxyProc(const char* name, const wchar_t* sysDll)
{
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.mod)
    {
        void* p = (void*)GetProcAddress(g_sl2.mod, name);
        if (p) return p;
    }
    HMODULE h = LoadLibraryExW(sysDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return h ? (void*)GetProcAddress(h, name) : nullptr;
}

// ============================================================================
//  SL1 API implementations (see sl1_bridge.def for the exported names)
// ============================================================================
extern "C" {

bool Bridge_Init(const void* sl1Prefs, int appId)   // bool sl::init(const sl::Preferences&, int)
{
    (void)sl1Prefs;
    if (g_initOk) { LOGI("sl::init called again - ignored"); return true; }

    if (!g_started)
    {
        g_started = true;
        ComputeSelfDir();
        LoadConfig();
        OpenLog();
        LOGI("==== sl1_bridge start (SL1 1.3.3 -> SL2 2.14.16) ====");
        LOGI("dir=%ls logLevel=%d pluginDir=%ls forceMode=%d forceNumFrames=%d uir=%d",
             g_dir.c_str(), g_cfg.logLevel, g_cfg.pluginDir.c_str(), g_cfg.forceMode,
             g_cfg.forceNumFrames, g_cfg.enableUIRecomposition);
    }
    if (!g_sl2.mod && !LoadSl2()) return false;

    // --- build SL2 preferences ---
    static std::wstring pluginPath;
    static std::wstring logPath;
    static const wchar_t* paths[1] = { nullptr };
    pluginPath = g_dir + L"\\" + g_cfg.pluginDir;
    logPath    = g_dir + L"\\" + g_cfg.logDir;
    if (g_cfg.logPerProcess)
    {
        logPath += L"\\";
        logPath += std::to_wstring((unsigned long)GetCurrentProcessId());
    }
    // SL2 does not create its logs/data directory - without the leaf dir it silently
    // fails to write sl.log at all (observed: empty sl2logs\<pid> after every run).
    CreateDirectoryW(logPath.c_str(), nullptr);
    paths[0]   = pluginPath.c_str();
    LOGI("sl2 logs+data dir: %ls", logPath.c_str());

    static sl::Feature feats[2] = { sl::kFeatureDLSS_G, sl::kFeatureReflex };

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = (g_cfg.logLevel >= 2) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = logPath.c_str();
    pref.allocateCallback = nullptr;   // SL1 callback signatures differ from SL2 - never forwarded
    pref.releaseCallback = nullptr;
    pref.logMessageCallback = nullptr;
    pref.flags = sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    if (g_cfg.bypassOSCheck) pref.flags |= sl::PreferenceFlags::eBypassOSVersionCheck;
    if (g_cfg.dxgiFactoryProxy)
    {
        pref.flags |= sl::PreferenceFlags::eUseDXGIFactoryProxy;
        LOGI("flags: eUseDXGIFactoryProxy enabled (overlay-injection compatible mode)");
    }
    pref.featuresToLoad = feats;
    pref.numFeaturesToLoad = 2;
    // --- NGX application identity -------------------------------------------------
    // Relay the id the game passes to slInit; if it is 0 SL2 maps it to
    // kTemporaryAppId and production builds disable NGX, so substitute a non-zero
    // stand-in (NGX accepts unregistered ids - stock SL1 passed 0 for years).
    int gameAppId = appId;
    int effAppId  = (g_cfg.appIdOverride >= 0) ? g_cfg.appIdOverride : gameAppId;
    bool useProjectId = !g_cfg.projectIdA.empty() && g_cfg.projectIdA != "0";
    if (!useProjectId && effAppId == 0)
        effAppId = 100721531 + 1;   // kTemporaryAppId + 1: non-zero, never the "temporary" id
    pref.applicationId = (uint32_t)effAppId;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = useProjectId ? g_cfg.projectIdA.c_str() : nullptr;
    LOGI("app identity: game appId=%d, ini AppId=%d -> applicationId=%d, projectId=%s (%s)",
         gameAppId, g_cfg.appIdOverride, effAppId,
         useProjectId ? g_cfg.projectIdA.c_str() : "(none)",
         useProjectId ? "Init_with_ProjectID" : "applicationId path");
    pref.renderAPI = sl::RenderAPI::eD3D12;

    uint64_t sdk = ParseHostSDK(g_cfg.hostSDK);
    if (!sdk) sdk = InterposerFileSDKVersion();
    if (!sdk) sdk = sl::kSDKVersion;

    sl::Result r = g_sl2.init(pref, sdk);
    LOGI("sl2 slInit(sdk=0x%llX -> host %u.%u.%u, ini '%ls') -> %d", (unsigned long long)sdk,
         (unsigned)((sdk >> 48) & 0xffff), (unsigned)((sdk >> 32) & 0xffff), (unsigned)((sdk >> 16) & 0xffff),
         g_cfg.hostSDK.c_str(), (int)r);
    if (r != sl::Result::eOk)
    {
        uint64_t fileSdk = InterposerFileSDKVersion();
        if (fileSdk && fileSdk != sdk)
        {
            LOGI("retrying slInit with interposer file version 0x%llX", (unsigned long long)fileSdk);
            r = g_sl2.init(pref, fileSdk);
            LOGI("sl2 slInit(file sdk) -> %d", (int)r);
        }
    }
    if (r != sl::Result::eOk) { LOGE("SL2 init failed - bridge disabled"); return false; }

    ResolveFeatureFunctions();
    g_initOk = true;
    LOGI("SL2 ready");
    return true;
}

bool Bridge_Shutdown()
{
    if (!g_initOk) return false;
    LOGI("sl::shutdown");
    if (g_sl2.shutdown) g_sl2.shutdown();
    g_initOk = false;
    return true;
}

bool Bridge_SetTag(const Sl1Resource* resource, uint32_t bufferType, uint32_t id, const Sl1Extent* extent)
{
    if (!g_initOk) return false;
    uint32_t vpId = CanonicalViewport(id);
    if (Throttle(4100 + bufferType, 8, 1200))
        LOGD("setTag(type=%u id=%u -> vp=%u res=%p extent=%s)", bufferType, id, vpId,
             resource ? resource->native : nullptr, extent ? "yes" : "no");

    // apply to the current frame so the tag is live immediately
    sl::FrameToken* tok = g_haveFrame ? g_lastFrame : GetFrameToken(0, false);
    sl::ViewportHandle vp(vpId);

    if (resource)
    {
        StoreTag(vpId, bufferType, resource, extent);
        if (tok && g_sl2.setTagForFrame)
        {
            auto& slot = g_tags[vpId][bufferType];
            sl::ResourceTag tag(&slot.res, (sl::BufferType)bufferType,
                                sl::ResourceLifecycle::eValidUntilPresent,
                                slot.hasExtent ? &slot.extent : nullptr);
            sl::Result r = g_sl2.setTagForFrame(*tok, vp, &tag, 1, nullptr);
            if (r != sl::Result::eOk)
                LOGE("slSetTagForFrame(type=%u) failed: %d", bufferType, (int)r);
        }
    }
    else
    {
        // null resource removes the tag: drop the stored tag and tell SL2 as well
        auto vpIt = g_tags.find(vpId);
        if (vpIt != g_tags.end()) vpIt->second.erase(bufferType);
        if (tok && g_sl2.setTagForFrame)
        {
            sl::ResourceTag tag(nullptr, (sl::BufferType)bufferType, sl::ResourceLifecycle::eValidUntilPresent, nullptr);
            g_sl2.setTagForFrame(*tok, vp, &tag, 1, nullptr);
        }
    }
    return true;
}

bool Bridge_SetConstants(const Sl1Constants& c, uint32_t frameIndex, uint32_t id)
{
    if (!g_initOk) return false;
    EnsureFeatureFns();
    uint32_t vpId = CanonicalViewport(id);

    sl::FrameToken* tok = GetFrameToken(frameIndex, true);
    if (!tok) return false;

    if (!g_haveFrame || frameIndex != g_lastFrameIndex)
    {
        g_haveFrame = true;
        g_lastFrame = tok;
        g_lastFrameIndex = frameIndex;
        g_frameCounter++;
        ReissueTags(tok, vpId, false);      // SL1 tags are sticky: replay them every frame

        // Periodic DLSS-G state probe: numFramesToGenerateMax exposes the driver's
        // frame-generation multiplier (the override) and numFramesActuallyPresented
        // proves frames are really being generated; log on change or occasionally.
        if (g_sl2.dlssgGetState && (g_frameCounter % 300) == 0)
        {
            sl::DLSSGState st{};
            sl::ViewportHandle vp(vpId);
            if (g_sl2.dlssgGetState(vp, st, nullptr) == sl::Result::eOk)
            {
                static uint32_t lastSig = 0xFFFFFFFFu;
                uint32_t sig = (uint32_t)st.status | (st.numFramesToGenerateMax << 8) |
                               ((uint32_t)st.bIsDynamicMFGSupported << 16);
                bool ch = (sig != lastSig); lastSig = sig;
                if (ch || Throttle(5000, 4, 20))
                    LOGI("DLSS-G state: status=0x%X presented=%u max=%u dynamic=%d vram=%lluMB",
                         (uint32_t)st.status, st.numFramesActuallyPresented, st.numFramesToGenerateMax,
                         st.bIsDynamicMFGSupported == sl::Boolean::eTrue ? 1 : 0,
                         (unsigned long long)(st.estimatedVRAMUsageInBytes / (1024ull * 1024ull)));
            }
        }
    }

    sl::Constants s2{};
    TranslateConstants(c, s2);
    sl::ViewportHandle vp(vpId);
    sl::Result r = g_sl2.setConstants(s2, *tok, vp);
    if (r != sl::Result::eOk)
        LOGE("slSetConstants(frame=%u vp=%u) failed: %d", frameIndex, vpId, (int)r);

    if (g_frameCounter <= 5 || (g_frameCounter % 300) == 0)
    {
        LOGI("frame %u (idx=%u): depthInv=%u camMotion=%u reset=%u ortho=%u dilated=%u jittered=%u jitter=(%.3f,%.3f) near=%.3f far=%.1f fov=%.4f ar=%.4f",
             g_frameCounter, frameIndex, c.depthInverted, c.cameraMotionIncluded, c.reset,
             c.orthographicProjection, c.motionVectorsDilated, c.motionVectorsJittered,
             c.jitterOffset[0], c.jitterOffset[1], c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio);
    }
    return true;
}

bool Bridge_SetFeatureConstants(uint32_t feature, const void* data, uint32_t frameIndex, uint32_t id)
{
    if (!g_initOk) return false;
    if (!data) { LOGD("setFeatureConstants(feature=%u) null data", feature); return false; }

    if (feature == sl::kFeatureDLSS_G)
    {
        const Sl1DlssgOptions& o = *(const Sl1DlssgOptions*)data;
        static uint32_t lastDlssgSig = 0xFFFFFFFFu;
        uint32_t dlssgSig = o.mode | (o.numFramesToGenerate << 8) | (o.flags << 16);
        bool dlssgChanged = (dlssgSig != lastDlssgSig); lastDlssgSig = dlssgSig;
        if (dlssgChanged || Throttle(2000, 6, 900))
            LOGD("setFeatureConstants(DLSSG) mode=%u frames=%u id=%u frame=%u", o.mode, o.numFramesToGenerate, id, frameIndex);
        // The game submits the constants once per viewport; with the canonical viewport
        // both collapse onto viewport 0 and SL2 warns "Repeated slDLSSGSetOptions() call
        // for the frame". Skip byte-identical duplicates for the same frame.
        static uint32_t lastFwdFrame = 0xFFFFFFFFu;
        static uint32_t lastFwdSig = 0xFFFFFFFFu;
        uint32_t fwdSig = o.mode | (o.numFramesToGenerate << 8) | (o.flags << 16);
        if (g_cfg.canonicalViewport && frameIndex == lastFwdFrame && fwdSig == lastFwdSig)
            return true;                    // duplicate call for this frame - already forwarded
        lastFwdFrame = frameIndex;
        lastFwdSig = fwdSig;
        ApplyDlssgOptionsFromApp(o, CanonicalViewport(id), "app");
        return true;
    }
    if (feature == sl::kFeatureReflex)
    {
        const Sl1ReflexConstants& rc = *(const Sl1ReflexConstants*)data;
        static uint32_t lastReflexSig = 0xFFFFFFFFu;
        uint32_t reflexSig = rc.mode | (rc.frameLimitUs << 4) | ((uint32_t)rc.useMarkersToOptimize << 2) |
                             ((uint32_t)rc.virtualKey << 24);
        bool reflexChanged = (reflexSig != lastReflexSig); lastReflexSig = reflexSig;
        if (reflexChanged || Throttle(2100, 6, 900))
            LOGD("setFeatureConstants(Reflex) mode=%u limit=%u markers=%d vk=0x%X", rc.mode, rc.frameLimitUs, rc.useMarkersToOptimize, rc.virtualKey);
        ApplyReflexOptions(rc, "app");
        return true;
    }

    // other features: forward constants through the modern call so SL still sees them
    LOGI("setFeatureConstants(feature=%u) - unmapped, ignored", feature);
    return false;
}

bool Bridge_GetFeatureSettings(uint32_t feature, const void* in, void* out)
{
    if (!g_initOk) return false;
    EnsureFeatureFns();
    if (feature == sl::kFeatureReflex)
    {
        if (out)
        {
            uint8_t* p = (uint8_t*)out;
            bool lowLat = false; uint32_t winMsg = 0;
            if (PtrUsable((const void*)g_sl2.reflexGetState))
            {
                sl::ReflexState st{};
                if (g_sl2.reflexGetState(st) == sl::Result::eOk) { lowLat = st.lowLatencyAvailable; winMsg = st.statsWindowMessage; }
            }
            p[0] = lowLat ? 1 : 0;
            p[1] = 0;                       // latency report gated off (report array not written - size safety)
            *(uint32_t*)(p + 4) = winMsg;
        }
        return true;
    }
    if (feature == sl::kFeatureDLSS_G)
    {
        // SL1 read a float frame time at +8 of a pre-zeroed buffer; leaving it zero is safe.
        if (PtrUsable((const void*)g_sl2.dlssgGetState) && (g_frameCounter <= 5 || (g_frameCounter % 300) == 0))
        {
            sl::DLSSGState st{};
            sl::ViewportHandle vp(0);
            if (g_sl2.dlssgGetState(vp, st, nullptr) == sl::Result::eOk)
            {
                LOGI("DLSS-G state: status=0x%X numFramesActuallyPresented=%u numFramesToGenerateMax=%u dynamicMFG=%d vram=%llu",
                     (uint32_t)st.status, st.numFramesActuallyPresented, st.numFramesToGenerateMax,
                     st.bIsDynamicMFGSupported == sl::Boolean::eTrue ? 1 : 0,
                     (unsigned long long)st.estimatedVRAMUsageInBytes);
                if (st.status != g_lastFgStatus)
                {
                    g_lastFgStatus = st.status;
                    LOGI("DLSS-G status changed to 0x%X", (uint32_t)st.status);
                }
            }
        }
        return true;
    }
    LOGD("getFeatureSettings(feature=%u) - unmapped, returning false", feature);
    return false;
}

bool Bridge_EvaluateFeature(void* cmd, uint32_t feature, uint32_t frameIndex, uint32_t id)
{
    if (!g_initOk) return false;
    EnsureFeatureFns();
    sl::FrameToken* tok = GetFrameToken(frameIndex, false);
    if (!tok) return false;

    uint64_t key = ((uint64_t)feature << 32) | id;
    uint64_t& cnt = g_loggedOnce[key];
    if (cnt <= 3) LOGD("evaluateFeature(feature=%u id=0x%X frame=%u cmd=%p)", feature, id, frameIndex, cmd);
    cnt++;

    if (feature == sl::kFeatureReflex)
    {
        if (id == 0x1000)   // eReflexMarkerSleep
        {
            if (PtrUsable((const void*)g_sl2.reflexSleep))
            {
                sl::Result r = g_sl2.reflexSleep(*tok);
                if (r != sl::Result::eOk && cnt <= 3) LOGD("slReflexSleep -> %d", (int)r);
            }
            return true;
        }
        // 0..8 are ReflexMarker values; SL2's PCLMarker uses the same numbering
        // except 6 (InputSample) which no longer exists -> map to eControllerInputSample
        uint32_t marker = id;
        if (id == 6) marker = (uint32_t)sl::PCLMarker::eControllerInputSample;
        if (id < (uint32_t)sl::PCLMarker::eMaximum)
        {
            // SL2's reflex plugin registers no evaluateFeature callback, so an
            // slEvaluateFeature(kFeatureReflex) call dies with eErrorMissingOrInvalidAPI.
            // The supported path is the plugin's slReflexSetMarker (ReflexHelper +
            // FrameToken -> slSetData -> driver marker + present-frame latch).
            EnsureFeatureFns();
            if (PtrUsable((const void*)g_sl2.reflexSetMarker))
            {
                sl::Result r = g_sl2.reflexSetMarker((sl::PCLMarker)marker, *tok);
                if (r != sl::Result::eOk && cnt <= 3) LOGD("slReflexSetMarker(%u) -> %d", marker, (int)r);
            }
            else if (g_sl2.evaluateFeature)
            {
                // legacy fallback: chain the viewport so the call fails late, not on
                // the input check (SL2 answers eErrorMissingOrInvalidAPI regardless)
                sl::ViewportHandle vh(0);
                sl::ReflexHelper helper((sl::MarkerUnderlying)marker);
                const sl::BaseStructure* inputs[2] = { &vh, &helper };
                sl::Result r = g_sl2.evaluateFeature(sl::kFeatureReflex, *tok, inputs, 2, (sl::CommandBuffer*)cmd);
                if (r != sl::Result::eOk && cnt <= 3) LOGD("slEvaluateFeature(reflex marker %u) -> %d", marker, (int)r);
            }
            return true;
        }
        LOGD("evaluateFeature(reflex) unknown id 0x%X - ignored", id);
        return true;
    }

    // SL2 requires a ViewportHandle in the inputs of every slEvaluateFeature call,
    // so chain the canonical viewport for the DLSS-G and generic forwards too.
    if (feature == sl::kFeatureDLSS_G)
    {
        sl::ViewportHandle vh(CanonicalViewport(id));
        const sl::BaseStructure* inputs[1] = { &vh };
        if (g_sl2.evaluateFeature)
        {
            sl::Result r = g_sl2.evaluateFeature(sl::kFeatureDLSS_G, *tok, inputs, 1, (sl::CommandBuffer*)cmd);
            if (r != sl::Result::eOk && cnt <= 3) LOGD("slEvaluateFeature(DLSS-G) -> %d", (int)r);
        }
        return true;
    }

    if (g_sl2.evaluateFeature)
    {
        sl::ViewportHandle vh(CanonicalViewport(id));
        const sl::BaseStructure* inputs[1] = { &vh };
        return g_sl2.evaluateFeature(feature, *tok, inputs, 1, (sl::CommandBuffer*)cmd) == sl::Result::eOk;
    }
    return false;
}

bool Bridge_IsFeatureSupported(uint32_t feature, uint32_t* outMask)
{
    if (outMask) *outMask = 1;
    if (!g_initOk) { LOGD("isFeatureSupported(feature=%u) before init -> true", feature); return true; }

    if (feature == sl::kFeatureDLSS_G || feature == sl::kFeatureReflex)
    {
        if (g_sl2.isFeatureSupported)
        {
            sl::AdapterInfo ai{};
            sl::Result r = g_sl2.isFeatureSupported(feature, ai);
            static std::map<uint32_t, int> lastRes;   // log the value, not every poll
            auto it = lastRes.find(feature);
            bool resChanged = (it == lastRes.end() || it->second != (int)r);
            if (resChanged) lastRes[feature] = (int)r;
            if (resChanged || Throttle(1000 + (feature & 0x3FF), 8, 1200))
                LOGI("sl2 slIsFeatureSupported(feature=%u) -> %d (reported to app as supported)", feature, (int)r);
        }
        return true;    // app must be told yes so it wires up the feature
    }
    if (g_sl2.isFeatureSupported)
    {
        sl::AdapterInfo ai{};
        return g_sl2.isFeatureSupported(feature, ai) == sl::Result::eOk;
    }
    return false;
}

bool Bridge_IsFeatureEnabled(uint32_t feature)
{
    auto it = g_featureEnabled.find(feature);
    if (it != g_featureEnabled.end()) return it->second;
    return true;    // SL1 default: enabled when supported
}

bool Bridge_SetFeatureEnabled(uint32_t feature, bool enabled)
{
    g_featureEnabled[feature] = enabled;
    LOGI("setFeatureEnabled(feature=%u, %d)", feature, enabled ? 1 : 0);
    if (g_initOk && g_sl2.setFeatureLoaded)
    {
        // only apply once the device exists; the pre-device call is queued and applied on the first frame
        if (g_haveFrame)
        {
            sl::Result r = g_sl2.setFeatureLoaded(feature, enabled);
            if (r != sl::Result::eOk) LOGE("slSetFeatureLoaded(feature=%u,%d) -> %d", feature, enabled ? 1 : 0, (int)r);
            if (r == sl::Result::eOk && enabled)
            {
                // the plugin for this feature may only become real now - force an
                // immediate fresh resolution of the feature functions
                g_lastFnAttemptTick = 0;
                g_fnsReady = false;
                ResolveFeatureFunctions();
            }
        }
        else LOGI("setFeatureLoaded(feature=%u) deferred until device is up", feature);
    }
    return true;
}

bool Bridge_AllocateResources(void* cmd, uint32_t feature, uint32_t id)
{
    if (!g_initOk || !g_sl2.allocateResources) return false;
    sl::ViewportHandle vp(CanonicalViewport(id));
    sl::Result r = g_sl2.allocateResources((sl::CommandBuffer*)cmd, feature, vp);
    LOGD("allocateResources(feature=%u id=%u) -> %d", feature, id, (int)r);
    return r == sl::Result::eOk;
}

bool Bridge_FreeResources(uint32_t feature, uint32_t id)
{
    if (!g_initOk || !g_sl2.freeResources) return false;
    sl::ViewportHandle vp(CanonicalViewport(id));
    sl::Result r = g_sl2.freeResources(feature, vp);
    LOGD("freeResources(feature=%u id=%u) -> %d", feature, id, (int)r);
    return r == sl::Result::eOk;
}

// ---- SL2 passthroughs (for external SL2-aware components doing export probing) ----
bool Bridge_PassSetD3DDevice(void* dev)
{
    if (!g_sl2.mod && g_started == false) { ComputeSelfDir(); LoadConfig(); OpenLog(); g_started = true; }
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.setD3DDevice) { sl::Result r = g_sl2.setD3DDevice(dev); LOGI("passthrough slSetD3DDevice(%p) -> %d", dev, (int)r); return r == sl::Result::eOk; }
    return false;
}

bool Bridge_PassGetFeatureFunction(uint32_t feature, const char* name, void** out)
{
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.getFeatureFunction && out) { void* fn = nullptr; sl::Result r = g_sl2.getFeatureFunction(feature, name, fn); if (r == sl::Result::eOk) { *out = fn; return true; } }
    return false;
}

bool Bridge_PassSetFeatureLoaded(uint32_t feature, bool loaded)
{
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.setFeatureLoaded) return g_sl2.setFeatureLoaded(feature, loaded) == sl::Result::eOk;
    return false;
}

bool Bridge_PassIsFeatureLoaded(uint32_t feature, bool* loaded)
{
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.isFeatureLoaded && loaded) { bool l = false; if (g_sl2.isFeatureLoaded(feature, l) == sl::Result::eOk) { *loaded = l; return true; } }
    return false;
}

bool Bridge_PassGetNewFrameToken(void** token, const uint32_t* frameIndex)
{
    if (!g_sl2.mod) LoadSl2();
    if (g_sl2.getNewFrameToken && token)
    {
        sl::FrameToken* t = nullptr;
        sl::Result r = g_sl2.getNewFrameToken(t, frameIndex);
        if (r == sl::Result::eOk) { *token = t; return true; }
    }
    return false;
}

// ---- D3D/DXGI proxy implementations (resolved lazily, cached per export) ----
typedef HRESULT (WINAPI *PFN_CREATEDXGIFACTORY_RIID)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CREATEDXGIFACTORY_UINT)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12CREATE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12GETDEBUG)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12GETIFACE)(REFCLSID, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12SERIALIZE_RS)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_D3D12SERIALIZE_VRS)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_D3D12RS_DESER)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12EXPERIMENTAL)(UINT, const IID*, void*, UINT*);
typedef HRESULT (WINAPI *PFN_D3D11CREATE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_D3D11CREATE_SC)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

#define PROXY(ftype, fname, export, dll)        \
    static ftype fn = nullptr;                  \
    if (!fn) fn = (ftype)ProxyProc(export, dll)

HRESULT WINAPI Bridge_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    PROXY(PFN_CREATEDXGIFACTORY_RIID, CreateDXGIFactory, "CreateDXGIFactory", L"dxgi.dll");
    HRESULT hr = fn ? fn(riid, ppFactory) : E_FAIL;
    LOGD("proxy CreateDXGIFactory -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    PROXY(PFN_CREATEDXGIFACTORY_RIID, CreateDXGIFactory1, "CreateDXGIFactory1", L"dxgi.dll");
    HRESULT hr = fn ? fn(riid, ppFactory) : E_FAIL;
    LOGD("proxy CreateDXGIFactory1 -> 0x%08X factory=%p", (unsigned)hr, ppFactory ? *ppFactory : nullptr);
    return hr;
}

HRESULT WINAPI Bridge_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    PROXY(PFN_CREATEDXGIFACTORY_UINT, CreateDXGIFactory2, "CreateDXGIFactory2", L"dxgi.dll");
    HRESULT hr = fn ? fn(flags, riid, ppFactory) : E_FAIL;
    LOGD("proxy CreateDXGIFactory2(flags=0x%X) -> 0x%08X", flags, (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** ppDebug)
{
    PROXY(PFN_CREATEDXGIFACTORY_UINT, DXGIGetDebugInterface1, "DXGIGetDebugInterface1", L"dxgi.dll");
    HRESULT hr = fn ? fn(flags, riid, ppDebug) : E_FAIL;
    LOGD("proxy DXGIGetDebugInterface1 -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL minLevel, REFIID riid, void** ppDevice)
{
    PROXY(PFN_D3D12CREATE, D3D12CreateDevice, "D3D12CreateDevice", L"d3d12.dll");
    HRESULT hr = fn ? fn(pAdapter, minLevel, riid, ppDevice) : E_FAIL;
    LOGD("proxy D3D12CreateDevice(adapter=%p minLevel=%d) -> 0x%08X device=%p",
         pAdapter, (int)minLevel, (unsigned)hr, (ppDevice && SUCCEEDED(hr)) ? *ppDevice : nullptr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12GetDebugInterface(REFIID riid, void** ppDebug)
{
    PROXY(PFN_D3D12GETDEBUG, D3D12GetDebugInterface, "D3D12GetDebugInterface", L"d3d12.dll");
    HRESULT hr = fn ? fn(riid, ppDebug) : E_FAIL;
    LOGD("proxy D3D12GetDebugInterface -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12GetInterface(REFCLSID clsid, REFIID riid, void** ppv)
{
    PROXY(PFN_D3D12GETIFACE, D3D12GetInterface, "D3D12GetInterface", L"d3d12.dll");
    HRESULT hr = fn ? fn(clsid, riid, ppv) : E_FAIL;
    LOGD("proxy D3D12GetInterface -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pDesc, D3D_ROOT_SIGNATURE_VERSION ver, ID3DBlob** ppBlob, ID3DBlob** ppErr)
{
    PROXY(PFN_D3D12SERIALIZE_RS, D3D12SerializeRootSignature, "D3D12SerializeRootSignature", L"d3d12.dll");
    HRESULT hr = fn ? fn(pDesc, ver, ppBlob, ppErr) : E_FAIL;
    LOGD("proxy D3D12SerializeRootSignature -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pDesc, ID3DBlob** ppBlob, ID3DBlob** ppErr)
{
    PROXY(PFN_D3D12SERIALIZE_VRS, D3D12SerializeVersionedRootSignature, "D3D12SerializeVersionedRootSignature", L"d3d12.dll");
    HRESULT hr = fn ? fn(pDesc, ppBlob, ppErr) : E_FAIL;
    LOGD("proxy D3D12SerializeVersionedRootSignature -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12CreateRootSignatureDeserializer(LPCVOID pData, SIZE_T size, REFIID riid, void** ppOut)
{
    PROXY(PFN_D3D12RS_DESER, D3D12CreateRootSignatureDeserializer, "D3D12CreateRootSignatureDeserializer", L"d3d12.dll");
    HRESULT hr = fn ? fn(pData, size, riid, ppOut) : E_FAIL;
    LOGD("proxy D3D12CreateRootSignatureDeserializer -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12CreateVersionedRootSignatureDeserializer(LPCVOID pData, SIZE_T size, REFIID riid, void** ppOut)
{
    PROXY(PFN_D3D12RS_DESER, D3D12CreateVersionedRootSignatureDeserializer, "D3D12CreateVersionedRootSignatureDeserializer", L"d3d12.dll");
    HRESULT hr = fn ? fn(pData, size, riid, ppOut) : E_FAIL;
    LOGD("proxy D3D12CreateVersionedRootSignatureDeserializer -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D12EnableExperimentalFeatures(UINT numFeatures, const IID* iids, void* configs, UINT* configSizes)
{
    PROXY(PFN_D3D12EXPERIMENTAL, D3D12EnableExperimentalFeatures, "D3D12EnableExperimentalFeatures", L"d3d12.dll");
    HRESULT hr = fn ? fn(numFeatures, iids, configs, configSizes) : E_FAIL;
    LOGD("proxy D3D12EnableExperimentalFeatures -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType, HMODULE sw, UINT flags,
                                        const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVer,
                                        ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pLevel, ID3D11DeviceContext** ppCtx)
{
    PROXY(PFN_D3D11CREATE, D3D11CreateDevice, "D3D11CreateDevice", L"d3d11.dll");
    HRESULT hr = fn ? fn(pAdapter, driverType, sw, flags, levels, numLevels, sdkVer, ppDevice, pLevel, ppCtx) : E_FAIL;
    LOGD("proxy D3D11CreateDevice -> 0x%08X", (unsigned)hr);
    return hr;
}

HRESULT WINAPI Bridge_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType, HMODULE sw, UINT flags,
                                                    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVer,
                                                    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** ppSwapChain,
                                                    ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pLevel, ID3D11DeviceContext** ppCtx)
{
    PROXY(PFN_D3D11CREATE_SC, D3D11CreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain", L"d3d11.dll");
    HRESULT hr = fn ? fn(pAdapter, driverType, sw, flags, levels, numLevels, sdkVer, scDesc, ppSwapChain, ppDevice, pLevel, ppCtx) : E_FAIL;
    LOGD("proxy D3D11CreateDeviceAndSwapChain -> 0x%08X", (unsigned)hr);
    return hr;
}

// ---- generic stub for the rest of the SL1 export surface ----
bool Bridge_Stub()
{
    return false;
}

} // extern "C"

// ============================================================================
//  DllMain
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
