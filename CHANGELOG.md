# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Streaming download API via `getStream()` for binary or arbitrary content (no JSON handling).
- Chunk-based delivery using `FetchChunkCallback` to process large payloads incrementally (files, firmware, blobs).
- Completion callback with `StreamResult` containing transport error, HTTP status code, and total received byte count.
- Per-request streaming size limits via `FetchRequestOptions::maxBodyBytes` (default: unlimited for streams).
- Added global (`FetchConfig`) and per-request (`FetchRequestOptions`) TX/RX HTTP client buffer sizing controls.
- Added explicit HTTPS trust-source configuration to `FetchConfig` and per-request TLS overrides to `FetchRequestOptions` (`caCertPem`, `useTlsCertBundle`, `useGlobalCaStore`, `skipTlsServerCertValidation`, `skipTlsCommonNameCheck`).
- Hooks for retries/backoff strategies on top of the existing async API.
- Added `FetchConfig::usePSRAMBuffers` and routed JSON-mode response body/header storage plus request-body/copied-request-header storage through `ESPBufferManager` with automatic fallback to normal heap paths.
- Switched request task creation/lifecycle to native FreeRTOS `xTaskCreatePinnedToCore(...)` handling.
- Added explicit teardown-contract lifecycle coverage (`deinit()` pre-init, repeated `deinit()`, and `init -> deinit -> init`).
- Added `isInitialized()` as the public runtime-state contract accessor.

### Fixed
- Normalize malformed `http:/` or `https:/` URLs to `http://`/`https://` to avoid DNS failures with parsed hosts like `:example.com`.
- Collapse extra slashes (`https:///`) and strip a leading `://:` host typo before handing URLs to esp_http_client.
- HTTPS requests now default to ESP certificate-bundle verification, matching the expected mixed Arduino + ESP-IDF transport behavior for public endpoints.
- HTTPS requests now reject missing/unsupported trust-source configurations before `esp_http_client` starts, replacing opaque `esp-tls` setup failures with deterministic errors.
- Teardown now requests active workers to abort in-flight operations and waits for worker completion before releasing shared runtime resources.
- CI now pins PIOArduino Core to `v6.1.19` and installs the ESP32 platform via `pio pkg install`, restoring PlatformIO compatibility with the current `platform-espressif32` package.

### Notes
- Streaming requests bypass all body buffering and ArduinoJson processing.
- Exceeding `maxBodyBytes` aborts the transfer with `ESP_ERR_INVALID_SIZE`.
- ArduinoJson `JsonDocument` allocations remain managed by ArduinoJson allocators; `usePSRAMBuffers` applies to ESPFetch-owned buffers.

### Documentation
- Added usage examples and API reference for streaming downloads.
- Clarified that stream chunk callbacks return `bool` to continue or abort.
- Documented that DNS must be configured before starting requests; `getaddrinfo()` failures indicate missing DNS.
- Documented bundle-backed HTTPS defaults, trust-source precedence, and the `skipTlsServerCertValidation` diagnostic-only contract.
- Flesh out troubleshooting tips for TLS and large bodies.


## [1.0.0] - 2025-11-19

### Added
- Initial ESPFetch release with async + sync (`JsonDocument`) APIs for GET and POST.
- Configurable worker tasks, concurrency semaphore, timeout/redirect/TLS guards, and header/body truncation limits.
- Basic example sketch plus README explaining result schema and configuration options.
