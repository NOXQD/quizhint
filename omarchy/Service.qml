import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland

// Сервис плагина: держит фоновый quizhint (дочерний процесс с --events),
// горячую клавишу Hyprland и настройки из ~/.config/quizhint/config.
// Один экземпляр на сессию; виджеты на панелях всех мониторов читают его состояние.
Item {
  id: root

  property var shell: null
  property var manifest: null

  // ---- настройки (quizhint --print-config)
  property var config: ({})
  property bool configLoaded: false
  readonly property string configError: config.ok === false ? String(config.error || "") : ""
  readonly property string configPath: String(config.path || "")
  readonly property string hotkey: String(config.hotkey || "")
  readonly property bool autostart: config.autostart !== false

  // Включатель панели: сразу показывает новое положение, пока настройка сохраняется.
  property int _desired: -1
  readonly property bool enabled: _desired === -1 ? autostart : _desired === 1

  // ---- состояние демона
  readonly property bool running: daemon.running
  property bool ready: false          // демон прислал ready
  property bool busy: false           // идёт снимок или запрос к модели
  property bool showingAnswer: false  // цифра (или «!») сейчас на экране
  property bool missingBinary: false
  property string fatalError: ""      // почему демон не запустился или упал
  property string lastError: ""       // ошибка последнего запроса
  property string lastAnswer: ""
  property string lastText: ""
  property real lastSeconds: 0
  property double lastAnswerAt: 0

  property string hotkeyError: ""
  property bool saving: false
  property string saveError: ""

  // ~/.local/bin/quizhint, иначе из PATH; код 127 — программа не установлена.
  readonly property var launcher: ["sh", "-c",
    "b=\"$HOME/.local/bin/quizhint\"; [ -x \"$b\" ] || b=\"$(command -v quizhint)\" || exit 127; exec \"$b\" \"$@\"",
    "quizhint"]

  property bool _stopping: false
  property bool _restartAfterExit: false
  property bool _replacedOther: false
  property string _daemonSignature: ""
  property double _startedAt: 0
  property string _lastStderr: ""
  property bool _refreshAgain: false
  property var _pendingSave: null
  property var _afterSave: null
  property string _bindQueued: ""
  property bool _bindPending: false

  // ------------------------------------------------------------- действия

  function setEnabled(on) {
    _desired = on ? 1 : 0
    if (on) {
      _replacedOther = false
      startDaemon()
    } else {
      stopDaemon()
    }
    save({ autostart: on ? "true" : "false" })
  }

  function toggle() { setEnabled(!enabled) }

  // Снимок запускает только горячая клавиша (бинд Hyprland шлёт SIGUSR1),
  // поэтому у сервиса нет «спросить». Спрятать цифру — SIGUSR2, когда демон готов.
  function hide() { if (daemon.running && ready) daemon.signal(12) }

  function restart() {
    _replacedOther = false
    restartDaemon()
  }

  function openConfig() {
    var path = configPath || (Quickshell.env("HOME") + "/.config/quizhint/config")
    function edit() { Quickshell.execDetached(["omarchy-launch-config-editor", path]) }
    // Файла ещё нет — сначала создаём его из примера, чтобы было что править.
    if (config.exists === false) save({ autostart: enabled ? "true" : "false" }, function(ok) { if (ok) edit() })
    else edit()
  }

  // values: { ключ: значение }. Значения уходят через stdin, а не в аргументах,
  // чтобы ключ API не светился в списке процессов.
  function save(values, done) {
    if (saveProc.running) {
      var merged = _pendingSave || {}
      for (var key in values) merged[key] = values[key]
      _pendingSave = merged
      return
    }
    var lines = []
    for (var k in values) {
      var v = String(values[k])
      if (/[\r\n]/.test(v)) {
        saveError = "Значение не может содержать перевод строки"
        return
      }
      lines.push(k + "=" + v)
    }
    if (lines.length === 0) return
    saveProc.payload = lines.join("\n") + "\n"
    _afterSave = done || null
    saving = true
    saveError = ""
    saveProc.stdinEnabled = true
    saveProc.running = true
  }

  function refreshConfig() {
    if (configProc.running) {
      _refreshAgain = true
      return
    }
    configProc.running = true
  }

  // ------------------------------------------------------------- демон

  // Всё, что влияет на работу демона (без настроек самого плагина).
  function signature(cfg) {
    var copy = {}
    for (var k in cfg) if (k !== "hotkey" && k !== "autostart" && k !== "exists") copy[k] = cfg[k]
    return JSON.stringify(copy)
  }

  function startDaemon() {
    if (daemon.running) return
    _stopping = false
    fatalError = ""
    lastError = ""
    ready = false
    busy = false
    showingAnswer = false
    _lastStderr = ""
    _daemonSignature = signature(config)
    _startedAt = Date.now()
    daemon.running = true
  }

  function stopDaemon() {
    _restartAfterExit = false
    crashRestart.stop()
    if (!daemon.running) return
    _stopping = true
    daemon.running = false
  }

  function restartDaemon() {
    crashRestart.stop()
    if (!daemon.running) {
      startDaemon()
      return
    }
    _restartAfterExit = true
    _stopping = true
    daemon.running = false
  }

  function handleEvent(line) {
    var ev
    try { ev = JSON.parse(line) } catch (e) { return }
    switch (String(ev.event || "")) {
    case "ready":
      ready = true
      fatalError = ""
      break
    case "busy":
      busy = true
      showingAnswer = false
      lastError = ""
      break
    case "answer":
      busy = false
      showingAnswer = true
      lastAnswer = String(ev.answer || "?")
      lastText = String(ev.text || "")
      lastSeconds = Number(ev.seconds || 0)
      lastAnswerAt = Date.now()
      break
    case "error":
      busy = false
      showingAnswer = true
      lastError = String(ev.message || "")
      break
    case "idle":
      busy = false
      showingAnswer = false
      break
    case "fatal":
      fatalError = String(ev.message || "")
      break
    }
  }

  function daemonExited(code) {
    ready = false
    busy = false
    showingAnswer = false
    if (code === 127) {
      missingBinary = true
      _stopping = false
      _restartAfterExit = false
      return
    }
    missingBinary = false
    if (_restartAfterExit) {
      _restartAfterExit = false
      startDaemon()
      return
    }
    if (_stopping) {
      _stopping = false
      return
    }
    if (fatalError === "") fatalError = _lastStderr !== "" ? _lastStderr : "quizhint завершился с кодом " + code
    // Копия, запущенная вручную, держит блокировку: панель берёт управление на себя.
    if (fatalError.indexOf("уже запущен") !== -1 && !_replacedOther) {
      _replacedOther = true
      killOther.running = true
      return
    }
    // Упал после нормальной работы — перезапускаем с паузой. Ошибки запуска
    // (нет ключа, неверный конфиг) так не лечатся, их ждём из настроек.
    if (enabled && Date.now() - _startedAt > 10000) crashRestart.restart()
  }

  Process {
    id: daemon
    command: root.launcher.concat(["--events"])
    stdout: SplitParser { onRead: function(line) { root.handleEvent(line) } }
    stderr: SplitParser {
      onRead: function(line) {
        var text = String(line).replace(/^quizhint: /, "").trim()
        if (text !== "") root._lastStderr = text
      }
    }
    onExited: function(code) { root.daemonExited(code) }
  }

  Timer {
    id: crashRestart
    interval: 3000
    onTriggered: if (root.enabled && !daemon.running) root.startDaemon()
  }

  Process {
    id: killOther
    command: ["pkill", "-x", "quizhint"]
    onExited: replaceDelay.restart()
  }

  Timer {
    id: replaceDelay
    interval: 400
    onTriggered: if (root.enabled) root.startDaemon()
  }

  // ------------------------------------------------------------- настройки

  function applyConfig(cfg) {
    var first = !configLoaded
    config = cfg
    configLoaded = true
    missingBinary = false
    _desired = -1

    if (first || hotkey !== boundHotkey) bindHotkey(hotkey)
    if (first) startWatcher()

    if (!autostart) {
      stopDaemon()
      return
    }
    var sig = signature(cfg)
    if (!daemon.running) {
      // Тот же конфиг, на котором демон уже упал, — не перезапускаем по кругу.
      if (first || sig !== _daemonSignature || fatalError === "") startDaemon()
      return
    }
    if (sig !== _daemonSignature) restartDaemon()
  }

  Process {
    id: configProc
    command: root.launcher.concat(["--print-config"])
    stdout: StdioCollector {
      id: configOut
      waitForEnd: true
      onStreamFinished: {
        var text = String(configOut.text || "").trim()
        if (text === "") return
        try { root.applyConfig(JSON.parse(text)) } catch (e) {}
      }
    }
    onExited: function(code) {
      if (code === 127) root.missingBinary = true
      if (root._refreshAgain) {
        root._refreshAgain = false
        root.refreshConfig()
      }
    }
  }

  Process {
    id: saveProc
    property string payload: ""
    command: root.launcher.concat(["--set-stdin"])
    stdinEnabled: true
    stderr: StdioCollector { id: saveErr; waitForEnd: true }
    onStarted: {
      saveProc.write(saveProc.payload)
      saveProc.stdinEnabled = false  // закрывает stdin: quizhint дочитывает до EOF
    }
    onExited: function(code) {
      root.saving = false
      var ok = code === 0
      if (!ok) {
        root._desired = -1
        root.saveError = code === 127 ? "quizhint не установлен"
          : (String(saveErr.text || "").replace(/^quizhint: /gm, "").trim() || "Не удалось сохранить настройки")
      }
      var done = root._afterSave
      root._afterSave = null
      if (done) done(ok)
      root.refreshConfig()
      if (root._pendingSave) {
        var next = root._pendingSave
        root._pendingSave = null
        root.save(next)
      }
    }
  }

  // Правки конфига руками (кнопка «Открыть конфиг») подхватываются сами.
  readonly property string configDir: configPath.substring(0, configPath.lastIndexOf("/"))
  readonly property string configName: configPath.substring(configPath.lastIndexOf("/") + 1)

  function startWatcher() {
    if (configDir !== "" && !watcher.running) watcher.running = true
  }

  Process {
    id: watcher
    command: ["sh", "-c", "mkdir -p \"$1\" && exec inotifywait -m -q -e close_write,moved_to,create,delete --format %f \"$1\"",
      "sh", root.configDir]
    stdout: SplitParser { onRead: function(name) { if (name === root.configName) configDebounce.restart() } }
    onExited: watcherRestart.restart()
  }

  Timer {
    id: watcherRestart
    interval: 5000
    onTriggered: root.startWatcher()
  }

  Timer {
    id: configDebounce
    interval: 300
    onTriggered: root.refreshConfig()
  }

  // ------------------------------------------------------------- горячая клавиша

  // Бинд создаётся через Lua API Hyprland (hyprctl eval), как у встроенных
  // плагинов: bindings.lua править не нужно. Ручка бинда хранится в Lua, поэтому
  // снимается ровно этот бинд. Метка владельца не даёт старому экземпляру при
  // перезапуске оболочки снять бинд, который уже поставил новый.
  property string boundHotkey: ""
  readonly property string _owner: String(Date.now()) + "-" + String(Math.floor(Math.random() * 1e9))
  readonly property string _unbindLua:
    "if _G.quizhint_bind then pcall(function() _G.quizhint_bind:unbind() end) _G.quizhint_bind = nil end"
  readonly property string _releaseLua:
    "if _G.quizhint_owner == \"" + _owner + "\" then " + _unbindLua + " _G.quizhint_owner = nil end"

  function bindHotkey(keys) {
    var spec = String(keys || "").trim()
    if (bindProc.running) {
      _bindQueued = spec
      _bindPending = true
      return
    }
    if (spec !== "" && !/^[A-Za-z0-9_+: ]+$/.test(spec)) {
      hotkeyError = "Недопустимое сочетание: " + spec
      return
    }
    var lua = _unbindLua + " _G.quizhint_owner = \"" + _owner + "\""
    if (spec !== "")
      lua += " _G.quizhint_bind = hl.bind(\"" + spec + "\", hl.dsp.exec_cmd(\"pkill -USR1 -x quizhint\"),"
        + " { description = \"Quiz Hint: спросить\" })"
    bindProc.spec = spec
    bindProc.command = ["hyprctl", "eval", lua]
    bindProc.running = true
  }

  Process {
    id: bindProc
    property string spec: ""
    stdout: StdioCollector { id: bindOut; waitForEnd: true }
    stderr: StdioCollector { id: bindErr; waitForEnd: true }
    onExited: function(code) {
      if (code === 0) {
        root.boundHotkey = bindProc.spec
        root.hotkeyError = ""
      } else {
        var text = (String(bindOut.text || "") + " " + String(bindErr.text || "")).trim()
        var match = text.match(/failed to parse key string: (.*)$/m)
        root.hotkeyError = "Сочетание не принято: " + (match ? match[1] : (text || "код " + code))
      }
      if (root._bindPending) {
        root._bindPending = false
        root.bindHotkey(root._bindQueued)
      }
    }
  }

  // Перезагрузка конфига Hyprland сбрасывает динамические бинды — ставим заново.
  Connections {
    target: Hyprland
    function onRawEvent(event) {
      if (event && event.name === "configreloaded") root.bindHotkey(root.hotkey)
    }
  }

  // Пока программа не установлена, время от времени проверяем снова.
  Timer {
    interval: 15000
    repeat: true
    running: root.missingBinary
    onTriggered: root.refreshConfig()
  }

  Component.onCompleted: refreshConfig()
  Component.onDestruction: Quickshell.execDetached(["hyprctl", "eval", root._releaseLua])
}
