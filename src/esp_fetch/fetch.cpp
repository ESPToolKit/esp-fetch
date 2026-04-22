#include "esp_fetch/fetch.h"
#include "esp_fetch/fetch_allocator.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <limits>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
}

#if ESP_FETCH_HAVE_CRT_BUNDLE
extern "C" {
#include <esp_crt_bundle.h>
}
#endif

namespace {
constexpr const char *TAG = "ESPFetch";

struct InternalFetchHeader {
	FetchString name;
	FetchString value;

	InternalFetchHeader(
	    const char *headerName, const char *headerValue, const FetchAllocator<char> &allocator
	)
	    : name(headerName ? headerName : "", allocator),
	      value(headerValue ? headerValue : "", allocator) {
	}
};

using InternalFetchHeaderVector = FetchVector<InternalFetchHeader>;

template <typename TString> bool equalsIgnoreCase(const TString &lhs, const char *rhs) {
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

struct InternalFetchRequestOptions {
	explicit InternalFetchRequestOptions(bool usePSRAMBuffers = false)
	    : charAllocator(usePSRAMBuffers), headerAllocator(usePSRAMBuffers),
	      headers(headerAllocator) {
	}

	FetchAllocator<char> charAllocator;
	FetchAllocator<InternalFetchHeader> headerAllocator;

	uint32_t timeoutMs = 0;
	size_t maxBodyBytes = 0;
	size_t maxHeaderBytes = 0;
	size_t rxBufferSize = 0;
	size_t txBufferSize = 0;
	const char *caCertPem = nullptr;
	std::optional<bool> useTlsCertBundle;
	std::optional<bool> useGlobalCaStore;
	std::optional<bool> skipTlsServerCertValidation;
	std::optional<bool> skipTlsCommonNameCheck;
	bool allowRedirects = true;
	InternalFetchHeaderVector headers;
	const char *contentType = nullptr;
};

struct FetchStringWriter {
	explicit FetchStringWriter(FetchString &target) : target_(target) {
	}

	size_t write(uint8_t value) {
		target_.push_back(static_cast<char>(value));
		return 1;
	}

	size_t write(const uint8_t *buffer, size_t size) {
		if (!buffer || size == 0) {
			return 0;
		}
		target_.append(reinterpret_cast<const char *>(buffer), size);
		return size;
	}

  private:
	FetchString &target_;
};

bool startsWithIgnoreCase(const std::string &value, const char *prefix) {
	if (!prefix) {
		return false;
	}
	const size_t prefixLen = std::strlen(prefix);
	if (value.size() < prefixLen) {
		return false;
	}
	for (size_t i = 0; i < prefixLen; ++i) {
		if (std::tolower(static_cast<unsigned char>(value[i])) !=
		    std::tolower(static_cast<unsigned char>(prefix[i]))) {
			return false;
		}
	}
	return true;
}

std::string trimUrl(const std::string &value) {
	size_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
		++start;
	}
	size_t end = value.size();
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		--end;
	}
	return value.substr(start, end - start);
}

bool normalizeScheme(std::string &url, const char *scheme) {
	const size_t schemeLen = std::strlen(scheme);
	if (url.size() <= schemeLen || !startsWithIgnoreCase(url, scheme)) {
		return false;
	}
	if (url[schemeLen] != ':') {
		return false;
	}

	size_t slashPos = schemeLen + 1;
	size_t slashCount = 0;
	while (slashPos + slashCount < url.size() && url[slashPos + slashCount] == '/') {
		++slashCount;
	}
	if (slashCount == 2) {
		return false;
	}
	if (slashCount > 2) {
		url.erase(slashPos + 2, slashCount - 2);
		return true;
	}
	if (slashCount == 1) {
		url.insert(slashPos, "/");
		return true;
	}
	url.insert(slashPos, "//");
	return true;
}

bool stripLeadingHostColon(std::string &url) {
	const size_t schemePos = url.find("://");
	if (schemePos == std::string::npos) {
		return false;
	}
	if (!startsWithIgnoreCase(url, "http") && !startsWithIgnoreCase(url, "https")) {
		return false;
	}
	const size_t hostPos = schemePos + 3;
	if (hostPos >= url.size() || url[hostPos] != ':') {
		return false;
	}
	url.erase(hostPos, 1);
	return true;
}

