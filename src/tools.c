#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_timer.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
#endif

#include "globals.h"
#include "varnum.h"
#include "procedures.h"
#include "tools.h"
#include <zlib.h>
#include <stdlib.h>

#ifndef htonll
  static uint64_t htonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32);
  #else
    return value;
  #endif
  }
#endif

// Keep track of the total amount of bytes received with recv_all
// Helps notice misread packets and clean up after errors
uint64_t total_bytes_received = 0;

// Outbound packet queue node
struct OutPacket {
  uint8_t *data;
  uint32_t len;
  uint32_t packet_id;
  uint32_t generation;
  uint8_t low_priority;
  OutPacket *next;
};

static pthread_t sender_threads[MAX_PLAYERS];
static uint8_t sender_workers_running = 0;
static THREAD_LOCAL int current_packet_id = -1;

// Queue limits - initialized from config in main()
static size_t max_client_send_queue_bytes = 6 * 1024 * 1024;
static size_t max_client_chunk_queue_bytes = 2 * 1024 * 1024;

void set_queue_limits(size_t send_limit, size_t chunk_limit) {
  max_client_send_queue_bytes = send_limit;
  max_client_chunk_queue_bytes = chunk_limit;
}

static ssize_t send_all_no_disconnect (int client_fd, const void *buf, ssize_t len);

static ClientState* get_client_state_by_fd(int client_fd) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (client_states[i].client_fd == client_fd) {
      return &client_states[i];
    }
  }
  return NULL;
}

// On desktop/server builds, task_yield() is a no-op. Back off a little when
// non-blocking sockets return EAGAIN/EWOULDBLOCK to avoid CPU spin loops.
static inline void socket_io_backoff(void) {
  #ifdef ESP_PLATFORM
  task_yield();
  #else
  usleep(1000);  // 1ms
  #endif
}

#ifdef DEV_LOG_CHUNK_STREAM_STATS
typedef struct {
  int64_t window_start_us;
  uint32_t stream_requests;
  uint32_t skipped_backpressure;
  uint32_t skipped_cache_miss;
  uint32_t enqueue_ok;
  uint32_t enqueue_fail;
  uint32_t queue_reject;
  uint32_t sent_ok;
  uint32_t sent_fail;
  uint64_t enqueued_bytes;
  uint64_t sent_bytes;
  size_t max_chunk_queue_bytes;
  int last_request_x;
  int last_request_z;
  int last_enqueue_x;
  int last_enqueue_z;
  uint8_t has_last_request;
  uint8_t has_last_enqueue;
} ChunkDebugStats;

static ChunkDebugStats chunk_debug_stats[MAX_PLAYERS];
static pthread_mutex_t chunk_debug_mutex = PTHREAD_MUTEX_INITIALIZER;

static void reset_chunk_debug_window(ChunkDebugStats *stats, int64_t now_us) {
  stats->window_start_us = now_us;
  stats->stream_requests = 0;
  stats->skipped_backpressure = 0;
  stats->skipped_cache_miss = 0;
  stats->enqueue_ok = 0;
  stats->enqueue_fail = 0;
  stats->queue_reject = 0;
  stats->sent_ok = 0;
  stats->sent_fail = 0;
  stats->enqueued_bytes = 0;
  stats->sent_bytes = 0;
  stats->max_chunk_queue_bytes = 0;
  stats->has_last_request = 0;
  stats->has_last_enqueue = 0;
}

