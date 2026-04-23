# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Streaming download API via `getStream()` for binary or arbitrary content (no JSON handling).
- Chunk-based delivery using `FetchChunkCallback` to process large payloads incrementally (files, firmware, blobs).
- Completion callback with `StreamResult` containing transport error, HTTP status code, and total received byte count.
- Status-gated streaming overloads with `FetchStreamStartCallback` and
  `StreamStartInfo` so callers can reject non-`2xx` responses before any body
  chunk is processed.
- Per-request streaming size limits via `FetchRequestOptions::maxBodyBytes` (default: unlimited for streams).
- Added global (`FetchConfig`) and per-request (`FetchRequestOptions`) TX/RX HTTP client buffer sizing controls.
- Added explicit HTTPS trust-source configuration to `FetchConfig` and per-request TLS overrides to `FetchRequestOptions` (`caCertPem`, `useTlsCertBundle`, `useGlobalCaStore`, `skipTlsServerCertValidation`, `skipTlsCommonNameCheck`).
- Added explicit TLS transport controls for TLS version selection and TLS dynamic-buffer strategy on both `FetchConfig` and `FetchRequestOptions`.
- Hooks for retries/backoff strategies on top of the existing async API.
- Added `FetchConfig::usePSRAMBuffers` and per-request `FetchRequestOptions::usePSRAMBuffers`, routing JSON-mode response body/header storage, request-body/copied-request-header storage, and the streaming read buffer through the resolved ESPFetch buffer-placement policy.
- Switched request task creation/lifecycle to native FreeRTOS `xTaskCreatePinnedToCore(...)` handling.
- Added explicit teardown-contract lifecycle coverage (`deinit()` pre-init, repeated `deinit()`, and `init -> deinit -> init`).
- Added `isInitialized()` as the public runtime-state contract accessor.

### Fixed
- Stream requests can now resolve HTTP status, content length, and chunked mode
  before chunk delivery when callers use the new status-aware overload, which
  prevents OTA/file consumers from processing HTML or error bodies as payload.
- Normalize malformed `http:/` or `https:/` URLs to `http://`/`https://` to avoid DNS failures with parsed hosts like `:example.com`.
- Collapse extra slashes (`https:///`) and strip a leading `://:` host typo before handing URLs to esp_http_client.
- HTTPS requests now default to ESP certificate-bundle verification, matching the expected mixed Arduino + ESP-IDF transport behavior for public endpoints.
- HTTPS requests now reject missing/unsupported trust-source configurations before `esp_http_client` starts, replacing opaque `esp-tls` setup failures with deterministic errors.
- Requests now resolve transport policy once at startup, and unsupported `RxStaticAfterHandshake` selections reject before network I/O when `CONFIG_MBEDTLS_DYNAMIC_BUFFER` is unavailable.
- Streaming requests now log the resolved TLS version, TLS dynamic-buffer strategy, RX/TX sizes, and fetch-owned buffer placement once before body reads begin.
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
- Documented TLS version / TLS dynamic-buffer controls, per-request buffer-placement overrides, and the `CONFIG_MBEDTLS_DYNAMIC_BUFFER` requirement for `RxStaticAfterHandshake`.
- Flesh out troubleshooting tips for TLS and large bodies.


## [1.0.0] - 2025-11-19

### Added
- Initial ESPFetch release with async + sync (`JsonDocument`) APIs for GET and POST.
- Configurable worker tasks, concurrency semaphore, timeout/redirect/TLS guards, and header/body truncation limits.
- Basic example sketch plus README explaining result schema and configuration options.
