#ifndef FALL_NETWORK_H
#define FALL_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

// Start the ESP32 station-mode Wi-Fi connection. This is non-blocking.
// Creates the async network task (new-link mode) or just Wi-Fi (legacy mode).
bool fall_network_init(void);
bool fall_network_is_connected(void);

// Unified entry: submit one fall event.
//   New-link mode (FALL_ENABLE_DIRECT_SERVER=1): enqueues and returns
//   immediately; the network task retries with backoff until the server
//   confirms (task book 6.1). Returns true when accepted into the queue.
//   Legacy mode (=0): synchronous POST to the PC receiver (old behavior).
bool fall_network_submit_fall_event(float radar_x,
                                    float radar_y,
                                    float radar_z,
                                    int fall_votes,
                                    int total_frames);

// Legacy synchronous POST (kept for the compile-time fallback path).
bool fall_network_post_result(float radar_x,
                              float radar_y,
                              float radar_z,
                              int fall_votes,
                              int total_frames);

// Number of events waiting in RAM queue (diagnostics).
uint32_t fall_network_pending_count(void);

#endif