static void maybe_flush_chunk_debug_locked(int idx, int64_t now_us) {
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  if (stats->window_start_us == 0) {
    reset_chunk_debug_window(stats, now_us);
    return;
  }
  if (now_us - stats->window_start_us < 1000000) return;

  uint8_t had_activity =
    stats->stream_requests ||
    stats->skipped_backpressure ||
    stats->skipped_cache_miss ||
    stats->enqueue_ok ||
    stats->enqueue_fail ||
    stats->queue_reject ||
    stats->sent_ok ||
    stats->sent_fail ||
    stats->enqueued_bytes ||
    stats->sent_bytes;

  if (had_activity) {
    int client_fd = client_states[idx].client_fd;
    printf(
      "[chunk-debug] slot=%d fd=%d req=%u bp=%u miss=%u enq_ok=%u enq_fail=%u q_reject=%u enq_kb=%.1f sent_ok=%u sent_fail=%u sent_kb=%.1f max_q_kb=%.1f",
      idx,
      client_fd,
      stats->stream_requests,
      stats->skipped_backpressure,
      stats->skipped_cache_miss,
      stats->enqueue_ok,
      stats->enqueue_fail,
      stats->queue_reject,
      stats->enqueued_bytes / 1024.0,
      stats->sent_ok,
      stats->sent_fail,
      stats->sent_bytes / 1024.0,
      stats->max_chunk_queue_bytes / 1024.0
    );
    if (stats->has_last_request) {
      printf(" last_req=(%d,%d)", stats->last_request_x, stats->last_request_z);
    }
    if (stats->has_last_enqueue) {
      printf(" last_enq=(%d,%d)", stats->last_enqueue_x, stats->last_enqueue_z);
    }
    printf("\n");
  }

  reset_chunk_debug_window(stats, now_us);
}

static int get_chunk_debug_index(int client_fd) {
  int idx = getClientIndex(client_fd);
  if (idx < 0 || idx >= MAX_PLAYERS) return -1;
  return idx;
}

static void chunk_debug_record_queue_reject(int client_fd, size_t chunk_queue_bytes) {
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  stats->queue_reject++;
  if (chunk_queue_bytes > stats->max_chunk_queue_bytes) {
    stats->max_chunk_queue_bytes = chunk_queue_bytes;
  }
  pthread_mutex_unlock(&chunk_debug_mutex);
}

static void chunk_debug_record_chunk_enqueued(int client_fd, size_t packet_len, size_t chunk_queue_bytes) {
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  stats->enqueued_bytes += packet_len;
  if (chunk_queue_bytes > stats->max_chunk_queue_bytes) {
    stats->max_chunk_queue_bytes = chunk_queue_bytes;
  }
  pthread_mutex_unlock(&chunk_debug_mutex);
}
#endif

static int writeVarIntToBuffer(uint32_t value, uint8_t *out) {
  int n = 0;
  while (true) {
    if ((value & ~SEGMENT_BITS) == 0) {
      out[n++] = (uint8_t)value;
      return n;
    }
    out[n++] = (uint8_t)((value & SEGMENT_BITS) | CONTINUE_BIT);
    value >>= 7;
  }
}

static void free_client_queue_locked(ClientState *state) {
  OutPacket *curr = state->send_head_hi;
  while (curr) {
    OutPacket *next = curr->next;
    free(curr->data);
    free(curr);
    curr = next;
  }
  curr = state->send_head_lo;
  while (curr) {
    OutPacket *next = curr->next;
    free(curr->data);
    free(curr);
    curr = next;
  }
  state->send_head_hi = NULL;
  state->send_tail_hi = NULL;
  state->send_head_lo = NULL;
  state->send_tail_lo = NULL;
  state->queued_bytes = 0;
  state->queued_chunk_bytes = 0;
}

