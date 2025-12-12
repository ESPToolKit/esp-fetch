#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <atomic>

extern "C" {
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

struct FetchHeader {
    std::string name;
    std::string value;
};

struct FetchRequestOptions {
    uint32_t timeoutMs = 0;
    size_t maxBodyBytes = 0;
    size_t maxHeaderBytes = 0;
    bool skipTlsCommonNameCheck = false;
    bool allowRedirects = true;
    std::vector<FetchHeader> headers;
    const char *contentType = nullptr;
};

struct FetchConfig {
    size_t maxConcurrentRequests = 4;
    size_t stackSize = 6144 * sizeof(StackType_t);
    UBaseType_t priority = 4;
    BaseType_t coreId = tskNO_AFFINITY;
    uint32_t defaultTimeoutMs = 15000;
    size_t maxBodyBytes = 16384;
    size_t maxHeaderBytes = 4096;
    TickType_t slotAcquireTicks = pdMS_TO_TICKS(0);
    bool skipTlsCommonNameCheck = false;
    bool followRedirects = true;
    const char *userAgent = "ESPFetch/1.0";
    const char *defaultContentType = "application/json";
};

using FetchCallback = std::function<void(JsonDocument result)>;

// ------------------------------
// Streaming (binary/any-content)
// ------------------------------
struct StreamResult {
    esp_err_t error = ESP_OK;
    int statusCode = 0;
    size_t receivedBytes = 0;
};

using FetchChunkCallback = std::function<void(const void *data, size_t size)>;
using FetchStreamCallback = std::function<void(StreamResult result)>;

class ESPFetch {
   public:
    ESPFetch() = default;
    ~ESPFetch();

    bool init(const FetchConfig &config = FetchConfig{});
    void deinit();
    bool initialized() const { return _initialized; }

    bool get(const char *url, FetchCallback callback, const FetchRequestOptions &options = FetchRequestOptions{});
    bool get(const String &url, FetchCallback callback, const FetchRequestOptions &options = FetchRequestOptions{});
    JsonDocument get(const char *url, TickType_t waitTicks, const FetchRequestOptions &options = FetchRequestOptions{});
    JsonDocument get(const String &url, TickType_t waitTicks, const FetchRequestOptions &options = FetchRequestOptions{});

    bool post(const char *url,
              const JsonDocument &payload,
              FetchCallback callback,
              const FetchRequestOptions &options = FetchRequestOptions{});
    bool post(const String &url,
              const JsonDocument &payload,
              FetchCallback callback,
              const FetchRequestOptions &options = FetchRequestOptions{});
    JsonDocument post(const char *url,
                      const JsonDocument &payload,
                      TickType_t waitTicks,
                      const FetchRequestOptions &options = FetchRequestOptions{});
    JsonDocument post(const String &url,
                      const JsonDocument &payload,
                      TickType_t waitTicks,
                      const FetchRequestOptions &options = FetchRequestOptions{});

    // Stream download (binary / any kind). No JSON handling.
    bool getStream(const char *url,
                   FetchChunkCallback onChunk,
                   FetchStreamCallback onDone = nullptr,
                   const FetchRequestOptions &options = FetchRequestOptions{});
    bool getStream(const String &url,
                   FetchChunkCallback onChunk,
                   FetchStreamCallback onDone = nullptr,
                   const FetchRequestOptions &options = FetchRequestOptions{});

   private:
    struct FetchJob;
    struct FetchResponse;
    struct SyncHandle;

    bool enqueueRequest(const std::string &url,
                        esp_http_client_method_t method,
                        std::string &&body,
                        FetchCallback callback,
                        std::shared_ptr<SyncHandle> syncHandle,
                        const FetchRequestOptions &options);

    bool enqueueStreamRequest(const std::string &url,
                              FetchChunkCallback onChunk,
                              FetchStreamCallback onDone,
                              const FetchRequestOptions &options);

    JsonDocument waitForResult(const std::shared_ptr<SyncHandle> &handle, TickType_t waitTicks) const;

    static void requestTask(void *arg);
    static esp_err_t handleHttpEvent(esp_http_client_event_t *event);

    void runJob(std::unique_ptr<FetchJob> job);
    JsonDocument buildResult(const FetchJob &job, const FetchResponse &response) const;
    static void deliverResult(const std::unique_ptr<FetchJob> &job, const JsonDocument &result);

    FetchConfig _config{};
    bool _initialized = false;
    std::atomic<size_t> _activeTasks{0};
    SemaphoreHandle_t _slotSemaphore = nullptr;
};
