/* Bench elevator: ISR counts edges, timer owns control, app_main owns NVS I/O.
 * L293D EN is fixed HIGH in hardware: IN1=IN2=0 commands electrical braking,
 * not independent shutdown or guaranteed load holding. Mechanical retention is mandatory. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#ifndef ELEVATOR_HOST_TEST
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
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

#define TOP_PIN CONFIG_ELEVATOR_LIMIT_TOP_GPIO
#define BOTTOM_PIN CONFIG_ELEVATOR_LIMIT_BOTTOM_GPIO
#define BUTTON_PIN CONFIG_ELEVATOR_BUTTON_GPIO
#define IN1_PIN CONFIG_ELEVATOR_MOTOR_IN1_GPIO
#define IN2_PIN CONFIG_ELEVATOR_MOTOR_IN2_GPIO
#define LED_PIN CONFIG_ELEVATOR_STATUS_LED_GPIO
#define WAIT_MS CONFIG_ELEVATOR_AUTO_UP_WAIT_MS
#define CAL_VERSION 2u
#define CAL_MAGIC 0x454C5631u
#ifdef CONFIG_ELEVATOR_STATUS_LED_ENABLED
#define LED_ENABLED true
#else
#define LED_ENABLED false
#endif
#ifdef CONFIG_ELEVATOR_SUPERVISED_TEST
#define SUPERVISED true
#else
#define SUPERVISED false
#endif
#ifdef CONFIG_ELEVATOR_AUTOMATIC_ENABLED
#define AUTOMATIC true
#else
#define AUTOMATIC false
#endif
#ifdef CONFIG_ELEVATOR_WEB_CONTROL_ENABLED
#define WEB_MODE true
#else
#define WEB_MODE false
#endif
#ifdef CONFIG_ELEVATOR_LIMITS_CONFIRMED
#define LIMITS_CONFIRMED true
#else
#define LIMITS_CONFIRMED false
#endif
#ifdef CONFIG_ELEVATOR_MOTOR_UP_IN1_HIGH
#define UP_IN1 true
#else
#define UP_IN1 false
#endif

typedef enum { BOOT, NEEDS_COMMISSIONING, IDLE_TOP, IDLE_BOTTOM,
    WAIT_AUTO_UP, MOVING_UP, MOVING_DOWN, FAULT } state_t;
typedef enum { NONE, BOTH_LIMITS_ACTIVE, UNKNOWN_POSITION_AT_BOOT,
    GPIO_CONFIGURATION, TRAVEL_TOO_FAST, TRAVEL_TIMEOUT, UNEXPECTED_LIMIT,
    LIMIT_SENSOR_INCONSISTENT, START_LIMIT_NOT_RELEASED,
    NVS_OR_CALIBRATION_INVALID, AP_OR_HTTP_FAILURE, GPIO_WRITE_FAILURE,
    REMOTE_STOP, REMOTE_HEARTBEAT_LOST } fault_t;
static const char *const state_names[] = {"BOOT","NEEDS_COMMISSIONING","IDLE_TOP",
    "IDLE_BOTTOM","WAIT_AUTO_UP","MOVING_UP","MOVING_DOWN","FAULT"};
static const char *const fault_names[] = {"NONE","BOTH_LIMITS_ACTIVE",
    "UNKNOWN_POSITION_AT_BOOT","GPIO_CONFIGURATION","TRAVEL_TOO_FAST",
    "TRAVEL_TIMEOUT","UNEXPECTED_LIMIT","LIMIT_SENSOR_INCONSISTENT",
    "START_LIMIT_NOT_RELEASED","NVS_OR_CALIBRATION_INVALID","AP_OR_HTTP_FAILURE","GPIO_WRITE_FAILURE",
    "REMOTE_STOP","REMOTE_HEARTBEAT_LOST"};
static const char *const fault_messages[] = {"Sem falha", "Ambos os fins ativos",
    "Posicao desconhecida no boot", "Pinos, console, senha ou tempos invalidos",
    "Destino antes do minimo fisico", "Tempo maximo excedido",
    "Fim mudou sem movimento ordenado", "Sequencia dos fins inconsistente",
    "Fim de origem nao liberou", "NVS ou calibracao invalida",
    "Falha de AP ou HTTP; motor bloqueado", "Falha de escrita GPIO; comando nao confirmado",
    "Parada remota; posicao requer verificacao", "Sinal web perdido durante movimento"};
typedef struct {
    int pin, active_level, raw_level;
    bool debounced_active, initialized, edge_seen;
    uint64_t last_change_ms;
    uint32_t edge_count, events;
    volatile uint32_t irq_edges;
} input_t;
typedef struct {
    uint32_t magic, version, fingerprint;
    uint32_t minimum_up_ms, minimum_down_ms, up_count, down_count, crc;
} calibration_t;
typedef struct {
    state_t state, fault_state;
    fault_t fault;
    uint64_t fault_ms, state_ms, command_ms, stopped_ms, now_ms;
    uint64_t button_pressed_ms, first_destination_ms;
    bool initialized, pins_ready, drive_commanded, origin_released;
    bool destination_pending, armed, arm_release_needed, calibration_dirty;
    bool calibration_valid, nvs_ok, web_ok, position_top, gpio_write_failed;
    bool output_known[3];
    /* Web handlers only enqueue intents; timer is the sole owner of motor GPIO. */
    uint8_t remote_requested; /* 0 none, 1 up, 2 down */
    uint64_t remote_heartbeat_ms, remote_block_until_ms;
    bool remote_owned, remote_stop_requested;
    int failed_gpio_pin, gpio_error;
    int motor_in1, motor_in2, led_level;
    uint32_t ignored_presses, last_up_ms, last_down_ms, generation;
    uint32_t output_events[3];
    uint64_t output_change_ms[3];
    input_t top, bottom, button;
    calibration_t cal;
} controller_t;
static controller_t ctl;
static portMUX_TYPE control_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE irq_mux = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "elevador";
static uint64_t clock_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static bool moving(void) { return ctl.state == MOVING_UP || ctl.state == MOVING_DOWN; }
static bool raw_active(const input_t *in) { return in->raw_level == in->active_level; }
static uint32_t min_time(bool up) { return up ? CONFIG_ELEVATOR_MIN_UP_MS : CONFIG_ELEVATOR_MIN_DOWN_MS; }
static uint32_t max_time(bool up) { return up ? CONFIG_ELEVATOR_MAX_UP_MS : CONFIG_ELEVATOR_MAX_DOWN_MS; }
static uint32_t fingerprint(void)
{
    const uint32_t config[] = { TOP_PIN, BOTTOM_PIN, BUTTON_PIN, IN1_PIN, IN2_PIN,
        CONFIG_ELEVATOR_LIMIT_TOP_ACTIVE_LEVEL,
        CONFIG_ELEVATOR_LIMIT_BOTTOM_ACTIVE_LEVEL, CONFIG_ELEVATOR_BUTTON_ACTIVE_LEVEL,
        UP_IN1, CONFIG_ELEVATOR_MIN_UP_MS, CONFIG_ELEVATOR_MAX_UP_MS,
        CONFIG_ELEVATOR_MIN_DOWN_MS, CONFIG_ELEVATOR_MAX_DOWN_MS,
        CONFIG_ELEVATOR_START_RELEASE_MS, CONFIG_ELEVATOR_REQUIRED_CYCLES,
        CONFIG_ELEVATOR_REVERSAL_MS, CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS,
        CONFIG_ELEVATOR_SAMPLE_MS };
    return esp_rom_crc32_le(0, (const uint8_t *)config, sizeof(config));
}
static uint32_t calibration_crc(const calibration_t *cal)
{ return esp_rom_crc32_le(0, (const uint8_t *)cal, offsetof(calibration_t, crc)); }
static bool calibration_sane(const calibration_t *cal)
{
    return cal->magic == CAL_MAGIC && cal->version == CAL_VERSION &&
        cal->fingerprint == fingerprint() && cal->crc == calibration_crc(cal) &&
        ((cal->up_count == 0 && cal->minimum_up_ms == 0) ||
         (cal->up_count > 0 && cal->minimum_up_ms >= min_time(true) && cal->minimum_up_ms <= max_time(true))) &&
        ((cal->down_count == 0 && cal->minimum_down_ms == 0) ||
         (cal->down_count > 0 && cal->minimum_down_ms >= min_time(false) && cal->minimum_down_ms <= max_time(false)));
}
static bool calibration_complete(void)
{
    return calibration_sane(&ctl.cal) && ctl.cal.up_count >= CONFIG_ELEVATOR_REQUIRED_CYCLES &&
        ctl.cal.down_count >= CONFIG_ELEVATOR_REQUIRED_CYCLES;
}
static void transition(state_t state)
{
    if (ctl.state != state) { ctl.state = state; ctl.state_ms = ctl.now_ms; }
}
/* Latch only: no GPIO calls here, so failure handling cannot recurse. */
static void latch_fault(fault_t reason)
{
    if (ctl.fault == NONE) {
        ctl.fault = reason; ctl.fault_state = ctl.state; ctl.fault_ms = ctl.now_ms;
    }
    ctl.armed = false; transition(FAULT);
}
/* Success means the driver accepted the write, never electrical feedback.
 * Preserve the last accepted value/event count on failure. Uncertainty is
 * latched until reboot, even if a best-effort brake retry later succeeds. */