std::string normalizeUrl(const std::string &url) {
	std::string normalized = trimUrl(url);
	bool changed = false;
	changed = normalizeScheme(normalized, "https") || changed;
	changed = normalizeScheme(normalized, "http") || changed;
	changed = stripLeadingHostColon(normalized) || changed;
	if (changed) {
		ESP_LOGW(TAG, "Normalized URL to %s", normalized.c_str());
	}
	return normalized;
}

int resolveHttpBufferSize(const char *label, size_t requestValue, size_t configValue) {
	const size_t selected = requestValue ? requestValue : configValue;
	if (selected == 0) {
		return 0;
	}
	if (selected > static_cast<size_t>(INT_MAX)) {
		ESP_LOGW(TAG, "%s exceeds INT_MAX, clamping to %d bytes", label, INT_MAX);
		return INT_MAX;
	}
	return static_cast<int>(selected);
}

template <typename Callback, typename... Args>
void invokeFetchCallback(const Callback &callback, Args... args) noexcept {
	if (!callback) {
		return;
	}

#if defined(__cpp_exceptions)
	try {
		callback(args...);
	} catch (...) {
	}
#else
	callback(args...);
#endif
}

template <typename Callback, typename... Args>
bool invokeFetchChunkCallback(const Callback &callback, Args... args) noexcept {
	if (!callback) {
		return true;
	}

#if defined(__cpp_exceptions)
	try {
		return callback(args...);
	} catch (...) {
		return false;
	}
#else
	return callback(args...);
#endif
}

template <typename Callback, typename... Args>
bool invokeFetchStartCallback(const Callback &callback, Args... args) noexcept {
	if (!callback) {
		return true;
	}

#if defined(__cpp_exceptions)
	try {
		return callback(args...);
	} catch (...) {
		return false;
	}
#else
	return callback(args...);
#endif
}

constexpr size_t STREAM_READ_BUFFER_SIZE_FALLBACK_BYTES = 1024;

bool isRedirectHttpStatus(int statusCode) {
	return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 ||
	       statusCode == 308;
}

esp_err_t mapStreamReadFailure(esp_http_client_handle_t client, int readResult) {
	if (readResult == -ESP_ERR_HTTP_EAGAIN) {
		return ESP_ERR_HTTP_READ_TIMEOUT;
	}

	if (esp_http_client_get_errno(client) == ENOTCONN) {
		return ESP_ERR_HTTP_CONNECTION_CLOSED;
	}

	return ESP_ERR_HTTP_INCOMPLETE_DATA;
}
} // namespace

struct ESPFetch::FetchResponse {
	explicit FetchResponse(bool usePSRAMBuffers = false)
	    : charAllocator(usePSRAMBuffers), headerAllocator(usePSRAMBuffers), body(charAllocator),
	      headers(headerAllocator) {
	}

	esp_err_t error = ESP_OK;
	int statusCode = 0;
	FetchAllocator<char> charAllocator;
	FetchAllocator<InternalFetchHeader> headerAllocator;
	FetchString body;
	InternalFetchHeaderVector headers;
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
	explicit FetchJob(bool usePSRAMBuffers = false)
	    : stringAllocator(usePSRAMBuffers), url(stringAllocator), body(stringAllocator),
	      requestOptions(usePSRAMBuffers), response(usePSRAMBuffers) {
	}

	ESPFetch *owner = nullptr;
	FetchAllocator<char> stringAllocator;
	FetchString url;
	esp_http_client_method_t method = HTTP_METHOD_GET;
	FetchString body;
	InternalFetchRequestOptions requestOptions;

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
	FetchStreamStartCallback onStart;
	FetchChunkCallback onChunk;
	FetchStreamCallback onDone;
	size_t receivedBytes = 0;
	esp_err_t streamAbortError = ESP_OK;
	bool streamStartRejected = false;
};

ESPFetch::~ESPFetch() {
	deinit();
}

