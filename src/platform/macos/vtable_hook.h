#pragma once
#include <cstddef>
#include <cstdint>
namespace VtableHook {
struct VtableInfo {
    void** vtable=nullptr;
    void* origSlot4=nullptr;
    void* origSlot5=nullptr;
    void* origSlot6=nullptr;
    void* origSlot7=nullptr;
    void* origSlot8=nullptr;
    bool typedInstalled=false;
    int originalProtection=0;
};
struct CloudEnabledHookInfo {
    void** vtable=nullptr;
    void* origAccountSlot=nullptr;
    void* origAppSlot=nullptr;
    size_t accountSlotIndex=SIZE_MAX;
    size_t appSlotIndex=SIZE_MAX;
};
uintptr_t FindSteamclient(size_t& outSize);
void** FindTransportVtable(uintptr_t base,size_t size);
void** FindRemoteStorageVtable(uintptr_t base,size_t size);
void** FindVtableByRTTIName(const char* mangledName,uintptr_t base,size_t size);
void* FindGlobalWithVtable(void* vtablePtr,uintptr_t base,size_t size);
bool InstallHooks(void** vtable,VtableInfo& info);
bool InstallCloudEnabledHook(void** vtable,CloudEnabledHookInfo& info);
void RemoveHooks(const VtableInfo& info);
void RemoveCloudEnabledHook(const CloudEnabledHookInfo& info);
}
