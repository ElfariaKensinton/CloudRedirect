#include "cloud_hooks.h"
#include "cloud_intercept.h"
#include "cloud_storage.h"
#include "local_storage.h"
#include "local_metadata_store.h"
#include "pending_ops_journal.h"
#include "http_server.h"
#include "cloud_provider.h"
#include "cloud_provider_base.h"
#include "rpc_handlers.h"
#include "metadata_sync.h"
#include "stats_hooks.h"
#include "stats_handlers.h"
#include "stats_store.h"
#include "protobuf.h"
#include "json.h"
#include "log.h"
#include "xdg.h"
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include <cstring>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <cstdlib>

using RawServerMethodFn=bool(*)(void*,const char*,const void*,unsigned int,std::string&,void*);
using RawServerNotificationFn=bool(*)(void*,const char*,const void*,unsigned int,void*);
using TypedServerMethodFn=bool(*)(void*,const char*,void*,void*,void*);
using TypedServerNotificationFn=bool(*)(void*,const char*,void*,void*);
using CloudEnabledFn=bool(*)(void*,unsigned int);

static std::atomic<RawServerMethodFn> g_origServerMethod{nullptr};
static std::atomic<RawServerNotificationFn> g_origServerNotification{nullptr};
static std::atomic<TypedServerMethodFn> g_origServerMethodTyped{nullptr};
static std::atomic<TypedServerNotificationFn> g_origServerNotificationTyped{nullptr};
static std::atomic<CloudEnabledFn> g_origCloud{nullptr};
static std::atomic<bool> g_initialized{false},g_shuttingDown{false};
static std::atomic<int> g_hookRefCount{0};
static std::once_flag g_initOnce;
static std::atomic<bool> g_statsSyncEnabled{false};
static std::thread g_seedThread;
static std::thread g_cloudPollerThread;
static std::mutex g_seedExitMtx, g_pollerExitMtx;
static std::condition_variable g_seedExitCv, g_pollerExitCv;
static std::atomic<bool> g_seedExited{false};
static std::atomic<bool> g_pollerExited{false};

struct HookGuard {
 bool active = false;
 HookGuard() {
   g_hookRefCount.fetch_add(1, std::memory_order_acq_rel);
   if (g_shuttingDown.load(std::memory_order_acquire)) {
     g_hookRefCount.fetch_sub(1, std::memory_order_release);
     return;
   }
   active = true;
 }
 ~HookGuard() {
   if (active) g_hookRefCount.fetch_sub(1, std::memory_order_release);
 }
};

static bool RequiresLocalHttp(const char* method)
{
 return method &&
   (strcmp(method,CloudIntercept::RPC_BEGIN_UPLOAD)==0 ||
    strcmp(method,CloudIntercept::RPC_FILE_DOWNLOAD)==0);
}