bool ESPFetch::init(const FetchConfig &config) {
	if (isInitialized()) {
		deinit();
	}

	if (config.maxConcurrentRequests == 0) {
		ESP_LOGE(TAG, "maxConcurrentRequests must be > 0");
		return false;
	}

	_config = config;
	_slotSemaphore =
	    xSemaphoreCreateCounting(_config.maxConcurrentRequests, _config.maxConcurrentRequests);
	if (!_slotSemaphore) {
		ESP_LOGE(TAG, "Failed to create fetch semaphore");
		return false;
	}

	_teardownRequested.store(false, std::memory_order_release);
	_initialized.store(true, std::memory_order_release);
	return true;
}

void ESPFetch::deinit() {
	if (!isInitialized() && _activeTasks.load(std::memory_order_acquire) == 0 &&
	    _slotSemaphore == nullptr) {
		return;
	}

	_teardownRequested.store(true, std::memory_order_release);
	_initialized.store(false, std::memory_order_release);

	while (_activeTasks.load(std::memory_order_acquire) > 0) {
#if defined(INCLUDE_xTaskGetSchedulerState) && (INCLUDE_xTaskGetSchedulerState == 1)
		if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
			break;
		}
#endif
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	if (_slotSemaphore) {
		vSemaphoreDelete(_slotSemaphore);
		_slotSemaphore = nullptr;
	}

	_teardownRequested.store(false, std::memory_order_release);
}

bool ESPFetch::isInitialized() const {
	return _initialized.load(std::memory_order_acquire);
}

bool ESPFetch::get(const char *url, FetchCallback callback, const FetchRequestOptions &options) {
	if (!url) {
		return false;
	}
	return enqueueRequest(
	    url,
	    HTTP_METHOD_GET,
	    FetchString{FetchAllocator<char>(_config.usePSRAMBuffers)},
	    std::move(callback),
	    nullptr,
	    options
	);
}

bool ESPFetch::get(const String &url, FetchCallback callback, const FetchRequestOptions &options) {
	return get(url.c_str(), std::move(callback), options);
}

JsonDocument
ESPFetch::get(const char *url, TickType_t waitTicks, const FetchRequestOptions &options) {
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

	const char *startError = nullptr;
	if (!enqueueRequest(
	        url,
	        HTTP_METHOD_GET,
	        FetchString{FetchAllocator<char>(_config.usePSRAMBuffers)},
	        nullptr,
	        handle,
	        options,
	        &startError
	    )) {
		JsonDocument doc;
		doc["ok"] = false;
		doc["error"]["message"] = startError ? startError : "failed to start http get";
		return doc;
	}

	return waitForResult(handle, waitTicks);
}

JsonDocument
ESPFetch::get(const String &url, TickType_t waitTicks, const FetchRequestOptions &options) {
	return get(url.c_str(), waitTicks, options);
}

bool ESPFetch::post(
    const char *url,
    const JsonDocument &payload,
    FetchCallback callback,
    const FetchRequestOptions &options
) {
	if (!url) {
		return false;
	}
	FetchString body{FetchAllocator<char>(_config.usePSRAMBuffers)};
	FetchStringWriter writer(body);
	serializeJson(payload, writer);
	return enqueueRequest(
	    url,
	    HTTP_METHOD_POST,
	    std::move(body),
	    std::move(callback),
	    nullptr,
	    options
	);
}

bool ESPFetch::post(
    const String &url,
    const JsonDocument &payload,
    FetchCallback callback,
    const FetchRequestOptions &options
) {
	return post(url.c_str(), payload, std::move(callback), options);
}

JsonDocument ESPFetch::post(
    const char *url,
    const JsonDocument &payload,
    TickType_t waitTicks,
    const FetchRequestOptions &options
) {
	if (!url) {
		JsonDocument doc;
		doc["ok"] = false;
		doc["error"]["message"] = "url is null";
		return doc;
	}
	FetchString body{FetchAllocator<char>(_config.usePSRAMBuffers)};
	FetchStringWriter writer(body);
	serializeJson(payload, writer);

	auto handle = std::make_shared<SyncHandle>();
	handle->done = xSemaphoreCreateBinary();
	if (!handle->done) {
		JsonDocument doc;
		doc["ok"] = false;
		doc["error"]["message"] = "failed to allocate sync semaphore";
		return doc;
	}

	const char *startError = nullptr;
	if (!enqueueRequest(
	        url,
	        HTTP_METHOD_POST,
	        std::move(body),
	        nullptr,
	        handle,
	        options,
	        &startError
	    )) {
		JsonDocument doc;
		doc["ok"] = false;
		doc["error"]["message"] = startError ? startError : "failed to start http post";
		return doc;
	}

	return waitForResult(handle, waitTicks);
}