static bool output_level(unsigned index, int pin, int *previous, int level)
{
    esp_err_t err = gpio_set_level(pin, level);
    if (err != ESP_OK) {
        ctl.output_known[index] = false;
        if (!ctl.gpio_write_failed) { ctl.failed_gpio_pin = pin; ctl.gpio_error = err; }
        ctl.gpio_write_failed = true; ctl.drive_commanded = false;
        latch_fault(GPIO_WRITE_FAILURE);
        return false;
    }
    ctl.output_known[index] = true;
    if (*previous != level) {
        ++ctl.output_events[index]; ctl.output_change_ms[index] = ctl.now_ms;
    }
    *previous = level;
    return true;
}
static bool motor_brake(void)
{
    bool was_driving = ctl.drive_commanded;
    bool in1_ok = false, in2_ok = false;
    if (ctl.pins_ready) {
        /* Attempt BOTH low writes, even if the first fails. Bounded, no recursion. */
        in1_ok = output_level(0, IN1_PIN, &ctl.motor_in1, 0);
        in2_ok = output_level(1, IN2_PIN, &ctl.motor_in2, 0);
    }
    if (was_driving && in1_ok && in2_ok) ctl.stopped_ms = ctl.now_ms;
    ctl.drive_commanded = false;
    return in1_ok && in2_ok && !ctl.gpio_write_failed;
}
static void fail(fault_t reason)
{
    motor_brake();
    latch_fault(reason);
}
static bool valid_gpio(int pin)
{
    return GPIO_IS_VALID_OUTPUT_GPIO(pin) && pin != 2 && pin != 8 && pin != 9 &&
        !(pin >= 12 && pin <= 19);
}
static bool pin_configuration_valid(void)
{
    const int pins[] = {TOP_PIN, BOTTOM_PIN, BUTTON_PIN, IN1_PIN, IN2_PIN, LED_PIN};
    unsigned count = LED_ENABLED ? 6 : 5;
    for (unsigned i = 0; i < count; ++i) {
        if (!(i == 5 && pins[i] == 8) && !valid_gpio(pins[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (pins[i] == pins[j]) return false;
    }
#ifdef CONFIG_ESP_CONSOLE_UART
    for (unsigned i = 0; i < count; ++i) {
#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
        if (pins[i] == 20 || pins[i] == 21) return false;
#elif defined(CONFIG_ESP_CONSOLE_UART_TX_GPIO) && defined(CONFIG_ESP_CONSOLE_UART_RX_GPIO)
        if (pins[i] == CONFIG_ESP_CONSOLE_UART_TX_GPIO || pins[i] == CONFIG_ESP_CONSOLE_UART_RX_GPIO) return false;
#else
        return false;
#endif
    }
#endif
    return true;
}
static bool configuration_valid(void)
{
    if (!pin_configuration_valid()) return false;
    size_t pass = strlen(CONFIG_ELEVATOR_AP_PASSWORD), ssid = strlen(CONFIG_ELEVATOR_AP_SSID);
    return ssid > 0 && ssid <= 32 && pass >= 8 && pass <= 63 &&
        min_time(true) > 0 && min_time(false) > 0 &&
        max_time(true) > min_time(true) && max_time(false) > min_time(false) &&
        CONFIG_ELEVATOR_START_RELEASE_MS < max_time(true) &&
        CONFIG_ELEVATOR_START_RELEASE_MS < max_time(false) &&
        !(SUPERVISED && AUTOMATIC) &&
        !(WEB_MODE && (SUPERVISED || AUTOMATIC)) &&
        (!WEB_MODE || strcmp(CONFIG_ELEVATOR_AP_PASSWORD, "12345678") != 0) &&
        CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS >= CONFIG_ELEVATOR_SAMPLE_MS * 3;
}
static void IRAM_ATTR input_isr(void *arg)
{
    input_t *in = arg;
    portENTER_CRITICAL_ISR(&irq_mux); ++in->irq_edges; portEXIT_CRITICAL_ISR(&irq_mux);
}
static bool sample(input_t *in, uint32_t debounce)
{
    int raw = gpio_get_level(in->pin);
    portENTER_CRITICAL(&irq_mux); uint32_t edges = in->irq_edges; portEXIT_CRITICAL(&irq_mux);
    in->edge_seen = edges != in->edge_count;
    if (raw != in->raw_level || in->edge_seen) {
        in->raw_level = raw; in->edge_count = edges; in->last_change_ms = ctl.now_ms;
    }
    if (ctl.now_ms - in->last_change_ms < debounce) return false;
    bool active = raw_active(in);
    if (!in->initialized) { in->initialized = true; in->debounced_active = active; return false; }
    if (active == in->debounced_active) return false;
    in->debounced_active = active; ++in->events;
    return active;
}
static bool operational(void)
{ return LIMITS_CONFIRMED && !SUPERVISED &&
    ((AUTOMATIC && ctl.calibration_valid) || WEB_MODE); }
static void stationary(void)
{
    if (ctl.top.debounced_active && ctl.bottom.debounced_active) { fail(BOTH_LIMITS_ACTIVE); return; }
    if (!ctl.top.debounced_active && !ctl.bottom.debounced_active) { fail(UNEXPECTED_LIMIT); return; }
    ctl.position_top = ctl.top.debounced_active;
    if (ctl.top.debounced_active) transition(IDLE_TOP);
    else transition(operational() ? (AUTOMATIC ? WAIT_AUTO_UP : IDLE_BOTTOM) : NEEDS_COMMISSIONING);
}
/* Sole function allowed to command directional drive. Timer owner only. */
static bool motor_start(bool up)
{
    input_t *origin = up ? &ctl.bottom : &ctl.top;
    input_t *dest = up ? &ctl.top : &ctl.bottom;
    if (!ctl.initialized || !ctl.pins_ready || ctl.fault != NONE || !ctl.nvs_ok || !ctl.web_ok ||
        !LIMITS_CONFIRMED || ctl.calibration_dirty || (!operational() && !(SUPERVISED && ctl.armed)) ||
        moving() || ctl.drive_commanded || !origin->debounced_active || !raw_active(origin) ||
        dest->debounced_active || raw_active(dest) ||
        ctl.now_ms - ctl.stopped_ms < CONFIG_ELEVATOR_REVERSAL_MS ||
        ctl.now_ms - origin->last_change_ms < CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS ||
        ctl.now_ms - dest->last_change_ms < CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS ||
        max_time(up) <= min_time(up)) return false;
    if (!motor_brake()) return false;
    bool in1 = up == UP_IN1;
    if (!output_level(0, IN1_PIN, &ctl.motor_in1, in1) ||
        !output_level(1, IN2_PIN, &ctl.motor_in2, !in1)) {
        motor_brake(); /* One best-effort rollback; the fault stays latched. */
        return false;
    }
    ctl.command_ms = ctl.now_ms; ctl.origin_released = false; ctl.destination_pending = false;
    ctl.first_destination_ms = 0;
    transition(up ? MOVING_UP : MOVING_DOWN);
    ctl.drive_commanded = true;
    return true;
}
static void finish_travel(bool up)
{
    if (!motor_brake()) return;
    uint64_t duration = ctl.first_destination_ms - ctl.command_ms;
    if (duration < min_time(up)) { fail(TRAVEL_TOO_FAST); return; }
    if (duration > max_time(up)) { fail(TRAVEL_TIMEOUT); return; }
    if (up) ctl.last_up_ms = duration; else ctl.last_down_ms = duration;
    if (SUPERVISED && ctl.armed) {
        uint32_t *minimum = up ? &ctl.cal.minimum_up_ms : &ctl.cal.minimum_down_ms;
        uint32_t *count = up ? &ctl.cal.up_count : &ctl.cal.down_count;
        if (!*minimum || duration < *minimum) *minimum = duration;
        if (*count < UINT32_MAX) ++*count;
        ctl.cal.crc = calibration_crc(&ctl.cal);
        ctl.calibration_dirty = true; ++ctl.generation;
        ctl.calibration_valid = false;
    }
    ctl.remote_owned = false;
    ctl.remote_requested = 0; /* never auto-repeat after arriving at destination */
    transition(up ? IDLE_TOP : IDLE_BOTTOM);
    stationary();
}
static void monitor_travel(void)
{
    bool up = ctl.state == MOVING_UP;
    input_t *origin = up ? &ctl.bottom : &ctl.top;
    input_t *dest = up ? &ctl.top : &ctl.bottom;
    uint64_t elapsed = ctl.now_ms - ctl.command_ms;
    if ((!ctl.destination_pending && elapsed > max_time(up)) ||
        (ctl.destination_pending && elapsed > (uint64_t)max_time(up) + CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS + CONFIG_ELEVATOR_SAMPLE_MS)) {
        fail(TRAVEL_TIMEOUT); return;
    }
    if (origin->debounced_active && dest->debounced_active) { fail(BOTH_LIMITS_ACTIVE); return; }
    if (!origin->debounced_active) ctl.origin_released = true;
    if (ctl.origin_released && (origin->debounced_active || raw_active(origin))) {
        fail(LIMIT_SENSOR_INCONSISTENT); return;
    }
    if (!ctl.origin_released && elapsed >= CONFIG_ELEVATOR_START_RELEASE_MS) {
        fail(START_LIMIT_NOT_RELEASED); return;
    }
    /* First sampled destination stops output before debounce. No bounce restart. */
    if ((raw_active(dest) || dest->edge_seen) && !ctl.destination_pending) {
        ctl.destination_pending = true; ctl.first_destination_ms = ctl.now_ms;
        if (!motor_brake()) return;
    }
    if (ctl.destination_pending) {
        if (dest->debounced_active) {
            if (!ctl.origin_released) fail(LIMIT_SENSOR_INCONSISTENT);
            else finish_travel(up);
        } else if (!raw_active(dest) && ctl.now_ms - dest->last_change_ms >= CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS) {
            fail(LIMIT_SENSOR_INCONSISTENT);
        }
    }
}
static void update_led(void)
{
    if (!LED_ENABLED || !ctl.pins_ready) return;
    uint64_t t = ctl.now_ms;
    bool on;
    switch (ctl.state) {
    case BOOT: on = t % 160 < 80; break;
    case MOVING_UP: case MOVING_DOWN: on = t % 500 < 250; break;
    case WAIT_AUTO_UP: on = t % 1000 < 80; break;
    case FAULT: on = t % 1600 < 100 || (t % 1600 >= 200 && t % 1600 < 300); break;
    case NEEDS_COMMISSIONING: on = ctl.armed ? t % 2000 < 500 : t % 2000 < 100; break;
    default: on = true; break;
    }
    int level = on ? CONFIG_ELEVATOR_STATUS_LED_ACTIVE_LEVEL : !CONFIG_ELEVATOR_STATUS_LED_ACTIVE_LEVEL;
    if (!output_level(2, LED_PIN, &ctl.led_level, level)) motor_brake();
}
static void control_tick(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&control_mux);
    ctl.now_ms = clock_ms();
    if (!ctl.pins_ready) { portEXIT_CRITICAL(&control_mux); return; }
    /* STOP and heartbeat watchdog are processed before all movement logic. */
    if (ctl.remote_stop_requested) {
        ctl.remote_stop_requested = false;
        ctl.remote_requested = 0;
        ctl.remote_block_until_ms = ctl.now_ms + CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS;
        bool active = ctl.remote_owned && (moving() || ctl.drive_commanded);
        if (active) fail(REMOTE_STOP);
        else motor_brake();
        ctl.remote_owned = false;
    }
    if (WEB_MODE && ctl.remote_owned && moving() &&
        ctl.now_ms - ctl.remote_heartbeat_ms > CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS) {
        ctl.remote_requested = 0;
        ctl.remote_owned = false;
        fail(REMOTE_HEARTBEAT_LOST);
    }
    if (WEB_MODE && ctl.remote_requested &&
        ctl.now_ms - ctl.remote_heartbeat_ms > CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS)
        ctl.remote_requested = 0;
    sample(&ctl.top, CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS);
    sample(&ctl.bottom, CONFIG_ELEVATOR_LIMIT_DEBOUNCE_MS);
    bool pressed = sample(&ctl.button, CONFIG_ELEVATOR_BUTTON_DEBOUNCE_MS);
    if (pressed) ctl.button_pressed_ms = ctl.now_ms;
    if (ctl.top.initialized && ctl.bottom.initialized && ctl.button.initialized && ctl.state == BOOT) {
        ctl.initialized = true;
        if (ctl.top.debounced_active && ctl.bottom.debounced_active) fail(BOTH_LIMITS_ACTIVE);
        else if (!ctl.top.debounced_active && !ctl.bottom.debounced_active) fail(UNKNOWN_POSITION_AT_BOOT);
        else stationary();
        ctl.arm_release_needed = ctl.button.debounced_active;
    }
    if (ctl.fault == NONE && ctl.initialized) {
        if (ctl.top.debounced_active && ctl.bottom.debounced_active) fail(BOTH_LIMITS_ACTIVE);
        else if (moving()) { if (pressed) ++ctl.ignored_presses; monitor_travel(); }
        else {
            bool at_top = ctl.top.debounced_active;
            if (ctl.top.debounced_active != ctl.position_top ||
                ctl.bottom.debounced_active == ctl.position_top) {
                fail(UNEXPECTED_LIMIT);
            } else if ((!at_top && !ctl.bottom.debounced_active) ||
                (raw_active(&ctl.top) != at_top) ||
                (raw_active(&ctl.bottom) != ctl.bottom.debounced_active)) {
                motor_brake();
                if (!at_top && !ctl.bottom.debounced_active) fail(UNEXPECTED_LIMIT);
            } else {
                if (!ctl.button.debounced_active) ctl.arm_release_needed = false;
                if (SUPERVISED && !ctl.armed && !ctl.arm_release_needed &&
                    ctl.button.debounced_active && ctl.button_pressed_ms &&
                    ctl.now_ms - ctl.button_pressed_ms >= CONFIG_ELEVATOR_ARM_HOLD_MS &&
                    LIMITS_CONFIRMED && ctl.nvs_ok && ctl.web_ok) {
                    ctl.armed = true; ctl.arm_release_needed = true;
                }
                if (WEB_MODE && operational() && ctl.remote_requested &&
                    ctl.now_ms >= ctl.remote_block_until_ms &&
                    ctl.now_ms - ctl.remote_heartbeat_ms <= CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS &&
                    ((ctl.remote_requested == 1 && !at_top) ||
                     (ctl.remote_requested == 2 && at_top))) {
                    if (motor_start(ctl.remote_requested == 1)) ctl.remote_owned = true;
                } else if (SUPERVISED && ctl.armed && !ctl.arm_release_needed && pressed)
                    motor_start(!at_top);
                else switch (ctl.state) {
                case IDLE_TOP: if (pressed && AUTOMATIC && operational()) motor_start(false); break;
                case WAIT_AUTO_UP:
                    if (AUTOMATIC && operational() && ctl.now_ms - ctl.state_ms >= WAIT_MS) motor_start(true);
                    break;
                case IDLE_BOTTOM: if (!WEB_MODE) stationary(); break;
                default: break;
                }
            }
        }
    }
    update_led();
    portEXIT_CRITICAL(&control_mux);
}

#ifndef ELEVATOR_HOST_TEST
static bool initialize_gpio(void)
{
    if (!pin_configuration_valid()) return false;
    const int outputs[] = {IN1_PIN, IN2_PIN};
    /* Preload BOTH low latches before assigning either pad to GPIO output. */
    for (unsigned i = 0; i < 2; ++i) {
        int *value = i == 0 ? &ctl.motor_in1 : &ctl.motor_in2;
        if (!output_level(i, outputs[i], value, 0)) return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << outputs[i], .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE};
        if (gpio_config(&cfg) != ESP_OK) return false;
    }
    ctl.pins_ready = true;
    if (!motor_brake()) return false;
    if (LED_ENABLED) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << LED_PIN, .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE};
        int inactive = !CONFIG_ELEVATOR_STATUS_LED_ACTIVE_LEVEL;
        if (!output_level(2, LED_PIN, &ctl.led_level, inactive) || gpio_config(&cfg) != ESP_OK) return false;
    }
    input_t *inputs[] = {&ctl.top, &ctl.bottom, &ctl.button};
    if (gpio_install_isr_service(0) != ESP_OK) return false;
    for (unsigned i = 0; i < 3; ++i) {
        gpio_config_t cfg = {.pin_bit_mask = 1ULL << inputs[i]->pin, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE};
        if (gpio_config(&cfg) != ESP_OK || gpio_isr_handler_add(inputs[i]->pin, input_isr, inputs[i]) != ESP_OK) return false;
        inputs[i]->raw_level = gpio_get_level(inputs[i]->pin);
        inputs[i]->last_change_ms = clock_ms();
    }
    return configuration_valid();
}
static nvs_handle_t cal_handle;
static bool load_calibration(void)
{
    ctl.calibration_valid = false;
    /* Do not erase damaged NVS or manufacture valid calibration. */
    if (nvs_flash_init() != ESP_OK || nvs_open("elevator", NVS_READWRITE, &cal_handle) != ESP_OK) return false;
    size_t size = sizeof(ctl.cal);
    esp_err_t err = nvs_get_blob(cal_handle, "calibration", &ctl.cal, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ctl.cal = (calibration_t){.magic = CAL_MAGIC, .version = CAL_VERSION, .fingerprint = fingerprint()};
        ctl.cal.crc = calibration_crc(&ctl.cal);
        return true;
    }
    if (err != ESP_OK || size != sizeof(ctl.cal) || !calibration_sane(&ctl.cal)) return false;
    ctl.calibration_valid = calibration_complete();
    return true;
}
/* Called from the IDF main task only; never from timer/ISR/HTTP. */
static void save_calibration(void)
{
    calibration_t pending; uint32_t generation;
    portENTER_CRITICAL(&control_mux);
    bool save = ctl.calibration_dirty && !moving() && ctl.fault == NONE;
    pending = ctl.cal; generation = ctl.generation;
    portEXIT_CRITICAL(&control_mux);
    if (!save) return;
    esp_err_t err = nvs_set_blob(cal_handle, "calibration", &pending, sizeof(pending));
    if (err == ESP_OK) err = nvs_commit(cal_handle);
    portENTER_CRITICAL(&control_mux);
    if (err != ESP_OK) {
        ctl.nvs_ok = false; ctl.now_ms = clock_ms(); fail(NVS_OR_CALIBRATION_INVALID);
    } else if (ctl.generation == generation) {
        ctl.calibration_dirty = false; ctl.calibration_valid = calibration_complete();
    }
    portEXIT_CRITICAL(&control_mux);
}
static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}
extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
#define ELEVATOR_INDEX_HTML ((const char *)index_html_start)
extern const unsigned char control_html_start[] asm("_binary_control_html_start");
extern const unsigned char control_html_end[] asm("_binary_control_html_end");
static esp_err_t root_control_handler(httpd_req_t *req)
{
    no_cache(req); httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)control_html_start,
        control_html_end - control_html_start - 1);
}
static esp_err_t index_handler(httpd_req_t *req)
{
    no_cache(req); httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req, ELEVATOR_INDEX_HTML, index_html_end - index_html_start - 1);
}
static const char *json_bool(bool value) { return value ? "true" : "false"; }
static void nullable_ms(char *out, size_t size, uint32_t value)
{ if (value) snprintf(out, size, "%" PRIu32, value); else snprintf(out, size, "null"); }
/* HTTP serialization stays outside control lock and timer. No user strings in JSON. */
#include <stdarg.h>
typedef struct { char data[3072]; size_t used; bool failed; } json_t;
static void json_add(json_t *j, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void json_add(json_t *j, const char *format, ...)
{
    if (j->failed) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(j->data + j->used, sizeof(j->data) - j->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(j->data) - j->used) j->failed = true;
    else j->used += n;
}
static void json_input(json_t *j, const char *name, const input_t *in)
{
    json_add(j, "\"%s\":{\"pin\":%d,\"raw\":%d,\"active\":%s,\"raw_active\":%s,"
        "\"sampled\":%s,\"edges\":%"PRIu32",\"events\":%"PRIu32",\"last_change_ms\":%"PRIu64"},",
        name, in->pin, in->raw_level, json_bool(in->debounced_active), json_bool(raw_active(in)),
        json_bool(in->initialized), in->edge_count, in->events, in->last_change_ms);
}
static void json_output(json_t *j, const controller_t *s, unsigned index, const char *name,
    int pin, int level, bool active, bool enabled)
{
    bool known = enabled && s->output_known[index] && !s->gpio_write_failed;
    char accepted[4];
    snprintf(accepted, sizeof(accepted), "%d", level);
    json_add(j, "\"%s\":{\"pin\":%d,\"level\":%s,\"active\":%s,\"enabled\":%s,"
        "\"write_ok\":%s,\"events\":%"PRIu32",\"last_change_ms\":%"PRIu64"}%s",
        name, pin, known ? accepted : "null", json_bool(active && known), json_bool(enabled),
        json_bool(known), s->output_events[index], s->output_change_ms[index], index == 2 ? "}," : ",");
}
static esp_err_t status_handler(httpd_req_t *req)
{
    controller_t s;
    portENTER_CRITICAL(&control_mux); s = ctl; portEXIT_CRITICAL(&control_mux);
    uint64_t now = clock_ms();
    bool motor_known = s.pins_ready && s.output_known[0] && s.output_known[1] && !s.gpio_write_failed;
    bool in_motion = s.state == MOVING_UP || s.state == MOVING_DOWN;
    uint64_t elapsed = in_motion ? now - s.command_ms : 0;
    uint64_t remaining = s.state == WAIT_AUTO_UP && now - s.state_ms < WAIT_MS ? WAIT_MS - (now - s.state_ms) : 0;
    const char *position = s.top.debounced_active && !s.bottom.debounced_active ? "TOP" :
        s.bottom.debounced_active && !s.top.debounced_active ? "BOTTOM" : "UNKNOWN";
    char up[16], down[16], last_up[16], last_down[16];
    nullable_ms(up, sizeof(up), s.cal.minimum_up_ms); nullable_ms(down, sizeof(down), s.cal.minimum_down_ms);
    nullable_ms(last_up, sizeof(last_up), s.last_up_ms); nullable_ms(last_down, sizeof(last_down), s.last_down_ms);
    json_t j = {0};
    json_add(&j, "{\"state\":\"%s\",\"motion\":\"%s\",\"position\":\"%s\",\"calibrated\":%s,",
        state_names[s.state], !motor_known ? "UNKNOWN" : s.drive_commanded ? (s.state == MOVING_UP ? "UP" : "DOWN") : "STOPPED", position, json_bool(s.calibration_valid));
    json_add(&j, "\"motor\":{\"command\":\"%s\",\"brake_commanded\":%s,"
        "\"inputs_defined\":%s,\"independent_shutdown\":false},",
        !motor_known ? "UNKNOWN" : s.drive_commanded ? (s.state == MOVING_UP ? "UP" : "DOWN") : "BRAKE",
        json_bool(motor_known && !s.drive_commanded), json_bool(motor_known));
    json_add(&j, "\"commissioning\":{\"supervised\":%s,\"armed\":%s,\"automatic_enabled\":%s,"
        "\"limits_confirmed\":%s,\"pending_save\":%s,\"up_count\":%"PRIu32",\"down_count\":%"PRIu32",\"required_cycles\":%d},",
        json_bool(SUPERVISED), json_bool(s.armed), json_bool(AUTOMATIC), json_bool(LIMITS_CONFIRMED),
        json_bool(s.calibration_dirty), s.cal.up_count, s.cal.down_count, CONFIG_ELEVATOR_REQUIRED_CYCLES);
    json_add(&j, "\"remote\":{\"enabled\":%s,\"ready\":%s,\"heartbeat_timeout_ms\":%d},",
        json_bool(WEB_MODE),
        json_bool(WEB_MODE && s.fault == NONE && s.initialized && s.nvs_ok &&
            s.web_ok && LIMITS_CONFIRMED && !s.calibration_dirty &&
            !s.gpio_write_failed && s.pins_ready),
        CONFIG_ELEVATOR_WEB_HEARTBEAT_TIMEOUT_MS);
    json_add(&j, "\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"state\":\"%s\",\"time_ms\":%"PRIu64",\"gpio_write_failed\":%s,\"gpio_pin\":%d,\"gpio_error\":%d},\"gpio\":{",
        fault_names[s.fault], fault_messages[s.fault], state_names[s.fault_state], s.fault_ms,
        json_bool(s.gpio_write_failed), s.failed_gpio_pin, s.gpio_error);
    json_input(&j, "limit_top", &s.top); json_input(&j, "limit_bottom", &s.bottom); json_input(&j, "button", &s.button);
    json_output(&j, &s, 0, "motor_in1", IN1_PIN, s.motor_in1, s.motor_in1, s.pins_ready);
    json_output(&j, &s, 1, "motor_in2", IN2_PIN, s.motor_in2, s.motor_in2, s.pins_ready);
    json_output(&j, &s, 2, "onboard_led", LED_PIN, s.led_level,
        s.led_level == CONFIG_ELEVATOR_STATUS_LED_ACTIVE_LEVEL, LED_ENABLED && s.pins_ready);
    json_add(&j, "\"auto_up_remaining_ms\":%"PRIu64",\"auto_up_wait_ms\":%d,"
        "\"travel\":{\"direction\":\"%s\",\"elapsed_ms\":%"PRIu64","
        "\"minimum_up_ms\":%s,\"minimum_down_ms\":%s,\"last_up_ms\":%s,\"last_down_ms\":%s,"
        "\"safety_minimum_up_ms\":%d,\"safety_minimum_down_ms\":%d,\"maximum_up_ms\":%d,\"maximum_down_ms\":%d},",
        remaining, WAIT_MS, in_motion ? (s.state == MOVING_UP ? "UP" : "DOWN") : "NONE",
        elapsed, up, down, last_up, last_down, CONFIG_ELEVATOR_MIN_UP_MS, CONFIG_ELEVATOR_MIN_DOWN_MS,
        CONFIG_ELEVATOR_MAX_UP_MS, CONFIG_ELEVATOR_MAX_DOWN_MS);
    json_add(&j, "\"ignored_presses\":%"PRIu32",\"motor_blocked\":%s,\"refresh_ms\":%d,\"uptime_ms\":%"PRIu64"}",
        s.ignored_presses, json_bool(s.fault != NONE || !s.initialized || (!s.calibration_valid && !s.armed && !WEB_MODE) ||
        !LIMITS_CONFIRMED || (!AUTOMATIC && !SUPERVISED && !WEB_MODE) || s.calibration_dirty), CONFIG_ELEVATOR_WEB_REFRESH_MS, now);
    no_cache(req); httpd_resp_set_type(req, "application/json");
    if (j.failed) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status excedeu buffer");
    return httpd_resp_send(req, j.data, j.used);
}
static esp_err_t health_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&control_mux);
    bool healthy = ctl.initialized && ctl.fault == NONE && ctl.web_ok && ctl.nvs_ok;
    portEXIT_CRITICAL(&control_mux);
    no_cache(req); httpd_resp_set_type(req, "application/json");
    if (!healthy) httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, healthy ? "{\"healthy\":true}" : "{\"healthy\":false}");
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)arg; (void)base; (void)data;
    if (event == WIFI_EVENT_AP_STOP || event == WIFI_EVENT_AP_STADISCONNECTED) {
        portENTER_CRITICAL(&control_mux);
        ctl.remote_requested = 0; ctl.remote_stop_requested = true;
        if (event == WIFI_EVENT_AP_STOP) {
            ctl.web_ok = false; ctl.now_ms = clock_ms(); fail(AP_OR_HTTP_FAILURE);
        }
        portEXIT_CRITICAL(&control_mux);
    }
}
/* POST endpoints only enqueue requests. The control timer alone owns outputs.
 * Origin check blocks drive-by cross-site requests on an AP without internet. */
