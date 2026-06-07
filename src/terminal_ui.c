#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ESP_PLATFORM
  #ifdef _WIN32
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
  #else
    #include <sys/ioctl.h>
    #include <unistd.h>
  #endif
#endif

#include "config.h"
#include "globals.h"
#include "terminal_ui.h"
#include "tools.h"

#define UI_LOG_LINES 12
#define UI_LOG_LEN 180
#define UI_RENDER_INTERVAL_US 250000
#define UI_RATE_INTERVAL_US 1000000
#define UI_MIN_WIDTH 72
#define UI_DEFAULT_WIDTH 100

#define UI_RESET "\033[0m"
#define UI_BOLD "\033[1m"
#define UI_DIM "\033[2m"
#define UI_CYAN "\033[36m"
#define UI_GREEN "\033[32m"
#define UI_YELLOW "\033[33m"
#define UI_RED "\033[31m"
#define UI_BLUE_BG "\033[48;2;91;206;250m"
#define UI_PINK_BG "\033[48;2;245;169;184m"
#define UI_WHITE_BG "\033[48;2;255;255;255m\033[30m"

typedef struct {
  int initialized;
  int enabled;
  int port;
  int max_players;
  int view_distance;
  int gamemode;
  int chunk_cache_size;
  char skin_backend[48];
  char motd[128];

  int64_t start_us;
  int64_t last_render_us;
  int64_t last_rate_us;

  uint64_t packets_in;
  uint64_t packets_out;
  uint64_t packets_out_dropped;
  uint64_t chunk_packets_out;
  uint64_t chunk_packets_dropped;
  uint64_t connections_total;
  uint64_t disconnections_total;
  int last_disconnect_cause;

  uint64_t last_bytes_in;
  uint64_t last_bytes_out;
  uint64_t last_packets_in;
  uint64_t last_packets_out;
  uint64_t last_chunks_out;
  uint64_t last_packet_drops;

  double rx_kib_s;
  double tx_kib_s;
  double packets_in_s;
  double packets_out_s;
  double chunks_out_s;
  double drops_s;
  double tps_ema;

  char logs[UI_LOG_LINES][UI_LOG_LEN];
  int log_start;
  int log_count;
} TerminalUiState;

static TerminalUiState ui;
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ui_atexit_registered = 0;

static int64_t ui_now_us(void) {
  #ifdef ESP_PLATFORM
  return get_program_time();
  #else
  return get_program_time();
  #endif
}

static void restore_terminal_at_exit(void) {
  if (ui.enabled) {
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    ui.enabled = 0;
  }
}

static int stdout_is_interactive(void) {
  #ifdef ESP_PLATFORM
  return 0;
  #else
  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) return 0;
  return isatty(fileno(stdout));
  #endif
}

static int terminal_width(void) {
  #ifdef ESP_PLATFORM
  return UI_DEFAULT_WIDTH;
  #elif defined(_WIN32)
  return UI_DEFAULT_WIDTH;
  #else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= UI_MIN_WIDTH) {
    return ws.ws_col;
  }
  return UI_DEFAULT_WIDTH;
  #endif
}

static void timestamp(char *out, size_t out_len) {
  time_t now = time(NULL);
  struct tm tm_now;
  #ifdef _WIN32
  localtime_s(&tm_now, &now);
  #else
  localtime_r(&now, &tm_now);
  #endif
  snprintf(out, out_len, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

static void format_duration(uint64_t seconds, char *out, size_t out_len) {
  uint64_t days = seconds / 86400;
  seconds %= 86400;
  uint64_t hours = seconds / 3600;
  seconds %= 3600;
  uint64_t minutes = seconds / 60;
  seconds %= 60;

  if (days > 0) {
    snprintf(out, out_len, "%llud %02llu:%02llu:%02llu",
      (unsigned long long)days,
      (unsigned long long)hours,
      (unsigned long long)minutes,
      (unsigned long long)seconds);
  } else {
    snprintf(out, out_len, "%02llu:%02llu:%02llu",
      (unsigned long long)hours,
      (unsigned long long)minutes,
      (unsigned long long)seconds);
  }
}

static void plain_log_unlocked(const char *line) {
  fputs(line, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void store_log_unlocked(const char *line) {
  int idx;
  if (ui.log_count < UI_LOG_LINES) {
    idx = (ui.log_start + ui.log_count) % UI_LOG_LINES;
    ui.log_count++;
  } else {
    idx = ui.log_start;
    ui.log_start = (ui.log_start + 1) % UI_LOG_LINES;
  }

  snprintf(ui.logs[idx], UI_LOG_LEN, "%s", line);
}

static void fit_plain(char *dst, size_t dst_len, const char *src, int width) {
  if (dst_len == 0) return;
  if (width < 0) width = 0;
  if ((size_t)width >= dst_len) width = (int)dst_len - 1;

  int len = src ? (int)strlen(src) : 0;
  if (len > width) len = width;
  if (len > 0) memcpy(dst, src, (size_t)len);
  for (int i = len; i < width; i++) dst[i] = ' ';
  dst[width] = '\0';
}

static void print_rule(int width, char edge, char fill) {
  fputc(edge, stdout);
  for (int i = 0; i < width - 2; i++) fputc(fill, stdout);
  fputc(edge, stdout);
  fputc('\n', stdout);
}

static void print_box_line(int width, const char *text) {
  char line[256];
  int inner = width - 4;
  if (inner < 0) inner = 0;
  if (inner > (int)sizeof(line) - 1) inner = (int)sizeof(line) - 1;
  fit_plain(line, sizeof(line), text ? text : "", inner);
  printf("| %s |\n", line);
}

static void print_flag_line(int width, const char *color, const char *text) {
  int text_len = text ? (int)strlen(text) : 0;
  int left = 0;
  int right = width;

  if (text_len > 0 && text_len < width) {
    left = (width - text_len) / 2;
    right = width - left - text_len;
  } else {
    text_len = 0;
    text = "";
    right = width;
  }

  fputs(color, stdout);
  for (int i = 0; i < left; i++) fputc(' ', stdout);
  fputs(text, stdout);
  for (int i = 0; i < right; i++) fputc(' ', stdout);
  fputs(UI_RESET "\n", stdout);
}

static void update_rates_unlocked(int64_t now_us) {
  if (ui.last_rate_us == 0) {
    ui.last_rate_us = now_us;
    ui.last_bytes_in = total_bytes_received;
    ui.last_bytes_out = total_bytes_sent;
    ui.last_packets_in = ui.packets_in;
    ui.last_packets_out = ui.packets_out;
    ui.last_chunks_out = ui.chunk_packets_out;
    ui.last_packet_drops = ui.packets_out_dropped;
    return;
  }

  int64_t elapsed_us = now_us - ui.last_rate_us;
  if (elapsed_us < UI_RATE_INTERVAL_US) return;

  double elapsed_s = elapsed_us / 1000000.0;
  uint64_t bytes_in = total_bytes_received;
  uint64_t bytes_out = total_bytes_sent;

  ui.rx_kib_s = (bytes_in - ui.last_bytes_in) / 1024.0 / elapsed_s;
  ui.tx_kib_s = (bytes_out - ui.last_bytes_out) / 1024.0 / elapsed_s;
  ui.packets_in_s = (ui.packets_in - ui.last_packets_in) / elapsed_s;
  ui.packets_out_s = (ui.packets_out - ui.last_packets_out) / elapsed_s;
  ui.chunks_out_s = (ui.chunk_packets_out - ui.last_chunks_out) / elapsed_s;
  ui.drops_s = (ui.packets_out_dropped - ui.last_packet_drops) / elapsed_s;

  ui.last_rate_us = now_us;
  ui.last_bytes_in = bytes_in;
  ui.last_bytes_out = bytes_out;
  ui.last_packets_in = ui.packets_in;
  ui.last_packets_out = ui.packets_out;
  ui.last_chunks_out = ui.chunk_packets_out;
  ui.last_packet_drops = ui.packets_out_dropped;
}

static void collect_live_stats(
  int *online_players,
  int *loaded_players,
  int *loading_players,
  int *alive_mobs,
  size_t *queue_bytes,
  size_t *chunk_queue_bytes
) {
  *online_players = 0;
  *loaded_players = 0;
  *loading_players = 0;
  *alive_mobs = 0;
  *queue_bytes = 0;
  *chunk_queue_bytes = 0;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd != -1) {
      (*online_players)++;
      if (player_data[i].flags & 0x20) (*loading_players)++;
      else (*loaded_players)++;
    }

    pthread_mutex_lock(&client_states[i].send_mutex);
    *queue_bytes += client_states[i].queued_bytes;
    *chunk_queue_bytes += client_states[i].queued_chunk_bytes;
    pthread_mutex_unlock(&client_states[i].send_mutex);
  }

  for (int i = 0; i < MAX_MOBS; i++) {
    if (mob_data[i].type == 0) continue;
    if ((mob_data[i].data & 31) == 0) continue;
    (*alive_mobs)++;
  }
}

void terminal_ui_init(void) {
  pthread_mutex_lock(&ui_mutex);
  if (ui.initialized) {
    pthread_mutex_unlock(&ui_mutex);
    return;
  }

  memset(&ui, 0, sizeof(ui));
  ui.initialized = 1;
  ui.enabled = stdout_is_interactive();
  ui.start_us = ui_now_us();
  ui.last_render_us = 0;
  ui.last_disconnect_cause = 0;
  snprintf(ui.skin_backend, sizeof(ui.skin_backend), "unknown");
  snprintf(ui.motd, sizeof(ui.motd), "A irongingot server");

  setvbuf(stdout, NULL, _IONBF, 0);

  if (ui.enabled) {
    if (!ui_atexit_registered) {
      atexit(restore_terminal_at_exit);
      ui_atexit_registered = 1;
    }
    fputs("\033[?1049h\033[?25l\033[H\033[2J", stdout);
    fflush(stdout);
  }

  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_shutdown(const char *final_message) {
  pthread_mutex_lock(&ui_mutex);
  int was_enabled = ui.enabled;
  pthread_mutex_unlock(&ui_mutex);

  if (final_message && final_message[0]) {
    terminal_ui_log("%s", final_message);
  }

  pthread_mutex_lock(&ui_mutex);
  if (ui.enabled) {
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
  }
  ui.enabled = 0;
  ui.initialized = 0;
  pthread_mutex_unlock(&ui_mutex);

  if (final_message && final_message[0] && was_enabled) {
    printf("%s\n", final_message);
  }
}

int terminal_ui_is_enabled(void) {
  pthread_mutex_lock(&ui_mutex);
  int enabled = ui.enabled;
  pthread_mutex_unlock(&ui_mutex);
  return enabled;
}

void terminal_ui_set_server_info(
  int port,
  int max_players,
  int view_distance,
  int gamemode,
  int chunk_cache_size,
  const char *skin_backend,
  const char *motd
) {
  pthread_mutex_lock(&ui_mutex);
  ui.port = port;
  ui.max_players = max_players;
  ui.view_distance = view_distance;
  ui.gamemode = gamemode;
  ui.chunk_cache_size = chunk_cache_size;
  snprintf(ui.skin_backend, sizeof(ui.skin_backend), "%s", skin_backend ? skin_backend : "unknown");
  snprintf(ui.motd, sizeof(ui.motd), "%s", motd ? motd : "");
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_log(const char *fmt, ...) {
  char message[UI_LOG_LEN - 16];
  char line[UI_LOG_LEN];
  char time_buf[16];

  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  timestamp(time_buf, sizeof(time_buf));
  snprintf(line, sizeof(line), "[%s] %s", time_buf, message);

  pthread_mutex_lock(&ui_mutex);
  if (!ui.initialized || !ui.enabled) {
    plain_log_unlocked(line);
  } else {
    store_log_unlocked(line);
  }
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_record_client_connect(void) {
  pthread_mutex_lock(&ui_mutex);
  ui.connections_total++;
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_record_client_disconnect(int cause) {
  pthread_mutex_lock(&ui_mutex);
  ui.disconnections_total++;
  ui.last_disconnect_cause = cause;
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_record_packet_in(size_t bytes) {
  (void)bytes;
  pthread_mutex_lock(&ui_mutex);
  ui.packets_in++;
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_record_packet_out(size_t bytes, int is_chunk_packet, int queued) {
  (void)bytes;
  pthread_mutex_lock(&ui_mutex);
  if (queued) {
    ui.packets_out++;
    if (is_chunk_packet) ui.chunk_packets_out++;
  } else {
    ui.packets_out_dropped++;
    if (is_chunk_packet) ui.chunk_packets_dropped++;
  }
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_record_tick(int64_t tick_duration_us) {
  if (tick_duration_us <= 0) return;
  double instant_tps = 1000000.0 / (double)tick_duration_us;
  pthread_mutex_lock(&ui_mutex);
  if (ui.tps_ema <= 0.01) ui.tps_ema = instant_tps;
  else ui.tps_ema = ui.tps_ema * 0.85 + instant_tps * 0.15;
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_render(void) {
  int online_players, loaded_players, loading_players, alive_mobs;
  size_t queue_bytes, chunk_queue_bytes;
  collect_live_stats(&online_players, &loaded_players, &loading_players, &alive_mobs, &queue_bytes, &chunk_queue_bytes);

  pthread_mutex_lock(&ui_mutex);
  if (!ui.initialized || !ui.enabled) {
    pthread_mutex_unlock(&ui_mutex);
    return;
  }

  int64_t now_us = ui_now_us();
  update_rates_unlocked(now_us);

  int width = terminal_width();
  if (width < UI_MIN_WIDTH) width = UI_MIN_WIDTH;
  if (width > 140) width = 140;

  uint64_t uptime_s = ui.start_us > 0 && now_us > ui.start_us ? (uint64_t)((now_us - ui.start_us) / 1000000) : 0;
  char uptime[32];
  format_duration(uptime_s, uptime, sizeof(uptime));

  char line[256];
  char block_capacity[32];
  #ifdef INFINITE_BLOCK_CHANGES
  snprintf(block_capacity, sizeof(block_capacity), "%d", block_changes_capacity);
  #else
  snprintf(block_capacity, sizeof(block_capacity), "%d", MAX_BLOCK_CHANGES);
  #endif

  fputs("\033[H\033[2J", stdout);
  // Server icon ASCII art (anvil-like block)
  char *server_icon[] = {
    "Irongingot server",
    "The smallest Minecraft server ever",
    NULL
  };

  for (int i = 0; server_icon[i] != NULL; i++) {
    print_flag_line(width, UI_RESET, server_icon[i]);
  }

  print_rule(width, '+', '=');
  snprintf(line, sizeof(line), "irongingot server  |  uptime %s  |  port %d  |  motd: %.72s", uptime, ui.port, ui.motd);
  print_box_line(width, line);
  print_rule(width, '+', '-');

  snprintf(line, sizeof(line),
    "players %d/%d (%d ready, %d loading)  |  mobs %d/%d  |  stored players %d/%d",
    online_players, ui.max_players > 0 ? ui.max_players : MAX_PLAYERS,
    loaded_players, loading_players,
    alive_mobs, MAX_MOBS,
    player_data_count, MAX_PLAYERS);
  print_box_line(width, line);

  snprintf(line, sizeof(line),
    "tps %.1f  |  tick %u  |  world time %u  |  blocks %d/%s",
    ui.tps_ema > 0.01 ? ui.tps_ema : 0.0,
    server_ticks,
    world_time,
    block_changes_count,
    block_capacity);
  print_box_line(width, line);

  snprintf(line, sizeof(line),
    "net rx %.1f KiB/s  tx %.1f KiB/s  |  packets %.1f/s in %.1f/s out  |  chunks %.1f/s",
    ui.rx_kib_s,
    ui.tx_kib_s,
    ui.packets_in_s,
    ui.packets_out_s,
    ui.chunks_out_s);
  print_box_line(width, line);

  snprintf(line, sizeof(line),
    "queues %.1f KiB total / %.1f KiB chunks  |  drops %.1f/s (%llu packets, %llu chunks total)",
    queue_bytes / 1024.0,
    chunk_queue_bytes / 1024.0,
    ui.drops_s,
    (unsigned long long)ui.packets_out_dropped,
    (unsigned long long)ui.chunk_packets_dropped);
  print_box_line(width, line);

  snprintf(line, sizeof(line),
    "view distance %d  |  gamemode %d  |  chunk cache %d  |  skins %s",
    ui.view_distance,
    ui.gamemode,
    ui.chunk_cache_size,
    ui.skin_backend);
  print_box_line(width, line);

  snprintf(line, sizeof(line),
    "connections %llu total  |  disconnects %llu total  |  last disconnect cause %d",
    (unsigned long long)ui.connections_total,
    (unsigned long long)ui.disconnections_total,
    ui.last_disconnect_cause);
  print_box_line(width, line);

  print_rule(width, '+', '-');
  print_box_line(width, "Logs:");
  for (int i = 0; i < UI_LOG_LINES; i++) {
    int log_index = i - (UI_LOG_LINES - ui.log_count);
    if (log_index < 0) {
      print_box_line(width, "");
      continue;
    }
    int idx = (ui.log_start + log_index) % UI_LOG_LINES;
    print_box_line(width, ui.logs[idx]);
  }
  print_rule(width, '+', '=');
  fputs(UI_DIM "Ctrl+C to stop." UI_RESET "\033[J", stdout);
  fflush(stdout);

  ui.last_render_us = now_us;
  pthread_mutex_unlock(&ui_mutex);
}

void terminal_ui_maybe_render(void) {
  pthread_mutex_lock(&ui_mutex);
  int should_render = 0;
  if (ui.initialized && ui.enabled) {
    int64_t now_us = ui_now_us();
    should_render = ui.last_render_us == 0 || now_us - ui.last_render_us >= UI_RENDER_INTERVAL_US;
  }
  pthread_mutex_unlock(&ui_mutex);

  if (should_render) terminal_ui_render();
}
