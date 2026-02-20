# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Streaming download API via `getStream()` for binary or arbitrary content (no JSON handling).
- Chunk-based delivery using `FetchChunkCallback` to process large payloads incrementally (files, firmware, blobs).
- Completion callback with `StreamResult` containing transport error, HTTP status code, and total received byte count.
- Per-request streaming size limits via `FetchRequestOptions::maxBodyBytes` (default: unlimited for streams).
- Hooks for retries/backoff strategies on top of the existing async API.
- Added `FetchConfig::usePSRAMBuffers` and routed JSON-mode response body/header storage plus request-body/copied-request-header storage through `ESPBufferManager` with automatic fallback to normal heap paths.
- Added `ESPWorker` runtime integration for request task creation/lifecycle.

### Fixed
- Normalize malformed `http:/` or `https:/` URLs to `http://`/`https://` to avoid DNS failures with parsed hosts like `:example.com`.
- Collapse extra slashes (`https:///`) and strip a leading `://:` host typo before handing URLs to esp_http_client.

### Notes
- Streaming requests bypass all body buffering and ArduinoJson processing.
- Exceeding `maxBodyBytes` aborts the transfer with `ESP_ERR_INVALID_SIZE`.
- ArduinoJson `JsonDocument` allocations remain managed by ArduinoJson allocators; `usePSRAMBuffers` applies to ESPFetch-owned buffers.

### Documentation
- Added usage examples and API reference for streaming downloads.
- Clarified that stream chunk callbacks return `bool` to continue or abort.
- Documented that DNS must be configured before starting requests; `getaddrinfo()` failures indicate missing DNS.
- Flesh out troubleshooting tips for TLS and large bodies.


## [1.0.0] - 2025-11-19

### Added
- Initial ESPFetch release with async + sync (`JsonDocument`) APIs for GET and POST.
- Configurable worker tasks, concurrency semaphore, timeout/redirect/TLS guards, and header/body truncation limits.
- Basic example sketch plus README explaining result schema and configuration options.
