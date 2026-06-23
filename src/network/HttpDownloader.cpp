#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <utility>

#include "SecureClient.h"
#include "util/UrlUtils.h"

// Arduino-wolfSSL's logging.c references this hook (normally supplied by the
// library's .ino serial glue, which we don't use). No-op stub to satisfy the linker.
extern "C" void wolfSSL_Arduino_Serial_Print(const char* const) {}

namespace {

// --- TLS 1.3 download path (FreeInk SecureNet / wolfSSL) ------------------
// HTTPS file downloads go through wolfSSL via freeink::SecureClient: the system
// mbedTLS handshake footprint OOMs this device (heap collapses 85KB->~10KB
// before any bytes flow). We speak just enough HTTP/1.1 to GET a file: follow
// redirects, honor Content-Length, stream the body straight to SD.

struct HttpsUrl {
  std::string host;
  uint16_t port = 443;
  std::string path;
};

bool parseHttpsUrl(const std::string& url, HttpsUrl& out) {
  constexpr const char* PFX = "https://";
  if (url.rfind(PFX, 0) != 0) return false;
  const size_t hostStart = 8;  // strlen("https://")
  const size_t pathStart = url.find('/', hostStart);
  const std::string hostport =
      pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  const size_t colon = hostport.find(':');
  if (colon == std::string::npos) {
    out.host = hostport;
    out.port = 443;
  } else {
    out.host = hostport.substr(0, colon);
    out.port = static_cast<uint16_t>(atoi(hostport.substr(colon + 1).c_str()));
  }
  return !out.host.empty();
}

// Pull up to `n` bytes, tolerating wolfSSL WANT_READ by polling until data,
// timeout, or the peer closes.
int secureReadSome(freeink::SecureClient& c, uint8_t* buf, size_t n, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  for (;;) {
    const int r = c.read(buf, n);
    if (r > 0) return r;
    if (!c.connected() && c.available() == 0) return 0;  // closed
    if (static_cast<int32_t>(millis() - deadline) >= 0) return 0;
    delay(2);
  }
}

HttpDownloader::DownloadError downloadHttpsToFile(const std::string& startUrl, const std::string& destPath,
                                                  HttpDownloader::ProgressCallback progress, bool* cancelFlag) {
  std::string url = startUrl;

  for (int redirect = 0; redirect < 6; ++redirect) {
    HttpsUrl u;
    if (!parseHttpsUrl(url, u)) {
      LOG_ERR("HTTP", "Bad https url: %s", url.c_str());
      return HttpDownloader::HTTP_ERROR;
    }

    freeink::SecureClient client;
    client.setInsecure();  // matches prior NetworkClientSecure::setInsecure()
    if (!client.connect(u.host.c_str(), u.port)) {
      LOG_ERR("HTTP", "TLS connect failed: %s:%u", u.host.c_str(), u.port);
      return HttpDownloader::HTTP_ERROR;
    }

    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION
                      "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size());

    // Read response headers (bounded) until the blank-line terminator.
    std::string resp;
    resp.reserve(1024);
    uint8_t tmp[512];
    size_t headerEnd = std::string::npos;
    while (true) {
      const int r = secureReadSome(client, tmp, sizeof(tmp), 15000);
      if (r <= 0) break;
      resp.append(reinterpret_cast<char*>(tmp), r);
      headerEnd = resp.find("\r\n\r\n");
      if (headerEnd != std::string::npos) break;
      if (resp.size() > 16384) {
        LOG_ERR("HTTP", "Response headers too large");
        return HttpDownloader::HTTP_ERROR;
      }
    }
    if (headerEnd == std::string::npos) {
      LOG_ERR("HTTP", "No response headers");
      return HttpDownloader::HTTP_ERROR;
    }

    const int status = atoi(resp.c_str() + 9);  // "HTTP/1.1 <status>"

    // Lowercased copy for case-insensitive header lookup (indices align with resp).
    std::string lower = resp.substr(0, headerEnd);
    for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));

