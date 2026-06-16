#ifndef H_TERMINAL_UI
#define H_TERMINAL_UI

#include <stddef.h>
#include <stdint.h>

void terminal_ui_init(void);
void terminal_ui_shutdown(const char *final_message);
int terminal_ui_is_enabled(void);

void terminal_ui_set_server_info(
  int port,
  int max_players,
  int view_distance,
  int gamemode,
  int chunk_cache_size,
  const char *skin_backend,
  const char *motd
);

void terminal_ui_log(const char *fmt, ...);
void terminal_ui_set_log_file(const char *path);
void terminal_ui_record_client_connect(void);
void terminal_ui_record_client_disconnect(int cause);
void terminal_ui_record_packet_in(size_t bytes);
void terminal_ui_record_packet_out(size_t bytes, int is_chunk_packet, int queued);
void terminal_ui_record_tick(int64_t tick_duration_us);
void terminal_ui_render(void);
void terminal_ui_maybe_render(void);

#endif
