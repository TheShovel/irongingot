#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef _WIN32
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/wait.h>
#endif

#include "config.h"
#include "mojang.h"

#ifdef MOJANG_SKIN_LOOKUP_AVAILABLE
  #include <curl/curl.h>
#endif

#define MOJANG_BACKEND_NONE 0
#define MOJANG_BACKEND_LIBCURL 1
#define MOJANG_BACKEND_CURL_COMMAND 2
#define CURL_COMMAND_PATH_MAX 512

typedef struct {
  char *data;
  size_t len;
  size_t capacity;
} HttpBuffer;

static uint8_t mojang_backend = MOJANG_BACKEND_NONE;
static char curl_command_path[CURL_COMMAND_PATH_MAX] = {0};

static uint8_t isSafeSkinName(const char *name) {
  if (!name || !name[0]) return false;
  for (int i = 0; name[i] != '\0'; i++) {
    if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return false;
  }
  return true;
}

static const char *skipWhitespace(const char *ptr) {
  while (*ptr && isspace((unsigned char)*ptr)) ptr++;
  return ptr;
}

static void uuidToHex(const uint8_t *uuid, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[i * 2] = hex[(uuid[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[uuid[i] & 0x0F];
  }
  out[32] = '\0';
}

static void clearTextureFields(PlayerAppearance *appearance) {
  appearance->texture_value[0] = '\0';
  appearance->texture_signature[0] = '\0';
  appearance->texture_value_len = 0;
  appearance->texture_signature_len = 0;
  appearance->has_texture = false;
  appearance->has_signature = false;
}

static uint8_t uuidIsZero(const uint8_t *uuid) {
  for (int i = 0; i < 16; i++) {
    if (uuid[i] != 0) return false;
  }
  return true;
}

static uint8_t extractJsonStringField(const char *start, const char *field, char *out, size_t out_capacity) {
  if (!start || !field || !out || out_capacity == 0) return false;

  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", field);

  const char *cursor = start;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    cursor += strlen(needle);
    cursor = skipWhitespace(cursor);
    if (*cursor != ':') continue;
    cursor = skipWhitespace(cursor + 1);
    if (*cursor != '"') continue;
    cursor++;

    size_t len = 0;
    while (*cursor) {
      if (*cursor == '"') {
        out[len] = '\0';
        return true;
      }
      if (*cursor == '\\') {
        cursor++;
        if (!*cursor) break;
      }
      if (len + 1 >= out_capacity) return false;
      out[len++] = *cursor++;
    }
    return false;
  }

  return false;
}

#ifdef MOJANG_SKIN_LOOKUP_AVAILABLE
static size_t curlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  HttpBuffer *buffer = (HttpBuffer *)userp;
  size_t total = size * nmemb;
  if (buffer->len + total + 1 > buffer->capacity) return 0;

  memcpy(buffer->data + buffer->len, contents, total);
  buffer->len += total;
  buffer->data[buffer->len] = '\0';
  return total;
}
#endif

#ifndef _WIN32
static uint8_t setCurlCommandPath(const char *path) {
  if (!path || !path[0]) return false;
  if (access(path, X_OK) != 0) return false;

  strncpy(curl_command_path, path, sizeof(curl_command_path) - 1);
  curl_command_path[sizeof(curl_command_path) - 1] = '\0';
  return true;
}

static uint8_t detectCurlCommand(void) {
  if (setCurlCommandPath("/usr/bin/curl")) return true;
  if (setCurlCommandPath("/bin/curl")) return true;

  const char *path_env = getenv("PATH");
  if (!path_env || !path_env[0]) return false;

  char *path_copy = malloc(strlen(path_env) + 1);
  if (!path_copy) return false;
  strcpy(path_copy, path_env);

  char *saveptr = NULL;
  for (char *dir = strtok_r(path_copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
    char candidate[CURL_COMMAND_PATH_MAX];
    if (snprintf(candidate, sizeof(candidate), "%s/curl", dir) >= (int)sizeof(candidate)) continue;
    if (setCurlCommandPath(candidate)) {
      free(path_copy);
      return true;
    }
  }

  free(path_copy);
  return false;
}
#endif

static uint8_t httpGet(const char *url, char *response, size_t response_capacity) {
  if (response_capacity == 0 || mojang_backend == MOJANG_BACKEND_NONE) return false;
  response[0] = '\0';

#ifdef MOJANG_SKIN_LOOKUP_AVAILABLE
  if (mojang_backend == MOJANG_BACKEND_LIBCURL) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    HttpBuffer buffer = {
      .data = response,
      .len = 0,
      .capacity = response_capacity,
    };

    long timeout_ms = config.mojang_api_timeout_ms;
    if (timeout_ms < 250) timeout_ms = 250;
    long connect_timeout_ms = timeout_ms / 2;
    if (connect_timeout_ms < 250) connect_timeout_ms = 250;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "irongingot/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    return result == CURLE_OK && response_code == 200 && buffer.len > 0;
  }
#endif

#ifndef _WIN32
  if (mojang_backend == MOJANG_BACKEND_CURL_COMMAND) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]);
      close(pipefd[1]);
      return false;
    }

    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);

      FILE *null_file = fopen("/dev/null", "w");
      if (null_file) dup2(fileno(null_file), STDERR_FILENO);

      close(pipefd[1]);

      long timeout_ms = config.mojang_api_timeout_ms;
      if (timeout_ms < 250) timeout_ms = 250;
      long connect_timeout_ms = timeout_ms / 2;
      if (connect_timeout_ms < 250) connect_timeout_ms = 250;

      char connect_timeout_s[16];
      char max_time_s[16];
      snprintf(connect_timeout_s, sizeof(connect_timeout_s), "%ld", (connect_timeout_ms + 999) / 1000);
      snprintf(max_time_s, sizeof(max_time_s), "%ld", (timeout_ms + 999) / 1000);

      execl(
        curl_command_path, curl_command_path,
        "-fsSL",
        "--connect-timeout", connect_timeout_s,
        "--max-time", max_time_s,
        "--user-agent", "irongingot/1.0",
        url,
        (char *)NULL
      );
      _exit(127);
    }

    close(pipefd[1]);

    size_t total = 0;
    ssize_t bytes_read;
    while ((bytes_read = read(pipefd[0], response + total, response_capacity - total - 1)) > 0) {
      total += (size_t)bytes_read;
      if (total + 1 >= response_capacity) break;
    }
    close(pipefd[0]);
    response[total] = '\0';

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    return total > 0;
  }