    if (status >= 301 && status <= 308) {
      const size_t locPos = lower.find("location:");
      if (locPos == std::string::npos) {
        LOG_ERR("HTTP", "Redirect %d without Location", status);
        return HttpDownloader::HTTP_ERROR;
      }
      size_t valStart = locPos + 9;
      while (valStart < headerEnd && (resp[valStart] == ' ' || resp[valStart] == '\t')) ++valStart;
      const size_t valEnd = resp.find("\r\n", valStart);
      url = resp.substr(valStart, valEnd - valStart);
      continue;  // follow redirect (new connection — old client torn down here)
    }

    if (status != 200) {
      LOG_ERR("HTTP", "Download failed: HTTP %d", status);
      return HttpDownloader::HTTP_ERROR;
    }

    size_t contentLength = 0;
    const size_t clPos = lower.find("content-length:");
    if (clPos != std::string::npos) {
      contentLength = static_cast<size_t>(strtoul(resp.c_str() + clPos + 15, nullptr, 10));
    }

    if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
    FsFile file;
    if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
      LOG_ERR("HTTP", "Failed to open file for writing");
      return HttpDownloader::FILE_ERROR;
    }

    const size_t bodyStart = headerEnd + 4;
    size_t downloaded = 0;
    bool ok = true;
    auto sink = [&](const uint8_t* data, size_t len) -> bool {
      if (file.write(data, len) != len) {
        LOG_ERR("HTTP", "SD write failed");
        ok = false;
        return false;
      }
      downloaded += len;
      if (progress) progress(downloaded, contentLength);
      return true;
    };

    if (resp.size() > bodyStart) {
      sink(reinterpret_cast<const uint8_t*>(resp.data() + bodyStart), resp.size() - bodyStart);
    }

    bool aborted = false;
    while (ok && (contentLength == 0 || downloaded < contentLength)) {
      if (cancelFlag && *cancelFlag) {
        aborted = true;
        break;
      }
      const int r = secureReadSome(client, tmp, sizeof(tmp), 15000);
      if (r <= 0) break;  // EOF / timeout
      if (!sink(tmp, static_cast<size_t>(r))) break;
    }

    file.close();
    client.stop();

    if (aborted) {
      Storage.remove(destPath.c_str());
      return HttpDownloader::ABORTED;
    }
    if (!ok) {
      Storage.remove(destPath.c_str());
      return HttpDownloader::FILE_ERROR;
    }
    if (downloaded == 0 || (contentLength > 0 && downloaded != contentLength)) {
      LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
      Storage.remove(destPath.c_str());
      return HttpDownloader::HTTP_ERROR;
    }

    LOG_INF("HTTP", "Downloaded %zu bytes", downloaded);
    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Too many redirects");
  return HttpDownloader::HTTP_ERROR;
}

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress, bool* cancelFlag)
      : file_(file), total_(total), progress_(std::move(progress)), cancelFlag_(cancelFlag) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    if (cancelFlag_ && *cancelFlag_) {
      writeOk_ = false;
      return 0;
    }
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_ && total_ > 0) {
      progress_(downloaded_, total_);
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  HttpDownloader::ProgressCallback progress_;
  bool* cancelFlag_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  StreamString stream;
  if (!fetchUrl(url, stream, username, password)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  // HTTPS goes through wolfSSL (SecureNet); mbedTLS's handshake footprint OOMs.
  // Basic-auth isn't used by the https download sources (GitHub releases).
  if (UrlUtils::isHttpsUrl(url)) {
    return downloadHttpsToFile(url, destPath, progress, cancelFlag);
  }

  std::unique_ptr<NetworkClient> client;
  client.reset(new NetworkClient());
  HTTPClient http;

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Let HTTPClient handle chunked decoding and stream body bytes into the file.
  FileWriteStream fileStream(file, contentLength, progress, cancelFlag);
  const int writeResult = http.writeToStream(&fileStream);

  file.close();
  http.end();

  if (cancelFlag && *cancelFlag) {
    Storage.remove(destPath.c_str());
    return ABORTED;
  }

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream error: %d", writeResult);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
