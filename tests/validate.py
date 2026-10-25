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
           ("status","home","up","down","stop","calibrate","set-min","settings","times"))
assert SOURCE.count("HTTP_POST") == 8
assert "origin_ok" in SOURCE and "GPIO_PULLUP_ENABLE" in SOURCE
assert "RETURN_WAIT_MS 10000u" in SOURCE
assert "GPIO_INTR_ANYEDGE" in SOURCE and "gpio_isr_handler_add(PIN_TOP,home_gpio_isr,NULL)" in SOURCE
assert 'nvs_set_u32(nvs,"last_home_ms",home_ms)' in SOURCE
assert "home_time_save_pending" in SOURCE and "last_home_ms" in HTML
assert "enable('up',ready&&['UNKNOWN','MIN']" in HTML
assert "enable('down',ready&&s.top_active&&s.state==='HOME'" in HTML
assert 'id="up-time"' in HTML and 'id="down-time"' in HTML
assert 'id="save-times"' in HTML
assert 'ctl.up_time_ms' in SOURCE and 'ctl.down_time_ms' in SOURCE
assert '"up_time_ms"' in SOURCE and '"down_time_ms"' in SOURCE
assert "nvs_set_blob(nvs,SETTINGS_KEY" in SOURCE
assert all('id="'+id+'"' in HTML for id in
           ("up","down","stop","home","direction","polarity","save-settings","save-times"))
assert "/dashboard" not in HTML
assert "const isStop=path==='/api/stop'" in HTML
assert "(ui.busy&&!isStop)" in HTML
assert "GPIO20" not in CONFIG and "LIMIT_BOTTOM" not in CONFIG
print("PASS architecture: one HOME input, timed minimum, 8 API routes, NVS controls")

HARNESS=r"""
#define ELEVATOR_HOST_TEST
#define IRAM_ATTR
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
#define portENTER_CRITICAL_ISR(m) ((void)(m))
#define portEXIT_CRITICAL_ISR(m) ((void)(m))
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
    home_irq_edges=0;home_irq_at_ms=0;
    pin_level[10]=top?1:0;
    ctl=(controller_t){.state=UNKNOWN,.settings=defaults(),.hardware_ok=true,
        .nvs_ok=true,.wifi_ok=true,.config_verified=true,
        .up_time_ms=DEFAULT_TIMED_MS,.down_time_ms=DEFAULT_TIMED_MS};
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
    uint32_t edge_ms=sim_now;
    pin_level[10]=1;home_gpio_isr(NULL);advance(10);
    assert(!ctl.motor_on);
    advance(100);
    assert(ctl.state==HOME_IDLE&&!ctl.fault_latched);
    assert(ctl.last_home_ms==edge_ms-ctl.started_ms);
    assert(ctl.home_time_save_pending);
}
int main(void){
    puts("TEST no movement at boot");
    start_at(false);advance(4000);
    assert(ctl.state==UNKNOWN && !ctl.motor_on && first_powerup_moves==0);
    puts("PASS: boot stays stopped");

    puts("TEST HOME interrupt timestamp and stable activation");
    command(R_HOME);
    assert(ctl.state==HOMING&&ctl.motor_on);
    advance(700);reached_home();
    assert(ctl.last_home_ms>=700 && ctl.last_home_ms<=720);
    puts("PASS: HOME rising edge stops ascent and records arrival");

    puts("TEST timed descent without minimum calibration");
    start_at(true);
    assert(ctl.settings.minimum_ms==0);
    ctl.down_time_ms=1200;
    command(R_DOWN);
    assert(ctl.state==DOWN && ctl.motor_on && pin_level[3]==0 && pin_level[4]==1);
    pin_level[10]=0;home_gpio_isr(NULL);advance(100);
    advance(1200);
    assert(ctl.state==MIN_IDLE&&!ctl.motor_on);
    assert(pin_level[3]==0&&pin_level[4]==0);
    advance(12000);
    assert(ctl.state==MIN_IDLE&&!ctl.motor_on);
    puts("PASS: down works without calibration, stops at time, no auto return");

    puts("TEST manual timed ascent from bottom");
    ctl.up_time_ms=1800;
    command(R_UP);
    assert(ctl.state==HOMING&&ctl.motor_on);
    advance(400);reached_home();
    assert(ctl.state==HOME_IDLE);
    puts("PASS: ascent stops when HOME triggers before timer");

    puts("TEST timed ascent ends safely when HOME absent");
    start_at(false);ctl.up_time_ms=1000;
    command(R_UP);assert(ctl.state==HOMING);
    advance(1010);
    assert(ctl.state==UNKNOWN&&!ctl.motor_on);
    assert(pin_level[3]==0&&pin_level[4]==0);
    puts("PASS: ascent times out without automatic retry");

    puts("TEST PARAR interrupts timed descent");
    start_at(true);command(R_DOWN);
    assert(ctl.state==DOWN);
    pin_level[10]=0;home_gpio_isr(NULL);advance(100);
    command(R_STOP);
    assert(ctl.state==UNKNOWN&&!ctl.motor_on);
    advance(5000);
    assert(ctl.state==UNKNOWN&&!ctl.motor_on);
    puts("PASS: immediate STOP");

    puts("TEST HOME sensor bounce rejects confirmation");
    start_at(false);command(R_HOME);advance(200);
    pin_level[10]=1;home_gpio_isr(NULL);advance(10);
    pin_level[10]=0;home_gpio_isr(NULL);advance(200);
    assert(ctl.state==FAULT&&!ctl.motor_on);
    puts("PASS: bouncing switch cannot confirm HOME");

    puts("TEST invalid GPIO write stops control");
    start_at(false);fail_pin=3;command(R_HOME);
    assert(ctl.state==FAULT&&!ctl.motor_on);
    puts("PASS: driver error latches fault");

    puts("TEST configurable alternative HOME polarity");
    start_at(false);ctl.settings.home_active_level=0;ctl.top_seen=false;
    advance(70);assert(ctl.state==HOME_IDLE);
    puts("PASS: HOME polarity option remains supported");

    puts("ALL TIMED MODE HOST TESTS PASS");
    return 0;
}
""".replace('"SOURCE_PATH"', '"'+str(ROOT/"main/main.c").replace("\\","/")+'"')
with tempfile.TemporaryDirectory(prefix="elevador-v2-") as tmp:
    source=Path(tmp)/"host_test.c"
    binary=Path(tmp)/"host_test"
    source.write_text(HARNESS,encoding="utf-8")
    match=re.search(r"<script>\s*([\s\S]*?)</script>",HTML)
    assert match, "Inline UI script missing"
    ui_script=Path(tmp)/"web_ui.js"
    ui_script.write_text(match.group(1),encoding="utf-8")
    subprocess.run(["node","--check",str(ui_script)],check=True)
    print("PASS inline JavaScript syntax")
    compile_cmd=["gcc","-std=gnu11","-Wall","-Wextra","-Werror",
                 "-Wno-unused-function","-Wno-unused-variable",
                 "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                 str(source),"-o",str(binary)]
    subprocess.run(compile_cmd,check=True)
    subprocess.run([str(binary)],check=True,cwd=tmp)
print("PASS software-only mocked tests; physical safety and power cut NOT verified")
