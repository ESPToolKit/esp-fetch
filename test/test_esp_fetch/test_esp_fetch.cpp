#include <Arduino.h>
#include <ESPFetch.h>
#include <unity.h>

static void test_init_rejects_zero_concurrency() {
	ESPFetch fetch;
	FetchConfig cfg{};
	cfg.maxConcurrentRequests = 0;
	TEST_ASSERT_FALSE(fetch.init(cfg));
	TEST_ASSERT_FALSE(fetch.isInitialized());
}

static void test_init_and_deinit_cycle_updates_initialized_flag() {
	ESPFetch fetch;
	TEST_ASSERT_TRUE(fetch.init());
	TEST_ASSERT_TRUE(fetch.isInitialized());
	fetch.deinit();
	TEST_ASSERT_FALSE(fetch.isInitialized());
}

static void test_init_accepts_psram_buffer_toggle() {
	ESPFetch fetch;
	FetchConfig cfg{};
	cfg.usePSRAMBuffers = true;
	TEST_ASSERT_TRUE(fetch.init(cfg));
	TEST_ASSERT_TRUE(fetch.isInitialized());
	fetch.deinit();
}

static void test_buffer_size_options_default_to_idf_defaults() {
	FetchConfig cfg{};
	FetchRequestOptions opts{};

	TEST_ASSERT_EQUAL_UINT32(0, cfg.rxBufferSize);
	TEST_ASSERT_EQUAL_UINT32(0, cfg.txBufferSize);
	TEST_ASSERT_TRUE(cfg.useTlsCertBundle);
	TEST_ASSERT_FALSE(cfg.useGlobalCaStore);
	TEST_ASSERT_FALSE(cfg.skipTlsServerCertValidation);
	TEST_ASSERT_EQUAL_UINT32(0, opts.rxBufferSize);
	TEST_ASSERT_EQUAL_UINT32(0, opts.txBufferSize);
	TEST_ASSERT_FALSE(opts.useTlsCertBundle.has_value());
	TEST_ASSERT_FALSE(opts.useGlobalCaStore.has_value());
	TEST_ASSERT_FALSE(opts.skipTlsServerCertValidation.has_value());
	TEST_ASSERT_FALSE(opts.skipTlsCommonNameCheck.has_value());
}

static void test_buffer_size_options_are_assignable() {
	FetchConfig cfg{};
	FetchRequestOptions opts{};

	cfg.rxBufferSize = 8192;
	cfg.txBufferSize = 2048;
	cfg.caCertPem = "CONFIG_CA";
	cfg.useTlsCertBundle = false;
	cfg.useGlobalCaStore = true;
	cfg.skipTlsServerCertValidation = true;
	opts.rxBufferSize = 4096;
	opts.txBufferSize = 1024;
	opts.caCertPem = "REQUEST_CA";
	opts.useTlsCertBundle = false;
	opts.useGlobalCaStore = true;
	opts.skipTlsServerCertValidation = true;
	opts.skipTlsCommonNameCheck = true;

	TEST_ASSERT_EQUAL_UINT32(8192, cfg.rxBufferSize);
	TEST_ASSERT_EQUAL_UINT32(2048, cfg.txBufferSize);
	TEST_ASSERT_EQUAL_STRING("CONFIG_CA", cfg.caCertPem);
	TEST_ASSERT_FALSE(cfg.useTlsCertBundle);
	TEST_ASSERT_TRUE(cfg.useGlobalCaStore);
	TEST_ASSERT_TRUE(cfg.skipTlsServerCertValidation);
	TEST_ASSERT_EQUAL_UINT32(4096, opts.rxBufferSize);
	TEST_ASSERT_EQUAL_UINT32(1024, opts.txBufferSize);
	TEST_ASSERT_EQUAL_STRING("REQUEST_CA", opts.caCertPem);
	TEST_ASSERT_TRUE(opts.useTlsCertBundle.has_value());
	TEST_ASSERT_FALSE(*opts.useTlsCertBundle);
	TEST_ASSERT_TRUE(opts.useGlobalCaStore.has_value());
	TEST_ASSERT_TRUE(*opts.useGlobalCaStore);
	TEST_ASSERT_TRUE(opts.skipTlsServerCertValidation.has_value());
	TEST_ASSERT_TRUE(*opts.skipTlsServerCertValidation);
	TEST_ASSERT_TRUE(opts.skipTlsCommonNameCheck.has_value());
	TEST_ASSERT_TRUE(*opts.skipTlsCommonNameCheck);
}

static void test_stream_start_info_defaults_are_safe() {
	StreamStartInfo info{};

	TEST_ASSERT_EQUAL(0, info.statusCode);
	TEST_ASSERT_EQUAL_INT64(-1, info.contentLength);
	TEST_ASSERT_FALSE(info.isChunked);
}

