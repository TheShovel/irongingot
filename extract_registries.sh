#!/usr/bin/env bash
set -euo pipefail

REQUIRED_MAJOR=21
SERVER_JAR="${SERVER_JAR:-server.jar}"
NOTCHIAN_DIR="notchian"
JS_RUNTIME=""

get_java_version() {
  java -version 2>&1 | awk -F[\".] '/version/ {print $2}'
}

check_java() {
  if ! command -v java >/dev/null 2>&1; then
    echo "Java not found in PATH."
    exit 1
  fi

  local major
  major="$(get_java_version)"

  if (( major < REQUIRED_MAJOR )); then
    echo "Java $REQUIRED_MAJOR or newer required, but found Java $major."
    exit 1
  fi
}

prepare_notchian_dir() {
  if [[ ! -d "$NOTCHIAN_DIR" ]]; then
    echo "Creating $NOTCHIAN_DIR directory..."
    mkdir -p "$NOTCHIAN_DIR"
  fi
  cd "$NOTCHIAN_DIR"
}

dump_registries() {
  if [[ ! -f "$SERVER_JAR" ]]; then
    echo "No server.jar found (looked for $SERVER_JAR)."
		echo "Please download the 1.21.8 server.jar (e.g. from https://mcversions.net/download/1.21.8)"
		echo "and place it in the \"notchian\" directory."
    exit 1
  fi

  java -DbundlerMainClass="net.minecraft.data.Main" -jar "$SERVER_JAR" --all
}

extract_village_structures() {
  if [[ ! -f "$SERVER_JAR" ]]; then
    return
  fi

  local nested_server
  nested_server="$(jar tf "$SERVER_JAR" | awk '/^META-INF\/versions\/.*\/server-.*\.jar$/ { print; exit }')"
  local structure_source="$SERVER_JAR"

  if [[ -n "$nested_server" ]]; then
    echo "Extracting nested vanilla server component: $nested_server"
    rm -rf META-INF/versions
    jar xf "$SERVER_JAR" "$nested_server"
    structure_source="$nested_server"
  fi

  echo "Extracting vanilla village structure templates..."
  rm -rf data/minecraft/structure/village generated/data/minecraft/structure/village
  jar xf "$structure_source" data/minecraft/structure/village
  if [[ -d data/minecraft/structure/village ]]; then
    mkdir -p generated/data/minecraft/structure
    mv data/minecraft/structure/village generated/data/minecraft/structure/village
  else
    echo "Warning: no data/minecraft/structure/village templates found in $structure_source"
  fi
  rm -rf data META-INF/versions
}

detect_js_runtime() {
  if command -v node >/dev/null 2>&1; then
    JS_RUNTIME="node"
  elif command -v bun >/dev/null 2>&1; then
    JS_RUNTIME="bun"
  elif command -v deno >/dev/null 2>&1; then
    JS_RUNTIME="deno run"
  else
    echo "No JavaScript runtime found (Node.js, Bun, or Deno)."
    exit 1
  fi
}

run_js_script() {
  local script="$1"
  if [[ -z "$JS_RUNTIME" ]]; then
    detect_js_runtime
  fi
  echo "Running $script with $JS_RUNTIME..."
  $JS_RUNTIME "$script"
}

check_java
prepare_notchian_dir
dump_registries
extract_village_structures
run_js_script "../build_registries.js"
run_js_script "../build_village_templates.js"
echo "Registry dump, village template extraction, and processing complete."