static bool valid_control_origin(httpd_req_t *req)
{
    char value[64];
    size_t len = httpd_req_get_hdr_value_len(req, "Origin");
    if (!len || len >= sizeof(value)) return false;
    if (httpd_req_get_hdr_value_str(req, "Origin", value, sizeof(value)) != ESP_OK) return false;
    return strcmp(value, "http://192.168.4.1") == 0;
}
static esp_err_t control_reply(httpd_req_t *req, const char *status, const char *json)
{
    no_cache(req); httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, json);
}
static bool remote_start_allowed(bool up)
{
    const input_t *origin = up ? &ctl.bottom : &ctl.top;
    const input_t *dest = up ? &ctl.top : &ctl.bottom;
    return WEB_MODE && operational() && ctl.initialized && ctl.pins_ready &&
        ctl.fault == NONE && ctl.nvs_ok && ctl.web_ok && !ctl.gpio_write_failed &&
        !ctl.calibration_dirty && !ctl.remote_stop_requested &&
        !ctl.button.debounced_active && !moving() && !ctl.drive_commanded &&
        ctl.now_ms >= ctl.remote_block_until_ms &&
        origin->initialized && origin->debounced_active && raw_active(origin) &&
        dest->initialized && !dest->debounced_active && !raw_active(dest);
}
static esp_err_t remote_control_handler(httpd_req_t *req)
{
    if (!valid_control_origin(req))
        return control_reply(req, "403 Forbidden", "{\"accepted\":false,\"reason\":\"origin\"}");
    if (req->content_len != 0)
        return control_reply(req, "400 Bad Request", "{\"accepted\":false,\"reason\":\"body\"}");
    bool stop = strcmp(req->uri, "/api/control/stop") == 0;
    bool up = strcmp(req->uri, "/api/control/up") == 0;
    bool down = strcmp(req->uri, "/api/control/down") == 0;
    if (!up && !down && !stop)
        return control_reply(req, "404 Not Found", "{\"accepted\":false,\"reason\":\"path\"}");
    bool accepted = false;
    portENTER_CRITICAL(&control_mux);
    if (stop) {
        ctl.remote_stop_requested = true;
        ctl.remote_requested = 0;
        accepted = true;
    } else if (WEB_MODE && ctl.remote_owned && moving() &&
               ctl.fault == NONE && !ctl.remote_stop_requested &&
               ((up && ctl.state == MOVING_UP) || (down && ctl.state == MOVING_DOWN))) {
        ctl.remote_heartbeat_ms = clock_ms();
        accepted = true;
    } else if (remote_start_allowed(up)) {
        ctl.remote_requested = up ? 1 : 2;
        ctl.remote_heartbeat_ms = clock_ms();
        accepted = true;
    }
    portEXIT_CRITICAL(&control_mux);
    return accepted ?
        control_reply(req, "202 Accepted", "{\"accepted\":true}") :
        control_reply(req, "409 Conflict", "{\"accepted\":false,\"reason\":\"interlock\"}");
}
static esp_err_t start_web(void)
{
    esp_err_t err;
#define TRY(call) do { err = (call); if (err != ESP_OK) return err; } while (0)
    TRY(esp_netif_init()); TRY(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_ap()) return ESP_FAIL;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&init)); TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_event, NULL));
    TRY(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event, NULL));
    wifi_config_t wifi = { .ap = { .authmode = WIFI_AUTH_WPA2_PSK,
        .max_connection = CONFIG_ELEVATOR_AP_MAX_CLIENTS, .channel = 1,
        .pmf_cfg = { .required = false } } };
    size_t ssid = strlen(CONFIG_ELEVATOR_AP_SSID), pass = strlen(CONFIG_ELEVATOR_AP_PASSWORD);
    if (!ssid || ssid > sizeof(wifi.ap.ssid) || pass < 8 || pass > 63) return ESP_ERR_INVALID_ARG;
    memcpy(wifi.ap.ssid, CONFIG_ELEVATOR_AP_SSID, ssid); wifi.ap.ssid_len = ssid;
    memcpy(wifi.ap.password, CONFIG_ELEVATOR_AP_PASSWORD, pass);
    TRY(esp_wifi_set_mode(WIFI_MODE_AP)); TRY(esp_wifi_set_config(WIFI_IF_AP, &wifi)); TRY(esp_wifi_start());
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192; cfg.max_open_sockets = 4; cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 2; cfg.send_wait_timeout = 2;
    httpd_handle_t server = NULL;
    TRY(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=root_control_handler},
        {.uri="/dashboard", .method=HTTP_GET, .handler=index_handler},
        {.uri="/api/status", .method=HTTP_GET, .handler=status_handler},
        {.uri="/api/health", .method=HTTP_GET, .handler=health_handler},
        {.uri="/api/control/up", .method=HTTP_POST, .handler=remote_control_handler},
        {.uri="/api/control/down", .method=HTTP_POST, .handler=remote_control_handler},
        {.uri="/api/control/stop", .method=HTTP_POST, .handler=remote_control_handler}
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) { httpd_stop(server); return err; }
    }
    return ESP_OK;
