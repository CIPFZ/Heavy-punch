#ifndef AGENT_H
#define AGENT_H

#include "esp_err.h"
#include "llm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stddef.h>

typedef bool (*agent_status_cb_t)(const char *event, const char *message, void *ctx);

// Start the agent task
esp_err_t agent_start(QueueHandle_t input_queue,
                      QueueHandle_t channel_output_queue,
                      QueueHandle_t telegram_output_queue);

void agent_set_runtime_ready(bool ready);
esp_err_t agent_process_web_request(const char *user_message,
                                    char *response_buf,
                                    size_t response_buf_size);
esp_err_t agent_process_web_request_streaming(const char *user_message,
                                             char *response_buf,
                                             size_t response_buf_size,
                                             agent_status_cb_t status_cb,
                                             void *status_ctx,
                                             llm_stream_chunk_cb_t stream_cb,
                                             void *stream_ctx);

#ifdef TEST_BUILD
// Test-only helpers to drive agent logic without spawning FreeRTOS tasks.
void agent_test_reset(void);
void agent_test_set_queues(QueueHandle_t channel_output_queue,
                           QueueHandle_t telegram_output_queue);
void agent_test_process_message(const char *user_message);
void agent_test_process_message_for_chat(const char *user_message, int64_t reply_chat_id);
#endif

#endif // AGENT_H
