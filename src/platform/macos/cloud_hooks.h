#pragma once
#include <cstdint>
#include <cstddef>
namespace CloudHooks {
void Initialize();
void SetOriginals(void* origSlot4, void* origSlot5, void* origSlot7, void* origSlot8);
void SetOriginalIsCloudEnabled(void* orig);
bool ResolveProtobufHelpers(void* steamclientBase,size_t steamclientSize);
void InstallGamesPlayedObserver(uintptr_t steamclientBase,size_t steamclientSize);
void BeginShutdown();
}
extern "C" bool hook_ServiceMethodDirect(void*,const char*,void*,void*,int*);
extern "C" bool hook_BYieldingSend(void*,const char*,void*,void*,int*);
extern "C" bool hook_NotificationDirect(void*,const char*,void*,int*);
extern "C" bool hook_SyncSend2(void*,const char*,void*,unsigned int,void*,int*);
extern "C" bool hook_IsCloudEnabledForApp(void*,unsigned int);