static int enqueue_packet_bytes(int client_fd, uint8_t *data, uint32_t len, uint8_t low_priority, uint32_t packet_id) {
  ClientState *state = get_client_state_by_fd(client_fd);
  if (state == NULL) return -1;

  OutPacket *pkt = (OutPacket *)malloc(sizeof(OutPacket));
  if (pkt == NULL) return -1;

  pkt->data = data;
  pkt->len = len;
  pkt->packet_id = packet_id;
  pkt->low_priority = low_priority;
  pkt->next = NULL;

  pthread_mutex_lock(&state->send_mutex);

  // Slot may have been reassigned between lookup and locking.
  if (state->client_fd != client_fd) {
    pthread_mutex_unlock(&state->send_mutex);
    free(pkt);
    return -1;
  }

  pkt->generation = state->connection_generation;

  // Keep queue bounded.
  if (state->queued_bytes + len > max_client_send_queue_bytes) {
    pthread_mutex_unlock(&state->send_mutex);
    free(pkt);
    return -1;
  }

  // Keep chunk queue separately bounded to avoid long chunk backlogs.
  if (low_priority && state->queued_chunk_bytes + len > max_client_chunk_queue_bytes) {
    #ifdef DEV_LOG_CHUNK_STREAM_STATS
    chunk_debug_record_queue_reject(client_fd, state->queued_chunk_bytes);
    #endif
    pthread_mutex_unlock(&state->send_mutex);
    free(pkt);
    return -1;
  }

  if (low_priority) {
    if (state->send_tail_lo == NULL) {
      state->send_head_lo = pkt;
      state->send_tail_lo = pkt;
    } else {
      state->send_tail_lo->next = pkt;
      state->send_tail_lo = pkt;
    }
    state->queued_chunk_bytes += len;
    #ifdef DEV_LOG_CHUNK_STREAM_STATS
    chunk_debug_record_chunk_enqueued(client_fd, len, state->queued_chunk_bytes);
    #endif
  } else {
    if (state->send_tail_hi == NULL) {
      state->send_head_hi = pkt;
      state->send_tail_hi = pkt;
    } else {
      state->send_tail_hi->next = pkt;
      state->send_tail_hi = pkt;
    }
  }
  state->queued_bytes += len;

  pthread_cond_signal(&state->send_cond);
  pthread_mutex_unlock(&state->send_mutex);
  return 0;
}

static void* packet_sender_worker(void *arg) {
  intptr_t idx = (intptr_t)arg;
  ClientState *state = &client_states[idx];

  while (true) {
    pthread_mutex_lock(&state->send_mutex);
    while (sender_workers_running &&
           state->send_head_hi == NULL &&
           state->send_head_lo == NULL) {
      pthread_cond_wait(&state->send_cond, &state->send_mutex);
    }

    if (!sender_workers_running &&
        state->send_head_hi == NULL &&
        state->send_head_lo == NULL) {
      pthread_mutex_unlock(&state->send_mutex);
      break;
    }

    OutPacket *pkt = state->send_head_hi;
    uint8_t from_chunk_queue = 0;
    if (pkt != NULL) {
      state->send_head_hi = pkt->next;
      if (state->send_head_hi == NULL) state->send_tail_hi = NULL;
    } else {
      pkt = state->send_head_lo;
      from_chunk_queue = 1;
      state->send_head_lo = pkt->next;
      if (state->send_head_lo == NULL) state->send_tail_lo = NULL;
    }

    if (state->queued_bytes >= pkt->len) state->queued_bytes -= pkt->len;
    else state->queued_bytes = 0;
    if (from_chunk_queue) {
      if (state->queued_chunk_bytes >= pkt->len) state->queued_chunk_bytes -= pkt->len;
      else state->queued_chunk_bytes = 0;
    }

    int client_fd = state->client_fd;
    uint32_t generation = state->connection_generation;
    pthread_mutex_unlock(&state->send_mutex);

    int send_ok = 0;
    if (client_fd != -1 && pkt->generation == generation) {
      int send_fd = client_fd;
      #ifndef _WIN32
      send_fd = dup(client_fd);
      #endif

      if (send_fd != -1) {
        send_ok = send_all_no_disconnect(send_fd, pkt->data, pkt->len) == (ssize_t)pkt->len;
        #ifndef _WIN32
        close(send_fd);
        #endif
      }
    }

    if (from_chunk_queue) {
      chunk_debug_record_sent_chunk_packet(client_fd, pkt->len, send_ok);
    }
    #ifdef DEV_LOG_ALL_PACKETS
    printf(
      "[pkt-send] fd=%d id=0x%X wire=%u prio=%s ok=%d\n",
      client_fd,
      pkt->packet_id,
      pkt->len,
      pkt->low_priority ? "chunk" : "normal",
      send_ok
    );
    #endif

    free(pkt->data);
    free(pkt);
  }

  return NULL;
}

ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  // If we have data in the input buffer, read from it first
  if (in_packet_buffer_len > 0) {
    size_t to_read = n;
    if (to_read > (size_t)(in_packet_buffer_len - in_packet_buffer_offset)) {
      to_read = in_packet_buffer_len - in_packet_buffer_offset;
    }
    memcpy(buf, in_packet_buffer + in_packet_buffer_offset, to_read);
    in_packet_buffer_offset += to_read;
    if (in_packet_buffer_offset >= in_packet_buffer_len) {
      in_packet_buffer_len = 0;
      in_packet_buffer_offset = 0;
    }
    return to_read;
  }

  char *p = buf;
  size_t total = 0;

  // Track time of last meaningful network update
  // Used to handle timeout when client is stalling
  int64_t last_update_time = get_program_time();

  // If requested, exit early when first byte not immediately available
  if (require_first) {
    ssize_t r = recv(client_fd, p, 1, MSG_PEEK);
    if (r <= 0) {
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; // no first byte available yet
      }
      return -1; // error or connection closed
    }
  }

  // Busy-wait (with task yielding) until we get exactly n bytes
  while (total < n) {
    ssize_t r = recv(client_fd, p + total, n - total, 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // handle network timeout
      if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
        disconnectClient(&client_fd, -1);
        return -1;
      }
      socket_io_backoff();
      continue;
    } else {
      total_bytes_received += total;
      return -1; // real error
      }
    } else if (r == 0) {
      // connection closed before full read
      total_bytes_received += total;
      return total;
    }
    total += r;
    last_update_time = get_program_time();
  }

  total_bytes_received += total;
  return total; // got exactly n bytes
}