#endif

  return false;
}

static uint8_t fetchSessionProfileByUuidHex(const char *uuid_hex, PlayerAppearance *appearance) {
  char url[256];
  char response[8192];
  snprintf(
    url, sizeof(url),
    "https://sessionserver.mojang.com/session/minecraft/profile/%s?unsigned=false",
    uuid_hex
  );

  if (!httpGet(url, response, sizeof(response))) return false;

  const char *properties = strstr(response, "\"properties\"");
  if (!properties) return false;
  if (!extractJsonStringField(properties, "value", appearance->texture_value, sizeof(appearance->texture_value))) {
    return false;
  }

  appearance->texture_value_len = (uint16_t)strlen(appearance->texture_value);
  appearance->has_texture = appearance->texture_value_len > 0;
  if (!appearance->has_texture) return false;

  if (extractJsonStringField(properties, "signature", appearance->texture_signature, sizeof(appearance->texture_signature))) {
    appearance->texture_signature_len = (uint16_t)strlen(appearance->texture_signature);
    appearance->has_signature = appearance->texture_signature_len > 0;
  }

  return true;
}

static uint8_t resolveUuidByName(const char *name, char *uuid_hex_out, size_t uuid_hex_capacity) {
  static const char *urls[] = {
    "https://api.minecraftservices.com/minecraft/profile/lookup/name/%s",
    "https://api.mojang.com/users/profiles/minecraft/%s",
  };
  char url[256];
  char response[1024];

  if (!isSafeSkinName(name)) return false;

  for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
    snprintf(url, sizeof(url), urls[i], name);
    if (!httpGet(url, response, sizeof(response))) continue;
    if (!extractJsonStringField(response, "id", uuid_hex_out, uuid_hex_capacity)) continue;
    if (strlen(uuid_hex_out) == 32) return true;
  }

  return false;
}

void init_mojang_api(void) {
  mojang_backend = MOJANG_BACKEND_NONE;

  if (!config.fetch_skins_from_mojang) return;

#ifdef MOJANG_SKIN_LOOKUP_AVAILABLE
  CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (result == CURLE_OK) {
    mojang_backend = MOJANG_BACKEND_LIBCURL;
    return;
  }
  printf("Mojang skin lookup: libcurl init failed, trying external curl command.\n");
#endif

#ifndef _WIN32
  if (detectCurlCommand()) {
    mojang_backend = MOJANG_BACKEND_CURL_COMMAND;
    printf("Mojang skin lookup: using external curl command backend (%s).\n", curl_command_path);
    return;
  }
#endif

  printf("Mojang skin lookup: no HTTP backend available, feature disabled.\n");
}

void shutdown_mojang_api(void) {
#ifdef MOJANG_SKIN_LOOKUP_AVAILABLE
  if (mojang_backend == MOJANG_BACKEND_LIBCURL) {
    curl_global_cleanup();
  }
#endif
  mojang_backend = MOJANG_BACKEND_NONE;
}

uint8_t mojang_skin_lookup_supported(void) {
  return mojang_backend != MOJANG_BACKEND_NONE;
}

const char *mojang_skin_backend_name(void) {
  switch (mojang_backend) {
    case MOJANG_BACKEND_LIBCURL:
      return "libcurl";
    case MOJANG_BACKEND_CURL_COMMAND:
      return "curl command";
    default:
      return "not available";
  }
}

uint8_t fetchMojangPlayerAppearance(const uint8_t *uuid, const char *name, PlayerAppearance *appearance) {
  clearTextureFields(appearance);

  if (!config.fetch_skins_from_mojang || mojang_backend == MOJANG_BACKEND_NONE) return false;

  char uuid_hex[33];
  if (!uuidIsZero(uuid)) {
    uuidToHex(uuid, uuid_hex);
    if (fetchSessionProfileByUuidHex(uuid_hex, appearance)) return true;
  }

  if (!isSafeSkinName(name)) return false;
  if (!resolveUuidByName(name, uuid_hex, sizeof(uuid_hex))) return false;
  return fetchSessionProfileByUuidHex(uuid_hex, appearance);
}
