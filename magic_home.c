/*
* This is an example of an rgb led strip using Magic Home wifi controller
*
* Debugging printf statements and UART are disabled below because it interfere with mutipwm
* you can uncomment them for debug purposes
*
* more info about the controller and flashing can be found here:
* https://github.com/arendst/Sonoff-Tasmota/wiki/MagicHome-LED-strip-controller
*
* Contributed April 2018 by https://github.com/PCSaito
*/

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <math.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>

#include "multipwm.h"

#define LPF_SHIFT 4  // divide by 16
#define LPF_INTERVAL 10  // in milliseconds

#define WHITE_PWM_PIN 15
#define RED_PWM_PIN 12
#define GREEN_PWM_PIN 5
#define BLUE_PWM_PIN 13

#define LED_RGB_SCALE 256       // this is the scaling factor used for color conversion

typedef union {
    struct {
        uint16_t blue;
        uint16_t green;
        uint16_t red;
        uint16_t white;
    };
    uint64_t color;
} rgb_color_t;

// Color smoothing variables
rgb_color_t current_color = { { 0, 0, 0, 0 } };
rgb_color_t target_color = { { 0, 0, 0, 0 } };

// Global variables
float led_hue = 0;              // hue is scaled 0 to 360
float led_saturation = 59;      // saturation is scaled 0 to 100
float led_brightness = 100;     // brightness is scaled 0 to 100
bool led_on = false;            // on is boolean on or off

//http://blog.saikoled.com/post/44677718712/how-to-convert-from-hsi-to-rgb-white
void hsi2rgbw(float h, float s, float i, rgb_color_t* rgbw) {
    uint16_t r, g, b, w;
    float cos_h, cos_1047_h;
    //h = fmod(h,360); // cycle h around to 0-360 degrees
    h = 3.14159*h/(float)180; // Convert to radians.
    s /=(float)100; i/=(float)100; //from percentage to ratio
    s = s>0?(s<1?s:1):0; // clamp s and i to interval [0,1]
    i = i>0?(i<1?i:1):0;
    i = i*sqrt(i); //shape intensity to have finer granularity near 0

    if(h < 2.09439) {
        cos_h = cos(h);
        cos_1047_h = cos(1.047196667-h);
        r = s*LED_RGB_SCALE*i/3*(1+cos_h/cos_1047_h);
        g = s*LED_RGB_SCALE*i/3*(1+(1-cos_h/cos_1047_h));
        b = 0;
        w = LED_RGB_SCALE*(1-s)*i;
    } else if(h < 4.188787) {
        h = h - 2.09439;
        cos_h = cos(h);
        cos_1047_h = cos(1.047196667-h);
        g = s*LED_RGB_SCALE*i/3*(1+cos_h/cos_1047_h);
        b = s*LED_RGB_SCALE*i/3*(1+(1-cos_h/cos_1047_h));
        r = 0;
        w = LED_RGB_SCALE*(1-s)*i;
    } else {
        h = h - 4.188787;
        cos_h = cos(h);
        cos_1047_h = cos(1.047196667-h);
        b = s*LED_RGB_SCALE*i/3*(1+cos_h/cos_1047_h);
        r = s*LED_RGB_SCALE*i/3*(1+(1-cos_h/cos_1047_h));
        g = 0;
        w = LED_RGB_SCALE*(1-s)*i;
    }

    rgbw->red=r;
    rgbw->green=g;
    rgbw->blue=b;
    rgbw->white=w;
}

void led_identify_task(void *_args) {
    printf("LED identify\n");

    rgb_color_t color = target_color;
    rgb_color_t black_color = { { 0, 0, 0, 0 } };
    rgb_color_t white_color = { { 128, 128, 128, 128 } };

    for (int i=0; i<3; i++) {
        for (int j=0; j<2; j++) {
            target_color = white_color;
            vTaskDelay(100 / portTICK_PERIOD_MS);

            target_color = black_color;
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        vTaskDelay(250 / portTICK_PERIOD_MS);
    }

    target_color = color;

    vTaskDelete(NULL);
}

void led_identify(homekit_value_t _value) {
    xTaskCreate(led_identify_task, "LED identify", 128, NULL, 2, NULL);
}

homekit_value_t led_on_get() {
    return HOMEKIT_BOOL(led_on);
}

void led_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        // printf("Invalid on-value format: %d\n", value.format);
        return;
    }

    led_on = value.bool_value;
}

homekit_value_t led_brightness_get() {
    return HOMEKIT_INT(led_brightness);
}