ssize_t send_all (int client_fd, const void *buf, ssize_t len) {
  // If we are in packet mode, buffer the data instead of sending it
  if (packet_mode) {
    if (packet_buffer_offset + len > MAX_PACKET_LEN) {
      printf("ERROR: Packet buffer overflow (%d + %zd > %d)\n", packet_buffer_offset, (size_t)len, MAX_PACKET_LEN);
      return -1;
    }
    memcpy(packet_buffer + packet_buffer_offset, buf, len);
    packet_buffer_offset += len;
    return len;
  }

  // Treat any input buffer as *uint8_t for simplicity
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;

  // Track time of last meaningful network update
  // Used to handle timeout when client is stalling
  int64_t last_update_time = get_program_time();

  // Busy-wait (with task yielding) until all data has been sent
  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) { // some data was sent, log it
      sent += n;
      last_update_time = get_program_time();
      continue;
    }
    if (n == 0) { // connection was closed, treat this as an error
      errno = ECONNRESET;
      return -1;
    }
    // not yet ready to transmit, try again
    #ifdef _WIN32 //handles windows socket timeout
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      // handle network timeout
      if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
        disconnectClient(&client_fd, -2);
        return -1;
      }
      socket_io_backoff();
      continue;
    }
    return -1; // real error
  }

  return sent;
}

// Same as send_all, but does not call disconnectClient on timeout/error.
// Used by packet sender workers on duplicated fds.
static ssize_t send_all_no_disconnect (int client_fd, const void *buf, ssize_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;
  int64_t last_update_time = get_program_time();

  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) {
      sent += n;
      last_update_time = get_program_time();
      continue;
    }
    if (n == 0) {
      errno = ECONNRESET;
      return -1;
    }
    #ifdef _WIN32
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
        return -1;
      }
      socket_io_backoff();
      continue;
    }
    return -1;
  }

  return sent;
}

void startPacket (int client_fd, int packet_id) {
  (void)client_fd;
  packet_buffer_offset = 0;
  packet_mode = true;
  current_packet_id = packet_id;
  writeVarInt(-1, packet_id);
}

