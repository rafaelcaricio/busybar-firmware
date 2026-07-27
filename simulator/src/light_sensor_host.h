#pragma once

/** Create the simulated light sensor's firmware event record. */
void light_sensor_host_init(void);

/** Report the simulator's bright environment through the firmware pubsub. */
void light_sensor_host_publish_max(void);
