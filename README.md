# ESPFetch

ESPFetch is a lightweight HTTP helper that keeps ESP32 firmware **asynchronous-first**.

Each request runs in its own FreeRTOS task and can operate in one of two modes:

- **JSON mode** – captures headers and body into a heap-backed `JsonDocument` (ArduinoJson v7)
- **Stream mode** – delivers raw response data incrementally via callbacks (no buffering, no JSON)

This design allows ESPFetch to handle both typical REST APIs and large binary downloads
(firmware, files, blobs) efficiently and safely on resource-constrained devices.

---

## CI / Release / License

[![CI](https://github.com/ESPToolKit/esp-fetch/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-fetch/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-fetch?sort=semver)](https://github.com/ESPToolKit/esp-fetch/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

---

## Features

- Async-first HTTP API built on FreeRTOS tasks
- JSON-based GET / POST helpers returning `JsonDocument`
- Optional synchronous wrappers that block the caller
- **Binary / streaming downloads** via chunk callbacks
- Zero-copy streaming (no body buffering, no JSON parsing)
- Configurable concurrency via counting semaphore
- Per-request and global limits for body and header sizes
- Optional `FetchConfig::usePSRAMBuffers` to prefer PSRAM-backed ESPFetch-owned buffers via `ESPBufferManager` (JSON response body/headers, request body strings, and copied request headers; safe fallback on non-PSRAM boards)
- Built on ESP-IDF `esp_http_client` (TLS, redirects, auth, streaming)
- Detailed result metadata (status, timing, truncation, transport errors)
- ArduinoJson v7 only (no Dynamic/Static split)

---

## Quick Start

```cpp
#include <Arduino.h>
#include <ESPFetch.h>

ESPFetch fetch;

void setup() {
    Serial.begin(115200);

    FetchConfig cfg;
    cfg.maxConcurrentRequests = 3;
    cfg.stackSize = 6144;
    cfg.priority = 4;
    cfg.defaultTimeoutMs = 12000;
    cfg.usePSRAMBuffers = true; // optional: best-effort PSRAM for ESPFetch-owned request/response buffers

    fetch.init(cfg);
}

void loop() {}
````

---

## JSON Fetch Examples

### Async POST

```cpp
JsonDocument payload;
payload["hello"] = "world";

bool started = fetch.post(
    "https://httpbin.org/post",
    payload,
    [](JsonDocument result) {
        if (!result["error"].isNull()) {
            ESP_LOGE("FETCH",
                     "POST failed: %s",
                     result["error"]["message"].as<const char*>());
            return;
        }

        ESP_LOGI("FETCH",
                 "POST %d in %d ms",
                 result["status"].as<int>(),
                 result["duration_ms"].as<int>());
    }
);

if (!started) {
    ESP_LOGE("FETCH", "Failed to start POST request");
}
```

---

### Sync GET

```cpp
JsonDocument result =
    fetch.get("https://httpbin.org/get", portMAX_DELAY);

if (!result["error"].isNull()) {
    ESP_LOGW("FETCH",
             "GET failed: %s",
             result["error"]["message"].as<const char*>());
} else {
    Serial.printf("Status: %d\n", result["status"].as<int>());
}
```

---

## Streaming Downloads (Binary / Any Content)

ESPFetch supports **streaming downloads** for arbitrary content types.

This mode:

* Does **not** allocate or buffer the response body
* Does **not** use ArduinoJson
* Delivers data incrementally via callbacks

Ideal for:

* Firmware updates
* File downloads
* Large blobs
* Logs or diagnostics

---

### Async Streaming Example

```cpp
bool started = fetch.getStream(
    "https://example.com/firmware.bin",

    // Called for every received chunk
    [](const void* data, size_t size) -> bool {
        // Write directly to flash, file, or buffer
        // file.write((const uint8_t*)data, size);
        return true; // return false to abort the transfer
    },

    // Called once when the transfer finishes
    [](StreamResult result) {
        if (result.error != ESP_OK) {
            ESP_LOGE("FETCH",
                     "Stream failed: %s",
                     esp_err_to_name(result.error));
            return;
        }

        ESP_LOGI("FETCH",
                 "Download complete: HTTP %d, %u bytes",
                 result.statusCode,
                 result.receivedBytes);
    }
);

if (!started) {
    ESP_LOGE("FETCH", "Failed to start streaming request");
}
```

---

### Streaming Size Limits

By default, streaming downloads are **unlimited**.

To enforce a hard cap:

```cpp
FetchRequestOptions opts;
opts.maxBodyBytes = 1024 * 1024; // 1 MB limit

fetch.getStream(url, onChunk, onDone, opts);
```

If the limit is exceeded:

* Transfer aborts immediately
* `StreamResult.error == ESP_ERR_INVALID_SIZE`
* `receivedBytes` reflects delivered data

---

## API Reference

### Initialization

```cpp
bool init(const FetchConfig& cfg = {});
void deinit();
```

---

### JSON APIs

```cpp
bool get(const char* url,
         FetchCallback cb,
         const FetchRequestOptions& opts = {});

bool post(const char* url,
          const JsonDocument& payload,
          FetchCallback cb,
          const FetchRequestOptions& opts = {});

JsonDocument get(const char* url,
                 TickType_t waitTicks,
                 const FetchRequestOptions& opts = {});

JsonDocument post(const char* url,
                  const JsonDocument& payload,
                  TickType_t waitTicks,
                  const FetchRequestOptions& opts = {});
```

---

### Streaming APIs

```cpp
bool getStream(const char* url,
               FetchChunkCallback onChunk,
               FetchStreamCallback onDone = nullptr,
               const FetchRequestOptions& opts = {});

bool getStream(const String& url,
               FetchChunkCallback onChunk,
               FetchStreamCallback onDone = nullptr,
               const FetchRequestOptions& opts = {});
```

#### Callbacks

```cpp
using FetchChunkCallback =
    std::function<bool(const void* data, size_t size)>;

using FetchStreamCallback =
    std::function<void(StreamResult result)>;

struct StreamResult {
    esp_err_t error;
    int statusCode;
    size_t receivedBytes;
};
```

---

## Result Shape (JSON Mode)

```json
{
  "url": "https://httpbin.org/post",
  "method": "POST",
  "status": 200,
  "ok": true,
  "duration_ms": 742,
  "body": "{...}",
  "body_truncated": false,
  "headers_truncated": false,
  "headers": {
    "content-type": "application/json"
  },
  "error": null
}
```

---

## Gotchas

* Call `fetch.init()` exactly once before issuing requests
* Each async request spawns its own FreeRTOS task
* Keep callbacks short; offload heavy work
* Streaming callbacks run in the worker task context
* Sync APIs still spawn worker tasks internally
* No cancellation once a request has started
* Skipping TLS CN checks is unsafe for production
* URLs must be absolute (`http://` or `https://`); malformed schemes like `https:/` or `https:///` are normalized and logged
* A leading `://:` host typo is normalized (e.g., `https://:example.com` -> `https://example.com`)
* DNS must be configured before requests (WiFi/ETH); `esp-tls` `getaddrinfo()` failures (e.g., 202) usually mean missing DNS setup
* `usePSRAMBuffers` affects ESPFetch-owned request/response string/header buffers; ArduinoJson `JsonDocument` allocations still follow ArduinoJson's own allocator behavior

---

## Restrictions

* ESP32 + FreeRTOS only (Arduino-ESP32 or ESP-IDF)
* Requires C++17 (`-std=gnu++17`)
* Depends on `esp_http_client`, `esp_timer`, ArduinoJson v7

---

## License

MIT — see [LICENSE.md](LICENSE.md)

---

## ESPToolKit

* GitHub: [https://github.com/orgs/ESPToolKit/repositories](https://github.com/orgs/ESPToolKit/repositories)
* Discord: [https://discord.gg/WG8sSqAy](https://discord.gg/WG8sSqAy)
* Support: [https://ko-fi.com/esptoolkit](https://ko-fi.com/esptoolkit)
* Website: [https://www.esptoolkit.hu/](https://www.esptoolkit.hu/)
