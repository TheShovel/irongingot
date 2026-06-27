#ifndef H_TERMINAL_UI
#define H_TERMINAL_UI

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static inline void terminal_ui_init(void) {}
static inline void terminal_ui_shutdown(const char *m) { (void)m; }
static inline int  terminal_ui_is_enabled(void) { return 0; }

static inline void terminal_ui_set_server_info(int p, int mp, int vd, int gm, int cs, const char *sk, const char *mo) {
  (void)p; (void)mp; (void)vd; (void)gm; (void)cs; (void)sk; (void)mo;
}

static inline void terminal_ui_log(const char *fmt, ...) {
  char buf[2048];
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  int off = (int)(size_t)strftime(buf, 32, "[%H:%M:%S] ", &tm);
  va_list ap;
  va_start(ap, fmt);
  off += vsnprintf(buf + off, sizeof(buf) - (size_t)off - 1, fmt, ap);
  va_end(ap);
  if (off > 0 && buf[off - 1] != '\n') { buf[off++] = '\n'; buf[off] = '\0'; }
  fputs(buf, stdout);
  fflush(stdout);
}

static inline void terminal_ui_set_log_file(const char *path)   { (void)path; }
static inline void terminal_ui_record_client_connect(void)       {}
static inline void terminal_ui_record_client_disconnect(int c)   { (void)c; }
static inline void terminal_ui_record_packet_in(size_t bytes)    { (void)bytes; }
static inline void terminal_ui_record_packet_out(size_t b, int ch, int q) { (void)b; (void)ch; (void)q; }
static inline void terminal_ui_record_tick(int64_t us)           { (void)us; }
static inline void terminal_ui_render(void)                      {}
static inline void terminal_ui_maybe_render(void)                {}

#endif
