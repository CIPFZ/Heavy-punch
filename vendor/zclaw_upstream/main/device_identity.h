#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>

void device_identity_get_hostname(char *buf, size_t buf_len);
void device_identity_get_setup_ssid(char *buf, size_t buf_len);

#endif // DEVICE_IDENTITY_H
