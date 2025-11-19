#include <Arduino.h>
#include <ESPFetch.h>

ESPFetch fetch;

const char *POST_URL = "https://httpbin.org/post";
const char *GET_URL = "https://httpbin.org/get";

void setup() {
    Serial.begin(115200);
    delay(200);

    fetch.init({
        .maxConcurrentRequests = 2,
        .workerStackWords = 6144,
        .workerPriority = 4,
        .defaultTimeoutMs = 12000,
    });

    JsonDocument payload;
    payload["hello"] = "world";

    bool posting = fetch.post(POST_URL, payload, [](JsonDocument result) {
        if (!result["error"].isNull()) {
            ESP_LOGE("FETCH_DEMO", "async post failed: %s", result["error"]["message"].as<const char *>());
            return;
        }
        ESP_LOGI("FETCH_DEMO", "async post status %d body len %u",
                 result["status"].as<int>(),
                 result["body"].as<String>().length());
    });
    if (!posting) {
        ESP_LOGE("FETCH_DEMO", "Failed to start http post");
    }

    FetchRequestOptions opts;
    opts.headers.push_back({"Accept", "application/json"});
    bool getting = fetch.get(GET_URL, [](JsonDocument result) {
        if (!result["error"].isNull()) {
            ESP_LOGE("FETCH_DEMO", "async get failed: %s", result["error"]["message"].as<const char *>());
            return;
        }
        Serial.printf("Server: %s\n", result["headers"]["server"].as<const char *>());
    }, opts);
    if (!getting) {
        ESP_LOGE("FETCH_DEMO", "Failed to start http get");
    }

    JsonDocument postResult = fetch.post(POST_URL, payload, portMAX_DELAY);
    if (!postResult["error"].isNull()) {
        ESP_LOGW("FETCH_DEMO", "sync post failed: %s", postResult["error"]["message"].as<const char *>());
    } else {
        Serial.printf("Sync POST status %d\n", postResult["status"].as<int>());
    }

    JsonDocument getResult = fetch.get(GET_URL, portMAX_DELAY);
    if (!getResult["error"].isNull()) {
        ESP_LOGW("FETCH_DEMO", "sync get failed: %s", getResult["error"]["message"].as<const char *>());
    } else {
        Serial.printf("Your IP: %s\n", getResult["body"].as<const char *>());
    }
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
