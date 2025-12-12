# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Streaming download API via `getStream()` for binary or arbitrary content (no JSON handling).
- Chunk-based delivery using `FetchChunkCallback` to process large payloads incrementally (files, firmware, blobs).
- Completion callback with `StreamResult` containing transport error, HTTP status code, and total received byte count.
- Per-request streaming size limits via `FetchRequestOptions::maxBodyBytes` (default: unlimited for streams).
- Hooks for retries/backoff strategies on top of the existing async API.

### Notes
- Streaming requests bypass all body buffering and ArduinoJson processing.
- Exceeding `maxBodyBytes` aborts the transfer with `ESP_ERR_INVALID_SIZE`.

### Documentation
- Added usage examples and API reference for streaming downloads.
- Flesh out troubleshooting tips for TLS and large bodies.


## [1.0.0] - 2025-11-19

### Added
- Initial ESPFetch release with async + sync (`JsonDocument`) APIs for GET and POST.
- Configurable worker tasks, concurrency semaphore, timeout/redirect/TLS guards, and header/body truncation limits.
- Basic example sketch plus README explaining result schema and configuration options.
