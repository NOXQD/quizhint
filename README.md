# quizhint

Подсказка для викторин под Linux/Wayland. По горячей клавише программа снимает экран, отправляет снимок
vision-модели и показывает в левом верхнем углу номер правильного варианта маленькой цифрой — или, в режиме
полного ответа, короткий, но полный ответ текстом. Отдельная клавиша позволяет выделить мышью нужный участок
экрана и спросить только про него.

Написано на C++20 под Hyprland (Omarchy). Должно работать и в других композиторах с wlr-layer-shell и
захватом экрана (Sway, niri, river…). В GNOME и KDE не заработает.

## Как это работает

1. `quizhint` работает в фоне и ничего не делает, пока не придёт сигнал: `SIGUSR1` — снять весь экран,
   `SIGRTMIN+1` — выделить область мышью (рамку рисует `slurp`). Сигналы отправляют горячие клавиши Hyprland
   (по умолчанию F1 и F3). Экран снимается только в этот момент — постоянной записи или снимков по таймеру нет.
2. Активный монитор (или только активное окно) снимается напрямую через Wayland: `ext-image-copy-capture-v1`,
   запасной вариант — `wlr-screencopy`. Курсор на снимок не попадает, внешние утилиты не нужны.
3. Снимок уменьшается до 1568 px по длинной стороне и сжимается в JPEG (~100–200 КБ).
4. Запрос уходит к Claude (Anthropic API), Gemini (Google AI Studio) или к любому OpenAI-совместимому API: OpenAI,
   OpenRouter, Groq, локальная Ollama. Модель просят ответить только номером.
5. Номер рисуется в слое overlay: поверх всех окон, в том числе полноэкранных. Фокус не забирается, поэтому
   таймер викторины не сбивается, а клики проходят сквозь цифру. Через 5 секунд она исчезает.

| На экране | Значение |
|-----------|----------|
| `•`       | снимок отправлен, ждём ответ |
| `2`       | правильный — второй вариант сверху (в сетке 2×2 — слева направо, затем следующая строка) |
| `1,3`     | правильных вариантов несколько |
| текст     | режим полного ответа: короткий ответ на вопрос или задачу (пропадает через 20 с) |
| `?`       | модель не нашла на экране вопрос |
| `!`       | ошибка, подробности в логе |

Повторное нажатие во время запроса отменяет его и снимает экран заново. Это удобно, если нажали раньше,
чем загрузился вопрос.

## Сборка

Нужны пакеты (в Omarchy они уже стоят):

```sh
sudo pacman -S --needed base-devel wayland wayland-protocols cairo pango curl libjpeg-turbo slurp
make
make install          # → ~/.local/bin/quizhint
```

## Omarchy: плагин для панели

Проще всего пользоваться через плагин: на панели появится лампочка, а по клику откроется панель с
включателем, последним ответом и настройками (Claude или Gemini, модель, ключ API, что снимать, горячая клавиша).

```sh
make install install-plugin
omarchy plugin enable noxqd.quizhint
```

Плагин сам держит quizhint в фоне и сам назначает горячую клавишу (по умолчанию F1), поэтому раздел ниже
в этом случае не нужен. Подробнее — в [omarchy/README.md](omarchy/README.md).

## Без плагина

```sh
mkdir -p ~/.config/quizhint
cp config.example ~/.config/quizhint/config
chmod 600 ~/.config/quizhint/config
```

