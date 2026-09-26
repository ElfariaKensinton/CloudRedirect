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
                    CloudHooks::ResolveProtobufHelpers((void*)base,size);
                    CloudHooks::Initialize();
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
    // DYLD_INSERT_LIBRARIES reaches child processes. Only the Steam client
    // itself is allowed to initialize hooks; every other process must simply
    // drop the inherited variable and continue untouched.
    if (!IsSteamProcess()) {
        unsetenv("DYLD_INSERT_LIBRARIES");
        return;
    }
    unsetenv("DYLD_INSERT_LIBRARIES");
    if (pthread_create(&g_initThread, nullptr, &InitThread, nullptr) == 0)
        g_threadStarted.store(true, std::memory_order_release);
}

__attribute__((destructor))
static void CR_OnUnload()
{
    g_unloading.store(true, std::memory_order_release);
    if (g_threadStarted.load(std::memory_order_acquire))
        pthread_join(g_initThread, nullptr);
    CloudHooks::BeginShutdown();
    VtableHook::RemoveHooks(g_transport);
}

extern "C" const char* CR_GetVersion()
{
    return kCRVersionMarker + (sizeof("CloudRedirectVersion/") - 1);
}