JsonDocument ESPFetch::post(
    const String &url,
    const JsonDocument &payload,
    TickType_t waitTicks,
    const FetchRequestOptions &options
) {
	return post(url.c_str(), payload, waitTicks, options);
}

// ------------------------------
// Stream API (new)
// ------------------------------
bool ESPFetch::getStream(
    const char *url,
    FetchChunkCallback onChunk,
    FetchStreamCallback onDone,
    const FetchRequestOptions &options
) {
	if (!url || !onChunk) {
		return false;
	}
	return enqueueStreamRequest(url, nullptr, std::move(onChunk), std::move(onDone), options);
}

bool ESPFetch::getStream(
    const char *url,
    FetchStreamStartCallback onStart,
    FetchChunkCallback onChunk,
    FetchStreamCallback onDone,
    const FetchRequestOptions &options
) {
	if (!url || !onChunk) {
		return false;
	}
	return enqueueStreamRequest(
	    url,
	    std::move(onStart),
	    std::move(onChunk),
	    std::move(onDone),
	    options
	);
}

bool ESPFetch::getStream(
    const String &url,
    FetchChunkCallback onChunk,
    FetchStreamCallback onDone,
    const FetchRequestOptions &options
) {
	return getStream(url.c_str(), std::move(onChunk), std::move(onDone), options);
}

bool ESPFetch::getStream(
    const String &url,
    FetchStreamStartCallback onStart,
    FetchChunkCallback onChunk,
    FetchStreamCallback onDone,
    const FetchRequestOptions &options
) {
	return getStream(
	    url.c_str(),
	    std::move(onStart),
	    std::move(onChunk),
	    std::move(onDone),
	    options
	);
}

