#pragma once
#include "ring_buffer.h"

/* Starts the Modbus-RTU slave task. It attaches its own reader to the ring,
 * so it neither disturbs nor is disturbed by the logger. */
void modbus_task_start(telemetry_ring_t *ring);
