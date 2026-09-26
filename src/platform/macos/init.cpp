#include "vtable_hook.h"
#include "cloud_hooks.h"
#include "log.h"
#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <stdlib.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>

#ifdef CR_VERSION_STRING
static constexpr char kCRVersionMarker[] = "CloudRedirectVersion/" CR_VERSION_STRING;
#else
static constexpr char kCRVersionMarker[] = "CloudRedirectVersion/0.0.0";
#endif

static std::atomic<bool> g_started{false};
static std::atomic<bool> g_unloading{false};
static std::atomic<bool> g_threadStarted{false};
static std::atomic<bool> g_steamProcess{false};
static std::atomic<bool> g_cloudInitialized{false};
static std::atomic<bool> g_hooksInstalled{false};
static pthread_t g_initThread{};
static VtableHook::VtableInfo g_transport{};
static VtableHook::CloudEnabledHookInfo g_cloudEnabled{};

static void* InitThread(void*)
{
    if(g_started.exchange(true)) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    for(int attempt=0;attempt<30;attempt++){
        if(g_unloading.load(std::memory_order_acquire)) return nullptr;
        size_t size=0; uintptr_t base=VtableHook::FindSteamclient(size);
        if(base){
            if(void** vt=VtableHook::FindTransportVtable(base,size)){
                if(g_unloading.load(std::memory_order_acquire)) return nullptr;
                if(VtableHook::InstallHooks(vt,g_transport)){
                    g_hooksInstalled.store(true, std::memory_order_release);
                    if(void** remoteVt=VtableHook::FindRemoteStorageVtable(base,size)){
                        if(!VtableHook::InstallCloudEnabledHook(remoteVt,g_cloudEnabled)){
                            LOG("[Mac] RemoteStorage Cloud-enabled hook installation failed");
                        }
                    } else {
                        LOG("[Mac] IClientRemoteStorage vtable not found; Cloud-enabled override not installed");
                    }
                    CloudHooks::Initialize();
                    g_cloudInitialized.store(true, std::memory_order_release);
                    LOG("[Mac] CloudRedirect transport hook active");
                    return nullptr;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    LOG("[Mac] steamclient.dylib was not ready; CloudRedirect did not install hooks");
    return nullptr;
}

static bool IsSteamProcess()
{
    const char* name = getprogname();
    return name &&
        (std::strcmp(name, "steam_osx") == 0 ||
         std::strcmp(name, "steam") == 0);
}

__attribute__((constructor))
static void CR_OnLoad()
{
    Log::Init();
    LOG("[Mac] dylib constructor pid=%d process=%s", getpid(),
        getprogname() ? getprogname() : "<unknown>");
    // DYLD_INSERT_LIBRARIES may contain multiple injected libraries. CloudRedirect
    // only initializes its Steam hooks; non-Steam processes must be left completely
    // untouched so the rest of the injection chain remains intact.
    if (!IsSteamProcess()) {
        LOG("[Mac] not Steam process; skipping hook initialization");
        return;
    }
    g_steamProcess.store(true, std::memory_order_release);

    // Keep DYLD_INSERT_LIBRARIES in the Steam client environment. Steam
    // bootstraps the real client from a second steam_osx executable, and the
    // macSteam launcher deliberately preserves DYLD_INSERT_LIBRARIES for
    // steam_osx children so injected libraries survive that relaunch.
    if (pthread_create(&g_initThread, nullptr, &InitThread, nullptr) == 0)
        g_threadStarted.store(true, std::memory_order_release);
}

__attribute__((destructor))
static void CR_OnUnload()
{
    // cloud_redirect_cli and other DYLD-injected child processes load this dylib
    // too. In those processes the constructor intentionally did nothing, so
    // teardown must be equally inert; otherwise we touch Steam-only C++ state
    // during dlclose and can abort with std::system_error.
    if (!g_steamProcess.load(std::memory_order_acquire))
        return;

    LOG("[Mac] dylib destructor pid=%d: begin teardown", getpid());
    g_unloading.store(true, std::memory_order_release);
    if (g_threadStarted.load(std::memory_order_acquire))
        pthread_join(g_initThread, nullptr);

    if (g_cloudInitialized.exchange(false, std::memory_order_acq_rel))
        CloudHooks::BeginShutdown();
    if (g_hooksInstalled.exchange(false, std::memory_order_acq_rel))
        VtableHook::RemoveHooks(g_transport);
    LOG("[Mac] dylib destructor pid=%d: teardown complete", getpid());
}

extern "C" const char* CR_GetVersion()
{
    return kCRVersionMarker + (sizeof("CloudRedirectVersion/") - 1);
}
