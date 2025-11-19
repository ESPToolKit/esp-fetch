#include <Arduino.h>
#include <ESPFetch.h>
#include <unity.h>

static void test_init_rejects_zero_concurrency() {
    ESPFetch fetch;
    FetchConfig cfg{};
    cfg.maxConcurrentRequests = 0;
    TEST_ASSERT_FALSE(fetch.init(cfg));
    TEST_ASSERT_FALSE(fetch.initialized());
}

static void test_init_and_deinit_cycle_updates_initialized_flag() {
    ESPFetch fetch;
    TEST_ASSERT_TRUE(fetch.init());
    TEST_ASSERT_TRUE(fetch.initialized());
    fetch.deinit();
    TEST_ASSERT_FALSE(fetch.initialized());
}

static void test_async_get_requires_initialization() {
    ESPFetch fetch;
    volatile bool invoked = false;
    auto cb = [&](JsonDocument) {
        invoked = true;
    };
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
    RUN_TEST(test_async_get_requires_initialization);
    RUN_TEST(test_sync_get_reports_error_when_not_initialized);
    RUN_TEST(test_sync_get_requires_url);
    RUN_TEST(test_sync_post_requires_url);
    RUN_TEST(test_sync_post_reports_error_when_not_initialized);
    UNITY_END();
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
