#!/usr/bin/env bash
# rebuild.sh — one script to rebuild everything from scratch
#
# Downloads the MC 1.21.8 server.jar (if not present),
# dumps registry data, generates registries.c/h and village templates,
# then compiles the server binary.
#
# Prerequisites: Java 21+, Node.js, curl
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
step()  { printf "${CYAN}==>${NC} %s\n" "$*"; }
ok()    { printf "${GREEN}  OK${NC} %s\n" "$*"; }
fail()  { printf "${RED}FAIL${NC} %s\n" "$*"; exit 1; }

# ── Configuration ──────────────────────────────────────────────────────────
NOTCHIAN_DIR="notchian"
SERVER_JAR_URL="https://piston-data.mojang.com/v1/objects/6bce4ef400e4efaa63a13d5e6f6b500be969ef81/server.jar"
SERVER_JAR_SHA1="6bce4ef400e4efaa63a13d5e6f6b500be969ef81"
# If SERVER_JAR_URL contains the sha1 in the path, the download is
# content-addressable; we still do a defensive sha1sum check.

# ── Helper functions ──────────────────────────────────────────────────────

check_prereqs() {
  step "Checking prerequisites..."
  command -v java >/dev/null 2>&1 || fail "java not found (need Java 21+)"
  local jver
  jver=$(java -version 2>&1 | awk -F[\".] '/version/ {print $2}')
  if [ "$jver" -lt 21 ] 2>/dev/null; then
    fail "Java 21+ required, got Java $jver"
  fi
  ok "Java $jver"

  local js_runtime=""
  for cmd in node bun "deno run"; do
    if command -v $cmd >/dev/null 2>&1; then
      js_runtime=$cmd
      break
    fi
  done
  if [ -z "$js_runtime" ]; then
    fail "No JS runtime (need Node.js, Bun, or Deno)"
  fi
  ok "JS runtime: $js_runtime"
  echo "$js_runtime"
}

download_server() {
  step "Downloading MC 1.21.8 server.jar..."
  mkdir -p "$NOTCHIAN_DIR"

  if [ -f "$SERVER_JAR" ]; then
    local actual
    actual=$(sha1sum "$SERVER_JAR" 2>/dev/null | awk '{print $1}' || true)
    if [ "$actual" = "$SERVER_JAR_SHA1" ]; then
      ok "server.jar already present, checksum matches"
      return 0
    fi
    echo "  server.jar exists but checksum mismatch (got $actual), re-downloading..."
    rm -f "$SERVER_JAR"
  fi

  curl -fL -o "$SERVER_JAR" "$SERVER_JAR_URL" || fail "Download failed"
  local actual
  actual=$(sha1sum "$SERVER_JAR" | awk '{print $1}')
  if [ "$actual" != "$SERVER_JAR_SHA1" ]; then
    rm -f "$SERVER_JAR"
    fail "Checksum mismatch (expected $SERVER_JAR_SHA1, got $actual)"
  fi
  ok "server.jar downloaded and verified"
}

dump_registries() {
  step "Dumping registry data from vanilla server.jar..."
  (
    cd "$NOTCHIAN_DIR"
    java -DbundlerMainClass="net.minecraft.data.Main" -jar server.jar --all
  )
  ok "Registry data dumped"
}

extract_village_templates() {
  step "Extracting village structure templates..."
  (
    cd "$NOTCHIAN_DIR"

    local nested_server
    nested_server=$(jar tf server.jar 2>/dev/null | awk '/^META-INF\/versions\/.*\/server-.*\.jar$/ { print; exit }' || true)

    local structure_source="server.jar"
    if [ -n "$nested_server" ]; then
      echo "  Extracting nested server jar: $nested_server"
      rm -rf META-INF/versions
      jar xf server.jar "$nested_server"
      structure_source="$nested_server"
    fi

    local tmp="$NOTCHIAN_DIR/structure_tmp"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    local abs_structure_source="$NOTCHIAN_DIR/$structure_source"
    if [ ! -f "$abs_structure_source" ]; then
      abs_structure_source="$NOTCHIAN_DIR/server.jar"
    fi
    cd "$tmp"
    jar xf "$abs_structure_source" data/minecraft/structure/village 2>/dev/null || true

    if [ -d data/minecraft/structure/village ]; then
      mkdir -p "$NOTCHIAN_DIR/generated/data/minecraft/structure/village"
      cp -r data/minecraft/structure/village/. "$NOTCHIAN_DIR/generated/data/minecraft/structure/village/"
    fi
    rm -rf "$tmp"
    cd "$NOTCHIAN_DIR"
    rm -rf META-INF/versions
  )
  ok "Village templates extracted"
}

generate_registries() {
  step "Running build_registries.js..."
  node build_registries.js || fail "build_registries.js failed"
  ok "registries.c and registries.h regenerated"
}

generate_village_templates() {
  step "Running build_village_templates.js..."
  node build_village_templates.js || fail "build_village_templates.js failed"
  ok "generated_village_templates.c/.h regenerated"
}

build_server() {
  step "Compiling irongingot..."
  ./build.sh --musl || fail "Build failed"
  ok "Build complete: irongingot"
}

# ── Main ──────────────────────────────────────────────────────────────────

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
NOTCHIAN_DIR="$PROJECT_ROOT/notchian"
SERVER_JAR="$NOTCHIAN_DIR/server.jar"
cd "$PROJECT_ROOT"

echo
echo "╔══════════════════════════════════════════════╗"
echo "║   irongingot — Full Rebuild                 ║"
echo "╚══════════════════════════════════════════════╝"
echo

js_runtime=$(check_prereqs)
download_server
dump_registries
extract_village_templates
generate_registries
generate_village_templates
build_server

echo
printf "${GREEN}All done!${NC} Run ./irongingot to start the server.\n"
