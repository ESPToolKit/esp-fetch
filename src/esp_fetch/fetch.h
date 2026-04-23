#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fetch_allocator.h"

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

enum class FetchTlsVersion {
	Any,
	Tls12,
	Tls13,
};

enum class FetchTlsDynBufferStrategy {
	Default,
	RxStaticAfterHandshake,
};

struct FetchRequestOptions {
	uint32_t timeoutMs = 0;
	size_t maxBodyBytes = 0;
	size_t maxHeaderBytes = 0;
	size_t rxBufferSize = 0;
	size_t txBufferSize = 0;
	const char *caCertPem = nullptr;
	std::optional<FetchTlsVersion> tlsVersion;
	std::optional<FetchTlsDynBufferStrategy> tlsDynBufferStrategy;
	std::optional<bool> usePSRAMBuffers;
	std::optional<bool> useTlsCertBundle;
	std::optional<bool> useGlobalCaStore;
	std::optional<bool> skipTlsServerCertValidation;
	std::optional<bool> skipTlsCommonNameCheck;
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
	size_t rxBufferSize = 0;
	size_t txBufferSize = 0;
	TickType_t slotAcquireTicks = pdMS_TO_TICKS(0);
	const char *caCertPem = nullptr;
	FetchTlsVersion tlsVersion = FetchTlsVersion::Any;
	FetchTlsDynBufferStrategy tlsDynBufferStrategy = FetchTlsDynBufferStrategy::Default;
	bool useTlsCertBundle = true;
	bool useGlobalCaStore = false;
	bool skipTlsServerCertValidation = false;
	bool skipTlsCommonNameCheck = false;
	bool followRedirects = true;
	bool usePSRAMBuffers = false;
	const char *userAgent = "ESPFetch/1.0";
	const char *defaultContentType = "application/json";
};

#if __has_include(<esp_crt_bundle.h>)
#define ESP_FETCH_HAVE_CRT_BUNDLE 1
#else
#define ESP_FETCH_HAVE_CRT_BUNDLE 0
#endif