int endPacket (int client_fd) {
  packet_mode = false;
  int packet_id = current_packet_id;
  current_packet_id = -1;
  uint8_t is_chunk_packet = packet_id == 0x27;

  int threshold = getCompressionThreshold(client_fd);
  int uncompressed_len = packet_buffer_offset;
  uint8_t *wire_buf = NULL;
  uint32_t wire_len = 0;

  if (threshold > 0 && uncompressed_len >= threshold) {
    // Compressed format: [Packet Length] [Data Length] [Compressed(Packet ID + Data)]

    // 1. Compress the data
    // Zlib may produce output larger than input for incompressible data, so allocate generously
    size_t max_compressed_len = compressBound(uncompressed_len);
    uint8_t *compressed_buf = malloc(max_compressed_len);
    if (!compressed_buf) {
      perror("Failed to allocate compression buffer");
      packet_buffer_offset = 0;
      return 1;
    }

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = uncompressed_len;
    strm.next_in = packet_buffer;
    strm.avail_out = max_compressed_len;
    strm.next_out = compressed_buf;

    // Chunk packets dominate outbound bandwidth and CPU time while moving.
    // Use best-speed compression for chunk packets to keep the game loop responsive.
    int compression_level = is_chunk_packet ? Z_BEST_SPEED : Z_DEFAULT_COMPRESSION;
    int ret = deflateInit(&strm, compression_level);
    if (ret != Z_OK) {
      fprintf(stderr, "deflateInit failed: %d\n", ret);
      free(compressed_buf);
      packet_buffer_offset = 0;
      return 1;
    }

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
      fprintf(stderr, "deflate failed: %d\n", ret);
      // Fallback: send uncompressed
      deflateEnd(&strm);
      free(compressed_buf);
      // Fall through to uncompressed path below
      goto send_uncompressed;
    }

    // Verify all input was compressed
    if (strm.avail_in != 0) {
      fprintf(stderr, "deflate did not consume all input: %u bytes remaining\n", strm.avail_in);
      deflateEnd(&strm);
      free(compressed_buf);
      goto send_uncompressed;
    }

    int compressed_len = strm.total_out;

    // Verify zlib header is present (first two bytes should be valid)
    if (compressed_len < 2) {
      fprintf(stderr, "Compression produced invalid output (too short: %d bytes)\n", compressed_len);
      deflateEnd(&strm);
      free(compressed_buf);
      goto send_uncompressed;
    }

    deflateEnd(&strm);

    // 2. Build wire packet
    int data_length_size = sizeVarInt(uncompressed_len);
    int packet_length = data_length_size + compressed_len;
    int packet_length_size = sizeVarInt(packet_length);
    wire_len = packet_length_size + data_length_size + compressed_len;
    wire_buf = (uint8_t *)malloc(wire_len);
    if (!wire_buf) {
      free(compressed_buf);
      packet_buffer_offset = 0;
      return 1;
    }
    int off = 0;
    off += writeVarIntToBuffer(packet_length, wire_buf + off);
    off += writeVarIntToBuffer(uncompressed_len, wire_buf + off);
    memcpy(wire_buf + off, compressed_buf, compressed_len);

    free(compressed_buf);
    packet_buffer_offset = 0;
    int enq_res = enqueue_packet_bytes(client_fd, wire_buf, wire_len, is_chunk_packet, packet_id);
    #ifdef DEV_LOG_ALL_PACKETS
    printf(
      "[pkt-out] fd=%d id=0x%X payload=%d wire=%u prio=%s enq=%s\n",
      client_fd,
      packet_id,
      uncompressed_len,
      wire_len,
      is_chunk_packet ? "chunk" : "normal",
      enq_res == 0 ? "ok" : "drop"
    );
    #endif
    if (enq_res != 0) {
      free(wire_buf);
      return 1;
    }
    return 0;

send_uncompressed:
    threshold = 0;  // Force uncompressed path
  }

  if (threshold > 0) {
    // Compressed format (but not compressed): [Packet Length] [0] [Packet ID + Data]
    int data_length_size = sizeVarInt(0);
    int packet_length = data_length_size + uncompressed_len;
    int packet_length_size = sizeVarInt(packet_length);
    wire_len = packet_length_size + data_length_size + uncompressed_len;
    wire_buf = (uint8_t *)malloc(wire_len);
    if (!wire_buf) {
      packet_buffer_offset = 0;
      return 1;
    }
    int off = 0;
    off += writeVarIntToBuffer(packet_length, wire_buf + off);
    off += writeVarIntToBuffer(0, wire_buf + off);
    memcpy(wire_buf + off, packet_buffer, uncompressed_len);
  } else {
    // Uncompressed format: [Packet Length] [Packet ID + Data]
    int packet_length_size = sizeVarInt(uncompressed_len);
    wire_len = packet_length_size + uncompressed_len;
    wire_buf = (uint8_t *)malloc(wire_len);
    if (!wire_buf) {
      packet_buffer_offset = 0;
      return 1;
    }
    int off = 0;
    off += writeVarIntToBuffer(uncompressed_len, wire_buf + off);
    memcpy(wire_buf + off, packet_buffer, uncompressed_len);
  }

  packet_buffer_offset = 0;
  int enq_res = enqueue_packet_bytes(client_fd, wire_buf, wire_len, is_chunk_packet, packet_id);
  #ifdef DEV_LOG_ALL_PACKETS
  printf(
    "[pkt-out] fd=%d id=0x%X payload=%d wire=%u prio=%s enq=%s\n",
    client_fd,
    packet_id,
    uncompressed_len,
    wire_len,
    is_chunk_packet ? "chunk" : "normal",
    enq_res == 0 ? "ok" : "drop"
  );
  #endif
  if (enq_res != 0) {
    free(wire_buf);
    return 1;
  }
  return 0;
}