bool ESPFetch::enqueueRequest(
    const std::string &url,
    esp_http_client_method_t method,
    FetchString &&body,
    FetchCallback callback,
    std::shared_ptr<SyncHandle> syncHandle,
    const FetchRequestOptions &options,
    const char **startErrorOut
) {
	if (!isInitialized()) {
		ESP_LOGE(TAG, "ESPFetch not initialized");
		if (startErrorOut != nullptr) {
			*startErrorOut = "ESPFetch not initialized";
		}
		return false;
	}

	const std::string normalizedUrl = normalizeUrl(url);
	const char *tlsError =
	    esp_fetch_detail::validateFetchTlsOptions(normalizedUrl, _config, options);
	if (tlsError != nullptr) {
		ESP_LOGE(TAG, "Rejected request for %s: %s", normalizedUrl.c_str(), tlsError);
		if (startErrorOut != nullptr) {
			*startErrorOut = tlsError;
		}
		return false;
	}

	if (xSemaphoreTake(_slotSemaphore, _config.slotAcquireTicks) != pdTRUE) {
		ESP_LOGW(TAG, "No available fetch slots");
		if (startErrorOut != nullptr) {
			*startErrorOut = "no available fetch slots";
		}
		return false;
	}

	auto job = std::make_unique<FetchJob>(_config.usePSRAMBuffers);
	job->owner = this;
	job->url.assign(normalizedUrl.c_str(), normalizedUrl.size());
	job->method = method;
	job->body = std::move(body);
	job->requestOptions.timeoutMs = options.timeoutMs;
	job->requestOptions.maxBodyBytes = options.maxBodyBytes;
	job->requestOptions.maxHeaderBytes = options.maxHeaderBytes;
	job->requestOptions.rxBufferSize = options.rxBufferSize;
	job->requestOptions.txBufferSize = options.txBufferSize;
	job->requestOptions.caCertPem = options.caCertPem;
	job->requestOptions.useTlsCertBundle = options.useTlsCertBundle;
	job->requestOptions.useGlobalCaStore = options.useGlobalCaStore;
	job->requestOptions.skipTlsServerCertValidation = options.skipTlsServerCertValidation;
	job->requestOptions.skipTlsCommonNameCheck = options.skipTlsCommonNameCheck;
	job->requestOptions.allowRedirects = options.allowRedirects;
	job->requestOptions.contentType = options.contentType;
	job->requestOptions.headers.clear();
	job->requestOptions.headers.reserve(options.headers.size());
	for (const auto &header : options.headers) {
		job->requestOptions.headers
		    .emplace_back(header.name.c_str(), header.value.c_str(), job->stringAllocator);
	}
	job->callback = std::move(callback);
	job->syncHandle = std::move(syncHandle);

	job->bodyLimit =
	    job->requestOptions.maxBodyBytes ? job->requestOptions.maxBodyBytes : _config.maxBodyBytes;
	job->headerLimit = job->requestOptions.maxHeaderBytes ? job->requestOptions.maxHeaderBytes
	                                                      : _config.maxHeaderBytes;
	if (job->bodyLimit == 0) {
		job->bodyLimit = std::numeric_limits<size_t>::max();
	}
	if (job->headerLimit == 0) {
		job->headerLimit = std::numeric_limits<size_t>::max();
	}

	size_t reserveBytes = job->bodyLimit == std::numeric_limits<size_t>::max()
	                          ? static_cast<size_t>(1024)
	                          : std::min(job->bodyLimit, static_cast<size_t>(1024));
	job->response.body.reserve(reserveBytes);

	size_t stackSize = _config.stackSize;
	if (stackSize == 0) {
		ESP_LOGE(TAG, "Invalid stack size for fetch worker");
		if (startErrorOut != nullptr) {
			*startErrorOut = "invalid stack size for fetch worker";
		}
		xSemaphoreGive(_slotSemaphore);
		return false;
	}

	_activeTasks.fetch_add(1, std::memory_order_acq_rel);

	FetchJob *jobPtr = job.release();
	TaskHandle_t taskHandle = nullptr;
	const BaseType_t created = xTaskCreatePinnedToCore(
	    &ESPFetch::requestTask,
	    "esp-fetch",
	    stackSize,
	    jobPtr,
	    _config.priority,
	    &taskHandle,
	    _config.coreId
	);
	if (created != pdPASS) {
		ESP_LOGE(TAG, "Failed to spawn fetch task");
		if (startErrorOut != nullptr) {
			*startErrorOut = "failed to spawn fetch task";
		}
		_activeTasks.fetch_sub(1, std::memory_order_acq_rel);
		delete jobPtr;
		xSemaphoreGive(_slotSemaphore);
		return false;
	}
	return true;
}

