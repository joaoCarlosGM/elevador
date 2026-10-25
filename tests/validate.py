#!/usr/bin/env python3
"""Compile the real controller source with host GPIO/clock mocks; no motor or PCB is powered."""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/"main/main.c").read_text(encoding="utf-8")
HTML=(ROOT/"main/control.html").read_text(encoding="utf-8")
CONFIG=(ROOT/"main/Kconfig.projbuild").read_text(encoding="utf-8")
CMAKE=(ROOT/"main/CMakeLists.txt").read_text(encoding="utf-8")

assert 'EMBED_TXTFILES "control.html"' in CMAKE
assert "index.html" not in CMAKE
assert "#define PIN_TOP CONFIG_ELEVATOR_HOME_GPIO" in SOURCE
assert "PIN_IN1 CONFIG_ELEVATOR_MOTOR_IN1_GPIO" in SOURCE
assert "PIN_IN2 CONFIG_ELEVATOR_MOTOR_IN2_GPIO" in SOURCE
assert "LIMIT_BOTTOM" not in SOURCE and "BUTTON_PIN" not in SOURCE
assert all('.uri="/api/'+action+'"' in SOURCE for action in
           ("status","home","up","down","stop","calibrate","set-min","settings"))
assert SOURCE.count("HTTP_POST") == 7
assert "origin_ok" in SOURCE and "GPIO_PULLUP_ENABLE" in SOURCE
assert "RETURN_WAIT_MS 10000u" in SOURCE
assert "nvs_set_blob(nvs,SETTINGS_KEY" in SOURCE
assert all('id="'+id+'"' in HTML for id in
           ("up","down","stop","home","calibrate","set-min","direction","polarity","save-settings"))
assert "/dashboard" not in HTML
assert "GPIO20" not in CONFIG and "LIMIT_BOTTOM" not in CONFIG
print("PASS architecture: one HOME input, timed minimum, 8 API routes, NVS controls")