void led_brightness_set(homekit_value_t value) {
    if (value.format != homekit_format_int) {
        // printf("Invalid brightness-value format: %d\n", value.format);
        return;
    }
    led_brightness = value.int_value;
}

homekit_value_t led_hue_get() {
    return HOMEKIT_FLOAT(led_hue);
}

void led_hue_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        // printf("Invalid hue-value format: %d\n", value.format);
        return;
    }
    led_hue = value.float_value;
}

homekit_value_t led_saturation_get() {
    return HOMEKIT_FLOAT(led_saturation);
}

void led_saturation_set(homekit_value_t value) {
    if (value.format != homekit_format_float) {
        // printf("Invalid sat-value format: %d\n", value.format);
        return;
    }
    led_saturation = value.float_value;
}

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "LED Strip");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id = 1, .category = homekit_accessory_category_lightbulb, .services = (homekit_service_t*[]) {
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics = (homekit_characteristic_t*[]) {
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "DL2ZZ"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "7001"),
            HOMEKIT_CHARACTERISTIC(MODEL, "Magic Home Led Strip RGBW"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary = true, .characteristics = (homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, "LED Strip"),
            HOMEKIT_CHARACTERISTIC(
                ON, true,
                .getter = led_on_get,
                .setter = led_on_set
            ),
            HOMEKIT_CHARACTERISTIC(
                BRIGHTNESS, 100,
                .getter = led_brightness_get,
                .setter = led_brightness_set
            ),
            HOMEKIT_CHARACTERISTIC(
                HUE, 0,
                .getter = led_hue_get,
                .setter = led_hue_set
            ),
            HOMEKIT_CHARACTERISTIC(
                SATURATION, 0,
                .getter = led_saturation_get,
                .setter = led_saturation_set
            ),
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",    //changed tobe valid
    .setupId = "1AA1"
};

IRAM void multipwm_task(void *pvParameters) {
    const TickType_t xPeriod = pdMS_TO_TICKS(LPF_INTERVAL);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    uint8_t pins[] = {RED_PWM_PIN, GREEN_PWM_PIN, BLUE_PWM_PIN, WHITE_PWM_PIN};

    pwm_info_t pwm_info;
    pwm_info.channels = 4;

    multipwm_init(&pwm_info);
    //multipwm_set_freq(&pwm_info, 65535);
    for (uint8_t i=0; i<pwm_info.channels; i++) {
        multipwm_set_pin(&pwm_info, i, pins[i]);
    }

    while(1) {
        if (led_on) {
            // convert HSI to RGBW
            hsi2rgbw(led_hue, led_saturation, led_brightness, &target_color);
        } else {
            target_color.red = 0;
            target_color.green = 0;
            target_color.blue = 0;
            target_color.white = 0;
        }

        current_color.red += ((target_color.red * 256) - current_color.red) >> LPF_SHIFT ;
        current_color.green += ((target_color.green * 256) - current_color.green) >> LPF_SHIFT ;
        current_color.blue += ((target_color.blue * 256) - current_color.blue) >> LPF_SHIFT ;
        current_color.white += ((target_color.white * 256) - current_color.white) >> LPF_SHIFT ;

        multipwm_stop(&pwm_info);
        multipwm_set_duty(&pwm_info, 0, current_color.red);
        multipwm_set_duty(&pwm_info, 1, current_color.green);
        multipwm_set_duty(&pwm_info, 2, current_color.blue);
        multipwm_set_duty(&pwm_info, 3, current_color.white);
        multipwm_start(&pwm_info);

        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

void on_wifi_ready() {
    homekit_server_init(&config);
}

void user_init(void) {
    //uart_set_baud(0, 115200);

    // This example shows how to use same firmware for multiple similar accessories
    // without name conflicts. It uses the last 3 bytes of accessory's MAC address as
    // accessory name suffix.
    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(STATION_IF, macaddr);
    int name_len = snprintf(NULL, 0, "LED Strip-%02X%02X%02X", macaddr[1], macaddr[2], macaddr[3]);
    char *name_value = malloc(name_len + 1);
    snprintf(name_value, name_len + 1, "LED Strip-%02X%02X%02X", macaddr[1], macaddr[2], macaddr[3]);
    name.value = HOMEKIT_STRING(name_value);

    wifi_config_init("MagicHome Led Strip", NULL, on_wifi_ready);

    xTaskCreate(multipwm_task, "multipwm", 256, NULL, 2, NULL);
}
