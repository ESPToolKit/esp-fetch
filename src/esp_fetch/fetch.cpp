#include "esp_fetch/fetch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
}

namespace {
constexpr const char *TAG = "ESPFetch";

bool equalsIgnoreCase(const std::string &lhs, const char *rhs) {
    if (!rhs) {
        return false;
    }
    const size_t rhsLen = std::strlen(rhs);
    if (lhs.size() != rhsLen) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}
}  // namespace

struct ESPFetch::FetchResponse {
    esp_err_t error = ESP_OK;
    int statusCode = 0;
    std::string body;
    std::vector<FetchHeader> headers;
    bool bodyTruncated = false;
    bool headersTruncated = false;
    int64_t durationUs = 0;
};

struct ESPFetch::SyncHandle {
    SyncHandle() = default;
    ~SyncHandle() {
        if (done) {
            vSemaphoreDelete(done);
            done = nullptr;
        }
    }

    SemaphoreHandle_t done = nullptr;
    bool ready = false;
    JsonDocument doc;
};

struct ESPFetch::FetchJob {
    ESPFetch *owner = nullptr;
    std::string url;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    std::string body;
    FetchRequestOptions options;

    // JSON mode callback (existing APIs)
    FetchCallback callback;
    std::shared_ptr<SyncHandle> syncHandle;

    // Limits (used differently depending on mode)
    size_t bodyLimit = 0;
    size_t headerLimit = 0;

    // Response bookkeeping
    FetchResponse response;

    // Stream mode (new APIs)
    bool isStream = false;
    FetchChunkCallback onChunk;
    FetchStreamCallback onDone;
    size_t receivedBytes = 0;
};

ESPFetch::~ESPFetch() {
    deinit();
}

bool ESPFetch::init(const FetchConfig &config) {
    if (_initialized) {
        deinit();
    }

    if (config.maxConcurrentRequests == 0) {
        ESP_LOGE(TAG, "maxConcurrentRequests must be > 0");
        return false;
    }

    _config = config;
    _slotSemaphore = xSemaphoreCreateCounting(_config.maxConcurrentRequests, _config.maxConcurrentRequests);
    if (!_slotSemaphore) {
        ESP_LOGE(TAG, "Failed to create fetch semaphore");
        return false;
    }

    _initialized = true;
    return true;
}

void ESPFetch::deinit() {
    _initialized = false;

    while (_activeTasks.load(std::memory_order_acquire) > 0) {
#if defined(INCLUDE_xTaskGetSchedulerState) && (INCLUDE_xTaskGetSchedulerState == 1)
        if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
            break;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    _activeTasks.store(0, std::memory_order_release);

    if (_slotSemaphore) {
        vSemaphoreDelete(_slotSemaphore);
        _slotSemaphore = nullptr;
    }
}

bool ESPFetch::get(const char *url, FetchCallback callback, const FetchRequestOptions &options) {
    if (!url) {
        return false;
    }
    return enqueueRequest(url, HTTP_METHOD_GET, std::string{}, std::move(callback), nullptr, options);
}

bool ESPFetch::get(const String &url, FetchCallback callback, const FetchRequestOptions &options) {
    return get(url.c_str(), std::move(callback), options);
}

JsonDocument ESPFetch::get(const char *url, TickType_t waitTicks, const FetchRequestOptions &options) {
    if (!url) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "url is null";
        return doc;
    }
    auto handle = std::make_shared<SyncHandle>();
    handle->done = xSemaphoreCreateBinary();
    if (!handle->done) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "failed to allocate sync semaphore";
        return doc;
    }

    if (!enqueueRequest(url, HTTP_METHOD_GET, std::string{}, nullptr, handle, options)) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "failed to start http get";
        return doc;
    }

    return waitForResult(handle, waitTicks);
}

JsonDocument ESPFetch::get(const String &url, TickType_t waitTicks, const FetchRequestOptions &options) {
    return get(url.c_str(), waitTicks, options);
}

bool ESPFetch::post(const char *url,
                    const JsonDocument &payload,
                    FetchCallback callback,
                    const FetchRequestOptions &options) {
    if (!url) {
        return false;
    }
    std::string body;
    serializeJson(payload, body);
    return enqueueRequest(url, HTTP_METHOD_POST, std::move(body), std::move(callback), nullptr, options);
}

bool ESPFetch::post(const String &url,
                    const JsonDocument &payload,
                    FetchCallback callback,
                    const FetchRequestOptions &options) {
    return post(url.c_str(), payload, std::move(callback), options);
}

JsonDocument ESPFetch::post(const char *url,
                            const JsonDocument &payload,
                            TickType_t waitTicks,
                            const FetchRequestOptions &options) {
    if (!url) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "url is null";
        return doc;
    }
    std::string body;
    serializeJson(payload, body);

    auto handle = std::make_shared<SyncHandle>();
    handle->done = xSemaphoreCreateBinary();
    if (!handle->done) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "failed to allocate sync semaphore";
        return doc;
    }

    if (!enqueueRequest(url, HTTP_METHOD_POST, std::move(body), nullptr, handle, options)) {
        JsonDocument doc;
        doc["ok"] = false;
        doc["error"]["message"] = "failed to start http post";
        return doc;
    }

    return waitForResult(handle, waitTicks);
}

JsonDocument ESPFetch::post(const String &url,
                            const JsonDocument &payload,
                            TickType_t waitTicks,
                            const FetchRequestOptions &options) {
    return post(url.c_str(), payload, waitTicks, options);
}

// ------------------------------
// Stream API (new)
// ------------------------------
bool ESPFetch::getStream(const char *url,
                         FetchChunkCallback onChunk,
                         FetchStreamCallback onDone,
                         const FetchRequestOptions &options) {
    if (!url || !onChunk) {
        return false;
    }
    return enqueueStreamRequest(url, std::move(onChunk), std::move(onDone), options);
}

bool ESPFetch::getStream(const String &url,
                         FetchChunkCallback onChunk,
                         FetchStreamCallback onDone,
                         const FetchRequestOptions &options) {
    return getStream(url.c_str(), std::move(onChunk), std::move(onDone), options);
}

bool ESPFetch::enqueueRequest(const std::string &url,
                              esp_http_client_method_t method,
                              std::string &&body,
                              FetchCallback callback,
                              std::shared_ptr<SyncHandle> syncHandle,
                              const FetchRequestOptions &options) {
    if (!_initialized) {
        ESP_LOGE(TAG, "ESPFetch not initialized");
        return false;
    }

    if (xSemaphoreTake(_slotSemaphore, _config.slotAcquireTicks) != pdTRUE) {
        ESP_LOGW(TAG, "No available fetch slots");
        return false;
    }

    auto job = std::make_unique<FetchJob>();
    job->owner = this;
    job->url = url;
    job->method = method;
    job->body = std::move(body);
    job->options = options;
    job->callback = std::move(callback);
    job->syncHandle = std::move(syncHandle);

    job->bodyLimit = options.maxBodyBytes ? options.maxBodyBytes : _config.maxBodyBytes;
    job->headerLimit = options.maxHeaderBytes ? options.maxHeaderBytes : _config.maxHeaderBytes;
    if (job->bodyLimit == 0) {
        job->bodyLimit = std::numeric_limits<size_t>::max();
    }
    if (job->headerLimit == 0) {
        job->headerLimit = std::numeric_limits<size_t>::max();
    }

    size_t reserveBytes =
        job->bodyLimit == std::numeric_limits<size_t>::max()
            ? static_cast<size_t>(1024)
            : std::min(job->bodyLimit, static_cast<size_t>(1024));
    job->response.body.reserve(reserveBytes);

    size_t stackSize = _config.stackSize;
    if (stackSize == 0) {
        ESP_LOGE(TAG, "Invalid stack size for fetch worker");
        xSemaphoreGive(_slotSemaphore);
        return false;
    }

    _activeTasks.fetch_add(1, std::memory_order_acq_rel);

    TaskHandle_t handle = nullptr;
    FetchJob *jobPtr = job.release();
    BaseType_t res =
        xTaskCreatePinnedToCore(&ESPFetch::requestTask,
                                "esp-fetch",
                                stackSize,
                                jobPtr,
                                _config.priority,
                                &handle,
                                _config.coreId);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn fetch task");
        _activeTasks.fetch_sub(1, std::memory_order_acq_rel);
        delete jobPtr;
        xSemaphoreGive(_slotSemaphore);
        return false;
    }
    return true;
}