bool ESPFetch::enqueueStreamRequest(
    const std::string &url,
    FetchStreamStartCallback onStart,
    FetchChunkCallback onChunk,
    FetchStreamCallback onDone,
    const FetchRequestOptions &options,
    const char **startErrorOut
) {
	if (!isInitialized()) {
		ESP_LOGE(TAG, "ESPFetch not initialized");
		if (startErrorOut != nullptr) {
			*startErrorOut = "ESPFetch not initialized";
		}
		return false;
	}

	if (!onChunk) {
		ESP_LOGE(TAG, "getStream requires onChunk callback");
		if (startErrorOut != nullptr) {
			*startErrorOut = "getStream requires onChunk callback";
		}
		return false;
	}

	const std::string normalizedUrl = normalizeUrl(url);
	const char *tlsError =
	    esp_fetch_detail::validateFetchTlsOptions(normalizedUrl, _config, options);
	if (tlsError != nullptr) {
		ESP_LOGE(TAG, "Rejected stream request for %s: %s", normalizedUrl.c_str(), tlsError);
		if (startErrorOut != nullptr) {
			*startErrorOut = tlsError;
		}
		return false;
	}

	if (xSemaphoreTake(_slotSemaphore, _config.slotAcquireTicks) != pdTRUE) {
		ESP_LOGW(TAG, "No available fetch slots");
		if (startErrorOut != nullptr) {
			*startErrorOut = "no available fetch slots";
		}
		return false;
	}

	auto job = std::make_unique<FetchJob>(_config.usePSRAMBuffers);
	job->owner = this;
	job->url.assign(normalizedUrl.c_str(), normalizedUrl.size());
	job->method = HTTP_METHOD_GET;
	job->requestOptions.timeoutMs = options.timeoutMs;
	job->requestOptions.maxBodyBytes = options.maxBodyBytes;
	job->requestOptions.maxHeaderBytes = options.maxHeaderBytes;
	job->requestOptions.rxBufferSize = options.rxBufferSize;
	job->requestOptions.txBufferSize = options.txBufferSize;
	job->requestOptions.caCertPem = options.caCertPem;
	job->requestOptions.useTlsCertBundle = options.useTlsCertBundle;
	job->requestOptions.useGlobalCaStore = options.useGlobalCaStore;
	job->requestOptions.skipTlsServerCertValidation = options.skipTlsServerCertValidation;
	job->requestOptions.skipTlsCommonNameCheck = options.skipTlsCommonNameCheck;
	job->requestOptions.allowRedirects = options.allowRedirects;
	job->requestOptions.contentType = options.contentType;
	job->requestOptions.headers.clear();
	job->requestOptions.headers.reserve(options.headers.size());
	for (const auto &header : options.headers) {
		job->requestOptions.headers
		    .emplace_back(header.name.c_str(), header.value.c_str(), job->stringAllocator);
	}

	job->isStream = true;
	job->onStart = std::move(onStart);
	job->onChunk = std::move(onChunk);
	job->onDone = std::move(onDone);
	job->receivedBytes = 0;
	job->streamAbortError = ESP_OK;
	job->streamStartRejected = false;

	// For streaming, default to "unlimited" unless the caller explicitly sets maxBodyBytes.
	job->bodyLimit = job->requestOptions.maxBodyBytes ? job->requestOptions.maxBodyBytes
	                                                  : std::numeric_limits<size_t>::max();
	job->headerLimit = job->requestOptions.maxHeaderBytes ? job->requestOptions.maxHeaderBytes
	                                                      : _config.maxHeaderBytes;
	if (job->headerLimit == 0) {
		job->headerLimit = std::numeric_limits<size_t>::max();
	}

	size_t stackSize = _config.stackSize;
	if (stackSize == 0) {
		ESP_LOGE(TAG, "Invalid stack size for fetch worker");
		if (startErrorOut != nullptr) {
			*startErrorOut = "invalid stack size for fetch worker";
		}
		xSemaphoreGive(_slotSemaphore);
		return false;
	}

	_activeTasks.fetch_add(1, std::memory_order_acq_rel);

	FetchJob *jobPtr = job.release();
	TaskHandle_t taskHandle = nullptr;
	const BaseType_t created = xTaskCreatePinnedToCore(
	    &ESPFetch::requestTask,
	    "esp-fetch",
	    stackSize,
	    jobPtr,
	    _config.priority,
	    &taskHandle,
	    _config.coreId
	);
	if (created != pdPASS) {
		ESP_LOGE(TAG, "Failed to spawn fetch task");
		if (startErrorOut != nullptr) {
			*startErrorOut = "failed to spawn fetch task";
		}
		_activeTasks.fetch_sub(1, std::memory_order_acq_rel);
		delete jobPtr;
		xSemaphoreGive(_slotSemaphore);
		return false;
	}
	return true;
}

