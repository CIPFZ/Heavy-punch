#include "tools_handlers.h"

#include "dht11_sensor.h"

bool tools_dht11_read_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input;
    return dht11_read(result, result_len);
}