bool ESPFetch::enqueueStreamRequest(const std::string &url,
                                    FetchChunkCallback onChunk,
                                    FetchStreamCallback onDone,
                                    const FetchRequestOptions &options) {
    if (!_initialized) {
        ESP_LOGE(TAG, "ESPFetch not initialized");
        return false;
    }

    if (!onChunk) {
        ESP_LOGE(TAG, "getStream requires onChunk callback");
        return false;
    }

    if (xSemaphoreTake(_slotSemaphore, _config.slotAcquireTicks) != pdTRUE) {
        ESP_LOGW(TAG, "No available fetch slots");
        return false;
    }

    auto job = std::make_unique<FetchJob>();
    job->owner = this;
    job->url = url;
    job->method = HTTP_METHOD_GET;
    job->options = options;

    job->isStream = true;
    job->onChunk = std::move(onChunk);
    job->onDone = std::move(onDone);
    job->receivedBytes = 0;

    // For streaming, default to "unlimited" unless the caller explicitly sets maxBodyBytes.
    job->bodyLimit = options.maxBodyBytes ? options.maxBodyBytes : std::numeric_limits<size_t>::max();
    job->headerLimit = options.maxHeaderBytes ? options.maxHeaderBytes : _config.maxHeaderBytes;
    if (job->headerLimit == 0) {
        job->headerLimit = std::numeric_limits<size_t>::max();
    }

    size_t stackSize = _config.stackSize;
    if (stackSize == 0) {
        ESP_LOGE(TAG, "Invalid stack size for fetch worker");
        xSemaphoreGive(_slotSemaphore);
        return false;
    }

    _activeTasks.fetch_add(1, std::memory_order_acq_rel);

    TaskHandle_t handle = nullptr;
    FetchJob *jobPtr = job.release();
    BaseType_t res =
        xTaskCreatePinnedToCore(&ESPFetch::requestTask,
                                "esp-fetch",
                                stackSize,
                                jobPtr,
                                _config.priority,
                                &handle,
                                _config.coreId);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn fetch task");
        _activeTasks.fetch_sub(1, std::memory_order_acq_rel);
        delete jobPtr;
        xSemaphoreGive(_slotSemaphore);
        return false;
    }
    return true;
}

JsonDocument ESPFetch::waitForResult(const std::shared_ptr<SyncHandle> &handle, TickType_t waitTicks) const {
    JsonDocument doc;
    if (!handle || !handle->done) {
        doc["ok"] = false;
        doc["error"]["message"] = "invalid sync handle";
        return doc;
    }

    if (xSemaphoreTake(handle->done, waitTicks) == pdTRUE && handle->ready) {
        doc = handle->doc;
    } else if (handle->ready) {
        doc = handle->doc;
    } else {
        doc["ok"] = false;
        doc["error"]["message"] = "timeout waiting for fetch result";
    }
    return doc;
}

void ESPFetch::requestTask(void *arg) {
    auto job = std::unique_ptr<FetchJob>(static_cast<FetchJob *>(arg));
    if (!job || !job->owner) {
        if (job && job->owner) {
            job->owner->_activeTasks.fetch_sub(1, std::memory_order_acq_rel);
        }
        vTaskDelete(nullptr);
        return;
    }
    job->owner->runJob(std::move(job));
    vTaskDelete(nullptr);
}

