#!/usr/bin/env bash
# Установка quizhint и плагина для панели Omarchy.
# Запускать из каталога со скачанным репозиторием: ./install.sh
set -euo pipefail

cd "$(dirname "$0")"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# Зависимости для сборки (Arch / Omarchy).
deps=(base-devel wayland wayland-protocols cairo curl libjpeg-turbo inotify-tools)
if command -v pacman >/dev/null; then
  missing=()
  for p in "${deps[@]}"; do pacman -Qq "$p" >/dev/null 2>&1 || pacman -Qqg "$p" >/dev/null 2>&1 || missing+=("$p"); done
  if ((${#missing[@]})); then
    say "Ставлю пакеты: ${missing[*]}"
    sudo pacman -S --needed --noconfirm "${missing[@]}"
  fi
else
  say "Не Arch: убедитесь, что установлены компилятор C++, make, pkg-config, wayland-scanner,"
  say "заголовки wayland-client, wayland-protocols, cairo, libcurl, libturbojpeg и inotify-tools."
fi

say "Сборка"
make -j"$(nproc)"

say "Установка программы в ~/.local/bin/quizhint"
make install

if command -v omarchy-shell >/dev/null; then
  say "Установка плагина для панели Omarchy"
  make install-plugin
  sleep 2
  omarchy plugin enable noxqd.quizhint || true
  say "Готово. Нажмите на лампочку на панели, выберите Claude или Gemini и вставьте ключ API."
else
  say "Omarchy не найден — плагин не ставлю. Настройка без плагина описана в README.md."
fi
