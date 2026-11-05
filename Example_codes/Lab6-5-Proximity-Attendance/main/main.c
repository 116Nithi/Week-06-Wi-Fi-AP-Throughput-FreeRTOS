#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

static const char *TAG = "SMART_ATTENDANCE";

#define AP_SSID          "CLASSROOM_ATTENDANCE_AP"
#define AP_PASS          "12345678"
#define MAX_STUDENTS     5
#define RSSI_THRESHOLD   -55   // ผ่านเมื่อ RSSI >= -55 dBm (อยู่ในรัศมีโต๊ะปฏิบัติการ)
#define HTML_BUF_SIZE    2560

typedef enum {
    CHECKIN_NONE = 0,
    CHECKIN_PASSED,
    CHECKIN_REJECTED,
} checkin_status_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    bool connected;
    checkin_status_t status;
    uint32_t timestamp_sec;
} student_record_t;

static student_record_t s_records[MAX_STUDENTS];
static int s_student_count = 0;
static SemaphoreHandle_t s_records_mutex;
static esp_netif_t *s_ap_netif;

// เรียกโดยถือ Mutex อยู่แล้วเท่านั้น
static student_record_t *find_record(const uint8_t *mac) {
    for (int i = 0; i < s_student_count; i++) {
        if (memcmp(s_records[i].mac, mac, 6) == 0) {
            return &s_records[i];
        }
    }
    return NULL;
}

// อ่าน RSSI จริงของ Client ทุกเครื่องจาก Wi-Fi Driver
static void refresh_rssi(void) {
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_records_mutex, portMAX_DELAY);
    for (int i = 0; i < sta_list.num; i++) {
        student_record_t *rec = find_record(sta_list.sta[i].mac);
        if (rec) {
            rec->rssi = sta_list.sta[i].rssi;
        }
    }
    xSemaphoreGive(s_records_mutex);
}

// แปลง IP ของ HTTP Client เป็น MAC เพื่อระบุว่าใครกดเช็กชื่อ
static bool get_client_mac(httpd_req_t *req, uint8_t *mac_out) {
    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &addr_len) != 0) {
        return false;
    }
    // HTTP Server ของ ESP-IDF ใช้ IPv6 socket โดย IPv4 จะอยู่ใน 4 ไบต์สุดท้าย
    uint32_t client_ip = addr.sin6_addr.un.u32_addr[3];

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < sta_list.num; i++) {
        esp_netif_pair_mac_ip_t pair = {0};
        memcpy(pair.mac, sta_list.sta[i].mac, 6);
        if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, 1, &pair) == ESP_OK &&
            pair.ip.addr == client_ip) {
            memcpy(mac_out, pair.mac, 6);
            return true;
        }
    }
    return false;
}

static esp_err_t http_attendance_html_handler(httpd_req_t *req) {
    refresh_rssi();

    char *resp = malloc(HTML_BUF_SIZE);
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int len = snprintf(resp, HTML_BUF_SIZE,
        "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='3'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:Arial;text-align:center;background:#f4f4f9;padding:20px;}"
        ".card{background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.2);}"
        "table{width:100%%;border-collapse:collapse;margin-top:15px;}"
        "th,td{border:1px solid #ddd;padding:8px;text-align:center;}"
        "th{background:#4CAF50;color:white;}"
        ".btn{background:#2196F3;color:white;padding:10px 20px;border:none;border-radius:5px;cursor:pointer;}"
        "</style></head><body>"
        "<div class='card'><h2>Smart Classroom Proximity Check-in</h2>"
        "<p>Threshold: <b>%d dBm</b> (Student ID 67030116)</p>"
        "<form action='/checkin' method='POST'><button class='btn'>Confirm Attendance (1-Click)</button></form>"
        "<h3>Connected Devices</h3>"
        "<table><tr><th>Device MAC</th><th>RSSI (dBm)</th><th>Distance</th><th>Check-in</th><th>Time (s)</th></tr>",
        RSSI_THRESHOLD);

    xSemaphoreTake(s_records_mutex, portMAX_DELAY);
    for (int i = 0; i < s_student_count && len < HTML_BUF_SIZE; i++) {
        const student_record_t *rec = &s_records[i];
        const char *distance = !rec->connected ? "Offline"
                             : rec->rssi >= RSSI_THRESHOLD ? "<font color='green'><b>Near</b></font>"
                                                           : "<font color='red'><b>Far</b></font>";
        const char *result = rec->status == CHECKIN_PASSED ? "<font color='green'><b>Passed</b></font>"
                           : rec->status == CHECKIN_REJECTED ? "<font color='red'><b>Rejected</b></font>"
                                                             : "-";
        len += snprintf(resp + len, HTML_BUF_SIZE - len,
            "<tr><td>" MACSTR "</td><td>%d</td><td>%s</td><td>%s</td><td>%lu</td></tr>",
            MAC2STR(rec->mac), rec->rssi, distance, result, (unsigned long)rec->timestamp_sec);
    }
    xSemaphoreGive(s_records_mutex);

    if (len < HTML_BUF_SIZE) {
        snprintf(resp + len, HTML_BUF_SIZE - len, "</table></div></body></html>");
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return err;
}

static esp_err_t http_checkin_handler(httpd_req_t *req) {
    uint8_t mac[6];
    bool identified = get_client_mac(req, mac);
    if (identified) {
        refresh_rssi();
        xSemaphoreTake(s_records_mutex, portMAX_DELAY);
        student_record_t *rec = find_record(mac);
        if (rec) {
            rec->status = rec->rssi >= RSSI_THRESHOLD ? CHECKIN_PASSED : CHECKIN_REJECTED;
            rec->timestamp_sec = (uint32_t)(esp_timer_get_time() / 1000000);
            ESP_LOGI(TAG, "[CHECK-IN]: " MACSTR " RSSI=%d dBm -> %s", MAC2STR(mac), rec->rssi,
                     rec->status == CHECKIN_PASSED ? "PASSED" : "REJECTED");
        }
        xSemaphoreGive(s_records_mutex);
    } else {
        ESP_LOGW(TAG, "[CHECK-IN]: Could not match HTTP client to a connected station");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = http_attendance_html_handler,
            .user_ctx = NULL
        };
        httpd_uri_t uri_post = {
            .uri      = "/checkin",
            .method   = HTTP_POST,
            .handler  = http_checkin_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_post);
        ESP_LOGI(TAG, "Attendance Web Server Started at http://192.168.4.1");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base != WIFI_EVENT) {
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        xSemaphoreTake(s_records_mutex, portMAX_DELAY);
        student_record_t *rec = find_record(event->mac);
        if (rec == NULL && s_student_count < MAX_STUDENTS) {
            rec = &s_records[s_student_count++];
            memcpy(rec->mac, event->mac, 6);
        }
        if (rec) {
            rec->connected = true;
        }
        xSemaphoreGive(s_records_mutex);
        ESP_LOGI(TAG, "[PROXIMITY DETECTED]: Device " MACSTR " connected, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        xSemaphoreTake(s_records_mutex, portMAX_DELAY);
        student_record_t *rec = find_record(event->mac);
        if (rec) {
            rec->connected = false;
        }
        xSemaphoreGive(s_records_mutex);
        ESP_LOGW(TAG, "[PROXIMITY LOST]: Device " MACSTR " disconnected", MAC2STR(event->mac));
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_records_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_records_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = MAX_STUDENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP \"%s\" started; RSSI threshold = %d dBm", AP_SSID, RSSI_THRESHOLD);
    start_web_server();
}
