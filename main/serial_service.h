#pragma once

#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

/* Callback payload is borrowed for the duration of the call. No UART bytes
 * are sent through this callback: they live in the bounded shared history. */
typedef void (*serial_event_cb_t)(uint32_t owner, const char *type,
                                  const char *request_id, const cJSON *payload);

esp_err_t serial_service_init(serial_event_cb_t callback);
/* A new owner (including zero) revokes all queued work of the previous owner.
 * A UART write which has already started is allowed to finish. */
void serial_service_set_owner(uint32_t owner);
/* Return NULL on acceptance, otherwise a stable machine-readable error code.
 * Called in the HTTP task: it must never wait for a UART transmission. */
const char *serial_service_command(uint32_t owner, const char *request_id,
                                  const char *type, const cJSON *payload,
                                  cJSON *reply);
cJSON *serial_service_status(void);
/* Return {records:[{seq,t_us,direction,data_b64,length,config_revision,id?}],
 * oldest_seq,latest_seq,gap,evicted_bytes}. At most max_records (1..4).
 * Caller owns the returned JSON. after_seq=0 means start at oldest. */
cJSON *serial_service_history(uint32_t after_seq, size_t max_records);
uint32_t serial_service_latest_seq(void);