#undef TRY
}
void app_main(void)
{
    ctl = (controller_t){.state=BOOT, .fault=NONE, .now_ms=clock_ms(),
        .top={.pin=TOP_PIN,.active_level=CONFIG_ELEVATOR_LIMIT_TOP_ACTIVE_LEVEL},
        .bottom={.pin=BOTTOM_PIN,.active_level=CONFIG_ELEVATOR_LIMIT_BOTTOM_ACTIVE_LEVEL},
        .button={.pin=BUTTON_PIN,.active_level=CONFIG_ELEVATOR_BUTTON_ACTIVE_LEVEL}};
    if (!initialize_gpio()) fail(GPIO_CONFIGURATION);
    ctl.nvs_ok = load_calibration();
    if (!ctl.nvs_ok) fail(NVS_OR_CALIBRATION_INVALID);
    esp_err_t web_error = start_web();
    portENTER_CRITICAL(&control_mux);
    ctl.web_ok = web_error == ESP_OK;
    if (!ctl.web_ok) fail(AP_OR_HTTP_FAILURE);
    portEXIT_CRITICAL(&control_mux);
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback=control_tick, .name="elevator_control"};
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, CONFIG_ELEVATOR_SAMPLE_MS * 1000ULL) != ESP_OK) {
        fail(GPIO_CONFIGURATION);
        ESP_LOGE(TAG, "Controle indisponivel; frenagem solicitada, sem confirmacao eletrica");
        return;
    }
    ESP_LOGI(TAG, "AP %s, HTTP somente leitura; confirmar LED e retencao mecanica", CONFIG_ELEVATOR_AP_SSID);
    fault_t logged_fault = NONE;
    bool logged_gpio_failure = false;
    /* Reuse IDF main task. Flash writes occur only when stationary. */
    for (;;) {
        save_calibration();
        portENTER_CRITICAL(&control_mux);
        fault_t fault = ctl.fault; state_t when = ctl.fault_state; uint64_t at = ctl.fault_ms;
        bool gpio_failed = ctl.gpio_write_failed; int pin = ctl.failed_gpio_pin, err = ctl.gpio_error;
        portEXIT_CRITICAL(&control_mux);
        if (fault != NONE && fault != logged_fault) {
            ESP_LOGE(TAG, "%s: %s; state=%s time_ms=%"PRIu64, fault_names[fault], fault_messages[fault], state_names[when], at);
            logged_fault = fault;
        }
        if (gpio_failed && !logged_gpio_failure) {
            ESP_LOGE(TAG, "Escrita GPIO%d falhou: err=%d; nivel eletrico desconhecido", pin, err);
            logged_gpio_failure = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif
