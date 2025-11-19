# ESPFetch

ESPFetch is a lightweight HTTP helper that keeps ESP32 firmware asynchronous-first. Each request runs on its own FreeRTOS task, streams headers/body into heap-backed `JsonDocument`s (ArduinoJson v7), and hands the result to either a callback or the caller waiting synchronously.

## CI / Release / License
[![CI](https://github.com/ESPToolKit/esp-fetch/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-fetch/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-fetch?sort=semver)](https://github.com/ESPToolKit/esp-fetch/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features
- Async-first API: `fetch.get`/`fetch.post` return immediately and invoke a callback with a `JsonDocument` that contains headers, body, status, and timing.
- Optional synchronous helpers spawn the same worker task but block the caller with `portMAX_DELAY` (or any timeout) until the response arrives.
- Configurable concurrency (counting semaphore), worker stack/priority/core, and sane per-request limits for body/header capture.
- Built on ESP-IDF's `esp_http_client`, so TLS, redirects, authentication headers, and streaming all ride on battle-tested primitives.
- Results encode transport detail (`status`, `method`, `duration_ms`, `error`, `body_truncated`, `headers_truncated`) so firmware can react to truncated payloads or network failures.
- ArduinoJson v7 only—no `DynamicJsonDocument`/`StaticJsonDocument` split—ensuring predictable heap usage across both IDF and Arduino-ESP32 builds.

## Examples
Quick start: initialize once, then mix async and sync styles as needed.

```cpp
#include <Arduino.h>
#include <ESPFetch.h>

ESPFetch fetch;
const char* URL = "https://httpbin.org/post";

void setup() {
    Serial.begin(115200);
    FetchConfig cfg;
    cfg.maxConcurrentRequests = 3;
    cfg.workerStackWords = 6144;
    cfg.workerPriority = 4;
    cfg.defaultTimeoutMs = 12000;
    fetch.init(cfg);

    JsonDocument payload;
    payload["hello"] = "world";

    // Async POST
    bool posting = fetch.post(URL, payload, [](JsonDocument result) {
        if (!result["error"].isNull()) {
            ESP_LOGE("FETCH", "post failed: %s", result["error"]["message"].as<const char*>());
            return;
        }
        ESP_LOGI("FETCH", "POST %d in %d ms", result["status"].as<int>(), result["duration_ms"].as<int>());
        ESP_LOGI("FETCH", "body: %s", result["body"].as<const char*>());
    });
    if (!posting) {
        ESP_LOGE("FETCH", "Failed to start http post");
    }

    // Async GET (headers + bearer token)
    FetchRequestOptions opts;
    opts.headers.push_back({"Accept", "application/json"});
    opts.headers.push_back({"Authorization", "Bearer my-token"});
    bool getting = fetch.get("https://httpbin.org/get", [](JsonDocument result) {
        if (!result["error"].isNull()) {
            ESP_LOGE("FETCH", "get failed: %s", result["error"]["message"].as<const char*>());
            return;
        }
        Serial.printf("Remote IP: %s\n", result["headers"]["cf-connecting-ip"].as<const char*>());
    }, opts);
    if (!getting) {
        ESP_LOGE("FETCH", "Failed to start http get");
    }

    // Sync style: same worker task, but caller blocks
    JsonDocument postResult = fetch.post(URL, payload, portMAX_DELAY);
    if (!postResult["error"].isNull()) {
        ESP_LOGW("FETCH", "Sync post failed: %s", postResult["error"]["message"].as<const char*>());
    }
    JsonDocument getResult = fetch.get("https://httpbin.org/get", portMAX_DELAY);
    Serial.printf("Sync GET status %d\n", getResult["status"].as<int>());
}

void loop() {}
```

Spin the `examples/basic_fetch` sketch if you prefer a ready-to-upload demo.

## Result Shape
All APIs resolve to the same `JsonDocument` shape:

```json
{
  "url": "https://httpbin.org/post",
  "method": "POST",
  "status": 200,
  "ok": true,
  "duration_ms": 742,
  "body": "{\"json\":{\"hello\":\"world\"}, ... }",
  "body_truncated": false,
  "headers_truncated": false,
  "headers": {
    "content-type": "application/json",
    "server": "gunicorn"
  },
  "error": null
}
```

- `error` is `null` on network success, otherwise `{ "code": <esp_err_t>, "message": "<name>" }`.
- `ok` mirrors the Fetch API (`true` only for HTTP 2xx/3xx without transport errors).
- `body_truncated`/`headers_truncated` flip to `true` when limits are reached—check them when expecting large payloads.

## API Reference
- `bool init(const FetchConfig& cfg = {})` – call once after boot. Configure max concurrency, worker stack/priority/core, default timeout, truncation limits, TLS guardrails, and default headers (`userAgent`, `defaultContentType`).
- `void deinit()` – releases the concurrency semaphore. Call only when no in-flight requests remain.
- `bool get(const char* url, FetchCallback cb, const FetchRequestOptions& opts = {})`
- `bool post(const char* url, const JsonDocument& payload, FetchCallback cb, const FetchRequestOptions& opts = {})`
- `JsonDocument get(const char* url, TickType_t waitTicks, const FetchRequestOptions& opts = {})`
- `JsonDocument post(const char* url, const JsonDocument& payload, TickType_t waitTicks, const FetchRequestOptions& opts = {})`

`FetchRequestOptions` per-call knobs:

| Field | Description |
| --- | --- |
| `timeoutMs` | Overrides the default request timeout. |
| `maxBodyBytes` / `maxHeaderBytes` | Override per-request truncation limits (0 = unlimited). |
| `skipTlsCommonNameCheck` | Set `true` to skip CN verification (useful for dev certs). |
| `allowRedirects` | Disable to keep responses from redirecting. |
| `headers` | Vector of `{name, value}` pairs applied before the request is sent. |
| `contentType` | Overrides the default `application/json` for POST payloads. |

`FetchConfig` adds:

| Field | Meaning |
| --- | --- |
| `maxConcurrentRequests` | Counting semaphore depth; returns `false` when no slot is available. |
| `workerStackWords` / `workerPriority` / `coreId` | `xTaskCreatePinnedToCore` arguments for each fetch worker. |
| `defaultTimeoutMs` | Used when `timeoutMs` is unset. |
| `maxBodyBytes` / `maxHeaderBytes` | Global truncation defaults. |
| `slotAcquireTicks` | How long to wait for a semaphore slot (0 = fail fast). |
| `skipTlsCommonNameCheck`, `followRedirects`, `userAgent`, `defaultContentType` | Global protocol defaults. |

## Gotchas
- Call `fetch.init()` exactly once before starting requests. The destructor automatically calls `deinit`, but avoid tearing down the semaphore while workers are running.
- Each async request spawns a dedicated FreeRTOS task. Keep callbacks short—long-running work should signal other tasks.
- Sync helpers still create worker tasks; they just block the caller with `xSemaphoreTake`. There is no cancellation once a request starts.
- Large responses respect `maxBodyBytes`/`maxHeaderBytes`. Watch the `*_truncated` flags if you bump limits or expect big payloads (defaults cap at 16KB body / 4KB headers).
- Skipping TLS checks (`skipTlsCommonNameCheck`) is handy for dev boxes but unsafe for production.
- Headers are exposed as a JSON object. Keys keep their original casing, but JSON requires valid UTF-8—exotic header names may be sanitized by ArduinoJson.

## Restrictions
- ESP32 + FreeRTOS only (Arduino-ESP32 or ESP-IDF). Depends on `esp_http_client`, `esp_timer`, and ArduinoJson v7 (`JsonDocument` API).
- Requires C++17 (`-std=gnu++17`) and enough heap for both the request worker and response `JsonDocument`.
- No retry/cancellation helpers yet; wrap `fetch` with your own policy if needed.

## Tests
Hardware tests are still being assembled. For now lean on `examples/basic_fetch` (PlatformIO + ESP-IDF target) and consider adding regression coverage when contributing features.

## License
MIT — see [LICENSE.md](LICENSE.md).

## ESPToolKit
- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
