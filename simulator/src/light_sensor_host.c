/**
 * Host light sensor input.
 *
 * Brightness control is the real firmware service. This adapter supplies only
 * the hardware-facing state and event stream it consumes, so automatic
 * brightness follows the same pubsub, queue, conversion and display path as it
 * does on the device.
 */
#include "light_sensor_host.h"

#include <furi.h>
#include <light_sensor/light_sensor.h>

static struct {
    FuriPubSub* events;
    LightSensorLevel level;
} light_sensor_host;

void light_sensor_host_init(void) {
    light_sensor_host.events = furi_pubsub_alloc();
    light_sensor_host.level = (LightSensorLevel){LIGHT_SENSOR_LIGHT_LEVEL_MIN};
    furi_record_create(RECORD_LIGHT_SENSOR_EVENTS, light_sensor_host.events);
}

void light_sensor_host_publish_max(void) {
    furi_check(light_sensor_host.events);

    light_sensor_host.level = (LightSensorLevel){LIGHT_SENSOR_LIGHT_LEVEL_MAX};
    LightSensorEvent event = {
        .type = LightSensorEventTypeLightLevelChanged,
        .light_level = light_sensor_host.level,
    };
    furi_pubsub_publish(light_sensor_host.events, &event);
}

LightSensorLevel light_sensor_get_light_level(void) {
    furi_check(light_sensor_host.events);
    return light_sensor_host.level;
}
