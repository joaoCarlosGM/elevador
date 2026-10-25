/* Miniature elevator: one upper HOME switch and a timed virtual lower limit.
 * Motor power must have independent isolation. L293D EN tied high in hardware:
 * IN1=IN2=0 requests a brake; it cannot guarantee a physical stop. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#ifndef ELEVATOR_HOST_TEST
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_rom_crc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#define PIN_IN1 CONFIG_ELEVATOR_MOTOR_IN1_GPIO
#define PIN_IN2 CONFIG_ELEVATOR_MOTOR_IN2_GPIO
#define PIN_TOP CONFIG_ELEVATOR_HOME_GPIO
#define PERIOD_MS 10u
#define DEBOUNCE_MS 40u
#define RELEASE_MS 1500u
#define REVERSE_MS 400u
#define RETURN_WAIT_MS 10000u
#define MIN_TRAVEL_MS 400u
#define MAX_HOME_MS CONFIG_ELEVATOR_HOME_TIMEOUT_MS
#define MAX_CAL_MS CONFIG_ELEVATOR_CAL_TIMEOUT_MS
#define DOWN_MARGIN_MS 1200u
#define SETTINGS_MAGIC 0x454C5634u
#define SETTINGS_VERSION 4u
#define SETTINGS_KEY "setup_v4"

typedef enum {
    UNKNOWN, HOME_IDLE, CAL_READY, HOMING, HOME_VERIFY, CAL_DOWN,
    SAVE_MIN, DOWN, MIN_IDLE, FAULT
} state_t;
typedef enum {
    R_NONE, R_HOME, R_UP, R_DOWN, R_STOP, R_BEGIN_CAL, R_SET_MIN
} request_t;
typedef struct {
    uint32_t magic, version, minimum_ms;
    uint8_t up_in1_high, home_active_level, reserved[2];
    uint32_t checksum;
} settings_t;
typedef struct {
    state_t state;
    request_t request;
    settings_t settings, staged;
    bool save_pending, saving, save_go_home, auto_home_after_save;
    bool cancel_auto_home;
    bool hardware_ok, nvs_ok, wifi_ok, motor_on, direction_up;
    bool top_raw, top_stable, top_seen, top_released, timer_running;
    bool fault_latched, config_verified;
    char reason[40];
    uint32_t started_ms, last_brake_ms, home_verify_ms, at_min_ms;
    uint32_t top_change_ms, travel_elapsed_ms, staged_minimum_ms;
    uint32_t now_ms;
} controller_t;
static controller_t ctl;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "elevador";

static uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static const char *state_name(state_t st) {
    switch (st) {
    case UNKNOWN: return "UNKNOWN";
    case HOME_IDLE: return "HOME";
    case CAL_READY: return "CAL_READY";
    case HOMING: return "HOMING";
    case HOME_VERIFY: return "HOME_VERIFY";
    case CAL_DOWN: return "CAL_DOWN";
    case SAVE_MIN: return "SAVING_MIN";
    case DOWN: return "DOWN";
    case MIN_IDLE: return "MIN";
    default: return "FAULT";
    }
}
static uint32_t settings_crc(const settings_t *s) {
    return esp_rom_crc32_le(0, (const uint8_t *)s, offsetof(settings_t,checksum));
}
static bool settings_valid(const settings_t *s) {
    return s->magic == SETTINGS_MAGIC && s->version == SETTINGS_VERSION &&
        s->up_in1_high <= 1 && s->home_active_level <= 1 &&
        (s->minimum_ms == 0 ||
         (s->minimum_ms >= MIN_TRAVEL_MS && s->minimum_ms <= MAX_CAL_MS)) &&
        s->checksum == settings_crc(s);
}
static settings_t defaults(void) {
    settings_t s = { .magic=SETTINGS_MAGIC, .version=SETTINGS_VERSION,
        .minimum_ms=0, .up_in1_high=1, .home_active_level=1 };
    s.checksum=settings_crc(&s);
    return s;
}
/* Attempt both GPIO low commands, even if the first write fails. */
static bool brake(void) {
    bool a=gpio_set_level(PIN_IN1,0)==ESP_OK;
    bool b=gpio_set_level(PIN_IN2,0)==ESP_OK;
    ctl.motor_on=false;
    ctl.last_brake_ms=ctl.now_ms;
    return a && b;
}
static void fault(const char *reason) {
    if (!brake()) reason="GPIO_STOP_FAILED";
    ctl.request=R_NONE;
    ctl.state=FAULT;
    ctl.fault_latched=true;
    snprintf(ctl.reason,sizeof(ctl.reason),"%s",reason);
}
static bool start_motor(bool up) {
    if (!ctl.hardware_ok || !ctl.wifi_ok || !ctl.nvs_ok || ctl.motor_on ||
        ctl.fault_latched || ctl.save_pending || ctl.saving ||
        (uint32_t)(ctl.now_ms-ctl.last_brake_ms)<REVERSE_MS) return false;
    if (!brake()) { fault("GPIO_STOP_FAILED"); return false; }
    int in1=(up == (ctl.settings.up_in1_high!=0)) ? 1 : 0;
    if (gpio_set_level(PIN_IN1,in1)!=ESP_OK ||
        gpio_set_level(PIN_IN2,!in1)!=ESP_OK) {
        fault("GPIO_START_FAILED"); return false;
    }
    ctl.motor_on=true; ctl.direction_up=up;
    ctl.started_ms=ctl.now_ms;
    ctl.travel_elapsed_ms=0;
    ctl.top_released=!ctl.top_stable;
    return true;
}
static void update_top(void) {
    bool active=gpio_get_level(PIN_TOP)==ctl.settings.home_active_level;
    if (!ctl.top_seen || active!=ctl.top_raw) {
        ctl.top_seen=true;
        ctl.top_raw=active;
        ctl.top_change_ms=ctl.now_ms;
    }
    if ((uint32_t)(ctl.now_ms-ctl.top_change_ms)>=DEBOUNCE_MS)
        ctl.top_stable=ctl.top_raw;
}
static bool ready(void) {
    return ctl.hardware_ok && ctl.nvs_ok && ctl.wifi_ok &&
        ctl.config_verified && ctl.top_seen &&
        (uint32_t)(ctl.now_ms-ctl.top_change_ms)>=DEBOUNCE_MS &&
        !ctl.fault_latched && !ctl.save_pending && !ctl.saving;
}
static void begin_home(void) {
    if (ctl.top_stable && ctl.top_raw) {
        if (!brake()) { fault("GPIO_STOP_FAILED"); return; }
        ctl.state=HOME_IDLE; ctl.travel_elapsed_ms=0;
        return;
    }
    if (start_motor(true)) ctl.state=HOMING;
}
static void controller_tick(void *unused) {
    (void)unused;
    portENTER_CRITICAL(&mux);
    ctl.now_ms=clock_ms();
    if (!ctl.hardware_ok) { portEXIT_CRITICAL(&mux); return; }
    update_top();
    request_t r=ctl.request; ctl.request=R_NONE;
    if (r==R_STOP) {
        bool known_home=(ctl.state==HOME_IDLE||ctl.state==CAL_READY) &&
            ctl.top_stable && ctl.top_raw;
        ctl.auto_home_after_save=false;
        ctl.save_go_home=false; ctl.cancel_auto_home=true;
        if (!brake()) fault("GPIO_STOP_FAILED");
        else if (ctl.state!=FAULT) {
            ctl.state=known_home?HOME_IDLE:UNKNOWN;
            ctl.travel_elapsed_ms=0;
        }
        portEXIT_CRITICAL(&mux); return;
    }
    if (!ctl.top_seen || (uint32_t)(ctl.now_ms-ctl.top_change_ms)<DEBOUNCE_MS) {
        /* Never start on an unconfirmed initial switch value. */
        if (ctl.state==UNKNOWN && ctl.motor_on) fault("HOME_INPUT_UNSTABLE");
    }
    if (ctl.state==FAULT || ctl.saving || ctl.save_pending) {
        portEXIT_CRITICAL(&mux); return;
    }
    if (ctl.state==UNKNOWN && ctl.top_seen &&
        (uint32_t)(ctl.now_ms-ctl.top_change_ms)>=DEBOUNCE_MS &&
        ctl.top_stable && ctl.top_raw) {
        ctl.state=HOME_IDLE; ctl.travel_elapsed_ms=0;
    }
    switch (ctl.state) {
    case HOMING:
        if (ctl.top_raw) {
            if (!brake()) { fault("GPIO_STOP_FAILED"); break; }
            ctl.state=HOME_VERIFY;
            ctl.home_verify_ms=ctl.now_ms;
            break;
        }
        if ((uint32_t)(ctl.now_ms-ctl.started_ms)>MAX_HOME_MS)
            fault("HOME_TIMEOUT");
        break;
    case HOME_VERIFY:
        if (ctl.top_stable && ctl.top_raw) {
            ctl.state=HOME_IDLE; ctl.travel_elapsed_ms=0;
        } else if ((uint32_t)(ctl.now_ms-ctl.home_verify_ms)>DEBOUNCE_MS*4)
            fault("HOME_SIGNAL_UNSTABLE");
        break;
    case CAL_DOWN:
    case DOWN: {
        uint32_t elapsed=ctl.now_ms-ctl.started_ms;
        ctl.travel_elapsed_ms=elapsed;
        if (ctl.top_stable && !ctl.top_raw) ctl.top_released=true;
        if (ctl.top_raw && ctl.top_released) { fault("HOME_RETRIGGERED"); break; }
        if (ctl.top_raw && elapsed>RELEASE_MS) { fault("HOME_NOT_RELEASED"); break; }
        uint32_t cap=ctl.state==CAL_DOWN ? MAX_CAL_MS :
            ctl.settings.minimum_ms+DOWN_MARGIN_MS;
        if (elapsed>=cap) {
            if (ctl.state==CAL_DOWN) fault("CALIBRATION_TIMEOUT");
            else fault("DOWN_TIMEOUT");
        } else if (ctl.state==DOWN && elapsed>=ctl.settings.minimum_ms) {
            if (!brake()) fault("GPIO_STOP_FAILED");
            else {ctl.state=MIN_IDLE; ctl.at_min_ms=ctl.now_ms;
                  ctl.travel_elapsed_ms=ctl.settings.minimum_ms;}
        }
        break;
    }
    case MIN_IDLE:
        if ((uint32_t)(ctl.now_ms-ctl.at_min_ms)>=RETURN_WAIT_MS && ready())
            begin_home();
        break;
    default: break;
    }
    if (!ready()) { portEXIT_CRITICAL(&mux); return; }
    if ((r==R_HOME||r==R_UP||r==R_DOWN) &&
        (uint32_t)(ctl.now_ms-ctl.last_brake_ms)<REVERSE_MS) {
        /* Retain a one-tap command until the fixed reversal dwell expires. */
        ctl.request=r;
        portEXIT_CRITICAL(&mux);
        return;
    }
    /* SET MIN returns HOME after the NVS commit and the reversal dwell. */
    if (ctl.auto_home_after_save && ctl.state==UNKNOWN &&
        (uint32_t)(ctl.now_ms-ctl.last_brake_ms)>=REVERSE_MS) {
        begin_home();
        if (ctl.state==HOMING || ctl.state==HOME_IDLE)
            ctl.auto_home_after_save=false;
    }
    switch (r) {
    case R_HOME: case R_UP:
        if (ctl.state==UNKNOWN || ctl.state==MIN_IDLE ||
            ctl.state==HOME_IDLE || ctl.state==CAL_READY) begin_home();
        break;
    case R_BEGIN_CAL:
        if (ctl.state==HOME_IDLE && ctl.top_stable && ctl.top_raw) {
            ctl.cancel_auto_home=false;
            ctl.state=CAL_READY;
        }
        break;
    case R_DOWN:
        if (ctl.state==CAL_READY && ctl.top_stable && ctl.top_raw) {
            if (start_motor(false)) ctl.state=CAL_DOWN;
        } else if (ctl.state==HOME_IDLE && ctl.settings.minimum_ms &&
                   ctl.top_stable && ctl.top_raw) {
            if (start_motor(false)) ctl.state=DOWN;
        }
        break;
    case R_SET_MIN:
        if (ctl.state==CAL_DOWN) {
            uint32_t elapsed=ctl.now_ms-ctl.started_ms;
            if (elapsed>=MIN_TRAVEL_MS && elapsed<=MAX_CAL_MS) {
                if (!brake()) fault("GPIO_STOP_FAILED");
                else {
                    ctl.staged=ctl.settings;
                    ctl.staged.minimum_ms=elapsed;
                    ctl.staged.checksum=settings_crc(&ctl.staged);
                    ctl.save_pending=true; ctl.save_go_home=true;
                    ctl.staged_minimum_ms=elapsed; ctl.state=SAVE_MIN;
                }
            }
        }
        break;
    default: break;
    }
    portEXIT_CRITICAL(&mux);
}
#ifndef ELEVATOR_HOST_TEST
static nvs_handle_t nvs;
static bool save_blob(const settings_t *value) {
    esp_err_t result=nvs_set_blob(nvs,SETTINGS_KEY,value,sizeof(*value));
    if (result==ESP_OK) result=nvs_commit(nvs);
    return result==ESP_OK;
}
static void flush_pending(void) {
    settings_t copy;bool do_save=false,go_home=false;
    portENTER_CRITICAL(&mux);
    if (ctl.save_pending && !ctl.saving) {
        ctl.saving=true; do_save=true; copy=ctl.staged;
        go_home=ctl.save_go_home;
    }
    portEXIT_CRITICAL(&mux);
    if (!do_save) return;
    bool ok=save_blob(&copy);
    portENTER_CRITICAL(&mux);
    ctl.save_pending=false; ctl.saving=false; ctl.save_go_home=false;
    if (!ok) fault("NVS_SAVE_FAILED");
    else {
        ctl.settings=copy;
        ctl.state=UNKNOWN; ctl.travel_elapsed_ms=0;
        ctl.auto_home_after_save=go_home && !ctl.cancel_auto_home;
        ctl.cancel_auto_home=false;
        ctl.top_seen=false; /* recheck physical input with the committed polarity */
        ctl.request=R_NONE;
    }
    portEXIT_CRITICAL(&mux);
}
static void no_cache(httpd_req_t *req) {
    httpd_resp_set_hdr(req,"Cache-Control","no-store,max-age=0");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
}
extern const unsigned char control_html_start[] asm("_binary_control_html_start");
extern const unsigned char control_html_end[] asm("_binary_control_html_end");
static esp_err_t page(httpd_req_t *req) {
    no_cache(req);
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Content-Security-Policy",
      "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req,(const char *)control_html_start,
        control_html_end-control_html_start-1);
}
static esp_err_t json_resp(httpd_req_t *req,const char *status,const char *body) {
    no_cache(req); httpd_resp_set_type(req,"application/json");
    if (status) httpd_resp_set_status(req,status);
    return httpd_resp_sendstr(req,body);
}
static bool origin_ok(httpd_req_t *req) {
    char value[64];
    size_t len=httpd_req_get_hdr_value_len(req,"Origin");
    return len && len<sizeof(value) &&
        httpd_req_get_hdr_value_str(req,"Origin",value,sizeof(value))==ESP_OK &&
        strcmp(value,"http://192.168.4.1")==0;
}
static esp_err_t status_handler(httpd_req_t *req) {
    controller_t s;
    portENTER_CRITICAL(&mux);s=ctl;portEXIT_CRITICAL(&mux);
    char buf[680];
    bool running=s.motor_on;
    uint32_t at=s.state==CAL_DOWN||s.state==DOWN||s.state==HOMING ?
        clock_ms()-s.started_ms : s.travel_elapsed_ms;
    bool position_known=s.state==HOME_IDLE || s.state==CAL_READY ||
                        s.state==MIN_IDLE || s.state==DOWN || s.state==CAL_DOWN;
    int percent=s.state==HOME_IDLE||s.state==CAL_READY?0:
        s.state==MIN_IDLE?100:
        s.settings.minimum_ms && position_known ?
          (int)((uint64_t)at*100/s.settings.minimum_ms) : -1;
    if (percent>100) percent=100;
    bool snapshot_ready=s.hardware_ok&&s.nvs_ok&&s.wifi_ok&&s.config_verified&&
        s.top_seen&&(uint32_t)(s.now_ms-s.top_change_ms)>=DEBOUNCE_MS&&
        !s.fault_latched&&!s.save_pending&&!s.saving;
    int n=snprintf(buf,sizeof(buf),
        "{\"state\":\"%s\",\"fault\":\"%s\",\"top_active\":%s,"
        "\"min_ms\":%"PRIu32",\"elapsed_ms\":%"PRIu32",\"position_pct\":%d,"
        "\"motor\":\"%s\",\"settings\":{\"up_in1_high\":%u,"
        "\"home_active_level\":%u,\"saving\":%s},"
        "\"ready\":%s,\"wifi_ok\":%s,\"hardware_ok\":%s,"
        "\"calibrating\":%s,\"return_wait_ms\":%"PRIu32",\"uptime_ms\":%"PRIu32"}",
        state_name(s.state),s.reason,s.top_stable&&s.top_raw?"true":"false",
        s.settings.minimum_ms,at,percent,
        running?(s.direction_up?"UP":"DOWN"):"STOP",
        s.settings.up_in1_high,s.settings.home_active_level,
        s.saving||s.save_pending?"true":"false",
        snapshot_ready?"true":"false",
        s.wifi_ok?"true":"false",s.hardware_ok?"true":"false",
        s.state==CAL_DOWN||s.state==CAL_READY?"true":"false",
        s.state==MIN_IDLE && clock_ms()-s.at_min_ms<RETURN_WAIT_MS?
            RETURN_WAIT_MS-(clock_ms()-s.at_min_ms):0,clock_ms());
    if (n<0 || n>=(int)sizeof(buf))
        return json_resp(req,"500 Internal Server Error","{\"error\":\"overflow\"}");
    return json_resp(req,NULL,buf);
}
static esp_err_t command_handler(httpd_req_t *req) {
    if (!origin_ok(req))
        return json_resp(req,"403 Forbidden","{\"accepted\":false,\"error\":\"origin\"}");
    if (req->content_len)
        return json_resp(req,"400 Bad Request","{\"accepted\":false,\"error\":\"body\"}");
    request_t request=R_NONE;
    if (!strcmp(req->uri,"/api/home"))request=R_HOME;
    else if (!strcmp(req->uri,"/api/up"))request=R_UP;
    else if (!strcmp(req->uri,"/api/down"))request=R_DOWN;
    else if (!strcmp(req->uri,"/api/stop"))request=R_STOP;
    else if (!strcmp(req->uri,"/api/calibrate"))request=R_BEGIN_CAL;
    else if (!strcmp(req->uri,"/api/set-min"))request=R_SET_MIN;
    if (request==R_NONE)
        return json_resp(req,"404 Not Found","{\"accepted\":false}");
    bool accepted=false;
    portENTER_CRITICAL(&mux);
    if (request==R_STOP) {
        ctl.request=R_STOP;
        accepted=true;
    } else if (ready() && ctl.request==R_NONE) {
        state_t st=ctl.state;
        if ((request==R_HOME||request==R_UP) &&
            (st==UNKNOWN||st==MIN_IDLE||st==HOME_IDLE||st==CAL_READY))
            accepted=true;
        else if (request==R_BEGIN_CAL && st==HOME_IDLE)
            accepted=true;
        else if (request==R_DOWN &&
            (st==CAL_READY||(st==HOME_IDLE&&ctl.settings.minimum_ms)))
            accepted=true;
        else if (request==R_SET_MIN && st==CAL_DOWN)
            accepted=true;
        if (accepted)ctl.request=request;
    }
    portEXIT_CRITICAL(&mux);
    return accepted?json_resp(req,"202 Accepted","{\"accepted\":true}"):
        json_resp(req,"409 Conflict","{\"accepted\":false,\"error\":\"interlock\"}");
}
static esp_err_t settings_handler(httpd_req_t *req) {
    if (!origin_ok(req))
        return json_resp(req,"403 Forbidden","{\"accepted\":false,\"error\":\"origin\"}");
    if (req->content_len==0||req->content_len>40)
        return json_resp(req,"400 Bad Request","{\"accepted\":false,\"error\":\"length\"}");
    char buf[42]={0};int len=req->content_len,used=0;
    while (used<len) {
        int n=httpd_req_recv(req,buf+used,len-used);
        if (n<=0)return json_resp(req,"408 Request Timeout","{\"accepted\":false}");
        used+=n;
    }
    int direction=-1,level=-1;char trailing=0;
    if (sscanf(buf,"up_in1=%d&top_active=%d%c",&direction,&level,&trailing)!=2 ||
        direction<0||direction>1||level<0||level>1)
        return json_resp(req,"400 Bad Request","{\"accepted\":false,\"error\":\"settings\"}");
    bool accepted=false;
    portENTER_CRITICAL(&mux);
    if ((ctl.state==HOME_IDLE||ctl.state==UNKNOWN||ctl.state==FAULT) &&
        !ctl.motor_on&&!ctl.save_pending&&!ctl.saving&&ctl.nvs_ok) {
        ctl.staged=ctl.settings;
        ctl.staged.up_in1_high=(uint8_t)direction;
        ctl.staged.home_active_level=(uint8_t)level;
        if (direction!=ctl.settings.up_in1_high||
            level!=ctl.settings.home_active_level)
            ctl.staged.minimum_ms=0; /* changing mapping invalidates calibration */
        ctl.staged.checksum=settings_crc(&ctl.staged);
        ctl.save_go_home=false;ctl.cancel_auto_home=false;
        ctl.save_pending=true;accepted=true;
    }
    portEXIT_CRITICAL(&mux);
    return accepted?json_resp(req,"202 Accepted","{\"accepted\":true}"):
        json_resp(req,"409 Conflict","{\"accepted\":false,\"error\":\"busy\"}");
}
static void wifi_events(void *arg,esp_event_base_t base,int32_t event,void *data) {
    (void)arg;(void)base;(void)data;
    if (event==WIFI_EVENT_AP_STOP||event==WIFI_EVENT_AP_STADISCONNECTED) {
        portENTER_CRITICAL(&mux);
        if (event==WIFI_EVENT_AP_STOP)ctl.wifi_ok=false;
        if (ctl.motor_on||ctl.state==CAL_READY)ctl.request=R_STOP;
        portEXIT_CRITICAL(&mux);
    }
}
static esp_err_t start_wifi(void) {
    esp_err_t e=esp_netif_init();if(e!=ESP_OK)return e;
    e=esp_event_loop_create_default();if(e!=ESP_OK)return e;
    if (!esp_netif_create_default_wifi_ap())return ESP_FAIL;
    wifi_init_config_t wifi_init=WIFI_INIT_CONFIG_DEFAULT();
    e=esp_wifi_init(&wifi_init);if(e!=ESP_OK)return e;
    e=esp_wifi_set_storage(WIFI_STORAGE_RAM);if(e!=ESP_OK)return e;
    e=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_AP_STOP,wifi_events,NULL);
    if(e!=ESP_OK)return e;
    e=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_AP_STADISCONNECTED,wifi_events,NULL);
    if(e!=ESP_OK)return e;
    wifi_config_t ap={.ap={.authmode=WIFI_AUTH_WPA2_PSK,.channel=1,
        .max_connection=CONFIG_ELEVATOR_AP_MAX_CLIENTS}};
    size_t ssid=strlen(CONFIG_ELEVATOR_AP_SSID),password=strlen(CONFIG_ELEVATOR_AP_PASSWORD);
    if(!ssid||ssid>32||password<8||password>63)return ESP_ERR_INVALID_ARG;
    memcpy(ap.ap.ssid,CONFIG_ELEVATOR_AP_SSID,ssid);ap.ap.ssid_len=ssid;
    memcpy(ap.ap.password,CONFIG_ELEVATOR_AP_PASSWORD,password);
    e=esp_wifi_set_mode(WIFI_MODE_AP);if(e!=ESP_OK)return e;
    e=esp_wifi_set_config(WIFI_IF_AP,&ap);if(e!=ESP_OK)return e;
    e=esp_wifi_start();if(e!=ESP_OK)return e;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    config.stack_size=8192;config.max_uri_handlers=12;
    config.lru_purge_enable=true;
    httpd_handle_t server=NULL;
    e=httpd_start(&server,&config);if(e!=ESP_OK)return e;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=page},
        {.uri="/api/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/api/home",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/up",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/down",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/stop",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/calibrate",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/set-min",.method=HTTP_POST,.handler=command_handler},
        {.uri="/api/settings",.method=HTTP_POST,.handler=settings_handler}
    };
    for (size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        e=httpd_register_uri_handler(server,&routes[i]);
        if(e!=ESP_OK){httpd_stop(server);return e;}
    }
    return ESP_OK;
}
void app_main(void) {
    ctl=(controller_t){.state=UNKNOWN,.settings=defaults(),.now_ms=clock_ms()};
    /* Preload both latches LOW before activating pad output. */
    esp_err_t a=gpio_set_level(PIN_IN1,0);
    esp_err_t b=gpio_set_level(PIN_IN2,0);
    gpio_config_t out={.pin_bit_mask=(1ULL<<PIN_IN1)|(1ULL<<PIN_IN2),
       .mode=GPIO_MODE_OUTPUT,.pull_down_en=GPIO_PULLDOWN_ENABLE};
    esp_err_t c=gpio_config(&out);
    gpio_config_t in={.pin_bit_mask=1ULL<<PIN_TOP,.mode=GPIO_MODE_INPUT,
       .pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_DISABLE};
    esp_err_t d=gpio_config(&in);
    bool map_ok=PIN_IN1!=PIN_IN2 && PIN_IN1!=PIN_TOP && PIN_IN2!=PIN_TOP &&
        PIN_IN1!=2 && PIN_IN1!=8 && PIN_IN1!=9 &&
        PIN_IN2!=2 && PIN_IN2!=8 && PIN_IN2!=9 &&
        PIN_TOP!=2 && PIN_TOP!=8 && PIN_TOP!=9 &&
        PIN_IN1<12 && PIN_IN2<12 && PIN_TOP<12;
    ctl.hardware_ok=map_ok&&a==ESP_OK&&b==ESP_OK&&c==ESP_OK&&d==ESP_OK;
    if(!ctl.hardware_ok){ctl.fault_latched=true;ctl.state=FAULT;
        snprintf(ctl.reason,sizeof(ctl.reason),"GPIO_SETUP_FAILED");}
    esp_err_t n=nvs_flash_init();
    if(n==ESP_OK)n=nvs_open("elevator",NVS_READWRITE,&nvs);
    ctl.nvs_ok=n==ESP_OK;
    if(ctl.nvs_ok) {
        size_t len=sizeof(ctl.settings);
        esp_err_t r=nvs_get_blob(nvs,SETTINGS_KEY,&ctl.settings,&len);
        if(r==ESP_ERR_NVS_NOT_FOUND)ctl.settings=defaults();
        else if(r!=ESP_OK||len!=sizeof(ctl.settings)||!settings_valid(&ctl.settings)) {
            ctl.settings=defaults();fault("NVS_INVALID");
        }
    }else fault("NVS_UNAVAILABLE");
    ctl.now_ms=clock_ms();
    if(ctl.hardware_ok&&!brake())fault("GPIO_STOP_FAILED");
    /* Explicit verification gate: never arm a motor after unexplained runaway. */
#ifdef CONFIG_ELEVATOR_HARDWARE_VERIFIED
    ctl.config_verified=strcmp(CONFIG_ELEVATOR_AP_PASSWORD,"12345678")!=0;
#else
    ctl.config_verified=false;
#endif
    esp_err_t web=start_wifi();
    ctl.wifi_ok=web==ESP_OK;
    if(!ctl.wifi_ok)fault("WIFI_UNAVAILABLE");
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args={.callback=controller_tick,.name="elevator"};
    if(esp_timer_create(&args,&timer)!=ESP_OK||
       esp_timer_start_periodic(timer,PERIOD_MS*1000)!=ESP_OK) {
        fault("TIMER_FAILED");return;
    }
    ESP_LOGI(TAG,"AP %s; HOME GPIO%d (NC/GND), IN1 GPIO%d, IN2 GPIO%d",
        CONFIG_ELEVATOR_AP_SSID,PIN_TOP,PIN_IN1,PIN_IN2);
    if(!ctl.config_verified)ESP_LOGW(TAG,
        "Motor blocked: confirm hardware protection and set a non-default AP password");
    for(;;){flush_pending();vTaskDelay(pdMS_TO_TICKS(20));}
}
#endif
