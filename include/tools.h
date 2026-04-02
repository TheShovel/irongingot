#ifndef H_TOOLS
#define H_TOOLS

#include <unistd.h>

#include "globals.h"

static inline int mod_abs (int a, int b) {
  return ((a % b) + b) % b;
}
static inline int div_floor (int a, int b) {
  return a % b < 0 ? (a - b) / b : a / b;
}

extern uint64_t total_bytes_received;
ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first);
ssize_t send_all (int client_fd, const void *buf, ssize_t len);

void startPacket (int client_fd, int packet_id);
int endPacket (int client_fd);
void sendPreformattedPackets (int client_fd, uint8_t *data, size_t len);
void init_packet_sender_workers (void);
void shutdown_packet_sender_workers (void);
void clear_client_send_queue (int client_fd);
size_t get_client_send_queue_bytes (int client_fd);

void chunk_debug_record_stream_request (int client_fd, int chunk_x, int chunk_z);
void chunk_debug_record_backpressure_skip (int client_fd, size_t queue_bytes);
void chunk_debug_record_cache_miss_skip (int client_fd);
void chunk_debug_record_enqueue_result (int client_fd, int chunk_x, int chunk_z, int ok);
void chunk_debug_record_sent_chunk_packet (int client_fd, size_t packet_len, int ok);

void discard_all (int client_fd, size_t remaining, uint8_t require_first);


ssize_t writeByte (int client_fd, uint8_t byte);
ssize_t writeUint16 (int client_fd, uint16_t num);
ssize_t writeUint32 (int client_fd, uint32_t num);
ssize_t writeUint64 (int client_fd, uint64_t num);
ssize_t writeFloat (int client_fd, float num);
ssize_t writeDouble (int client_fd, double num);

uint8_t readByte (int client_fd);
uint16_t readUint16 (int client_fd);
int16_t readInt16 (int client_fd);
uint32_t readUint32 (int client_fd);
uint64_t readUint64 (int client_fd);
int64_t readInt64 (int client_fd);
float readFloat (int client_fd);
double readDouble (int client_fd);

ssize_t readLengthPrefixedData (int client_fd);
void readString (int client_fd);
void readStringN (int client_fd, uint32_t max_length);

uint32_t fast_rand ();
uint64_t splitmix64 (uint64_t state);

#ifdef ESP_PLATFORM
  #include "esp_timer.h"
  #define get_program_time esp_timer_get_time
#else
  int64_t get_program_time ();
#endif

#endif
