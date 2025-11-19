# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Hooks for retries/backoff strategies on top of the existing async API.

### Documentation
- Flesh out troubleshooting tips for TLS and large bodies.

## [1.0.0] - 2025-09-16

### Added
- Initial ESPFetch release with async + sync (`JsonDocument`) APIs for GET and POST.
- Configurable worker tasks, concurrency semaphore, timeout/redirect/TLS guards, and header/body truncation limits.
- Basic example sketch plus README explaining result schema and configuration options.