esp_err_t ESPFetch::handleHttpEvent(esp_http_client_event_t *event) {
    if (!event || !event->user_data) {
        return ESP_OK;
    }
    auto *job = static_cast<FetchJob *>(event->user_data);

    switch (event->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (event->data && event->data_len > 0) {
                // Stream mode: do NOT buffer body; forward chunks directly.
                if (job->isStream) {
                    size_t toSend = static_cast<size_t>(event->data_len);

                    if (job->bodyLimit != std::numeric_limits<size_t>::max()) {
                        if (job->receivedBytes >= job->bodyLimit) {
                            job->response.error = ESP_ERR_INVALID_SIZE;
                            return ESP_FAIL;
                        }
                        const size_t remaining = job->bodyLimit - job->receivedBytes;
                        toSend = std::min(toSend, remaining);
                    }

                    if (toSend > 0 && job->onChunk) {
                        job->onChunk(event->data, toSend);
                        job->receivedBytes += toSend;
                    }

                    // If we had to clip the chunk, we've hit the limit and abort.
                    if (toSend < static_cast<size_t>(event->data_len)) {
                        job->response.error = ESP_ERR_INVALID_SIZE;
                        return ESP_FAIL;
                    }
                } else {
                    // JSON mode (existing): buffer into response.body with limit/truncation.
                    size_t available =
                        job->response.body.size() < job->bodyLimit ? (job->bodyLimit - job->response.body.size()) : 0;
                    size_t copyLen = std::min(available, static_cast<size_t>(event->data_len));
                    if (copyLen > 0) {
                        job->response.body.append(static_cast<const char *>(event->data), copyLen);
                    }
                    if (copyLen < static_cast<size_t>(event->data_len)) {
                        job->response.bodyTruncated = true;
                    }
                }
            }
            break;

        case HTTP_EVENT_ON_HEADER:
            if (event->header_key && event->header_value) {
                size_t projected = 0;
                for (const auto &hdr : job->response.headers) {
                    projected += hdr.name.size() + hdr.value.size();
                }
                projected += strlen(event->header_key) + strlen(event->header_value);
                if (projected <= job->headerLimit) {
                    job->response.headers.push_back({event->header_key, event->header_value});
                } else {
                    job->response.headersTruncated = true;
                }
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

void ESPFetch::runJob(std::unique_ptr<FetchJob> job) {
    if (!job) {
        return;
    }

    const int64_t start = esp_timer_get_time();

    esp_http_client_config_t config = {};
    config.url = job->url.c_str();
    config.method = job->method;
    config.timeout_ms = job->options.timeoutMs ? job->options.timeoutMs : _config.defaultTimeoutMs;
    config.event_handler = &ESPFetch::handleHttpEvent;
    config.user_data = job.get();
    config.disable_auto_redirect = !(job->options.allowRedirects && _config.followRedirects);
    config.skip_cert_common_name_check = job->options.skipTlsCommonNameCheck || _config.skipTlsCommonNameCheck;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        job->response.error = ESP_ERR_NO_MEM;
    } else {
        auto hasHeader = [&](const char *key) {
            for (const auto &header : job->options.headers) {
                if (equalsIgnoreCase(header.name, key)) {
                    return true;
                }
            }
            return false;
        };

        if (_config.userAgent && !hasHeader("User-Agent")) {
            esp_http_client_set_header(client, "User-Agent", _config.userAgent);
        }

        const char *contentType = job->options.contentType ? job->options.contentType : _config.defaultContentType;
        if (!job->isStream && job->method == HTTP_METHOD_POST && contentType && !hasHeader("Content-Type")) {
            esp_http_client_set_header(client, "Content-Type", contentType);
        }

        for (const auto &header : job->options.headers) {
            esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
        }

        if (!job->body.empty()) {
            esp_http_client_set_post_field(client, job->body.c_str(), job->body.length());
        }

        job->response.error = esp_http_client_perform(client);
        if (job->response.error == ESP_OK) {
            job->response.statusCode = esp_http_client_get_status_code(client);
        }
        esp_http_client_cleanup(client);
    }

    job->response.durationUs = esp_timer_get_time() - start;

    if (job->isStream) {
        StreamResult r;
        r.error = job->response.error;
        r.statusCode = job->response.statusCode;
        r.receivedBytes = job->receivedBytes;
        if (job->onDone) {
            job->onDone(r);
        }
    } else {
        JsonDocument result = buildResult(*job, job->response);
        deliverResult(job, result);
    }

    if (_slotSemaphore) {
        xSemaphoreGive(_slotSemaphore);
    }

    _activeTasks.fetch_sub(1, std::memory_order_acq_rel);
}

JsonDocument ESPFetch::buildResult(const FetchJob &job, const FetchResponse &response) const {
    JsonDocument doc;
    auto root = doc.to<JsonObject>();
    root["url"] = job.url.c_str();
    root["method"] = job.method == HTTP_METHOD_POST ? "POST" : "GET";
    const bool httpOk = response.statusCode >= 200 && response.statusCode < 400;
    root["status"] = response.statusCode;
    root["ok"] = response.error == ESP_OK && httpOk;
    root["duration_ms"] = static_cast<int>(response.durationUs / 1000);
    root["body"] = response.body.c_str();
    root["body_truncated"] = response.bodyTruncated;
    root["headers_truncated"] = response.headersTruncated;

    auto headersObj = root["headers"].to<JsonObject>();
    for (const auto &header : response.headers) {
        headersObj[header.name] = header.value;
    }

    if (response.error == ESP_OK) {
        root["error"] = nullptr;
    } else {
        auto err = root["error"].to<JsonObject>();
        err["code"] = static_cast<int>(response.error);
        err["message"] = esp_err_to_name(response.error);
    }
    return doc;
}

void ESPFetch::deliverResult(const std::unique_ptr<FetchJob> &job, const JsonDocument &result) {
    if (!job) {
        return;
    }
    if (job->callback) {
        job->callback(result);
    }
    if (job->syncHandle) {
        job->syncHandle->doc = result;
        job->syncHandle->ready = true;
        if (job->syncHandle->done) {
            xSemaphoreGive(job->syncHandle->done);
        }
    }
}