JsonDocument
ESPFetch::waitForResult(const std::shared_ptr<SyncHandle> &handle, TickType_t waitTicks) const {
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
	if (job->owner && job->owner->_teardownRequested.load(std::memory_order_acquire)) {
		if (job->isStream && job->streamAbortError == ESP_OK) {
			job->streamAbortError = ESP_ERR_INVALID_STATE;
		}
		return ESP_FAIL;
	}

	switch (event->event_id) {
	case HTTP_EVENT_ON_DATA:
		if (event->data && event->data_len > 0) {
			// Stream mode: do NOT buffer body; forward chunks directly.
			if (job->isStream) {
				break;
			} else {
				// JSON mode (existing): buffer into response.body with limit/truncation.
				size_t available = job->response.body.size() < job->bodyLimit
				                       ? (job->bodyLimit - job->response.body.size())
				                       : 0;
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
				job->response.headers.emplace_back(
				    event->header_key,
				    event->header_value,
				    job->response.charAllocator
				);
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

	if (_teardownRequested.load(std::memory_order_acquire)) {
		job->response.error = ESP_ERR_INVALID_STATE;
	} else {
		FetchRequestOptions requestTlsOptions;
		requestTlsOptions.caCertPem = job->requestOptions.caCertPem;
		requestTlsOptions.useTlsCertBundle = job->requestOptions.useTlsCertBundle;
		requestTlsOptions.useGlobalCaStore = job->requestOptions.useGlobalCaStore;
		requestTlsOptions.skipTlsServerCertValidation =
		    job->requestOptions.skipTlsServerCertValidation;
		requestTlsOptions.skipTlsCommonNameCheck = job->requestOptions.skipTlsCommonNameCheck;
		const esp_fetch_detail::ResolvedFetchTlsOptions tlsOptions =
		    esp_fetch_detail::resolveFetchTlsOptions(_config, requestTlsOptions);

		esp_http_client_config_t config = {};
		config.url = job->url.c_str();
		config.method = job->method;
		config.timeout_ms = job->requestOptions.timeoutMs ? job->requestOptions.timeoutMs
		                                                  : _config.defaultTimeoutMs;
		config.buffer_size = resolveHttpBufferSize(
		    "RX buffer size",
		    job->requestOptions.rxBufferSize,
		    _config.rxBufferSize
		);
		config.buffer_size_tx = resolveHttpBufferSize(
		    "TX buffer size",
		    job->requestOptions.txBufferSize,
		    _config.txBufferSize
		);
		config.event_handler = &ESPFetch::handleHttpEvent;
		config.user_data = job.get();
		config.disable_auto_redirect =
		    !(job->requestOptions.allowRedirects && _config.followRedirects);
		config.cert_pem = tlsOptions.caCertPem;
		config.use_global_ca_store = tlsOptions.useGlobalCaStore;
		config.skip_cert_common_name_check = tlsOptions.skipTlsCommonNameCheck;
#if ESP_FETCH_HAVE_CRT_BUNDLE
		if (tlsOptions.useTlsCertBundle) {
			config.crt_bundle_attach = esp_crt_bundle_attach;
		}
#endif

		esp_http_client_handle_t client = esp_http_client_init(&config);
		if (!client) {
			ESP_LOGE(TAG, "esp_http_client_init failed");
			job->response.error = ESP_ERR_NO_MEM;
		} else if (_teardownRequested.load(std::memory_order_acquire)) {
			job->response.error = ESP_ERR_INVALID_STATE;
			esp_http_client_cleanup(client);
		} else {
			auto hasHeader = [&](const char *key) {
				for (const auto &header : job->requestOptions.headers) {
					if (equalsIgnoreCase(header.name, key)) {
						return true;
					}
				}
				return false;
			};

			if (_config.userAgent && !hasHeader("User-Agent")) {
				esp_http_client_set_header(client, "User-Agent", _config.userAgent);
			}

			const char *contentType = job->requestOptions.contentType
			                              ? job->requestOptions.contentType
			                              : _config.defaultContentType;
			if (!job->isStream && job->method == HTTP_METHOD_POST && contentType &&
			    !hasHeader("Content-Type")) {
				esp_http_client_set_header(client, "Content-Type", contentType);
			}

			for (const auto &header : job->requestOptions.headers) {
				esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
			}

			if (!job->body.empty()) {
				esp_http_client_set_post_field(client, job->body.c_str(), job->body.length());
			}

			if (!job->isStream) {
				job->response.error = esp_http_client_perform(client);
				if (job->response.error == ESP_OK) {
					job->response.statusCode = esp_http_client_get_status_code(client);
				}
			} else {
				const int configuredReadBufferSize = resolveHttpBufferSize(
				    "Stream read buffer size",
				    job->requestOptions.rxBufferSize,
				    _config.rxBufferSize
				);
				const size_t readBufferSize = configuredReadBufferSize > 0
				                                  ? static_cast<size_t>(configuredReadBufferSize)
				                                  : STREAM_READ_BUFFER_SIZE_FALLBACK_BYTES;
				std::vector<char> readBuffer(readBufferSize);
				bool retryRequest = false;
				do {
					retryRequest = false;
					job->response.error = esp_http_client_open(client, 0);
					if (job->response.error != ESP_OK) {
						break;
					}

					job->response.statusCode = 0;
					job->response.headers.clear();
					job->response.headersTruncated = false;

					const int64_t fetchHeadersResult = esp_http_client_fetch_headers(client);
					if (fetchHeadersResult < 0) {
						job->response.error = fetchHeadersResult == -ESP_ERR_HTTP_EAGAIN
						                          ? ESP_ERR_HTTP_READ_TIMEOUT
						                          : ESP_ERR_HTTP_FETCH_HEADER;
						esp_http_client_close(client);
						break;
					}

					job->response.statusCode = esp_http_client_get_status_code(client);
					const StreamStartInfo startInfo{
					    .statusCode = job->response.statusCode,
					    .contentLength = esp_http_client_get_content_length(client),
					    .isChunked = esp_http_client_is_chunked_response(client),
					};

					if (!job->requestOptions.allowRedirects || !_config.followRedirects) {
						// Keep the response exactly as received.
					} else if (isRedirectHttpStatus(startInfo.statusCode)) {
						int discardedLength = 0;
						(void)esp_http_client_flush_response(client, &discardedLength);
						job->response.error = esp_http_client_set_redirection(client);
						esp_http_client_close(client);
						if (job->response.error == ESP_OK) {
							retryRequest = true;
						}
						continue;
					} else if (startInfo.statusCode == 401) {
						job->response.error = esp_http_client_add_auth(client);
						if (job->response.error != ESP_OK) {
							esp_http_client_close(client);
							break;
						}
						int discardedLength = 0;
						job->response.error = esp_http_client_flush_response(client, &discardedLength);
						esp_http_client_close(client);
						if (job->response.error == ESP_OK) {
							retryRequest = true;
						}
						continue;
					}

					if (job->onStart && !invokeFetchStartCallback(job->onStart, startInfo)) {
						job->streamStartRejected = true;
						job->response.error = ESP_OK;
						esp_http_client_close(client);
						break;
					}

					while (job->response.error == ESP_OK) {
						const int readResult = esp_http_client_read(
						    client,
						    readBuffer.data(),
						    static_cast<int>(readBuffer.size())
						);
						if (readResult < 0) {
							job->response.error = mapStreamReadFailure(client, readResult);
							break;
						}
						if (readResult == 0) {
							if (!esp_http_client_is_complete_data_received(client)) {
								job->response.error = mapStreamReadFailure(client, readResult);
							}
							break;
						}

						size_t toSend = static_cast<size_t>(readResult);
						if (job->bodyLimit != std::numeric_limits<size_t>::max()) {
							if (job->receivedBytes >= job->bodyLimit) {
								job->streamAbortError = ESP_ERR_INVALID_SIZE;
								job->response.error = job->streamAbortError;
								break;
							}
							const size_t remaining = job->bodyLimit - job->receivedBytes;
							toSend = std::min(toSend, remaining);
						}

						if (toSend > 0 && job->onChunk &&
						    !invokeFetchChunkCallback(job->onChunk, readBuffer.data(), toSend)) {
							job->streamAbortError = ESP_ERR_INVALID_STATE;
							job->response.error = job->streamAbortError;
							break;
						}

						job->receivedBytes += toSend;
						if (toSend < static_cast<size_t>(readResult)) {
							job->streamAbortError = ESP_ERR_INVALID_SIZE;
							job->response.error = job->streamAbortError;
							break;
						}
					}

					esp_http_client_close(client);
				} while (retryRequest && job->response.error == ESP_OK);
			}

			if (job->isStream && job->response.error != ESP_OK && job->streamAbortError != ESP_OK) {
				job->response.error = job->streamAbortError;
			}
			if (job->isStream && job->streamStartRejected) {
				job->response.error = ESP_OK;
			}
			esp_http_client_cleanup(client);
		}
	}

	job->response.durationUs = esp_timer_get_time() - start;

	if (job->isStream) {
		StreamResult r;
		r.error = job->response.error;
		r.statusCode = job->response.statusCode;
		r.receivedBytes = job->receivedBytes;
		if (job->onDone) {
			invokeFetchCallback(job->onDone, r);
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
		headersObj[header.name.c_str()] = header.value.c_str();
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
		invokeFetchCallback(job->callback, result);
	}
	if (job->syncHandle) {
		job->syncHandle->doc = result;
		job->syncHandle->ready = true;
		if (job->syncHandle->done) {
			xSemaphoreGive(job->syncHandle->done);
		}
	}
}