namespace esp_fetch_detail {
struct ResolvedFetchTlsOptions {
	const char *caCertPem = nullptr;
	bool useTlsCertBundle = false;
	bool useGlobalCaStore = false;
	bool skipTlsServerCertValidation = false;
	bool skipTlsCommonNameCheck = false;
	bool bundleRequested = false;
	bool globalCaStoreRequested = false;
	bool skipServerCertValidationRequested = false;
};

struct ResolvedFetchTransportOptions {
	ResolvedFetchTlsOptions tls;
	FetchTlsVersion tlsVersion = FetchTlsVersion::Any;
	FetchTlsDynBufferStrategy tlsDynBufferStrategy = FetchTlsDynBufferStrategy::Default;
	int rxBufferSize = 0;
	int txBufferSize = 0;
	bool usePSRAMBuffers = false;
};

inline bool hasFetchCaCertPem(const char *certPem) {
	return certPem != nullptr && certPem[0] != '\0';
}

inline bool resolveFetchTlsBool(const std::optional<bool> &requestValue, bool configValue) {
	return requestValue.has_value() ? *requestValue : configValue;
}

template <typename T>
inline T resolveFetchOption(const std::optional<T> &requestValue, T configValue) {
	return requestValue.has_value() ? *requestValue : configValue;
}

inline bool fetchCanSkipServerCertVerification() {
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
	return true;
#else
	return false;
#endif
}

inline bool fetchHasTlsDynamicBufferSupport() {
#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
	return true;
#else
	return false;
#endif
}

inline int resolveFetchHttpBufferSize(size_t requestValue, size_t configValue) {
	const size_t selected = requestValue ? requestValue : configValue;
	if (selected == 0) {
		return 0;
	}
	if (selected > static_cast<size_t>(INT_MAX)) {
		return INT_MAX;
	}
	return static_cast<int>(selected);
}

inline ResolvedFetchTlsOptions
resolveFetchTlsOptions(const FetchConfig &config, const FetchRequestOptions &requestOptions) {
	ResolvedFetchTlsOptions resolved;
	resolved.skipTlsCommonNameCheck =
	    resolveFetchTlsBool(requestOptions.skipTlsCommonNameCheck, config.skipTlsCommonNameCheck);

	const char *requestCaCertPem =
	    hasFetchCaCertPem(requestOptions.caCertPem) ? requestOptions.caCertPem : nullptr;
	const char *configCaCertPem = hasFetchCaCertPem(config.caCertPem) ? config.caCertPem : nullptr;
	resolved.caCertPem = requestCaCertPem != nullptr ? requestCaCertPem : configCaCertPem;

	resolved.bundleRequested =
	    resolveFetchTlsBool(requestOptions.useTlsCertBundle, config.useTlsCertBundle);
	resolved.globalCaStoreRequested =
	    resolveFetchTlsBool(requestOptions.useGlobalCaStore, config.useGlobalCaStore);
	resolved.skipServerCertValidationRequested = resolveFetchTlsBool(
	    requestOptions.skipTlsServerCertValidation,
	    config.skipTlsServerCertValidation
	);

	if (resolved.caCertPem != nullptr) {
		return resolved;
	}

	if (resolved.globalCaStoreRequested) {
		resolved.useGlobalCaStore = true;
		return resolved;
	}

#if ESP_FETCH_HAVE_CRT_BUNDLE
	if (resolved.bundleRequested) {
		resolved.useTlsCertBundle = true;
		return resolved;
	}
#endif

#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
	if (resolved.skipServerCertValidationRequested) {
		resolved.skipTlsServerCertValidation = true;
	}
#endif

	return resolved;
}

inline ResolvedFetchTransportOptions
resolveFetchTransportOptions(const FetchConfig &config, const FetchRequestOptions &requestOptions) {
	ResolvedFetchTransportOptions resolved;
	resolved.tls = resolveFetchTlsOptions(config, requestOptions);
	resolved.tlsVersion = resolveFetchOption(requestOptions.tlsVersion, config.tlsVersion);
	resolved.tlsDynBufferStrategy =
	    resolveFetchOption(requestOptions.tlsDynBufferStrategy, config.tlsDynBufferStrategy);
	resolved.rxBufferSize = resolveFetchHttpBufferSize(requestOptions.rxBufferSize, config.rxBufferSize);
	resolved.txBufferSize = resolveFetchHttpBufferSize(requestOptions.txBufferSize, config.txBufferSize);
	resolved.usePSRAMBuffers =
	    resolveFetchTlsBool(requestOptions.usePSRAMBuffers, config.usePSRAMBuffers);
	return resolved;
}

inline bool fetchUrlUsesHttps(const std::string &url) {
	return url.size() >= 8 && (url[0] == 'h' || url[0] == 'H') && (url[1] == 't' || url[1] == 'T') &&
	       (url[2] == 't' || url[2] == 'T') && (url[3] == 'p' || url[3] == 'P') &&
	       (url[4] == 's' || url[4] == 'S') && url[5] == ':' && url[6] == '/' && url[7] == '/';
}

inline const char *validateFetchTlsOptions(const std::string &url, const ResolvedFetchTlsOptions &resolved) {
	if (!fetchUrlUsesHttps(url)) {
		return nullptr;
	}

	if (resolved.caCertPem != nullptr || resolved.useGlobalCaStore || resolved.useTlsCertBundle ||
	    resolved.skipTlsServerCertValidation) {
		return nullptr;
	}

	if (resolved.bundleRequested && !ESP_FETCH_HAVE_CRT_BUNDLE) {
		return "useTlsCertBundle requested but esp_crt_bundle_attach is unavailable in this build";
	}

	if (resolved.skipServerCertValidationRequested && !fetchCanSkipServerCertVerification()) {
		return "skipTlsServerCertValidation requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY";
	}

	return "https requests require caCertPem, useTlsCertBundle, useGlobalCaStore, or skipTlsServerCertValidation";
}

inline const char *validateFetchTlsOptions(
    const std::string &url, const FetchConfig &config, const FetchRequestOptions &requestOptions
) {
	const ResolvedFetchTlsOptions resolved = resolveFetchTlsOptions(config, requestOptions);
	return validateFetchTlsOptions(url, resolved);
}

inline const char *
validateFetchTransportOptions(const std::string &url, const ResolvedFetchTransportOptions &resolved) {
	const char *tlsError = validateFetchTlsOptions(url, resolved.tls);
	if (tlsError != nullptr) {
		return tlsError;
	}

	if (fetchUrlUsesHttps(url) &&
	    resolved.tlsDynBufferStrategy == FetchTlsDynBufferStrategy::RxStaticAfterHandshake &&
	    !fetchHasTlsDynamicBufferSupport()) {
		return "tlsDynBufferStrategy=RxStaticAfterHandshake requires CONFIG_MBEDTLS_DYNAMIC_BUFFER";
	}

	return nullptr;
}

inline const char *validateFetchTransportOptions(
    const std::string &url, const FetchConfig &config, const FetchRequestOptions &requestOptions
) {
	const ResolvedFetchTransportOptions resolved =
	    resolveFetchTransportOptions(config, requestOptions);
	return validateFetchTransportOptions(url, resolved);
}
} // namespace esp_fetch_detail