Впишите `api_key` (или задайте `ANTHROPIC_API_KEY`). По умолчанию используется `claude-haiku-5-5` — самая быстрая
модель Claude. Для Gemini: `provider = gemini` и ключ из [aistudio.google.com/apikey](https://aistudio.google.com/apikey)
(или `GEMINI_API_KEY`), модель по умолчанию — `gemini-flash-latest`. Остальные параметры описаны в `config.example`.

Горячая клавиша и автозапуск в Omarchy — `~/.config/hypr/bindings.lua`:

```lua
o.bind("F1", "Quiz hint", "pkill -USR1 -x quizhint")
o.bind("F3", "Quiz hint: область", "pkill -RTMIN+1 -x quizhint")
```

`~/.config/hypr/autostart.lua`:

```lua
o.launch_on_start(os.getenv("HOME") .. "/.local/bin/quizhint")
```

Чтобы не перезаходить в сессию, запустите программу сразу: `setsid -f ~/.local/bin/quizhint`.

Hyprland перехватывает клавишу раньше программ, поэтому, пока бинд есть, F1 не открывает справку в браузере.

Для обычного Hyprland с `hyprland.conf`:

```
bind = , F1, exec, pkill -USR1 -x quizhint
bind = , F3, exec, pkill -RTMIN+1 -x quizhint
exec-once = ~/.local/bin/quizhint
```

## Проверка

```sh
quizhint --show 3                       # как выглядит цифра
quizhint --once -v                      # снимок → ответ в терминал, с замерами времени
quizhint --once --save /tmp/shot.jpg    # посмотреть, что именно уходит модели
quizhint --image вопрос.png             # проверить модель на готовом скриншоте
```

## Управление

```sh
pkill -USR1 -x quizhint     # спросить про весь экран
pkill -RTMIN+1 -x quizhint  # выделить область мышью и спросить про неё (повторно — отмена)
pkill -USR2 -x quizhint     # спрятать ответ / отменить запрос
pkill -x quizhint           # выйти
```

Режим ответа — `answer_mode = number` (цифра) или `full` (полный ответ) в конфиге, в плагине — переключатель
«Ответ». Внешний вид (угол экрана, размеры, цвета, шрифт) и стиль ответа (длина, пояснение, язык, свои указания)
настраиваются в `config.example` или на странице «Настройки» плагина. Для пробы без конфига: `quizhint --once --mode full`, `quizhint --once --region`.

Для скриптов (этим же пользуется плагин):

```sh
quizhint --print-config                 # настройки в JSON, без самого ключа
quizhint --set capture=window           # поменять параметр, комментарии в файле сохраняются
echo "api_key=sk-ant-..." | quizhint --set-stdin   # то же через stdin — ключ не виден в списке процессов
quizhint --events                       # фоновый режим + события в stdout, по JSON-объекту на строку
```

## Если что-то не так

- Остановите фоновый процесс (`pkill -x quizhint`), запустите `quizhint -v` в терминале и нажмите клавишу:
  в логе видно, что происходит и сколько занимает каждый шаг.
- `HTTP 401` — неверный ключ; `HTTP 404` — неверное имя модели или `base_url`.
- Ошибка захвата экрана: если в Hyprland включено `ecosystem:enforce_permissions`, разрешите quizhint
  захват экрана. Ещё можно попробовать `capture_protocol = wlr`.
- Цифра мешает или её плохо видно — настройте `font_size`, `color`, `background`, `margin_*`, `over_panels`.

Снимок экрана целиком уходит выбранному провайдеру. Если это нежелательно, используйте `capture = window`
(только активное окно) или локальную модель через Ollama.

## Устройство

| Файл | Что делает |
|------|------------|
| `src/main.cpp` | цикл событий: Wayland, сигналы (signalfd), таймеры (timerfd) и HTTP в одном `curl_multi_poll`, без потоков |
| `src/capture.cpp` | снимок монитора: ext-image-copy-capture / wlr-screencopy |
| `src/overlay.cpp` | плашка с цифрой или текстом: wlr-layer-shell + cairo/pango, дробный масштаб через wp-fractional-scale |
| `src/image.cpp` | кадр → RGB, уменьшение, JPEG (libjpeg-turbo), base64 |
| `src/llm.cpp` | запрос к API и разбор ответа модели |
| `src/http.cpp` | обёртка над libcurl multi |
| `src/hyprland.cpp` | активный монитор и окно через IPC-сокет Hyprland |
| `src/json.cpp`, `src/config.cpp` | маленький JSON-парсер и конфиг: чтение, точечная правка файла, вывод в JSON |
| `omarchy/` | плагин для панели Omarchy: `Service.qml` держит процесс и горячую клавишу, `Panel.qml` — значок и панель |
| `protocols/` | XML протоколов из wlroots/wlr-protocols: лежат в проекте, чтобы не ставить пакет `wlr-protocols` |