HARNESS=r"""
#define ELEVATOR_HOST_TEST
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
typedef int esp_err_t;
#define CONFIG_ELEVATOR_MOTOR_IN1_GPIO 3
#define CONFIG_ELEVATOR_MOTOR_IN2_GPIO 4
#define CONFIG_ELEVATOR_HOME_GPIO 10
#define CONFIG_ELEVATOR_HOME_TIMEOUT_MS 20000
#define CONFIG_ELEVATOR_CAL_TIMEOUT_MS 20000
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static unsigned sim_now;
static int pin_level[22],fail_pin=-1,first_powerup_moves;
static int64_t esp_timer_get_time(void){return (int64_t)sim_now*1000;}
static int gpio_get_level(int pin){return pin_level[pin];}
static int gpio_set_level(int pin,int level){
    assert(pin==3||pin==4);
    if(pin==fail_pin) return -1;
    pin_level[pin]=level;
    assert(!(pin_level[3]&&pin_level[4]));
    if(level)first_powerup_moves++;
    return 0;
}
static uint32_t esp_rom_crc32_le(uint32_t seed,const uint8_t *data,size_t len) {
    uint32_t crc=~seed;
    for(size_t i=0;i<len;i++){crc^=data[i];for(int j=0;j<8;j++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
    return ~crc;
}
#include "SOURCE_PATH"
static void advance(unsigned ms) {
    assert(ms%10==0);
    for(unsigned k=0;k<ms;k+=10){
        sim_now+=10;controller_tick(NULL);
        assert(!(pin_level[3]&&pin_level[4]));
        if(ctl.state==FAULT) assert(!ctl.motor_on);
    }
}
static void start_at(bool top) {
    memset(&ctl,0,sizeof(ctl));memset(pin_level,0,sizeof(pin_level));
    sim_now=0;fail_pin=-1;first_powerup_moves=0;
    pin_level[10]=top?1:0;
    ctl=(controller_t){.state=UNKNOWN,.settings=defaults(),.hardware_ok=true,
        .nvs_ok=true,.wifi_ok=true,.config_verified=true};
    advance(70);
    assert(ctl.state==(top?HOME_IDLE:UNKNOWN));
    assert(!ctl.motor_on);
}
static void command(request_t r){
    ctl.request=r;
    if(r!=R_STOP&&(uint32_t)(sim_now-ctl.last_brake_ms)<REVERSE_MS) {
        advance(REVERSE_MS+PERIOD_MS);
    } else advance(10);
}
static void begin_calibration(void) {
    start_at(true);
    command(R_BEGIN_CAL);assert(ctl.state==CAL_READY);
    advance(450);
    command(R_DOWN);assert(ctl.state==CAL_DOWN&&ctl.motor_on);
    assert(pin_level[3]==0&&pin_level[4]==1);
    pin_level[10]=0;
    advance(100);assert(ctl.top_released);
}
static void simulate_saved_min(void) {
    assert(ctl.save_pending);
    /* After STOP, the save remains pending but motion state is UNKNOWN. */
    assert(ctl.state==SAVE_MIN || ctl.state==UNKNOWN);
    assert(settings_valid(&ctl.staged));
    bool go_home=ctl.save_go_home;
    ctl.settings=ctl.staged;
    ctl.save_pending=false;ctl.save_go_home=false;
    ctl.auto_home_after_save=go_home;
    ctl.state=UNKNOWN;ctl.top_seen=false;ctl.request=R_NONE;
}
static void reached_home(void){
    pin_level[10]=1;advance(10);
    assert(!ctl.motor_on);
    advance(100);
    assert(ctl.state==HOME_IDLE&&!ctl.fault_latched);
}
int main(void){
    puts("TEST no movement at boot, top NC HIGH");
    start_at(false);advance(4000);
    assert(ctl.state==UNKNOWN&&first_powerup_moves==0);
    puts("PASS: unknown at boot does not move");

    puts("TEST one-tap homing with upper sensor");
    command(R_HOME);
    assert(ctl.state==HOMING&&ctl.motor_on&&pin_level[3]==1&&pin_level[4]==0);
    advance(800);reached_home();
    assert(ctl.travel_elapsed_ms==0);
    puts("PASS: HOME stops at raw limit and verifies debounce");

    puts("TEST time-defined bottom calibration and NVS staging");
    begin_calibration();advance(2650);
    uint32_t expected=ctl.now_ms-ctl.started_ms;
    command(R_SET_MIN);
    assert(ctl.state==SAVE_MIN&&!ctl.motor_on&&pin_level[3]==0&&pin_level[4]==0);
    assert(ctl.staged.minimum_ms==expected+10);
    simulate_saved_min();advance(500);
    assert(ctl.state==HOMING&&ctl.motor_on&&ctl.direction_up);
    reached_home();
    assert(ctl.settings.minimum_ms>=2600);
    puts("PASS: SET MIN stores timed lower limit and returns to HOME");

    puts("TEST PARAR during pending NVS save cancels automatic HOME");
    begin_calibration();advance(900);command(R_SET_MIN);
    assert(ctl.state==SAVE_MIN&&ctl.save_pending);
    command(R_STOP);
    assert(ctl.state==UNKNOWN&&!ctl.save_go_home&&ctl.cancel_auto_home);
    simulate_saved_min();advance(600);
    assert(ctl.state==UNKNOWN&&!ctl.motor_on);
    puts("PASS: STOP prevents post-save restart");

    puts("TEST normal descent then ten-second auto home");
    advance(500);command(R_DOWN);
    assert(ctl.state==DOWN&&ctl.motor_on);
    pin_level[10]=0;advance(100);
    while(ctl.state==DOWN)advance(10);
    assert(ctl.state==MIN_IDLE&&!ctl.motor_on);
    advance(9990);
    assert(ctl.state==MIN_IDLE&&!ctl.motor_on);
    advance(10);
    assert(ctl.state==HOMING&&ctl.motor_on);
    reached_home();
    puts("PASS: timed MIN and exact 10-second return");

    puts("TEST STOP while travelling invalidates estimated position");
    advance(500);command(R_DOWN);
    assert(ctl.state==DOWN);
    pin_level[10]=0;advance(100);
    command(R_STOP);
    assert(ctl.state==UNKNOWN&&!ctl.motor_on&&pin_level[3]==0&&pin_level[4]==0);
    assert(ctl.request==R_NONE);
    puts("PASS: STOP brakes and demands re-homing");

    puts("TEST HOME never runs indefinitely without sensor");
    start_at(false);command(R_HOME);
    assert(ctl.state==HOMING);advance(20010);
    assert(ctl.state==FAULT&&!ctl.motor_on&&!strcmp(ctl.reason,"HOME_TIMEOUT"));
    puts("PASS: homing watchdog trips");

    puts("TEST calibration cannot descend indefinitely");
    begin_calibration();advance(20010);
    assert(ctl.state==FAULT&&!ctl.motor_on&&!strcmp(ctl.reason,"CALIBRATION_TIMEOUT"));
    puts("PASS: calibration watchdog trips");

    puts("TEST wrong home sensor sequence while descending");
    begin_calibration();pin_level[10]=1;advance(100);
    assert(ctl.state==FAULT&&!ctl.motor_on);
    puts("PASS: sensor re-trigger stops motor");

    puts("TEST persisted direction/polarity checks");
    settings_t s=defaults();
    s.up_in1_high=0;s.home_active_level=0;s.minimum_ms=1240;s.checksum=settings_crc(&s);
    assert(settings_valid(&s));
    s.checksum^=1;assert(!settings_valid(&s));
    s=defaults();s.minimum_ms=21000;s.checksum=settings_crc(&s);assert(!settings_valid(&s));
    puts("PASS: NVS schema, direction, polarity and bounds");

    puts("TEST active-low switch behavior for alternative wiring");
    start_at(false);ctl.settings.home_active_level=0;ctl.top_seen=false;
    advance(70);assert(ctl.state==HOME_IDLE);
    puts("PASS: configurable HOME polarity");

    puts("TEST failed GPIO write does not report a running motor");
    start_at(false);fail_pin=3;command(R_HOME);
    assert(ctl.state==FAULT&&!ctl.motor_on);
    puts("PASS: GPIO failure latches FAULT");

    puts("ALL HOST TESTS PASS");
    return 0;
}
""".replace('"SOURCE_PATH"', '"'+str(ROOT/"main/main.c").replace("\\","/")+'"')
with tempfile.TemporaryDirectory(prefix="elevador-v2-") as tmp:
    source=Path(tmp)/"host_test.c"
    binary=Path(tmp)/"host_test"
    source.write_text(HARNESS,encoding="utf-8")
    compile_cmd=["gcc","-std=gnu11","-Wall","-Wextra","-Werror",
                 "-Wno-unused-function","-Wno-unused-variable",
                 "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                 str(source),"-o",str(binary)]
    subprocess.run(compile_cmd,check=True)
    subprocess.run([str(binary)],check=True,cwd=tmp)
print("PASS software-only mocked tests; physical safety and power cut NOT verified")