using FetchCallback = std::function<void(JsonDocument result)>;

// ------------------------------
// Streaming (binary/any-content)
// ------------------------------
struct StreamStartInfo {
	int statusCode = 0;
	int64_t contentLength = -1;
	bool isChunked = false;
};

struct StreamResult {
	esp_err_t error = ESP_OK;
	int statusCode = 0;
	size_t receivedBytes = 0;
};

using FetchStreamStartCallback = std::function<bool(const StreamStartInfo &info)>;
using FetchChunkCallback = std::function<bool(const void *data, size_t size)>;
using FetchStreamCallback = std::function<void(StreamResult result)>;

class ESPFetch {
  public:
	ESPFetch() = default;
	~ESPFetch();

	bool init(const FetchConfig &config = FetchConfig{});
	void deinit();
	bool isInitialized() const;

	bool
	get(const char *url,
	    FetchCallback callback,
	    const FetchRequestOptions &options = FetchRequestOptions{});
	bool
	get(const String &url,
	    FetchCallback callback,
	    const FetchRequestOptions &options = FetchRequestOptions{});
	JsonDocument
	get(const char *url,
	    TickType_t waitTicks,
	    const FetchRequestOptions &options = FetchRequestOptions{});
	JsonDocument
	get(const String &url,
	    TickType_t waitTicks,
	    const FetchRequestOptions &options = FetchRequestOptions{});

	bool post(
	    const char *url,
	    const JsonDocument &payload,
	    FetchCallback callback,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	bool post(
	    const String &url,
	    const JsonDocument &payload,
	    FetchCallback callback,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	JsonDocument post(
	    const char *url,
	    const JsonDocument &payload,
	    TickType_t waitTicks,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	JsonDocument post(
	    const String &url,
	    const JsonDocument &payload,
	    TickType_t waitTicks,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);

	// Stream download (binary / any kind). No JSON handling.
	bool getStream(
	    const char *url,
	    FetchChunkCallback onChunk,
	    FetchStreamCallback onDone = nullptr,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	bool getStream(
	    const char *url,
	    FetchStreamStartCallback onStart,
	    FetchChunkCallback onChunk,
	    FetchStreamCallback onDone = nullptr,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	bool getStream(
	    const String &url,
	    FetchChunkCallback onChunk,
	    FetchStreamCallback onDone = nullptr,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);
	bool getStream(
	    const String &url,
	    FetchStreamStartCallback onStart,
	    FetchChunkCallback onChunk,
	    FetchStreamCallback onDone = nullptr,
	    const FetchRequestOptions &options = FetchRequestOptions{}
	);

  private:
	struct FetchJob;
	struct FetchResponse;
	struct SyncHandle;

	bool enqueueRequest(
	    const std::string &url,
	    esp_http_client_method_t method,
	    FetchString &&body,
	    FetchCallback callback,
	    std::shared_ptr<SyncHandle> syncHandle,
	    const FetchRequestOptions &options,
	    const char **startErrorOut = nullptr
	);

	bool enqueueStreamRequest(
	    const std::string &url,
	    FetchStreamStartCallback onStart,
	    FetchChunkCallback onChunk,
	    FetchStreamCallback onDone,
	    const FetchRequestOptions &options,
	    const char **startErrorOut = nullptr
	);

	JsonDocument
	waitForResult(const std::shared_ptr<SyncHandle> &handle, TickType_t waitTicks) const;

	static void requestTask(void *arg);
	static esp_err_t handleHttpEvent(esp_http_client_event_t *event);

	void runJob(std::unique_ptr<FetchJob> job);
	JsonDocument buildResult(const FetchJob &job, const FetchResponse &response) const;
	static void deliverResult(const std::unique_ptr<FetchJob> &job, const JsonDocument &result);

	FetchConfig _config{};
	std::atomic<bool> _initialized{false};
	std::atomic<bool> _teardownRequested{false};
	std::atomic<size_t> _activeTasks{0};
	SemaphoreHandle_t _slotSemaphore = nullptr;
};
