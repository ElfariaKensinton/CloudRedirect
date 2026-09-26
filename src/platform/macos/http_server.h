#pragma once
// macOS http_server.h - matches the common HTTP server API surface.
// Uses POSIX sockets instead of Winsock.

#include <cstdint>
#include <string>
#include <vector>

namespace HttpServer {

// Start blob HTTP server on 127.0.0.1 (OS-assigned port)
bool Start(const std::string& blobRoot, uint32_t accountId = 0);

// Set the account ID for blob path namespacing
void SetAccountId(uint32_t accountId);

// Set the max upload size in MB. 0 = unlimited.
void SetMaxUploadMB(int mb);

// Stop the server
void Stop();

// Get the port the server bound to (0 if not started)
uint16_t GetPort();

// Per-process bearer token embedded in Steam's localhost transfer URLs.
std::string GetAuthToken();

// Check if a blob exists for the given account/app/filename
bool HasBlob(uint32_t accountId, uint32_t appId, const std::string& filename);

// Get the size of a stored blob. Returns 0 if not found.
uint64_t GetBlobSize(uint32_t accountId, uint32_t appId, const std::string& filename);

// Read a stored blob into memory. Returns empty vector if not found.
std::vector<uint8_t> ReadBlob(uint32_t accountId, uint32_t appId, const std::string& filename);

// Delete a stored blob. Returns true if it existed and was removed.
bool DeleteBlob(uint32_t accountId, uint32_t appId, const std::string& filename);

} // namespace HttpServer
