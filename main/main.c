#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

// Pinos ESP32-C3
#define PIN_IN1 3
#define PIN_IN2 4
#define PIN_TOP 10
#define PWM_FREQ 5000

typedef enum { IDLE, UP, DOWN } motor_state_t;

volatile motor_state_t current_state = IDLE;
volatile int pwm_speed = 200;
volatile uint32_t descent_time_ms = 3000;
volatile uint32_t start_time_ms = 0;
volatile uint32_t travel_elapsed_ms = 0;

// Lê o tempo em ms
static uint32_t get_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Retorna 1 se o sensor TOP está pressionado (supondo que fecha pro GND)
static bool is_top_active() {
    return gpio_get_level(PIN_TOP) == 0;
}

// Inicializa Motores (LEDC) e Sensores
void hardware_init() {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = PWM_FREQ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel[2] = {
        { .channel = LEDC_CHANNEL_0, .duty = 0, .gpio_num = PIN_IN1,
          .speed_mode = LEDC_LOW_SPEED_MODE, .hpoint = 0, .timer_sel = LEDC_TIMER_0 },
        { .channel = LEDC_CHANNEL_1, .duty = 0, .gpio_num = PIN_IN2,
          .speed_mode = LEDC_LOW_SPEED_MODE, .hpoint = 0, .timer_sel = LEDC_TIMER_0 }
    };
    ledc_channel_config(&ledc_channel[0]);
    ledc_channel_config(&ledc_channel[1]);

    gpio_reset_pin(PIN_TOP);
    gpio_set_direction(PIN_TOP, GPIO_MODE_INPUT);
    gpio_pullup_en(PIN_TOP);
}

// Comandos de Movimento
void motor_stop() {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    if (current_state != IDLE) {
        travel_elapsed_ms = get_ms() - start_time_ms;
    }
    current_state = IDLE;
}

void motor_up() {
    if (!is_top_active()) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pwm_speed);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        current_state = UP;
        start_time_ms = get_ms();
    }
}

void motor_down() {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, pwm_speed);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    current_state = DOWN;
    start_time_ms = get_ms();
}

// Task de Segurança (Roda a cada 20ms)
void control_task(void *pvParameter) {
    while(1) {
        uint32_t now = get_ms();
        
        if (current_state == UP) {
            travel_elapsed_ms = now - start_time_ms;
            // Se bateu no teto ou se passou de 20s (Timeout de segurança)
            if (is_top_active() || travel_elapsed_ms > 20000) {
                motor_stop();
                travel_elapsed_ms = 0; // Zera deslocamento no topo
            }
        } 
        else if (current_state == DOWN) {
            travel_elapsed_ms = now - start_time_ms;
            // Desce até atingir o tempo configurado
            if (travel_elapsed_ms >= descent_time_ms) {
                motor_stop();
                travel_elapsed_ms = descent_time_ms; // Trava o tempo máximo
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ----------------------------------------------------
// SERVIDOR WEB
// ----------------------------------------------------
extern const unsigned char control_html_start[] asm("_binary_control_html_start");
extern const unsigned char control_html_end[]   asm("_binary_control_html_end");

static esp_err_t html_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)control_html_start, control_html_end - control_html_start - 1);
}

static esp_err_t cmd_handler(httpd_req_t *req) {
    if (strstr(req->uri, "/api/up")) motor_up();
    else if (strstr(req->uri, "/api/down")) motor_down();
    else motor_stop(); 
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t settings_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "v", param, sizeof(param)) == ESP_OK) {
            int v = atoi(param);
            if (v >= 80 && v <= 255) pwm_speed = v;
        }
        if (httpd_query_key_value(query, "t", param, sizeof(param)) == ESP_OK) {
            int t = atoi(param);
            if (t >= 500 && t <= 20000) descent_time_ms = t;
        }
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t status_handler(httpd_req_t *req) {
    char buf[200];
    const char* st = (current_state == UP) ? "UP" : (current_state == DOWN) ? "DOWN" : "IDLE";
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"home_active\":%s,\"elapsed_ms\":%"PRIu32",\"descent_time\":%"PRIu32"}", 
             st, is_top_active() ? "true" : "false", travel_elapsed_ms, descent_time_ms);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

void start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = html_handler };
        httpd_uri_t uri_up = { .uri = "/api/up*", .method = HTTP_POST, .handler = cmd_handler };
        httpd_uri_t uri_dw = { .uri = "/api/down*", .method = HTTP_POST, .handler = cmd_handler };
        httpd_uri_t uri_st = { .uri = "/api/stop*", .method = HTTP_POST, .handler = cmd_handler };
        httpd_uri_t uri_set = { .uri = "/api/settings*", .method = HTTP_POST, .handler = settings_handler };
        httpd_uri_t uri_stat = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
        
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_up);
        httpd_register_uri_handler(server, &uri_dw);
        httpd_register_uri_handler(server, &uri_st);
        httpd_register_uri_handler(server, &uri_set);
        httpd_register_uri_handler(server, &uri_stat);
    }
}

// ----------------------------------------------------
// WIFI PONTO DE ACESSO
// ----------------------------------------------------
void start_wifi() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "Elevador",
            .password = "12345678",
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
}

void app_main() {
    nvs_flash_init();
    hardware_init();
    start_wifi();
    start_webserver();

    // Roda a verificação de hardware de tempos em tempos
    xTaskCreate(control_task, "control_task", 2048, NULL, 5, NULL);
}