static void test_get_stream_with_start_requires_initialization() {
	ESPFetch fetch;
	volatile bool startInvoked = false;
	volatile bool chunkInvoked = false;
	auto onStart = [&](const StreamStartInfo &) {
		startInvoked = true;
		return true;
	};
	auto onChunk = [&](const void *, size_t) {
		chunkInvoked = true;
		return true;
	};

	bool started = fetch.getStream("https://example.com/data.bin", onStart, onChunk);

	TEST_ASSERT_FALSE(started);
	TEST_ASSERT_FALSE(startInvoked);
	TEST_ASSERT_FALSE(chunkInvoked);
}

static void test_get_stream_with_start_requires_chunk_callback() {
	ESPFetch fetch;
	auto onStart = [](const StreamStartInfo &) { return true; };

	TEST_ASSERT_FALSE(fetch.getStream("https://example.com/data.bin", onStart, nullptr));
}

static void test_default_https_tls_resolution_uses_cert_bundle() {
	FetchConfig cfg{};
	FetchRequestOptions opts{};

	const esp_fetch_detail::ResolvedFetchTlsOptions resolved =
	    esp_fetch_detail::resolveFetchTlsOptions(cfg, opts);

	TEST_ASSERT_NULL(resolved.caCertPem);
	TEST_ASSERT_TRUE(resolved.bundleRequested);
#if ESP_FETCH_HAVE_CRT_BUNDLE
	TEST_ASSERT_TRUE(resolved.useTlsCertBundle);
#else
	TEST_ASSERT_FALSE(resolved.useTlsCertBundle);
#endif
	TEST_ASSERT_FALSE(resolved.useGlobalCaStore);
	TEST_ASSERT_FALSE(resolved.skipTlsServerCertValidation);
}

static void test_request_ca_cert_overrides_bundle_and_global_store() {
	FetchConfig cfg{};
	cfg.caCertPem = "CONFIG_CA";
	cfg.useTlsCertBundle = true;
	cfg.useGlobalCaStore = true;
	FetchRequestOptions opts{};
	opts.caCertPem = "REQUEST_CA";
	opts.useTlsCertBundle = false;
	opts.useGlobalCaStore = false;

	const esp_fetch_detail::ResolvedFetchTlsOptions resolved =
	    esp_fetch_detail::resolveFetchTlsOptions(cfg, opts);

	TEST_ASSERT_EQUAL_STRING("REQUEST_CA", resolved.caCertPem);
	TEST_ASSERT_FALSE(resolved.useTlsCertBundle);
	TEST_ASSERT_FALSE(resolved.useGlobalCaStore);
	TEST_ASSERT_FALSE(resolved.skipTlsServerCertValidation);
}

static void test_request_global_store_override_disables_default_bundle() {
	FetchConfig cfg{};
	cfg.useTlsCertBundle = true;
	cfg.useGlobalCaStore = false;
	FetchRequestOptions opts{};
	opts.useTlsCertBundle = false;
	opts.useGlobalCaStore = true;

	const esp_fetch_detail::ResolvedFetchTlsOptions resolved =
	    esp_fetch_detail::resolveFetchTlsOptions(cfg, opts);

	TEST_ASSERT_FALSE(resolved.useTlsCertBundle);
	TEST_ASSERT_TRUE(resolved.useGlobalCaStore);
}

static void test_skip_server_cert_verification_is_only_used_when_requested() {
	FetchConfig cfg{};
	cfg.useTlsCertBundle = false;
	FetchRequestOptions opts{};
	opts.useTlsCertBundle = false;
	opts.skipTlsServerCertValidation = true;

	const esp_fetch_detail::ResolvedFetchTlsOptions resolved =
	    esp_fetch_detail::resolveFetchTlsOptions(cfg, opts);

#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
	TEST_ASSERT_TRUE(resolved.skipTlsServerCertValidation);
#else
	TEST_ASSERT_FALSE(resolved.skipTlsServerCertValidation);
#endif
}

static void test_https_validation_rejects_missing_trust_source() {
	FetchConfig cfg{};
	cfg.useTlsCertBundle = false;
	cfg.useGlobalCaStore = false;
	cfg.skipTlsServerCertValidation = false;
	FetchRequestOptions opts{};
	opts.useTlsCertBundle = false;
	opts.useGlobalCaStore = false;
	opts.skipTlsServerCertValidation = false;

	const char *error =
	    esp_fetch_detail::validateFetchTlsOptions("https://example.com", cfg, opts);

	TEST_ASSERT_EQUAL_STRING(
	    "https requests require caCertPem, useTlsCertBundle, useGlobalCaStore, or skipTlsServerCertValidation",
	    error
	);
}

static void test_sync_get_reports_tls_preflight_error_before_network_io() {
	ESPFetch fetch;
	FetchConfig cfg{};
	cfg.useTlsCertBundle = false;
	cfg.useGlobalCaStore = false;
	cfg.skipTlsServerCertValidation = false;
	TEST_ASSERT_TRUE(fetch.init(cfg));

	FetchRequestOptions opts{};
	opts.useTlsCertBundle = false;
	opts.useGlobalCaStore = false;
	opts.skipTlsServerCertValidation = false;

	JsonDocument doc = fetch.get("https://example.com", pdMS_TO_TICKS(1), opts);
	auto msg = doc["error"]["message"] | "";
	TEST_ASSERT_EQUAL_STRING(
	    "https requests require caCertPem, useTlsCertBundle, useGlobalCaStore, or skipTlsServerCertValidation",
	    msg
	);
	TEST_ASSERT_FALSE(doc["ok"] | true);
	fetch.deinit();
}