void init_packet_sender_workers (void) {
  if (sender_workers_running) return;
  sender_workers_running = 1;
  for (intptr_t i = 0; i < MAX_PLAYERS; i++) {
    pthread_create(&sender_threads[i], NULL, packet_sender_worker, (void *)i);
  }
}

void shutdown_packet_sender_workers (void) {
  if (!sender_workers_running) return;
  sender_workers_running = 0;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    pthread_mutex_lock(&client_states[i].send_mutex);
    pthread_cond_broadcast(&client_states[i].send_cond);
    pthread_mutex_unlock(&client_states[i].send_mutex);
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    pthread_join(sender_threads[i], NULL);
  }
}

void clear_client_send_queue (int client_fd) {
  ClientState *state = get_client_state_by_fd(client_fd);
  if (state == NULL) return;
  pthread_mutex_lock(&state->send_mutex);
  free_client_queue_locked(state);
  pthread_mutex_unlock(&state->send_mutex);
}

size_t get_client_send_queue_bytes (int client_fd) {
  ClientState *state = get_client_state_by_fd(client_fd);
  if (state == NULL) return 0;
  pthread_mutex_lock(&state->send_mutex);
  // This function is used by chunk streaming backpressure checks, so it
  // intentionally reports low-priority chunk backlog only.
  size_t queued = state->queued_chunk_bytes;
  pthread_mutex_unlock(&state->send_mutex);
  return queued;
}

void chunk_debug_record_stream_request (int client_fd, int chunk_x, int chunk_z) {
  #ifndef DEV_LOG_CHUNK_STREAM_STATS
  (void)client_fd;
  (void)chunk_x;
  (void)chunk_z;
  return;
  #else
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  stats->stream_requests++;
  stats->last_request_x = chunk_x;
  stats->last_request_z = chunk_z;
  stats->has_last_request = 1;
  pthread_mutex_unlock(&chunk_debug_mutex);
  #endif
}

