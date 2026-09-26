#include "cloud_provider_base.h"
#include "file_util.h"
#include <filesystem>
#include <fstream>
#include <memory>

class MacTokenStore : public ITokenStore {
public:
 std::string Read(const std::string& path) override {
  std::ifstream f(path,std::ios::binary); return f?std::string((std::istreambuf_iterator<char>(f)),{}):std::string();
 }
 bool Write(const std::string& path,const std::string& json) override {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  if (ec) return false;
  return FileUtil::AtomicWriteText(path, json);
 }
 bool IsEncryptionAvailable() const override { return false; }
};
std::unique_ptr<ITokenStore> CreateTokenStore(){ return std::make_unique<MacTokenStore>(); }
