#pragma once

#include <stdbool.h>

/** Put the named scene or application on screen.
 *
 * A scene is built on a bare task; an application is handed to the desktop
 * service, which stops whatever the mode selector started and runs it instead.
 */
void scene_host_start(const char* name);

/** Whether the name is a built-in scene rather than an application.
 *
 * Scenes draw straight onto the main layer, so they and the desktop cannot
 * both be running.
 */
bool scene_host_is_demo(const char* name);

/** Print every app the simulator links, as --scene values. */
void scene_host_list_apps(void);
