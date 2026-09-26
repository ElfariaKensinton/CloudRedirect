#pragma once
#include <cstdint>
#include <cstddef>
namespace CloudHooks {
void Initialize();
void SetOriginalRaw(void* origServerMethod, void* origServerNotification);
bool TypedHooksAvailable();
void SetOriginalTyped(void* origServerMethod, void* origServerNotification);
void SetOriginalIsCloudEnabled(void* orig);
void SetOriginalIsCloudEnabledAccount(void* orig);
void SetOriginalSetCloudEnabledApp(void* orig);
void SetOriginalSetCloudEnabledAccount(void* orig);
void SetOriginalRemoteStorageSync(void* syncApp, void* isSyncInProgress, void* runLaunch, void* runExit);
void InstallGamesPlayedObserver(uintptr_t steamclientBase,size_t steamclientSize);
void BeginShutdown();
}
extern "C" bool hook_SyncSend2(void*,const char*,const void*,unsigned int,void*,int*);
extern "C" bool hook_ServerMethodTyped(void*,const char*,void*,void*,void*);
extern "C" bool hook_ServerNotificationTyped(void*,const char*,void*,void*);
extern "C" bool hook_NotificationDirect(void*,const char*,void*,int*);
extern "C" void hook_SetCloudEnabledForApp(void*,unsigned int,bool);
extern "C" void hook_SetCloudEnabledForAccount(void*,bool);
extern "C" bool hook_IsCloudEnabledForAccount(void*);
extern "C" bool hook_IsCloudEnabledForApp(void*,unsigned int);
extern "C" bool hook_SynchronizeApp(void*,unsigned int,bool,bool);
extern "C" bool hook_IsAppSyncInProgress(void*,unsigned int);
extern "C" void hook_RunAutoCloudOnAppLaunch(void*,unsigned int);
extern "C" void hook_RunAutoCloudOnAppExit(void*,unsigned int);
