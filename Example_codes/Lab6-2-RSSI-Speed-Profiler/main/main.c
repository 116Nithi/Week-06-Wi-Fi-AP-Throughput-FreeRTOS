#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "CLIENT_PROFILER";

// กำลังส่งสำหรับตาราง Lab 6.2 (หน่วย 0.25 dBm): 80=20dBm, 60=15dBm, 40=10dBm, 20=5dBm, 8=2dBm
// ต้องตั้งค่าเท่ากันทั้ง Lab6-1 (AP) และ Lab6-2 (Client)
#define TX_POWER_QUARTER_DBM       80
#define AP_SSID            "ESP32_AP_0116"
#define AP_PASS            "12345678"
#define SERVER_IP          "192.168.4.1"
#define SERVER_PORT        8080
#define TEST_DATA_LEN      1024
#define TEST_ROUNDS        50
#define BENCHMARK_ROUNDS   10
#define WIFI_MAXIMUM_RETRY 10

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "[FORENSIC EVENT]: Station started; connecting to %s", AP_SSID);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "[FORENSIC EVENT]: Disconnected, reason=%d", event->reason);

        if (s_retry_count < WIFI_MAXIMUM_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)",
                     s_retry_count, WIFI_MAXIMUM_RETRY);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;

        s_retry_count = 0;
        ESP_LOGI(TAG, "[FORENSIC EVENT]: Connected; IP=" IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t send_all(int socket_fd, const char *data, size_t data_len,
                          int *total_bytes)
{
    size_t bytes_remaining = data_len;
    const char *position = data;

    while (bytes_remaining > 0) {
        int written = send(socket_fd, position, bytes_remaining, 0);
        if (written < 0) {
            ESP_LOGE(TAG, "send() failed, errno=%d", errno);
            return ESP_FAIL;
        }

        position += written;
        bytes_remaining -= written;
        *total_bytes += written;
    }

    return ESP_OK;
}

static void perform_throughput_test(int benchmark_round)
{
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read AP information: %s", esp_err_to_name(err));
        return;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket, errno=%d", errno);
        return;
    }

    struct sockaddr_in destination = {
        .sin_addr.s_addr = inet_addr(SERVER_IP),
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
    };

    ESP_LOGI(TAG, "[ROUND %d/%d]: Connecting to %s:%d",
             benchmark_round, BENCHMARK_ROUNDS, SERVER_IP, SERVER_PORT);
    if (connect(socket_fd, (struct sockaddr *)&destination, sizeof(destination)) != 0) {
        ESP_LOGE(TAG, "Socket connect failed, errno=%d", errno);
        close(socket_fd);
        return;
    }

    char payload[TEST_DATA_LEN];
    memset(payload, 'A', sizeof(payload));

    int total_bytes = 0;
    int64_t start_time = esp_timer_get_time();

    for (int chunk = 0; chunk < TEST_ROUNDS; chunk++) {
        if (send_all(socket_fd, payload, sizeof(payload), &total_bytes) != ESP_OK) {
            close(socket_fd);
            return;
        }
    }

    int64_t end_time = esp_timer_get_time();
    shutdown(socket_fd, SHUT_WR);
    close(socket_fd);

    double elapsed_sec = (double)(end_time - start_time) / 1000000.0;
    double throughput_kbps = elapsed_sec > 0.0
                                 ? (total_bytes * 8.0) / (elapsed_sec * 1000.0)
                                 : 0.0;

    ESP_LOGI(TAG, "=======================================================");
    ESP_LOGI(TAG, " [BENCHMARK RESULT %d/%d]", benchmark_round, BENCHMARK_ROUNDS);
    ESP_LOGI(TAG, "  -> Current RSSI       : %d dBm", ap_info.rssi);
    ESP_LOGI(TAG, "  -> Total Transferred  : %d Bytes", total_bytes);
    ESP_LOGI(TAG, "  -> Time Elapsed       : %.3f Seconds", elapsed_sec);
    ESP_LOGI(TAG, "  -> Measured Speed     : %.2f Kbps", throughput_kbps);
    ESP_LOGI(TAG, "=======================================================");
}

static void profiler_task(void *arg)
{
    for (int round = 1; round <= BENCHMARK_ROUNDS; round++) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE,
                                                pdFALSE,
                                                portMAX_DELAY);
        if ((bits & WIFI_FAIL_BIT) != 0) {
            ESP_LOGE(TAG, "Cannot connect to %s; profiler stopped", AP_SSID);
            break;
        }

        perform_throughput_test(round);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "All benchmark rounds completed");
    vTaskDelete(NULL);
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_init()");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_event_loop_create_default()");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_create_default_wifi_sta()");
    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(station_netif == NULL ? ESP_FAIL : ESP_OK);

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_init(&config)");
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_mode(WIFI_MODE_STA)");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_config(WIFI_IF_STA, &wifi_config)");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_start()");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(TX_POWER_QUARTER_DBM));
    ESP_LOGI(TAG, "[FORENSIC]: Client Tx Power limit = %.2f dBm", TX_POWER_QUARTER_DBM / 4.0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[FORENSIC]: Call nvs_flash_init()");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_init_sta();
    ESP_LOGI(TAG, "Client profiler ready: 50 KB x %d rounds", BENCHMARK_ROUNDS);
    xTaskCreate(profiler_task, "profiler_task", 4096, NULL, 5, NULL);
}