void chunk_debug_record_backpressure_skip (int client_fd, size_t queue_bytes) {
  #ifndef DEV_LOG_CHUNK_STREAM_STATS
  (void)client_fd;
  (void)queue_bytes;
  return;
  #else
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  stats->skipped_backpressure++;
  if (queue_bytes > stats->max_chunk_queue_bytes) {
    stats->max_chunk_queue_bytes = queue_bytes;
  }
  pthread_mutex_unlock(&chunk_debug_mutex);
  #endif
}

void chunk_debug_record_cache_miss_skip (int client_fd) {
  #ifndef DEV_LOG_CHUNK_STREAM_STATS
  (void)client_fd;
  return;
  #else
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  chunk_debug_stats[idx].skipped_cache_miss++;
  pthread_mutex_unlock(&chunk_debug_mutex);
  #endif
}

void chunk_debug_record_enqueue_result (int client_fd, int chunk_x, int chunk_z, int ok) {
  #ifndef DEV_LOG_CHUNK_STREAM_STATS
  (void)client_fd;
  (void)chunk_x;
  (void)chunk_z;
  (void)ok;
  return;
  #else
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  if (ok) {
    stats->enqueue_ok++;
    stats->last_enqueue_x = chunk_x;
    stats->last_enqueue_z = chunk_z;
    stats->has_last_enqueue = 1;
  } else {
    stats->enqueue_fail++;
  }
  #ifdef DEV_LOG_CHUNK_STREAM_VERBOSE
  printf(
    "[chunk-debug-verbose] fd=%d chunk=(%d,%d) enqueue=%s\n",
    client_fd,
    chunk_x,
    chunk_z,
    ok ? "ok" : "fail"
  );
  #endif
  pthread_mutex_unlock(&chunk_debug_mutex);
  #endif
}

void chunk_debug_record_sent_chunk_packet (int client_fd, size_t packet_len, int ok) {
  #ifndef DEV_LOG_CHUNK_STREAM_STATS
  (void)client_fd;
  (void)packet_len;
  (void)ok;
  return;
  #else
  int idx = get_chunk_debug_index(client_fd);
  if (idx < 0) return;
  pthread_mutex_lock(&chunk_debug_mutex);
  int64_t now_us = get_program_time();
  maybe_flush_chunk_debug_locked(idx, now_us);
  ChunkDebugStats *stats = &chunk_debug_stats[idx];
  if (ok) {
    stats->sent_ok++;
    stats->sent_bytes += packet_len;
  } else {
    stats->sent_fail++;
  }
  pthread_mutex_unlock(&chunk_debug_mutex);
  #endif
}

void sendPreformattedPackets (int client_fd, uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    // 1. Read packet length (VarInt)
    uint32_t packet_len = 0;
    int shift = 0;
    while (true) {
      uint8_t byte = data[offset++];
      packet_len |= (byte & SEGMENT_BITS) << shift;
      if (!(byte & CONTINUE_BIT)) break;
      shift += 7;
    }

    // 2. Extract packet ID (VarInt)
    uint32_t packet_id = 0;
    shift = 0;
    int id_start_offset = offset;
    while (true) {
      uint8_t byte = data[offset++];
      packet_id |= (byte & SEGMENT_BITS) << shift;
      if (!(byte & CONTINUE_BIT)) break;
      shift += 7;
    }
    int id_size = offset - id_start_offset;

    // 3. Send as a new packet
    startPacket(client_fd, packet_id);
    send_all(client_fd, data + offset, packet_len - id_size);
    endPacket(client_fd);

    offset += (packet_len - id_size);
  }
}

void discard_all (int client_fd, size_t remaining, uint8_t require_first) {
  while (remaining > 0) {
    size_t recv_n = remaining > MAX_RECV_BUF_LEN ? MAX_RECV_BUF_LEN : remaining;
    ssize_t received = recv_all(client_fd, recv_buffer, recv_n, require_first);
    if (received < 0) return;
    if (received > remaining) return;
    remaining -= received;
    require_first = false;
  }
}