static std::optional<CloudIntercept::RpcResult> Dispatch(const char*m,uint32_t app,const std::vector<PB::Field>&f){
 using namespace CloudIntercept;
 if(strcmp(m,RPC_GET_CHANGELIST)==0)return HandleGetChangelist(app,f);
 if(strcmp(m,RPC_LAUNCH_INTENT)==0)return HandleLaunchIntent(app,f);
 if(strcmp(m,RPC_SUSPEND_SESSION)==0)return HandleSuspendSession(app,f);
 if(strcmp(m,RPC_RESUME_SESSION)==0)return HandleResumeSession(app,f);
 if(strcmp(m,RPC_QUOTA_USAGE)==0)return HandleQuotaUsage(app,f);
 if(strcmp(m,RPC_BEGIN_BATCH)==0)return HandleBeginBatch(app,f);
 if(strcmp(m,RPC_BEGIN_UPLOAD)==0)return HandleBeginFileUpload(app,f);
 if(strcmp(m,RPC_COMMIT_UPLOAD)==0)return HandleCommitFileUpload(app,f);
 if(strcmp(m,RPC_COMPLETE_BATCH)==0)return HandleCompleteBatch(app,f);
 if(strcmp(m,RPC_FILE_DOWNLOAD)==0)return HandleFileDownload(app,f);
 if(strcmp(m,RPC_DELETE_FILE)==0)return HandleDeleteFile(app,f);
 return std::nullopt;
}
static void EnsureInitialized(){
 if(g_initialized.load(std::memory_order_acquire)||g_shuttingDown.load(std::memory_order_acquire))return;
 std::call_once(g_initOnce,[](){
   if(g_shuttingDown.load(std::memory_order_acquire)) return;

   CloudIntercept::InitMac();
   const std::string root=XdgConfigHome()+"/CloudRedirect/";
   const std::string storage=root+"storage";
   std::unique_ptr<ICloudProvider> provider;
   std::string cfgText;
   {
     std::ifstream f(root+"config.json");
     cfgText.assign(std::istreambuf_iterator<char>(f),{});
   }

   bool statsEnabled = MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                       MetadataSync::syncPlaytime.load(std::memory_order_relaxed);

   if(!cfgText.empty()){
     auto cfg=Json::Parse(cfgText);
     std::string name=cfg["provider"].str();

     if(cfg["stats_sync_enabled"].type==Json::Type::Bool &&
        !cfg["stats_sync_enabled"].boolean()){
       MetadataSync::syncAchievements.store(false,std::memory_order_relaxed);
       MetadataSync::syncPlaytime.store(false,std::memory_order_relaxed);
     }else{
       if(cfg["sync_achievements"].type==Json::Type::Bool)
         MetadataSync::syncAchievements.store(cfg["sync_achievements"].boolean(),std::memory_order_relaxed);
       if(cfg["sync_playtime"].type==Json::Type::Bool)
         MetadataSync::syncPlaytime.store(cfg["sync_playtime"].boolean(),std::memory_order_relaxed);
     }

     statsEnabled = MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                    MetadataSync::syncPlaytime.load(std::memory_order_relaxed);

     if(!name.empty()&&name!="local"){
       provider=CreateCloudProvider(name);
       if(provider){
         std::string tokenPath=ResolveProviderTokenPath(root,cfgText,name);
         if(!provider->Init(tokenPath)||!provider->IsAuthenticated())provider.reset();
       }
     }
   }

   g_statsSyncEnabled.store(statsEnabled,std::memory_order_release);
   CloudStorage::Init(root,std::move(provider));
   LocalStorage::Init(storage);
   LocalMetadataStore::Init(storage);
   PendingOpsJournal::Init(storage);
   HttpServer::SetAccountId(CloudIntercept::GetAccountId());
   if(!HttpServer::Start(storage,CloudIntercept::GetAccountId()))
     LOG("[Mac] local cloud HTTP server failed to start; Cloud RPCs will pass through");

   if(statsEnabled){
     StatsStore::SetCloudProvider(
       [](std::unordered_map<uint32_t,std::string>& out)->bool{
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return false;
         std::vector<uint8_t> data;
         auto fetch=CloudStorage::FetchCloudMetadataStatus(
             accountId,CloudIntercept::kAccountScopeAppId,"stats.json",data);
         if(fetch==CloudStorage::MetadataFetch::Error)return false;
         if(fetch==CloudStorage::MetadataFetch::Missing||data.empty())return true;
         auto root=Json::Parse(std::string(reinterpret_cast<const char*>(data.data()),data.size()));
         if(root.type!=Json::Type::Object)return true;
         for(const auto& [appIdStr,appVal]:root.objVal){
           uint32_t appId=0;
           try{appId=(uint32_t)std::stoul(appIdStr);}catch(...){continue;}
           if(appId)out[appId]=Json::Stringify(appVal);
         }
         return true;
       },
       [](const std::unordered_map<uint32_t,std::string>& all){
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return;
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return;

         Json::Value root=Json::Object();
         std::vector<uint8_t> cur;
         if(CloudStorage::DownloadCloudMetadataWithLegacyFallback(
              accountId,CloudIntercept::kAccountScopeAppId,"stats.json",nullptr,cur)&&!cur.empty()){
           auto parsed=Json::Parse(std::string(reinterpret_cast<const char*>(cur.data()),cur.size()));
           if(parsed.type==Json::Type::Object)root=std::move(parsed);
         }

         auto resetApps=StatsStore::ConsumeResetApps();
         bool changed=false;
         for(const auto& [appId,json]:all){
           if(appId==0)continue;
           std::string key=std::to_string(appId);
           std::string merged;
           if(resetApps.count(appId)) merged=json;
           else {
             std::string baseEntry=root.has(key)?Json::Stringify(root.objVal[key]):std::string();
             merged=StatsStore::MergeAppStatsJson(baseEntry,json);
           }
           auto appVal=Json::Parse(merged);
           if(appVal.type!=Json::Type::Object)continue;
           if(!root.has(key)||!Json::DeepEqual(root.objVal[key],appVal)){
             root.objVal[key]=std::move(appVal);
             changed=true;
           }
         }
         if(changed)
           CloudStorage::UploadCloudMetadataTextAsync(
             accountId,CloudIntercept::kAccountScopeAppId,"stats.json",Json::Stringify(root));
       },
       [](uint32_t appId)->std::string{
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return {};
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return {};
         std::vector<uint8_t> data;
         if(CloudStorage::DownloadCloudMetadataWithLegacyFallback(
              accountId,appId,"stats.json",nullptr,data)&&!data.empty())
           return std::string(reinterpret_cast<const char*>(data.data()),data.size());
         return {};
       },
       [](uint32_t appId)->std::string{
         CloudStorage::InflightSyncScope guard;
         if(!guard.entered)return {};
         uint32_t accountId=CloudIntercept::GetAccountId();
         if(accountId==0)return {};
         std::vector<uint8_t> data;
         if(CloudStorage::DownloadLegacyPlaytimeBlob(accountId,appId,data))
           return std::string(reinterpret_cast<const char*>(data.data()),data.size());
         return {};
       });

     StatsHandlers::SetNamespacePredicate(
       [](uint32_t appId){return CloudIntercept::IsNamespaceApp(appId);});
     StatsStore::SetNamespacePredicate(
       [](uint32_t appId){return CloudIntercept::IsNamespaceApp(appId);});
     StatsStore::SetAccountIdProvider(
       []()->uint32_t{return CloudIntercept::GetAccountId();});
     StatsStore::Init(root,CloudIntercept::GetSteamPath());
     StatsHandlers::Init();

     if(MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
        MetadataSync::syncPlaytime.load(std::memory_order_relaxed)){
       g_seedExited.store(false,std::memory_order_release);
       g_seedThread=std::thread([]{
         if(!g_shuttingDown.load(std::memory_order_acquire))
           StatsStore::SeedApps(CloudIntercept::GetNamespaceApps());
         g_seedExited.store(true,std::memory_order_release);
         g_seedExitCv.notify_all();
       });

       g_pollerExited.store(false,std::memory_order_release);
       g_cloudPollerThread=std::thread([]{
         while(!g_shuttingDown.load(std::memory_order_acquire)){
           for(int i=0;i<60 && !g_shuttingDown.load(std::memory_order_acquire);++i)
             std::this_thread::sleep_for(std::chrono::seconds(1));
           if(g_shuttingDown.load(std::memory_order_acquire))break;
           const auto apps=StatsStore::GetKnownApps();
           if(MetadataSync::syncPlaytime.load(std::memory_order_relaxed)) {
             StatsStore::RefreshLocalPlaytime();
             auto changed=StatsStore::RefreshFromCloud(apps);
             if(!changed.empty())
               LOG("[Stats] macOS cloud poll advanced %zu app(s)",changed.size());
           }
           if(MetadataSync::syncAchievements.load(std::memory_order_relaxed)) {
             for(uint32_t appId:apps) {
               if(appId) StatsStore::CaptureNativeUnlocks(appId);
             }
           }
         }
         g_pollerExited.store(true,std::memory_order_release);
         g_pollerExitCv.notify_all();
       });
     }
   }

   g_initialized.store(true,std::memory_order_release);
   LOG("[Mac] CloudRedirect initialized account=%u namespaceApps=%zu stats=%d achievements=%d playtime=%d",
       CloudIntercept::GetAccountId(),CloudIntercept::GetNamespaceApps().size(),
       statsEnabled?1:0,
       MetadataSync::syncAchievements.load()?1:0,
       MetadataSync::syncPlaytime.load()?1:0);
 });
}
namespace CloudHooks {
void Initialize(){ EnsureInitialized(); }
void SetOriginalRaw(void* serverMethod,void* serverNotification){
 g_origServerMethod.store((RawServerMethodFn)serverMethod,std::memory_order_release);
 g_origServerNotification.store((RawServerNotificationFn)serverNotification,std::memory_order_release);
}
void SetOriginalTyped(void* serverMethod,void* serverNotification){
 g_origServerMethodTyped.store((TypedServerMethodFn)serverMethod,std::memory_order_release);
 g_origServerNotificationTyped.store((TypedServerNotificationFn)serverNotification,std::memory_order_release);
}
void SetOriginalIsCloudEnabled(void*o){g_origCloud.store((CloudEnabledFn)o,std::memory_order_release);}
void InstallGamesPlayedObserver(uintptr_t,size_t){
 LOG("[Mac] GamesPlayed observer unavailable on this build; playtime uses native stats reconciliation/poller");
}
void BeginShutdown(){
 g_shuttingDown.store(true,std::memory_order_release);
 HttpServer::Stop();

 if(g_seedThread.joinable()){
   std::unique_lock<std::mutex> lk(g_seedExitMtx);
   g_seedExitCv.wait(lk,[] { return g_seedExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_seedThread.join();
 }
 if(g_cloudPollerThread.joinable()){
   std::unique_lock<std::mutex> lk(g_pollerExitMtx);
   g_pollerExitCv.wait(lk,[] { return g_pollerExited.load(std::memory_order_acquire); });
   lk.unlock();
   g_cloudPollerThread.join();
 }
 if(g_statsSyncEnabled.load(std::memory_order_acquire)) {
   if(MetadataSync::syncPlaytime.load(std::memory_order_relaxed))
     StatsStore::RefreshLocalPlaytime();
   StatsHandlers::Shutdown();
 }

 while(g_hookRefCount.load(std::memory_order_acquire)>0)
   std::this_thread::sleep_for(std::chrono::milliseconds(1));

 CloudStorage::Shutdown();
}
}

static uint32_t ExtractRequestAppId(const char* method,const std::vector<PB::Field>& fields){
 if(!method) return 0;
 if(strcmp(method,StatsHandlers::RPC_GET_USER_STATS)==0){
   if(auto* f=PB::FindField(fields,2)) return (uint32_t)f->varintVal;
   return 0;
 }
 return CloudRpcUtils::ExtractAppId(method,fields);
}

static void SetRawResponse(std::string& response,const PB::Writer& body){
 const auto& data=body.Data();
 if(data.empty()) response.clear();
 else response.assign(reinterpret_cast<const char*>(data.data()),data.size());
}

extern "C" bool hook_ServerMethodTyped(
    void*t,const char*m,void*request,void*response,void*options)
{
    auto orig=g_origServerMethodTyped.load(std::memory_order_acquire);
    if(!orig) return false;
    if(m && (strncmp(m,"Cloud.",6)==0 ||
             strcmp(m,StatsHandlers::RPC_GET_USER_STATS)==0 ||
             strcmp(m,StatsHandlers::RPC_GET_LAST_PLAYED)==0)) {
        LOG("[Mac] typed server method observed: %s req=%p resp=%p",m,request,response);
    }
    return orig(t,m,request,response,options);
}

extern "C" bool hook_ServerNotificationTyped(
    void*t,const char*m,void*message,void*options)
{
    auto orig=g_origServerNotificationTyped.load(std::memory_order_acquire);
    if(!orig) return false;
    if(m && strncmp(m,"Cloud.",6)==0)
        LOG("[Mac] typed server notification observed: %s msg=%p",m,message);
    return orig(t,m,message,options);
}

extern "C" bool hook_ServerMethodRaw(
    void*t,const char*m,const void*buf,unsigned int len,std::string&response,void*options)
{
 HookGuard guard;
 auto orig=g_origServerMethod.load(std::memory_order_acquire);
 if(!orig||g_shuttingDown.load(std::memory_order_acquire)||!m||!buf)
   return orig?orig(t,m,buf,len,response,options):false;

 const bool isCloud = strncmp(m,"Cloud.",6)==0;
 const bool isGetUserStats = strcmp(m,StatsHandlers::RPC_GET_USER_STATS)==0;
 const bool isGetLastPlayed = strcmp(m,StatsHandlers::RPC_GET_LAST_PLAYED)==0;
 if(!isCloud && !isGetUserStats && !isGetLastPlayed)
   return orig(t,m,buf,len,response,options);

 EnsureInitialized();
 auto fields=PB::Parse((const uint8_t*)buf,len);

 if(isGetLastPlayed){
   if(!MetadataSync::syncPlaytime.load(std::memory_order_relaxed))
     return orig(t,m,buf,len,response,options);
   auto res=StatsHandlers::HandleGetLastPlayedTimes(fields);
   SetRawResponse(response,res.body);
   return true;
 }

 const uint32_t app=ExtractRequestAppId(m,fields);
 if(!app||!CloudIntercept::IsNamespaceApp(app))
   return orig(t,m,buf,len,response,options);

 if(isGetUserStats){
   if(!MetadataSync::syncAchievements.load(std::memory_order_relaxed))
     return orig(t,m,buf,len,response,options);
   auto res=StatsHandlers::HandleGetUserStats(app,fields);
   SetRawResponse(response,res.body);
   return true;
 }

 if(HttpServer::GetPort()==0 && RequiresLocalHttp(m))
   return orig(t,m,buf,len,response,options);

 const uint32_t account=CloudIntercept::GetAccountId();
 if(!account)
   return orig(t,m,buf,len,response,options);

 LocalStorage::InitApp(account,app);
 LocalMetadataStore::InitApp(account,app);

 auto res=Dispatch(m,app,fields);
 if(!res.has_value() || res->eresult!=CloudIntercept::kEResultOK)
   return orig(t,m,buf,len,response,options);

 SetRawResponse(response,res->body);
 return true;
}

extern "C" bool hook_ServerNotificationRaw(
    void*t,const char*m,const void*buf,unsigned int len,void*options)
{
 HookGuard guard;
 auto orig=g_origServerNotification.load(std::memory_order_acquire);
 if(!orig||g_shuttingDown.load(std::memory_order_acquire)||!m||!buf)
   return orig?orig(t,m,buf,len,options):false;

 if(strncmp(m,"Cloud.",6)!=0)
   return orig(t,m,buf,len,options);

 auto fields=PB::Parse((const uint8_t*)buf,len);
 auto* appField=PB::FindField(fields,1);
 uint32_t app=appField ? (uint32_t)appField->varintVal : 0;
 if(!app||!CloudIntercept::IsNamespaceApp(app))
   return orig(t,m,buf,len,options);

 if(strcmp(m,CloudIntercept::RPC_EXIT_SYNC)==0){
   uint64_t clientId=0;
   bool uploadsCompleted=false, uploadsRequired=false;
   if(auto* x=PB::FindField(fields,2))clientId=x->varintVal;
   if(auto* x=PB::FindField(fields,3))uploadsCompleted=x->varintVal!=0;
   if(auto* x=PB::FindField(fields,4))uploadsRequired=x->varintVal!=0;

   uint32_t account=CloudIntercept::GetAccountId();
   if(account){
     PendingOpsJournal::RecordExitSyncState(account,app,uploadsCompleted,uploadsRequired,clientId);
     std::thread([account,app,clientId]{
       CloudStorage::InflightSyncScope guard;
       if(!guard.entered)return;
       CloudStorage::ReleaseCloudSession(account,app,clientId);
     }).detach();
   }
   return true;
 }

 if(strcmp(m,CloudIntercept::RPC_CONFLICT)==0){
   bool choseLocal=false;
   if(auto* x=PB::FindField(fields,2))choseLocal=x->varintVal!=0;
   CloudIntercept::RecordConflictResolution(app,choseLocal);
 }

 return true;
}

extern "C" bool hook_IsCloudEnabledForApp(void*t,unsigned int app){
 auto orig=g_origCloud.load(std::memory_order_acquire);
 if(CloudIntercept::IsNamespaceApp(app)) return true;
 return orig?orig(t,app):true;
}
extern "C" void CR_SetCrashContext(const char*,const char*,uint32_t){}