static void test_deinit_is_safe_before_init() {
	ESPFetch fetch;
	fetch.deinit();
	TEST_ASSERT_FALSE(fetch.isInitialized());
}

static void test_deinit_is_idempotent() {
	ESPFetch fetch;
	TEST_ASSERT_TRUE(fetch.init());
	TEST_ASSERT_TRUE(fetch.isInitialized());
	fetch.deinit();
	fetch.deinit();
	TEST_ASSERT_FALSE(fetch.isInitialized());
}

static void test_reinit_after_deinit_is_supported() {
	ESPFetch fetch;
	TEST_ASSERT_TRUE(fetch.init());
	fetch.deinit();
	TEST_ASSERT_TRUE(fetch.init());
	TEST_ASSERT_TRUE(fetch.isInitialized());
	fetch.deinit();
	TEST_ASSERT_FALSE(fetch.isInitialized());
}

static void test_async_get_requires_initialization() {
	ESPFetch fetch;
	volatile bool invoked = false;
	auto cb = [&](JsonDocument) { invoked = true; };
	bool started = fetch.get("https://example.com", cb);
	TEST_ASSERT_FALSE(started);
	TEST_ASSERT_FALSE(invoked);
}

static void test_sync_get_reports_error_when_not_initialized() {
	ESPFetch fetch;
	JsonDocument doc = fetch.get("https://example.com", pdMS_TO_TICKS(1));
	auto msg = doc["error"]["message"] | "";
	TEST_ASSERT_EQUAL_STRING("failed to start http get", msg);
	TEST_ASSERT_FALSE(doc["ok"] | true);
}

static void test_sync_get_requires_url() {
	ESPFetch fetch;
	JsonDocument doc = fetch.get(nullptr, pdMS_TO_TICKS(1));
	auto msg = doc["error"]["message"] | "";
	TEST_ASSERT_EQUAL_STRING("url is null", msg);
}

static void test_sync_post_requires_url() {
	ESPFetch fetch;
	JsonDocument payload;
	payload["hello"] = "world";
	JsonDocument doc = fetch.post(nullptr, payload, pdMS_TO_TICKS(1));
	auto msg = doc["error"]["message"] | "";
	TEST_ASSERT_EQUAL_STRING("url is null", msg);
}

static void test_sync_post_reports_error_when_not_initialized() {
	ESPFetch fetch;
	JsonDocument payload;
	payload["value"] = 42;
	JsonDocument doc = fetch.post("https://example.com", payload, pdMS_TO_TICKS(1));
	auto msg = doc["error"]["message"] | "";
	TEST_ASSERT_EQUAL_STRING("failed to start http post", msg);
	TEST_ASSERT_FALSE(doc["ok"] | true);
}

void setUp() {
}

void tearDown() {
}

void setup() {
	delay(2000);
	UNITY_BEGIN();
	RUN_TEST(test_init_rejects_zero_concurrency);
	RUN_TEST(test_init_and_deinit_cycle_updates_initialized_flag);
	RUN_TEST(test_init_accepts_psram_buffer_toggle);
	RUN_TEST(test_buffer_size_options_default_to_idf_defaults);
	RUN_TEST(test_buffer_size_options_are_assignable);
	RUN_TEST(test_stream_start_info_defaults_are_safe);
	RUN_TEST(test_default_https_tls_resolution_uses_cert_bundle);
	RUN_TEST(test_request_ca_cert_overrides_bundle_and_global_store);
	RUN_TEST(test_request_global_store_override_disables_default_bundle);
	RUN_TEST(test_skip_server_cert_verification_is_only_used_when_requested);
	RUN_TEST(test_https_validation_rejects_missing_trust_source);
	RUN_TEST(test_deinit_is_safe_before_init);
	RUN_TEST(test_deinit_is_idempotent);
	RUN_TEST(test_reinit_after_deinit_is_supported);
	RUN_TEST(test_async_get_requires_initialization);
	RUN_TEST(test_get_stream_with_start_requires_initialization);
	RUN_TEST(test_get_stream_with_start_requires_chunk_callback);
	RUN_TEST(test_sync_get_reports_error_when_not_initialized);
	RUN_TEST(test_sync_get_reports_tls_preflight_error_before_network_io);
	RUN_TEST(test_sync_get_requires_url);
	RUN_TEST(test_sync_post_requires_url);
	RUN_TEST(test_sync_post_reports_error_when_not_initialized);
	UNITY_END();
}

void loop() {
	vTaskDelay(pdMS_TO_TICKS(1000));
}