ssize_t writeByte (int client_fd, uint8_t byte) {
  return send_all(client_fd, &byte, 1);
}
ssize_t writeUint16 (int client_fd, uint16_t num) {
  uint16_t be = htons(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint32 (int client_fd, uint32_t num) {
  uint32_t be = htonl(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint64 (int client_fd, uint64_t num) {
  uint64_t be = htonll(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeFloat (int client_fd, float num) {
  uint32_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonl(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}
ssize_t writeDouble (int client_fd, double num) {
  uint64_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonll(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}

uint8_t readByte (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 1, false);
  return recv_buffer[0];
}
uint16_t readUint16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((uint16_t)recv_buffer[0] << 8) | recv_buffer[1];
}
int16_t readInt16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((int16_t)recv_buffer[0] << 8) | (int16_t)recv_buffer[1];
}
uint32_t readUint32 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 4, false);
  return ((uint32_t)recv_buffer[0] << 24) |
         ((uint32_t)recv_buffer[1] << 16) |
         ((uint32_t)recv_buffer[2] << 8) |
         ((uint32_t)recv_buffer[3]);
}
uint64_t readUint64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((uint64_t)recv_buffer[0] << 56) |
         ((uint64_t)recv_buffer[1] << 48) |
         ((uint64_t)recv_buffer[2] << 40) |
         ((uint64_t)recv_buffer[3] << 32) |
         ((uint64_t)recv_buffer[4] << 24) |
         ((uint64_t)recv_buffer[5] << 16) |
         ((uint64_t)recv_buffer[6] << 8) |
         ((uint64_t)recv_buffer[7]);
}
int64_t readInt64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((int64_t)recv_buffer[0] << 56) |
         ((int64_t)recv_buffer[1] << 48) |
         ((int64_t)recv_buffer[2] << 40) |
         ((int64_t)recv_buffer[3] << 32) |
         ((int64_t)recv_buffer[4] << 24) |
         ((int64_t)recv_buffer[5] << 16) |
         ((int64_t)recv_buffer[6] << 8) |
         ((int64_t)recv_buffer[7]);
}
float readFloat (int client_fd) {
  uint32_t bytes = readUint32(client_fd);
  float output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}
double readDouble (int client_fd) {
  uint64_t bytes = readUint64(client_fd);
  double output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}

// Receive length prefixed data with bounds checking
ssize_t readLengthPrefixedData (int client_fd) {
  uint32_t length = readVarInt(client_fd);
  if (length >= MAX_RECV_BUF_LEN) {
    printf("ERROR: Received length (%lu) exceeds maximum (%u)\n", length, MAX_RECV_BUF_LEN);
    disconnectClient(&client_fd, -1);
    recv_count = 0;
    return 0;
  }
  return recv_all(client_fd, recv_buffer, length, false);
}

// Reads a networked string into recv_buffer
void readString (int client_fd) {
  recv_count = readLengthPrefixedData(client_fd);
  recv_buffer[recv_count] = '\0';
}
// Reads a networked string of up to N bytes into recv_buffer
void readStringN (int client_fd, uint32_t max_length) {
  // Forward to readString if max length is invalid
  if (max_length >= MAX_RECV_BUF_LEN) {
    readString(client_fd);
    return;
  }
  // Attempt to read full string within maximum
  uint32_t length = readVarInt(client_fd);
  if (max_length > length) {
    recv_count = recv_all(client_fd, recv_buffer, length, false);
    recv_buffer[recv_count] = '\0';
    return;
  }
  // Read string up to maximum, dump the rest
  recv_count = recv_all(client_fd, recv_buffer, max_length, false);
  recv_buffer[recv_count] = '\0';
  uint8_t dummy;
  for (uint32_t i = max_length; i < length; i ++) {
    recv_all(client_fd, &dummy, 1, false);
  }
}

uint32_t fast_rand () {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64 (uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

#ifndef ESP_PLATFORM
// Returns system time in microseconds.
// On ESP-IDF, this is available in "esp_timer.h", and returns time *since
// the start of the program*, and NOT wall clock time. To ensure
// compatibility, this should only be used to measure time intervals.
int64_t get_program_time () {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif
