#pragma once
#include <cstdint>
#include <cstddef>
namespace CloudHooks {
void Initialize();
void SetOriginalRaw(void* origServerMethod, void* origServerNotification);
bool TypedHooksAvailable();
void SetOriginalTyped(void* origServerMethod, void* origServerNotification);
void SetOriginalIsCloudEnabled(void* orig);
void InstallGamesPlayedObserver(uintptr_t steamclientBase,size_t steamclientSize);
void BeginShutdown();
}
extern "C" bool hook_SyncSend2(void*,const char*,const void*,unsigned int,void*,int*);
extern "C" bool hook_ServerMethodTyped(void*,const char*,void*,void*,void*);
extern "C" bool hook_ServerNotificationTyped(void*,const char*,void*,void*);
extern "C" bool hook_NotificationDirect(void*,const char*,void*,int*);
extern "C" bool hook_IsCloudEnabledForApp(void*,unsigned int);
