#ifndef DHT11_SENSOR_H
#define DHT11_SENSOR_H

#include <stdbool.h>
#include <stddef.h>

bool dht11_read(char *result, size_t result_len);

#endif // DHT11_SENSOR_H
