#!/usr/bin/env python3
"""Reproducible software checks. No hardware is energized; test artifacts stay outside Git."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

CONTROL = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#define ELEVATOR_HOST_TEST
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
#define IRAM_ATTR
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
#define GPIO_IS_VALID_OUTPUT_GPIO(p) ((p)>=0 && (p)<=21)
#define CONFIG_ELEVATOR_LIMIT_TOP_GPIO 10
#define CONFIG_ELEVATOR_LIMIT_BOTTOM_GPIO 1
#define CONFIG_ELEVATOR_BUTTON_GPIO 20
#define CONFIG_ELEVATOR_MOTOR_IN1_GPIO 3
#define CONFIG_ELEVATOR_MOTOR_IN2_GPIO 4
#define CONFIG_ELEVATOR_STATUS_LED_GPIO 8
#define CONFIG_ELEVATOR_STATUS_LED_ENABLED 1
#define CONFIG_ELEVATOR_STATUS_LED_ACTIVE_LEVEL 0
#define CONFIG_ELEVATOR_MOTOR_UP_IN1_HIGH 1
#define CONFIG_ELEVATOR_LIMIT_TOP_ACTIVE_LEVEL 1
#define CONFIG_ELEVATOR_LIMIT_BOTTOM_ACTIVE_LEVEL 1
#define CONFIG_ELEVATOR_BUTTON_ACTIVE_LEVEL 0
#define CONFIG_ELEVATOR_AUTO_UP_WAIT_MS 10000
#ifndef CONFIG_ELEVATOR_MIN_UP_MS
#define CONFIG_ELEVATOR_MIN_UP_MS 1000
#endif
#define CONFIG_ELEVATOR_MIN_DOWN_MS 1000
#define CONFIG_ELEVATOR_MAX_UP_MS 5000
#define CONFIG_ELEVATOR_MAX_DOWN_MS 5000
#define CONFIG_ELEVATOR_START_RELEASE_MS 1500
#define CONFIG_ELEVATOR_REQUIRED_CYCLES 2
#define CONFIG_ELEVATOR_REVERSAL_MS 500
#define CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS 35
#define CONFIG_ELEVATOR_BUTTON_DEBOUNCE_MS 50
#define CONFIG_ELEVATOR_SAMPLE_MS 10
#define CONFIG_ELEVATOR_ARM_HOLD_MS 3000
#define CONFIG_ELEVATOR_AP_SSID "Elevador"
#define CONFIG_ELEVATOR_AP_PASSWORD "12345678"
#ifndef UNCONFIRMED
#define CONFIG_ELEVATOR_LIMITS_CONFIRMED 1
#endif
#ifdef TEST_SUPERVISED
#define CONFIG_ELEVATOR_SUPERVISED_TEST 1
#else
#define CONFIG_ELEVATOR_AUTOMATIC_ENABLED 1
#endif
static uint64_t simulated_ms;
static int levels[22], commands, gpio_calls[22];
static int fail_pin=-1, fail_level=-1, fail_skip, fail_remaining;
static int64_t esp_timer_get_time(void) { return simulated_ms * 1000; }
static int gpio_get_level(int pin) { return levels[pin]; }
static int gpio_set_level(int pin, int level)
{
    assert(pin != 21); /* Default map must not touch the formerly assigned pad. */
    ++gpio_calls[pin];
    if(pin==fail_pin && (fail_level<0 || level==fail_level) && fail_remaining) {
        if(fail_skip) --fail_skip;
        else {if(fail_remaining>0)--fail_remaining;return ESP_FAIL;}
    }
    if ((pin == 3 || pin == 4) && level) { assert(!levels[pin == 3 ? 4 : 3]); ++commands; }
    levels[pin] = level; return 0;
}
static uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *bytes, size_t n)
{
    uint32_t crc = ~seed;
    for (size_t i=0;i<n;i++) { crc ^= bytes[i]; for(int b=0;b<8;b++) crc = (crc>>1)^((crc&1)?0xedb88320u:0); }
    return ~crc;
}
#include ELEVATOR_SOURCE
static void advance(unsigned ms)
{
    assert(ms%10==0);
    for(unsigned t=0;t<ms;t+=10){simulated_ms+=10;control_tick(NULL);assert(!(levels[3]&&levels[4]));if(!ctl.gpio_write_failed)assert(ctl.drive_commanded==(bool)(levels[3]||levels[4]));}
}
static void edge(int pin, int value)
{
    if(levels[pin] != value) {
        levels[pin]=value;
        input_isr(pin==10?&ctl.top:pin==1?&ctl.bottom:&ctl.button);
    }
}
static void fresh(int top,int bottom,bool calibrated)
{
    memset(&ctl,0,sizeof(ctl));memset(levels,0,sizeof(levels));memset(gpio_calls,0,sizeof(gpio_calls));simulated_ms=0;commands=0;
    fail_pin=-1;fail_level=-1;fail_skip=0;fail_remaining=0;
    levels[10]=top;levels[1]=bottom;levels[20]=1;
    ctl=(controller_t){.state=BOOT,.pins_ready=true,.web_ok=true,.nvs_ok=true,.output_known={true,true,true},
        .top={.pin=10,.active_level=1,.raw_level=top},
        .bottom={.pin=1,.active_level=1,.raw_level=bottom},
        .button={.pin=20,.active_level=0,.raw_level=1}};
    ctl.cal=(calibration_t){.magic=CAL_MAGIC,.version=CAL_VERSION,.fingerprint=fingerprint(),
        .minimum_up_ms=calibrated?1200:0,.minimum_down_ms=calibrated?1200:0,
        .up_count=calibrated?2:0,.down_count=calibrated?2:0};
    ctl.cal.crc=calibration_crc(&ctl.cal);ctl.calibration_valid=calibration_complete();
    advance(60);
}
static void press(void) { edge(20,0);advance(70); }
static void release(void) { edge(20,1);advance(70); }
static void begin_down(void)
{
    fresh(1,0,true);advance(500);
#ifdef TEST_SUPERVISED
    press();advance(3000);assert(ctl.armed);assert(!moving());release();
#endif
    press();assert(ctl.state==MOVING_DOWN&&ctl.drive_commanded);
}
static void release_origin(void) { edge(10,0);advance(100);assert(ctl.origin_released); }
static void at_elapsed(unsigned t) { assert(simulated_ms-ctl.command_ms<=t);advance(t-(simulated_ms-ctl.command_ms)); }
static void arrive_bottom(void) { edge(1,1);advance(10);assert(!ctl.drive_commanded);advance(50); }
static int cases;
#define CASE(name) for(int once=(printf("TEST %s ... ",name),1);once;once=0,++cases,puts("PASS"))
int main(void)
{
    (void)TAG;(void)state_names;(void)fault_names;(void)fault_messages;
#ifdef TEST_BAD_CONFIG
    assert(!configuration_valid());puts("Invalid timing configuration rejected: PASS");return 0;
#endif
    CASE("valid configuration and reserved GPIO rejection") {
        assert(configuration_valid());assert(!valid_gpio(2));assert(!valid_gpio(8));assert(!valid_gpio(9));assert(!valid_gpio(18));assert(!valid_gpio(19));assert(!valid_gpio(12));assert(valid_gpio(20));
    }
    CASE("both limits fault stays latched") {
        fresh(1,1,true);assert(ctl.fault==BOTH_LIMITS_ACTIVE&&!ctl.drive_commanded);
        edge(1,0);advance(1000);assert(ctl.state==FAULT);assert(!motor_start(false));
    }
    CASE("unknown boot never homes") { fresh(0,0,true);advance(20000);assert(ctl.fault==UNKNOWN_POSITION_AT_BOOT&&commands==0); }
    CASE("missing calibration never starts automatically") { fresh(0,1,false);advance(20000);assert(ctl.state==NEEDS_COMMISSIONING&&commands==0); }
    CASE("CRC schema fingerprint and measured bounds") {
        fresh(1,0,true);assert(calibration_complete());calibration_t c=ctl.cal;
        c.crc^=1;assert(!calibration_sane(&c));c=ctl.cal;c.version++;c.crc=calibration_crc(&c);assert(!calibration_sane(&c));
        c=ctl.cal;c.fingerprint++;c.crc=calibration_crc(&c);assert(!calibration_sane(&c));
        c=ctl.cal;c.minimum_up_ms=999;c.crc=calibration_crc(&c);assert(!calibration_sane(&c));
    }
#ifndef TEST_SUPERVISED
#ifndef UNCONFIRMED
    CASE("exact 10000 ms automatic wait") {
        fresh(0,1,true);assert(ctl.state==WAIT_AUTO_UP);
        advance(9990-(simulated_ms-ctl.state_ms));assert(!ctl.drive_commanded);
        advance(10);assert(ctl.state==MOVING_UP&&ctl.drive_commanded);
    }
#endif
#endif
#ifdef UNCONFIRMED
    CASE("unconfirmed physical limits block all movement") {
        fresh(1,0,true);advance(1000);press();assert(!motor_start(false)&&!ctl.drive_commanded&&commands==0);
    }
    printf("%d cases passed\n",cases);return 0;
#endif
    CASE("failed output preserves last accepted value and counters") {
        fresh(1,0,true);uint32_t count=ctl.output_events[0];uint64_t changed=ctl.output_change_ms[0];
        fail_pin=3;fail_level=1;fail_remaining=-1;
        assert(!output_level(0,3,&ctl.motor_in1,1));
        assert(ctl.motor_in1==0&&!ctl.output_known[0]&&ctl.output_events[0]==count&&ctl.output_change_ms[0]==changed);
        assert(ctl.state==FAULT&&ctl.fault==GPIO_WRITE_FAILURE&&ctl.failed_gpio_pin==3&&ctl.gpio_error==ESP_FAIL);
    }
    CASE("pre-start brake failure attempts both pads and refuses start") {
        for(int pin=3;pin<=4;pin++){
            fresh(1,0,true);advance(500);
#ifdef TEST_SUPERVISED
            ctl.armed=true;
#endif
            fail_pin=pin;fail_level=0;fail_remaining=-1;int other=pin==3?4:3;int before=gpio_calls[other];
            assert(!motor_start(false)&&ctl.state==FAULT&&!ctl.drive_commanded&&commands==0);
            assert(gpio_calls[other]>before);
        }
    }
    CASE("directional write failure refuses start in either direction") {
        for(int up=0;up<=1;up++){
            fresh(!up,up,true);advance(500);
#ifdef TEST_SUPERVISED
            ctl.armed=true;
#endif
            fail_pin=up?3:4;fail_level=1;fail_remaining=-1;
            uint64_t before=ctl.command_ms;
            assert(!motor_start(up)&&ctl.state==FAULT&&!moving()&&!ctl.drive_commanded);
            assert(ctl.command_ms==before&&commands==0&&!levels[3]&&!levels[4]);
            fail_remaining=0;assert(!motor_start(up));advance(1000);assert(ctl.state==FAULT&&commands==0);
        }
    }
    CASE("partial start rolls back and never reports successful motion") {
        fresh(0,1,true);advance(500);
#ifdef TEST_SUPERVISED
        ctl.armed=true;
#endif
        fail_pin=4;fail_level=0;fail_skip=1;fail_remaining=-1;
        assert(!motor_start(true)&&ctl.state==FAULT&&!ctl.drive_commanded&&commands==1);
        assert(!levels[3]&&!levels[4]&&ctl.gpio_write_failed);
    }
    CASE("destination stop write failure latches unknown output and no calibration") {
        begin_down();release_origin();at_elapsed(1200);
        uint32_t cycles=ctl.cal.down_count;int before=commands;
        fail_pin=4;fail_level=0;fail_remaining=-1;edge(1,1);advance(60);
        assert(ctl.fault==GPIO_WRITE_FAILURE&&ctl.state==FAULT&&!ctl.drive_commanded);
        assert(levels[4]==1&&ctl.motor_in2==1&&!ctl.output_known[1]);
        assert(ctl.last_down_ms==0&&ctl.cal.down_count==cycles&&!ctl.calibration_dirty);
        advance(1000);assert(ctl.state==FAULT&&commands==before&&!motor_start(true));
    }
    CASE("failed first stop write still attempts second low write") {
        begin_down();fail_pin=3;fail_level=0;fail_remaining=-1;int before=gpio_calls[4];
        assert(!motor_brake()&&ctl.state==FAULT&&gpio_calls[4]==before+1);
        assert(!levels[4]&&!ctl.drive_commanded&&ctl.gpio_write_failed);
    }
    CASE("transient stop error remains latched after best effort succeeds") {
        begin_down();fail_pin=4;fail_level=0;fail_remaining=1;
        assert(!motor_brake()&&levels[4]==1);
        assert(!motor_brake()&&!levels[4]&&ctl.state==FAULT&&ctl.gpio_write_failed);
        advance(1000);assert(!motor_start(false)&&ctl.state==FAULT);
    }
    CASE("timeout braking error cannot return to normal operation") {
        begin_down();release_origin();fail_pin=4;fail_level=0;fail_remaining=-1;at_elapsed(5010);
        assert(ctl.state==FAULT&&ctl.gpio_write_failed&&!ctl.drive_commanded&&levels[4]==1);
    }
    CASE("LED write error preserves metadata and attempts motor braking") {
        begin_down();int previous=ctl.led_level;uint32_t count=ctl.output_events[2];
        fail_pin=8;fail_level=-1;fail_remaining=-1;ctl.now_ms+=250;update_led();
        assert(ctl.state==FAULT&&!ctl.drive_commanded&&!levels[3]&&!levels[4]);
        assert(ctl.led_level==previous&&ctl.output_events[2]==count&&!ctl.output_known[2]);
    }
    CASE("raw destination stops before debounce; valid finish") {
        begin_down();release_origin();at_elapsed(1200);arrive_bottom();assert(ctl.fault==NONE&&!moving());assert(ctl.last_down_ms==1210);
#ifdef TEST_SUPERVISED
        assert(ctl.calibration_dirty&&ctl.cal.down_count==3&&!ctl.calibration_valid);
#endif
    }
    CASE("destination too fast; no invalid NVS measurement") {
        begin_down();release_origin();at_elapsed(500);uint32_t n=ctl.cal.down_count;arrive_bottom();
        assert(ctl.fault==TRAVEL_TOO_FAST&&!ctl.drive_commanded&&ctl.cal.down_count==n&&!ctl.calibration_dirty);
    }
    CASE("travel timeout") { begin_down();release_origin();at_elapsed(5010);assert(ctl.fault==TRAVEL_TIMEOUT&&!ctl.drive_commanded); }
    CASE("origin release deadline") { begin_down();at_elapsed(1500);assert(ctl.fault==START_LIMIT_NOT_RELEASED&&!ctl.drive_commanded); }
    CASE("origin reassertion fails immediately") { begin_down();release_origin();edge(10,1);advance(10);assert(ctl.fault==LIMIT_SENSOR_INCONSISTENT&&!ctl.drive_commanded); }
    CASE("bounce destination never reenergizes") {
        begin_down();release_origin();at_elapsed(1200);edge(1,1);advance(10);int count=commands;
        edge(1,0);advance(10);edge(1,1);advance(10);edge(1,0);advance(100);
        assert(ctl.fault==LIMIT_SENSOR_INCONSISTENT&&commands==count&&!ctl.drive_commanded);
    }
    CASE("button bounce one command; hold and transit press ignored") {
        fresh(1,0,true);advance(500);
#ifdef TEST_SUPERVISED
        press();advance(3000);release();
#endif
        for(int i=0;i<3;i++){edge(20,0);advance(10);edge(20,1);advance(10);}
        assert(!moving());press();assert(commands==1);release_origin();advance(600);assert(commands==1);
        release();press();assert(ctl.ignored_presses==1&&ctl.state==MOVING_DOWN&&commands==1);
    }
    CASE("stationary endpoint loss is latched") { fresh(1,0,true);edge(10,0);advance(100);assert(ctl.fault==UNEXPECTED_LIMIT&&commands==0); }
    CASE("reversal interval interlock") {
        fresh(1,0,true);advance(500);
#ifdef TEST_SUPERVISED
        ctl.armed=true;
#endif
        ctl.stopped_ms=simulated_ms;assert(!motor_start(false));advance(500);assert(motor_start(false));
    }
    CASE("NVS and AP failures inhibit start") {
        fresh(1,0,true);advance(500);ctl.nvs_ok=false;assert(!motor_start(false));ctl.nvs_ok=true;ctl.web_ok=false;assert(!motor_start(false));
    }
    CASE("both active during movement stops") { begin_down();edge(1,1);advance(60);assert(ctl.fault==BOTH_LIMITS_ACTIVE&&!ctl.drive_commanded); }
    CASE("restart in transit never resumes") { begin_down();release_origin();fresh(0,0,true);assert(ctl.state==FAULT&&commands==0); }
    CASE("late but within maximum arrival accepted after debounce") {
        begin_down();release_origin();at_elapsed(4970);arrive_bottom();assert(ctl.fault==NONE&&ctl.last_down_ms==4980);
    }
    CASE("upward trip and upper destination") {
        fresh(0,1,true);advance(500);
#ifdef TEST_SUPERVISED
        ctl.armed=true;assert(motor_start(true));
#else
        advance(10000-(simulated_ms-ctl.state_ms));
#endif
        assert(ctl.state==MOVING_UP&&levels[3]==1&&levels[4]==0);
        edge(1,0);advance(100);assert(ctl.origin_released);
        at_elapsed(1200);edge(10,1);advance(10);assert(!ctl.drive_commanded&&levels[3]==0&&levels[4]==0);
        advance(50);assert(ctl.fault==NONE&&ctl.last_up_ms==1210);
    }
    CASE("upward timeout and early upper endpoint") {
        for(int early=0;early<2;early++) {
            fresh(0,1,true);advance(500);
#ifdef TEST_SUPERVISED
            ctl.armed=true;assert(motor_start(true));
#else
            advance(10000-(simulated_ms-ctl.state_ms));
#endif
            edge(1,0);advance(100);
            if(early){at_elapsed(500);edge(10,1);advance(60);assert(ctl.fault==TRAVEL_TOO_FAST);}
            else{at_elapsed(5010);assert(ctl.fault==TRAVEL_TIMEOUT);}
            assert(!levels[3]&&!levels[4]);
        }
    }
    CASE("active-low input debounce and single press event") {
        fresh(1,0,true);input_t in={.pin=6,.active_level=0,.raw_level=1,.initialized=true};
        levels[6]=0;input_isr(&in);assert(!sample(&in,50));ctl.now_ms+=49;assert(!sample(&in,50));
        ctl.now_ms+=1;assert(sample(&in,50));assert(in.debounced_active&&in.events==1);
        ctl.now_ms+=500;assert(!sample(&in,50)&&in.events==1);
    }
    CASE("pulse entirely between samples stops destination") {
        begin_down();release_origin();at_elapsed(1200);edge(1,1);edge(1,0);
        advance(10);assert(!ctl.drive_commanded&&ctl.destination_pending);
        advance(50);assert(ctl.fault==LIMIT_SENSOR_INCONSISTENT);
    }
    CASE("held button at boot cannot start or arm") {
        fresh(1,0,true);ctl.button.initialized=false;edge(20,0);ctl.state=BOOT;ctl.initialized=false;
        advance(5000);assert(!ctl.armed&&!moving()&&commands==0);release();
    }
    CASE("swapping stationary endpoints is inconsistent") {
        fresh(1,0,true);edge(10,0);edge(1,1);advance(100);assert(ctl.fault==UNEXPECTED_LIMIT);
    }
    CASE("top without calibration remains idle and cannot descend") {
        fresh(1,0,false);assert(ctl.state==IDLE_TOP);advance(500);press();assert(!moving()&&commands==0);
    }
    CASE("LED fault and movement patterns") {
        begin_down();ctl.now_ms=500;update_led();assert(ctl.led_level==0);ctl.now_ms=750;update_led();assert(ctl.led_level==1);
        fail(TRAVEL_TIMEOUT);ctl.now_ms=1600;update_led();assert(ctl.led_level==0);ctl.now_ms=2000;update_led();assert(ctl.led_level==1);
    }
#ifdef TEST_SUPERVISED
    CASE("supervised hold arms only; release and new press required") {
        fresh(0,1,false);press();advance(2990-(simulated_ms-ctl.button_pressed_ms));assert(!ctl.armed&&!moving());advance(20);assert(ctl.armed&&!moving());
        advance(5000);assert(commands==0);release();press();assert(ctl.state==MOVING_UP&&commands==1);
    }
#endif
    printf("%d cases passed\n",cases);return 0;
}
'''

PLATFORM_STUB = r'''

#define main control_tests_main
#include "control.txt"
#undef main
#define CONFIG_ELEVATOR_WEB_REFRESH_MS 350
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct { char body[32768], type[80], status[80]; bool no_cache, csp; } httpd_req_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READWRITE 1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
static int nvsmode;
static calibration_t stored, staged;
static unsigned writes, commits;
static int nvs_flash_init(void) { return nvsmode==3?99:0; }
static int nvs_open(const char *n,int mode,nvs_handle_t *h) {(void)n;(void)mode;*h=1;return nvsmode==5?99:0;}
static int nvs_get_blob(nvs_handle_t h,const char *n,void *out,size_t *size) {
 (void)h;(void)n;if(nvsmode==0)return ESP_ERR_NVS_NOT_FOUND;
 if(nvsmode==4)return 99;
 memcpy(out,&stored,sizeof(stored));*size=sizeof(stored);return 0;
}

static int nvs_set_blob(nvs_handle_t h,const char *n,const void *value,size_t size) {
 (void)h;(void)n;++writes;assert(size==sizeof(staged));
 if(nvsmode==6)return 99;
 memcpy(&staged,value,size);return 0;
}
static int nvs_commit(nvs_handle_t h) {
 (void)h;++commits;if(nvsmode==7)return 99;stored=staged;return 0;
}
static int httpd_resp_set_hdr(httpd_req_t *r,const char *n,const char *v) {if(!strcmp(n,"Cache-Control")&&!strcmp(v,"no-store, max-age=0"))r->no_cache=true;if(!strcmp(n,"Content-Security-Policy"))r->csp=true;return 0;}
static int httpd_resp_set_type(httpd_req_t *r,const char *s) {snprintf(r->type,sizeof(r->type),"%s",s);return 0;}
static int httpd_resp_set_status(httpd_req_t *r,const char *s) {snprintf(r->status,sizeof(r->status),"%s",s);return 0;}
static int httpd_resp_send(httpd_req_t *r,const char *s,int n) {assert(n>=0 && (size_t)n<sizeof(r->body));memcpy(r->body,s,n);r->body[n]=0;return 0;}
static int httpd_resp_sendstr(httpd_req_t *r,const char *s) {return httpd_resp_send(r,s,strlen(s));}
static int httpd_resp_send_err(httpd_req_t *r,int code,const char *s) {(void)r;(void)s;return code;}

#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static unsigned configured, isrs, service;
static int gpio_install_isr_service(int flags){assert(flags==0);++service;return 0;}
static int gpio_config(const gpio_config_t *c) {
 assert(!(c->pin_bit_mask & (1ULL<<21)));
 if(c->mode==GPIO_MODE_OUTPUT && (c->pin_bit_mask&((1ULL<<3)|(1ULL<<4)))) {
   assert(!levels[3]&&!levels[4]);assert(c->pull_down_en==1&&c->pull_up_en==0);
 }else if(c->mode==GPIO_MODE_INPUT){assert(c->pull_up_en==1&&c->intr_type==GPIO_INTR_ANYEDGE);}
 ++configured;return 0;
}
static int gpio_isr_handler_add(int pin,void (*handler)(void*),void *arg){
 assert(pin==1||pin==10||pin==20);assert(handler==input_isr);assert(arg);++isrs;return 0;
}
'''

PLATFORM_TESTS = r'''

int main(void) {
 fresh(1,0,true);levels[3]=levels[4]=1;assert(initialize_gpio());assert(configured==6&&isrs==3&&service==1);
 assert(!levels[3]&&!levels[4]);puts("PASS gpio initialization, both low latches, pulls, ANYEDGE, no GPIO21");
 for(int pin=3;pin<=4;pin++){
   fresh(1,0,true);ctl.pins_ready=false;fail_pin=pin;fail_level=0;fail_remaining=-1;
   assert(!initialize_gpio()&&ctl.fault==GPIO_WRITE_FAILURE&&!ctl.pins_ready);
 }
 puts("PASS GPIO preload write failures refuse initialization");
 fresh(1,0,true);
 stored=ctl.cal;nvsmode=1;ctl.calibration_valid=false;assert(load_calibration()&&ctl.calibration_valid);
 nvsmode=0;assert(load_calibration()&&!ctl.calibration_valid&&ctl.cal.up_count==0);
 nvsmode=1;stored.crc^=1;assert(!load_calibration()&&!ctl.calibration_valid);stored.crc^=1;
 stored.version=1;stored.crc=calibration_crc(&stored);assert(!load_calibration());stored.version=CAL_VERSION;stored.crc=calibration_crc(&stored);
 nvsmode=3;assert(!load_calibration());nvsmode=4;assert(!load_calibration());nvsmode=5;assert(!load_calibration());
 puts("PASS NVS valid/absent/corrupt/legacy/init/open/read");
 nvsmode=1;fresh(1,0,true);ctl.calibration_dirty=true;ctl.calibration_valid=false;
 save_calibration();assert(writes==1&&commits==1&&!ctl.calibration_dirty&&ctl.calibration_valid);
 fresh(1,0,false);assert(load_calibration()&&ctl.calibration_valid&&ctl.cal.up_count==2);
 puts("PASS NVS set_blob/commit/reload restores only validated calibration");
 for(int error=6;error<=7;error++){
   fresh(1,0,true);ctl.calibration_dirty=true;ctl.calibration_valid=false;nvsmode=error;save_calibration();
   assert(ctl.fault==NVS_OR_CALIBRATION_INVALID&&!ctl.nvs_ok&&!ctl.drive_commanded);
   assert(!levels[3]&&!levels[4]&&!motor_start(false));
 }
 nvsmode=1;begin_down();ctl.calibration_dirty=true;unsigned before=writes;save_calibration();assert(writes==before);
 puts("PASS NVS write/commit failure latches brake; no writes while moving");
 fresh(1,0,true);httpd_req_t r={0};assert(status_handler(&r)==0);
 assert(r.no_cache&&!strcmp(r.type,"application/json"));FILE *f=fopen("status-fixture.json","w");assert(f);fputs(r.body,f);fclose(f);
 memset(&r,0,sizeof(r));assert(health_handler(&r)==0);assert(!strcmp(r.body,"{\"healthy\":true}"));
 fail(TRAVEL_TIMEOUT);memset(&r,0,sizeof(r));assert(status_handler(&r)==0);f=fopen("fault-fixture.json","w");assert(f);fputs(r.body,f);fclose(f);
 memset(&r,0,sizeof(r));assert(health_handler(&r)==0);assert(!strcmp(r.status,"503 Service Unavailable"));
 begin_down();fail_pin=4;fail_level=0;fail_remaining=-1;assert(!motor_brake()&&levels[4]==1);
 memset(&r,0,sizeof(r));assert(status_handler(&r)==0);f=fopen("gpio-fault-fixture.json","w");assert(f);fputs(r.body,f);fclose(f);
 memset(&r,0,sizeof(r));assert(health_handler(&r)==0);assert(!strcmp(r.status,"503 Service Unavailable"));
 puts("PASS GPIO failure status/health reports unknown command, not confirmed braking");
 memset(&r,0,sizeof(r));assert(index_handler(&r)==0);assert(!strcmp(r.type,"text/html; charset=utf-8")&&r.no_cache&&r.csp);
 assert(strlen(r.body)==(size_t)(index_html_end-index_html_start-1));assert(strstr(r.body,"IN1=IN2=0"));
 memset(&r,0,sizeof(r));assert(root_control_handler(&r)==0);assert(!strcmp(r.type,"text/html; charset=utf-8")&&r.no_cache&&r.csp);
 assert(strlen(r.body)==(size_t)(control_html_end-control_html_start-1));
 assert(strstr(r.body,"id=\"up\"")&&strstr(r.body,"id=\"down\"")&&strstr(r.body,"id=\"stop\""));
 assert(strstr(r.body,"isDemo=location.protocol==='file:'"));
 json_t j={0};char big[4000];memset(big,'x',sizeof(big)-1);big[sizeof(big)-1]=0;json_add(&j,"%s",big);assert(j.failed);
 json_add(&j,"ignored");assert(j.failed);
 puts("PASS GET /, /dashboard, /api/status, /api/health 200/503, no-store, CSP, bounded JSON");
 return 0;
}
'''

BROWSER = r'''
const {chromium}=require('playwright');
const fs=require('fs');
const assert=require('assert');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM_PATH||'/usr/bin/chromium',headless:true,args:['--no-sandbox']});
 const errors=[];
 const page=await browser.newPage({viewport:{width:360,height:800}});
 page.on('pageerror',e=>errors.push(e.message));
 const remote=[];
 page.on('request',r=>{if(!r.url().startsWith('http://demo.test/'))remote.push(r.url());});
 await page.route('http://demo.test/**',route=>route.fulfill({contentType:'text/html',body:fs.readFileSync(process.env.ELEVATOR_HTML,'utf8').replace("const demo=location.protocol==='file:';","const demo=true;")}));
 await page.goto('http://demo.test/');
 await page.waitForFunction(()=>document.querySelector('#connection').textContent==='DEMONSTRAÇÃO LOCAL');
 assert((await page.locator('#source').textContent()).includes('SIMULADOS')||(await page.locator('#source').textContent()).includes('simulados'));
 for(const width of [320,360,430,1280]){
  await page.setViewportSize({width,height:800});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'Horizontal overflow at '+width);
  await page.screenshot({path:'demo-'+width+'.png',fullPage:true});
 }
 assert.equal(await page.locator('.gpio').count(),6);
 assert.equal(remote.length,0);
 assert.equal(await page.locator('#gpio-motor_enable').count(),0);
 assert((await page.locator('footer').textContent()).includes('frenagem elétrica'));
 assert.equal(errors.length,0);
 console.log('PASS demo flag simulated (file:// blocked by browser policy), 6 GPIOs, no remote requests, no JS errors, 320/360/430/1280 px without overflow');
 await page.close();
 const live=await browser.newPage({viewport:{width:360,height:800}});
 live.on('pageerror',e=>errors.push(e.message));
 let failure=false;
 const fixture=JSON.parse(fs.readFileSync('status-fixture.json','utf8'));
 fixture.gpio.limit_top.active=true;fixture.gpio.limit_top.raw=0;fixture.gpio.limit_top.raw_active=false;fixture.gpio.limit_top.events=7;
 fixture.state='IDLE_TOP';fixture.position='TOP';fixture.motion='STOPPED';
 await live.route('http://elevator.test/**',async route=>{
  const url=route.request().url();
  if(url.endsWith('/api/status')){
   if(failure)return route.abort();
   return route.fulfill({contentType:'application/json',body:JSON.stringify(fixture)});
  }
  return route.fulfill({contentType:'text/html',body:fs.readFileSync(process.env.ELEVATOR_HTML,'utf8')});
 });
 await live.goto('http://elevator.test/dashboard');
 await live.waitForFunction(()=>document.querySelector('#connection').textContent==='API CONECTADA');
 assert(await live.locator('#gpio-limit_top').evaluate(e=>e.classList.contains('active')));
 assert((await live.locator('#gpio-limit_top small').textContent()).includes('Bruto 0'));
 assert((await live.locator('#updated').textContent()).includes('DADOS REAIS'));
 assert((await live.locator('#braking').textContent()).includes('frenagem elétrica'));
 failure=true;
 await live.waitForFunction(()=>document.querySelector('#connection').textContent==='SEM TELEMETRIA');
 assert.equal(await live.locator('.gpio.active').count(),0);
 assert.equal(await live.locator('#state').textContent(),'SEM TELEMETRIA');
 assert.equal(await live.locator('#cab').textContent(),'SEM DADOS');
 assert.equal(errors.length,0);
 console.log('PASS mocked API integration, raw/logical distinction, offline neutralization, no JS errors');
 // Text injection remains text.
 failure=false;fixture.error={code:'FAULT',message:'<img src=x onerror=alert(1)>',state:'BOOT',time_ms:0};fixture.state='FAULT';
 await live.waitForFunction(()=>document.querySelector('#state').textContent==='FALHA');
 assert.equal(await live.locator('#error img').count(),0);
 assert((await live.locator('#error').textContent()).includes('<img'));
 await live.emulateMedia({reducedMotion:'reduce'});
 assert.equal(await live.locator('#cab').evaluate(e=>getComputedStyle(e).transitionDuration),'0s');
 fixture.gpio.onboard_led.enabled=false;fixture.gpio.onboard_led.write_ok=false;
 await live.waitForFunction(()=>document.querySelector('#gpio-onboard_led b').textContent.includes('DESABILITADO'));
 fixture.state='INVALID';await live.waitForFunction(()=>document.querySelector('#state').textContent==='SEM TELEMETRIA');assert.equal(await live.locator('.gpio.active').count(),0);
 console.log('PASS escaped dynamic text, reduced-motion, malformed API rejected');
 Object.assign(fixture,JSON.parse(fs.readFileSync('gpio-fault-fixture.json','utf8')));
 await live.waitForFunction(()=>document.querySelector('#cab').textContent==='NÃO CONFIRMADO');
 assert((await live.locator('#braking').textContent()).includes('Comando GPIO não confirmado'));
 for(const key of ['motor_in1','motor_in2']){
   assert((await live.locator('#gpio-'+key+' b').textContent()).includes('NÃO CONFIRMADO'));
   assert(!(await live.locator('#gpio-'+key).evaluate(e=>e.classList.contains('active'))));
 }
 console.log('PASS GPIO failure UI: no confirmed movement/braking, unknown outputs, no active glow');

 // Distinct root control page: buttons must stay disabled when live.
 fixture.state='FAULT';
 fixture.error={code:'BOTH_LIMITS_ACTIVE',message:'Ambos os fins ativos',state:'BOOT',time_ms:368};
 const ctrl=await browser.newPage({viewport:{width:360,height:800}});
 ctrl.on('pageerror',e=>errors.push(e.message));
 const controlHtml=fs.readFileSync(process.env.ELEVATOR_CONTROL_HTML,'utf8');
 await ctrl.route('http://control.test/**',route=>{
  if(route.request().url().endsWith('/api/status'))
   return route.fulfill({contentType:'application/json',body:JSON.stringify(fixture)});
  return route.fulfill({contentType:'text/html',body:controlHtml});
 });
 await ctrl.goto('http://control.test/');
 await ctrl.waitForFunction(()=>document.querySelector('#connection').textContent==='API CONECTADA');
 for(const id of ['up','down','stop'])assert(await ctrl.locator('#'+id).isDisabled());
 assert.equal(await ctrl.locator('a[href="/dashboard"]').count(),1);
 assert(await ctrl.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
 await ctrl.close();
 // In the browser test only, force local-demo detection; verify no network motor commands.
 const local=await browser.newPage({viewport:{width:320,height:800}});
 local.on('pageerror',e=>errors.push(e.message));
 await local.route('http://local.test/**',route=>route.fulfill({contentType:'text/html',body:controlHtml.replace("const isDemo=location.protocol==='file:';","const isDemo=true;")}));
 await local.goto('http://local.test/');
 await local.waitForFunction(()=>document.querySelector('#connection').textContent==='DEMONSTRAÇÃO');
 for(const id of ['up','down','stop'])assert(await local.locator('#'+id).isEnabled());
 await local.locator('#up').click();await local.waitForFunction(()=>document.querySelector('#status').textContent==='SUBINDO');
 await local.locator('#stop').click();
 assert.equal(await local.locator('#status').textContent(),'NA BASE');
 assert.equal(errors.length,0);
 await local.close();
 console.log('PASS root-only controls, disabled on device, simulated in local demo, 320/360px responsive');
 await browser.close();
})().catch(e=>{console.error(e);process.exit(1)});
'''

def run(args, output, name, env=None):
    result = subprocess.run(args, cwd=output, env=env, capture_output=True, text=True)
    log = result.stdout + result.stderr
    (output / (name + ".log")).write_text(log)
    if result.returncode:
        raise RuntimeError(f"{name} failed ({result.returncode}):\n{log}")
    print(log.strip() or f"PASS {name}")
    return log

def compile_c(source, target, output, flags=(), assembly=None):
    args = [os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f'-DELEVATOR_SOURCE="{ROOT / "main/main.c"}"', *flags, "-x", "c", str(source)]
    if assembly:
        args += ["-x", "assembler", str(assembly)]
    run([*args, "-o", str(target)], output, target.name + "-compile")

def validate(output, browser):
    source = (ROOT / "main/main.c").read_text()
    html = (ROOT / "main/index.html").read_text()
    kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
    removed = r"ENABLE_PIN|ELEVATOR_MOTOR_ENABLE_GPIO|motor_enable|GPIO_MOTOR_ENABLE"
    if re.search(removed, source + html + kconfig):
        raise AssertionError("Removed motor pad still exists")
    assert sorted(p.name for p in (ROOT / "main").glob("*.c")) == ["main.c"]
    assert 'EMBED_TXTFILES "index.html" "control.html"' in (ROOT / "main/CMakeLists.txt").read_text()
    assert source.count(".uri=") == 4 and source.count(".method=HTTP_GET") == 4 and "xTaskCreate" not in source
    assert '.uri="/dashboard"' in source and '.uri="/"' in source
    assert '.method=HTTP_POST' not in source
    root_page=(ROOT / "main/control.html").read_text()
    assert all('id="' + x + '"' in root_page for x in ('up','down','stop'))
    assert "if(!isDemo)return;" in root_page
    assert "for(const id of ['up','down','stop'])$(id).disabled=true;" in root_page
    assert "default 3" in kconfig and "default 4" in kconfig
    print("PASS minimal architecture and removal regression")
    control = output / "control.txt"
    control.write_text(CONTROL)
    for name, flags in [
        ("automatic", []), ("supervised", ["-DTEST_SUPERVISED"]),
        ("unconfirmed", ["-DUNCONFIRMED"]),
        ("invalid-timing", ["-DTEST_BAD_CONFIG", "-DCONFIG_ELEVATOR_MIN_UP_MS=0"]),
    ]:
        target = output / name
        compile_c(control, target, output, flags)
        run([str(target)], output, name)
    variants = {
        "duplicate": ("#define CONFIG_ELEVATOR_MOTOR_IN1_GPIO 3", "#define CONFIG_ELEVATOR_MOTOR_IN1_GPIO 4"),
        "strapping": ("#define CONFIG_ELEVATOR_BUTTON_GPIO 20", "#define CONFIG_ELEVATOR_BUTTON_GPIO 9"),
        "usb": ("#define CONFIG_ELEVATOR_BUTTON_GPIO 20", "#define CONFIG_ELEVATOR_BUTTON_GPIO 18"),
        "flash": ("#define CONFIG_ELEVATOR_BUTTON_GPIO 20", "#define CONFIG_ELEVATOR_BUTTON_GPIO 12"),
        "unsupported": ("#define CONFIG_ELEVATOR_BUTTON_GPIO 20", "#define CONFIG_ELEVATOR_BUTTON_GPIO 25"),
        "password": ('#define CONFIG_ELEVATOR_AP_PASSWORD "12345678"', '#define CONFIG_ELEVATOR_AP_PASSWORD "short"'),
        "timing-order": ("#define CONFIG_ELEVATOR_MAX_UP_MS 5000", "#define CONFIG_ELEVATOR_MAX_UP_MS 1000"),
        "release-deadline": ("#define CONFIG_ELEVATOR_START_RELEASE_MS 1500", "#define CONFIG_ELEVATOR_START_RELEASE_MS 6000"),
        "uart-default": ("#define ELEVATOR_HOST_TEST", "#define ELEVATOR_HOST_TEST\n#define CONFIG_ESP_CONSOLE_UART 1\n#define CONFIG_ESP_CONSOLE_UART_DEFAULT 1"),
        "uart-custom": ("#define ELEVATOR_HOST_TEST", "#define ELEVATOR_HOST_TEST\n#define CONFIG_ESP_CONSOLE_UART 1\n#define CONFIG_ESP_CONSOLE_UART_TX_GPIO 3\n#define CONFIG_ESP_CONSOLE_UART_RX_GPIO 20"),
        "incompatible-modes": ("#define CONFIG_ELEVATOR_AUTOMATIC_ENABLED 1", "#define CONFIG_ELEVATOR_AUTOMATIC_ENABLED 1\n#define CONFIG_ELEVATOR_SUPERVISED_TEST 1"),
    }
    for name, (before, after) in variants.items():
        assert before in CONTROL
        variant = output / "variant.txt"
        variant.write_text(CONTROL.replace(before, after))
        target = output / "variant"
        compile_c(variant, target, output, ["-DTEST_BAD_CONFIG"])
        run([str(target)], output, "config-" + name)
    init = source[source.index("static bool initialize_gpio"):source.index("static nvs_handle_t cal_handle;")]
    platform = source[source.index("static nvs_handle_t cal_handle;"):source.index("static void wifi_event")]
    integration = output / "integration.txt"
    integration.write_text(PLATFORM_STUB + init + platform + PLATFORM_TESTS)
    assembly = output / "embedded.s"
    # Test both HTML handlers with symbols identical to ESP-IDF text embedding.
    assembly.write_text('.section .rodata\n.global _binary_index_html_start\n.global _binary_index_html_end\n'
                        '_binary_index_html_start:\n.incbin "' + str(ROOT / "main/index.html") +
                        '"\n.byte 0\n_binary_index_html_end:\n'
                        '.global _binary_control_html_start\n.global _binary_control_html_end\n'
                        '_binary_control_html_start:\n.incbin "' + str(ROOT / "main/control.html") +
                        '"\n.byte 0\n_binary_control_html_end:\n.section .note.GNU-stack,"",@progbits\n')
    target = output / "integration"
    compile_c(integration, target, output, assembly=assembly)
    run([str(target)], output, "gpio-nvs-http")
    status = json.loads((output / "status-fixture.json").read_text())
    fault = json.loads((output / "fault-fixture.json").read_text())
    expected = {"limit_top", "limit_bottom", "button", "motor_in1", "motor_in2", "onboard_led"}
    assert set(status["gpio"]) == expected and set(fault["gpio"]) == expected
    assert status["motor"]["brake_commanded"] is True
    assert status["motor"]["independent_shutdown"] is False
    assert fault["error"]["code"] == "TRAVEL_TIMEOUT" and fault["state"] == "FAULT"
    assert fault["gpio"]["motor_in1"]["level"] == fault["gpio"]["motor_in2"]["level"] == 0
    failed = json.loads((output / "gpio-fault-fixture.json").read_text())
    assert failed["state"] == "FAULT" and failed["motion"] == "UNKNOWN"
    assert failed["motor"]["command"] == "UNKNOWN" and failed["motor"]["brake_commanded"] is False
    assert failed["error"]["gpio_write_failed"] and failed["error"]["gpio_pin"] == 4 and failed["error"]["gpio_error"] == -1
    for key in ("motor_in1", "motor_in2"):
        assert failed["gpio"][key]["level"] is None and not failed["gpio"][key]["active"] and not failed["gpio"][key]["write_ok"]
    print("PASS parsed API fixtures: six real GPIOs, brake semantics, no independent shutdown")
    if browser:
        test = output / "browser.cjs"
        test.write_text(BROWSER)
        env = dict(os.environ, ELEVATOR_HTML=str(ROOT / "main/index.html"), ELEVATOR_CONTROL_HTML=str(ROOT / "main/control.html"))
        run(["node", str(test)], output, "browser", env)
    else:
        print("NOT RUN browser checks (use --browser)")
    (output / "summary.txt").write_text(
        "PASS requested software checks; browser=" + str(browser) +
        "\nPhysical tests NOT RUN. GPIO/time/NVS/HTTP use mocks; browser uses intercepted responses.\n"
        "ESP-IDF build is a separate required command. file:// demo selection simulated.\n")
    print("Software checks completed. Physical tests NOT RUN.")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", action="store_true")
    parser.add_argument("--output", type=Path, help="New directory outside the checkout for evidence")
    args = parser.parse_args()
    if args.output:
        output = args.output.resolve()
        if output == ROOT or ROOT in output.parents:
            parser.error("--output must be outside the checkout")
        output.mkdir(parents=True, exist_ok=False)
        validate(output, args.browser)
        print("Evidence:", output)
    else:
        with tempfile.TemporaryDirectory(prefix="elevador-validation-") as directory:
            validate(Path(directory), args.browser)

if __name__ == "__main__":
    main()
