#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "LAB_SOFTAP";

// กำลังส่งสำหรับตาราง Lab 6.2 (หน่วย 0.25 dBm): 80=20dBm, 60=15dBm, 40=10dBm, 20=5dBm, 8=2dBm
// ต้องตั้งค่าเท่ากันทั้ง Lab6-1 (AP) และ Lab6-2 (Client)
#define TX_POWER_QUARTER_DBM       80
#define EXAMPLE_ESP_WIFI_SSID      "ESP32_AP_0116"
#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_MAX_STA_CONN       4
#define SERVER_PORT                8080
#define RECV_BUF_SIZE              1024

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "=======================================================");
            ESP_LOGI(TAG, "[FORENSIC EVENT]: Client Connected to ESP32 SoftAP!");
            ESP_LOGI(TAG, "  -> Client MAC Address : %02X:%02X:%02X:%02X:%02X:%02X",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            ESP_LOGI(TAG, "  -> Assigned AID       : %d", event->aid);
            ESP_LOGI(TAG, "=======================================================");
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGW(TAG, "=======================================================");
            ESP_LOGW(TAG, "[FORENSIC EVENT]: Client Disconnected from ESP32 SoftAP!");
            ESP_LOGW(TAG, "  -> Client MAC Address : %02X:%02X:%02X:%02X:%02X:%02X",
                     event->mac[0], event->mac[1], event->mac[2],
                     event->mac[3], event->mac[4], event->mac[5]);
            ESP_LOGW(TAG, "  -> Released AID       : %d", event->aid);
            ESP_LOGW(TAG, "=======================================================");
        }
    }
}

// ------------------------------------------------------------
// TCP Server Task
// รอรับ Connection จาก Client (Lab 6-2) บน Port 8080
// แต่ละ Connection: recv() จนหมด → log สถิติ → close → รอรับใหม่
// ------------------------------------------------------------
static void tcp_server_task(void *arg)
{
    char rx_buf[RECV_BUF_SIZE];

    // 1) สร้าง Server Socket
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "[TCP SERVER]: socket() failed, errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    // 2) ตั้งค่า SO_REUSEADDR เพื่อให้ bind() ใหม่ได้ทันทีหลัง reset
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3) bind() ผูก socket กับทุก IP บน Port 8080
    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "[TCP SERVER]: bind() failed, errno=%d", errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    // 4) listen() เปิดรับ Connection (backlog = 4)
    if (listen(server_fd, 4) != 0) {
        ESP_LOGE(TAG, "[TCP SERVER]: listen() failed, errno=%d", errno);
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[TCP SERVER]: Listening on 192.168.4.1:%d", SERVER_PORT);

    int session = 0;
    while (1) {
        // 5) accept() รอรับ Client เชื่อมต่อ
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "[TCP SERVER]: accept() failed, errno=%d", errno);
            continue;
        }

        session++;
        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "=======================================================");
        ESP_LOGI(TAG, "[TCP SERVER SESSION %d]: Client connected from %s:%d",
                 session, client_ip, ntohs(client_addr.sin_port));

        // 6) recv() รับข้อมูลจนกว่า Client จะปิด Socket
        int total_bytes = 0;
        int received;
        while ((received = recv(client_fd, rx_buf, sizeof(rx_buf), 0)) > 0) {
            total_bytes += received;
        }

        ESP_LOGI(TAG, "[TCP SERVER SESSION %d]: Transfer complete", session);
        ESP_LOGI(TAG, "  -> Total Received : %d Bytes", total_bytes);
        ESP_LOGI(TAG, "=======================================================");

        // 7) ปิด Client Socket แล้ววนรอรับใหม่
        close(client_fd);
    }

    close(server_fd);
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "[FORENSIC]: Call nvs_flash_init()");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_init()");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_event_loop_create_default()");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_create_default_wifi_ap()");
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_LOGI(TAG, "[FORENSIC]: SoftAP Interface created at %p (Default IP: 192.168.4.1)", ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_init(&cfg)");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_event_handler_instance_register(WIFI_EVENT)");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = 1,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_mode(WIFI_MODE_AP)");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_config(WIFI_IF_AP, &wifi_config)");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_start()");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(TX_POWER_QUARTER_DBM));
    ESP_LOGI(TAG, "[FORENSIC]: AP Tx Power limit = %.2f dBm", TX_POWER_QUARTER_DBM / 4.0);

    ESP_LOGI(TAG, "==================================================================");
    ESP_LOGI(TAG, "  ESP32 SoftAP Running! SSID: \"%s\", Channel: %d",
             EXAMPLE_ESP_WIFI_SSID, wifi_config.ap.channel);
    ESP_LOGI(TAG, "==================================================================");

    // เปิด TCP Server Task (Stack 4096 bytes, Priority 5)
    xTaskCreate(tcp_server_task, "tcp_server_task", 4096, NULL, 5, NULL);
}
