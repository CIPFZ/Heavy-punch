#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

esp_err_t web_server_start(void);

/* Sends one text frame to a single WebSocket client. Safe from any task. */
void web_server_send(int fd, const char *text);

/* Broadcasts a status line to every WebSocket client, appending ":1" to the
 * current controller and ":0" to everyone else. Safe from any task. */
void web_server_broadcast_status(const char *text);

#endif
