#pragma once
#include "cloud_metadata_paths.h"
#include "common.h"
namespace CloudIntercept {
void InitMac();
bool IsNamespaceApp(uint32_t appId);
bool HasNamespaceApps();
std::vector<uint32_t> GetNamespaceApps();
void RegisterNamespaceApp(uint32_t appId);
std::string GetSteamPath();
uint32_t GetAccountId();
void SetAccountId(uint32_t id);
void SetSteamPath(const std::string& path);
void Shutdown();
}
