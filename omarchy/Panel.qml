import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui

// Значок quizhint на панели и выпадающая панель: включатель, последний ответ,
// настройки модели и снимка. Всё состояние живёт в Service.qml.
//
// Корень называется quiz, а не root: внутри PanelHero.trailingControl имя root
// указывает на сам PanelHero.
Panel {
  id: quiz
  moduleName: "noxqd.quizhint"
  ipcTarget: "noxqd.quizhint"
  manageIpc: false

  property var svc: null

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property color accent: Color.accent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  readonly property var cfg: svc ? svc.config : ({})
  readonly property bool enabledOn: svc ? svc.enabled : false
  readonly property bool running: svc ? svc.running : false
  readonly property bool ready: svc ? svc.ready : false
  readonly property bool busy: svc ? svc.busy : false
  readonly property string provider: String(cfg.provider || "anthropic")
  readonly property bool openai: provider === "openai"
  readonly property string hotkeyText: cfg.hotkey ? String(cfg.hotkey) : ""
  readonly property string regionHotkeyText: cfg.region_hotkey ? String(cfg.region_hotkey) : ""
  readonly property bool fullMode: cfg.answer_mode === "full"
  readonly property bool selecting: svc ? svc.selecting : false
  readonly property bool lastIsFull: !!svc && svc.lastMode === "full" && svc.lastAnswer !== ""

  // Где взять ключ для выбранного провайдера.
  readonly property var keyPage: provider === "gemini"
    ? { label: "aistudio.google.com/apikey", url: "https://aistudio.google.com/apikey" }
    : provider === "anthropic"
      ? { label: "console.anthropic.com", url: "https://console.anthropic.com/settings/keys" }
      : null
  readonly property bool editing: modelField.editing || keyField.editing || urlField.editing || hotkeyField.editing
    || regionField.editing || instructionsField.editing || fontField.editing

  // main — главная страница, settings — настройки вида и ответа.
  property string page: "main"

  // Что мешает работать: показывается в панели и красит значок.
  readonly property string problem: {
    if (!svc) return ""
    if (svc.missingBinary) return "Программа quizhint не найдена. Соберите и установите её: make install в каталоге проекта."
    if (!svc.enabled) return ""
    if (svc.configError !== "") return svc.configError
    var fatal = svc.fatalError
    if (fatal.indexOf("нет ключа API") === 0) return "Нет ключа API. Вставьте его в поле «Ключ API» ниже и нажмите Enter."
    if (fatal.indexOf("укажите model") !== -1) return "Укажите модель в поле «Модель» ниже."
    return fatal
  }
  readonly property bool requestFailed: !!svc && svc.showingAnswer && svc.lastError !== ""
  // Ошибка последнего запроса остаётся в панели до следующего нажатия клавиши.
  readonly property bool lastFailed: !!svc && !svc.busy && svc.lastError !== ""

  readonly property string stateText: {
    if (!svc) return "Загрузка"
    if (svc.missingBinary) return "Не установлен"
    if (!svc.enabled) return "Выключен"
    if (problem !== "") return "Нужна настройка"
    if (selecting) return "Выделите область"
    if (busy) return "Думаю"
    if (requestFailed) return "Ошибка запроса"
    if (running && !ready) return "Запуск"
    if (running) return "Работает"
    return "Остановлен"
  }

  // Лампочка с лучами — включён, без лучей — выключен. Пока модель думает, мигает.
  readonly property string glyph: String.fromCodePoint(enabledOn ? 0xF06E8 : 0xF0336)
  property real pulse: 1.0
  readonly property color glyphColor: {
    if (problem !== "" || requestFailed) return urgent
    if (busy) return Qt.rgba(accent.r, accent.g, accent.b, pulse)
    if (selecting) return accent
    return barForeground
  }

  // Полный ответ целиком не влезает в квадратик — там значок, текст ниже.
  readonly property string answerText: !svc || svc.lastAnswer === "" ? "–" : (lastIsFull ? "Aa" : svc.lastAnswer)
  readonly property string keysText: hotkeyText !== ""
    ? hotkeyText + (regionHotkeyText !== "" ? " (область — " + regionHotkeyText + ")" : "")
    : (regionHotkeyText !== "" ? regionHotkeyText + " и выделите область" : "")
  readonly property string answerMeta: {
    if (!svc || svc.lastAnswerAt === 0)
      return keysText !== "" ? "Откройте вопрос и нажмите " + keysText : "Назначьте клавишу ниже"
    var t = new Date(svc.lastAnswerAt)
    var parts = [("0" + t.getHours()).slice(-2) + ":" + ("0" + t.getMinutes()).slice(-2)]
    if (svc.lastSeconds > 0) parts.push(svc.lastSeconds.toFixed(1).replace(".", ",") + " с")
    if (svc.lastText !== "" && svc.lastText !== svc.lastAnswer) parts.push("модель: «" + svc.lastText + "»")
    return parts.join(" · ")
  }

  readonly property string keyPlaceholder: {
    if (!cfg.api_key_set) return openai ? "не нужен для локальных моделей" : "вставьте ключ и нажмите Enter"
    if (cfg.api_key_source && cfg.api_key_source !== "config") return "из переменной " + cfg.api_key_source
    return "сохранён" + (cfg.api_key_hint ? " …" + cfg.api_key_hint : "")
  }

  function resolveService() {
    if (!svc && bar && bar.shell && typeof bar.shell.serviceFor === "function")
      svc = bar.shell.serviceFor(moduleName)
    return svc
  }

  function save(values) {
    if (resolveService()) svc.save(values)
  }

  function showPage(name) {
    page = name
    panelFlick.contentY = 0
    keyCatcher.forceActiveFocus()
  }

  // Числовые поля сохраняются с задержкой: стрелки и колесо меняют значение по
  // шагу, и без задержки на каждый шаг перезапускался бы quizhint.
  property var _pendingSave: ({})

  function queueSave(key, value) {
    var pending = _pendingSave
    pending[key] = String(value)
    _pendingSave = pending
    saveDebounce.restart()
  }

  function flushSave() {
    saveDebounce.stop()
    var pending = _pendingSave
    _pendingSave = ({})
    if (Object.keys(pending).length > 0) save(pending)
  }

  function preview(kind) {
    flushSave()
    if (resolveService()) svc.preview(kind)
  }

  // #rrggbbaa → #rrggbb; плотность фона — альфа-канал background.
  function rgbOf(color, fallback) {
    var c = String(color || "").toLowerCase()
    return /^#[0-9a-f]{6}/.test(c) ? c.substring(0, 7) : fallback
  }

  function alphaHex(alpha) {
    var v = Math.max(0, Math.min(255, Math.round(Number(alpha) * 255)))
    return ("0" + v.toString(16)).slice(-2)
  }

  readonly property real bgAlpha: {
    var c = String(cfg.background || "#000000cc")
    return c.length >= 9 ? parseInt(c.substring(7, 9), 16) / 255 : 1
  }

  function resetAppearance() {
    saveDebounce.stop()
    _pendingSave = ({})
    save({
      position: "top-left", over_panels: "false", margin_x: "6", margin_y: "6",
      font: "sans-serif", font_size: "15", color: "#ffffff", background: "#000000cc",
      hide_after: "5", busy_indicator: "true",
      full_font_size: "13", full_width: "440", full_max_lines: "10", hide_after_full: "20"
    })
  }

  // Ключ сохраняется отдельно для каждого провайдера: при переключении не теряется.
  function saveKey(key) {
    var values = {}
    values[provider + "_api_key"] = key
    save(values)
  }

  // Модель и адрес API привязаны к провайдеру, поэтому при смене сбрасываем
  // их к значениям по умолчанию.
  function setProvider(value) {
    if (value === cfg.provider) return
    save({ provider: value, model: "", base_url: "" })
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: if (opened) {
    if (resolveService()) svc.refreshConfig()
    page = "main"
    panelFlick.contentY = 0
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }

  Component.onCompleted: resolveService()

  Timer {
    interval: 1000
    repeat: true
    running: !quiz.svc
    onTriggered: quiz.resolveService()
  }

  // omarchy-shell noxqd.quizhint open | close | toggle | settings | preview number|full
  IpcHandler {
    target: quiz.ipcTarget

    function open(): void { quiz.open() }
    function close(): void { quiz.close() }
    function toggle(): void { quiz.toggle() }
    function settings(): void {
      quiz.open()
      quiz.showPage("settings")
    }
    function preview(kind: string): void { quiz.preview(kind === "full" ? "full" : "number") }
  }

  Timer {
    id: saveDebounce
    interval: 600
    onTriggered: quiz.flushSave()
  }

  SequentialAnimation on pulse {
    running: quiz.busy
    loops: Animation.Infinite
    NumberAnimation { to: 0.35; duration: 500; easing.type: Easing.InOutSine }
    NumberAnimation { to: 1.0; duration: 500; easing.type: Easing.InOutSine }
    onStopped: quiz.pulse = 1.0
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: quiz.bar
    text: quiz.glyph
    foreground: quiz.glyphColor
    dimmed: !quiz.enabledOn
    tooltipText: "Quiz Hint · " + quiz.stateText.toLowerCase()
      + (quiz.running && quiz.hotkeyText !== "" ? " · " + quiz.hotkeyText : "")
    // Значок только открывает панель и включает/выключает: снимок — исключительно по клавише.
    onPressed: function(code) {
      if (code === Qt.MiddleButton) { if (quiz.resolveService()) quiz.svc.toggle() }
      else quiz.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: quiz
    bar: quiz.bar
    open: quiz.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(392))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(660))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      blocked: quiz.editing
      onCloseRequested: quiz.page === "main" ? quiz.close() : quiz.showPage("main")
      onTabRequested: function(direction) { quiz.switchPanel(direction) }
      onTextKey: function(t) {
        if (!quiz.svc) return
        if (t === "s" || t === "ы") quiz.svc.toggle()
        else if (t === "c" || t === "с") quiz.svc.openConfig()
      }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: column
          width: panelFlick.width

          // ---- главная страница
          Column {
            id: mainPage
            visible: quiz.page === "main"
            width: parent.width
            spacing: Style.space(12)

            PanelHero {
              id: hero
              width: parent.width
              title: "Quiz Hint"
              meta: quiz.stateText + (quiz.stateText === "Работает" && quiz.hotkeyText !== "" ? " · " + quiz.hotkeyText : "")
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
              iconOpacity: quiz.enabledOn ? 1.0 : 0.5
              iconComponent: Component {
                Text {
                  textFormat: Text.PlainText
                  text: quiz.glyph
                  color: quiz.problem !== "" ? quiz.urgent : quiz.foreground
                  font.family: quiz.fontFamily
                  font.pixelSize: Style.font.display
                }
              }
              trailingControl: Component {
                ToggleSwitch {
                  id: powerSwitch
                  visible: !!quiz.svc && !quiz.svc.missingBinary
                  checked: quiz.enabledOn
                  busy: !!quiz.svc && quiz.svc.saving
                  foreground: quiz.foreground
                  onToggled: if (quiz.svc) quiz.svc.toggle()

                  PanelToolTip {
                    visible: powerSwitch.containsMouse
                    text: quiz.enabledOn ? "Выключить quizhint" : "Включить quizhint"
                    fontFamily: quiz.fontFamily
                  }
                }
              }
            }

            Text {
              textFormat: Text.PlainText
              visible: quiz.problem !== ""
              width: parent.width
              text: quiz.problem
              color: quiz.urgent
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.bodySmall
              wrapMode: Text.Wrap
            }

            // ---- последний ответ
            RowLayout {
              width: parent.width
              spacing: Style.space(12)

              BorderSurface {
                Layout.preferredWidth: Math.max(Style.space(46), answerLabel.implicitWidth + Style.space(18))
                Layout.preferredHeight: Style.space(46)
                radius: Style.cornerRadius
                color: Style.normalFillFor(quiz.foreground, quiz.accent)
                borderSpec: Border.controlSpec(quiz.lastFailed ? "selected" : "normal",
                  quiz.lastFailed ? quiz.urgent : quiz.foreground, quiz.accent)

                Text {
                  id: answerLabel
                  anchors.centerIn: parent
                  textFormat: Text.PlainText
                  text: quiz.busy ? "…" : (quiz.lastFailed ? "!" : quiz.answerText)
                  color: quiz.lastFailed ? quiz.urgent : quiz.foreground
                  font.family: quiz.fontFamily
                  font.pixelSize: Style.font.display
                  font.bold: true
                }
              }

              ColumnLayout {
                Layout.fillWidth: true
                spacing: Style.space(2)

                Text {
                  textFormat: Text.PlainText
                  Layout.fillWidth: true
                  text: quiz.lastFailed ? "Запрос не удался"
                    : (quiz.svc && quiz.svc.lastRegion ? "Последний ответ · область" : "Последний ответ")
                  color: quiz.foreground
                  font.family: quiz.fontFamily
                  font.pixelSize: Style.font.body
                  font.bold: true
                  elide: Text.ElideRight
                }

                Text {
                  textFormat: Text.PlainText
                  Layout.fillWidth: true
                  text: quiz.lastFailed ? quiz.svc.lastError : quiz.answerMeta
                  color: quiz.lastFailed ? quiz.urgent : quiz.dim
                  font.family: quiz.fontFamily
                  font.pixelSize: Style.font.caption
                  wrapMode: Text.Wrap
                  maximumLineCount: 3
                  elide: Text.ElideRight
                }
              }
            }

            // Полный ответ целиком: на экране он прячется через несколько секунд, здесь — остаётся.
            BorderSurface {
              visible: quiz.lastIsFull && !quiz.lastFailed && !quiz.busy
              width: parent.width
              implicitHeight: fullText.implicitHeight + Style.space(16)
              radius: Style.cornerRadius
              color: Style.normalFillFor(quiz.foreground, quiz.accent)
              borderSpec: Border.controlSpec("normal", quiz.foreground, quiz.accent)

              Text {
                id: fullText
                anchors.left: parent.left
                anchors.right: copyButton.left
                anchors.top: parent.top
                anchors.margins: Style.space(8)
                textFormat: Text.PlainText
                text: quiz.svc ? quiz.svc.lastAnswer : ""
                color: quiz.foreground
                font.family: quiz.fontFamily
                font.pixelSize: Style.font.bodySmall
                wrapMode: Text.Wrap
                maximumLineCount: 12
                elide: Text.ElideRight
              }

              PanelActionButton {
                id: copyButton
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Style.space(4)
                iconText: String.fromCodePoint(0xF018F)
                tooltipText: "Скопировать ответ"
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: if (quiz.svc) Quickshell.execDetached(["wl-copy", "--", quiz.svc.lastAnswer])
              }
            }

            RowLayout {
              width: parent.width
              spacing: Style.space(10)

              Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: quiz.keysText !== ""
                  ? String.fromCodePoint(0xF0100) + "  Снимок только по " + quiz.keysText
                  : String.fromCodePoint(0xF0100) + "  Клавиши не назначены — снимков нет"
                color: quiz.keysText !== "" ? quiz.dim : quiz.urgent
                font.family: quiz.fontFamily
                font.pixelSize: Style.font.caption
                wrapMode: Text.Wrap
              }

              Button {
                text: "Спрятать"
                iconText: String.fromCodePoint(0xF0209)
                tooltipText: "Убрать ответ с экрана"
                bordered: true
                enabled: quiz.running && !!quiz.svc && (quiz.svc.showingAnswer || quiz.busy)
                opacity: enabled ? 1.0 : 0.45
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.svc.hide()
              }
            }

            SettingRow {
              label: "Ответ"
              ButtonGroup {
                options: [
                  { value: "number", label: "Цифра", tooltip: "Только номер правильного варианта" },
                  { value: "full", label: "Полный", tooltip: "Короткий, но полный ответ текстом — на любой вопрос или задачу" }
                ]
                value: quiz.fullMode ? "full" : "number"
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ answer_mode: value }) }
              }
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- модель
            PanelSectionHeader {
              text: "МОДЕЛЬ"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingRow {
              label: "Провайдер"
              ButtonGroup {
                options: [
                  { value: "anthropic", label: "Claude" },
                  { value: "gemini", label: "Gemini" },
                  { value: "openai", label: "Другой", tooltip: "Любой OpenAI-совместимый API: OpenAI, OpenRouter, Ollama…" }
                ]
                value: quiz.provider
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.setProvider(value) }
              }
            }

            // Gemini: Flash-Lite отвечает за 1–2 с, старшие Flash точнее, но медленнее.
            SettingRow {
              visible: quiz.provider === "gemini"
              label: "Скорость"
              ButtonGroup {
                options: [
                  { value: "gemini-flash-lite-latest", label: "Быстрая", tooltip: "gemini-flash-lite-latest — ответ за 1–2 с" },
                  { value: "gemini-3.6-flash", label: "Точнее", tooltip: "gemini-3.6-flash — умнее, ответ за 3–5 с" }
                ]
                value: String(quiz.cfg.model || "")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ model: value }) }
              }
            }

            SettingField {
              id: modelField
              label: "Модель"
              value: String(quiz.cfg.model || "")
              placeholder: quiz.openai ? "например gpt-4.1-mini"
                : quiz.provider === "gemini" ? "gemini-flash-lite-latest" : "claude-haiku-5-5"
              onCommitted: function(text) { quiz.save({ model: text }) }
            }

            SettingField {
              id: keyField
              label: "Ключ API"
              password: true
              placeholder: quiz.keyPlaceholder
              onCommitted: function(text) { quiz.saveKey(text) }
            }

            // Ссылка на страницу, где выдают ключ.
            Text {
              id: keyLink
              visible: quiz.keyPage !== null
              x: Style.space(88)
              width: parent.width - x
              textFormat: Text.PlainText
              text: quiz.keyPage ? "Ключ: " + quiz.keyPage.label + " " + String.fromCodePoint(0xF03CC) : ""
              color: keyLinkArea.containsMouse ? quiz.foreground : quiz.dim
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.caption
              font.underline: keyLinkArea.containsMouse
              elide: Text.ElideRight

              MouseArea {
                id: keyLinkArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                  quiz.close()
                  Qt.openUrlExternally(quiz.keyPage.url)
                }
              }
            }

            SettingField {
              id: urlField
              visible: quiz.openai
              label: "Адрес API"
              value: String(quiz.cfg.base_url || "")
              placeholder: "https://api.openai.com/v1"
              onCommitted: function(text) { quiz.save({ base_url: text }) }
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- снимок и цифра
            PanelSectionHeader {
              text: "КЛАВИШИ"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingField {
              id: hotkeyField
              label: "Экран"
              value: quiz.hotkeyText
              placeholder: "например F1 или CTRL + SHIFT + Q"
              onCommitted: function(text) { quiz.save({ hotkey: text.toUpperCase() }) }
            }

            SettingField {
              id: regionField
              label: "Область"
              value: quiz.regionHotkeyText
              placeholder: "например F3 — выделить мышью"
              onCommitted: function(text) { quiz.save({ region_hotkey: text.toUpperCase() }) }
            }

            Text {
              textFormat: Text.PlainText
              readonly property string message: quiz.svc ? (quiz.svc.saveError || quiz.svc.hotkeyError) : ""
              visible: message !== ""
              width: parent.width
              text: message
              color: quiz.urgent
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.bodySmall
              wrapMode: Text.Wrap
            }

            PanelSeparator { foreground: quiz.foreground }

            Row {
              spacing: Style.space(8)

              Button {
                text: "Настройки"
                iconText: String.fromCodePoint(0xF0493)
                tooltipText: "Вид цифры и ответа, длина и язык ответа, модель"
                bordered: true
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.showPage("settings")
              }

              Button {
                text: "Перезапустить"
                iconText: String.fromCodePoint(0xF0709)
                bordered: true
                enabled: !!quiz.svc && quiz.enabledOn && !quiz.svc.missingBinary
                opacity: enabled ? 1.0 : 0.45
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.svc.restart()
              }
            }

            Text {
              textFormat: Text.PlainText
              width: parent.width
              text: "На значке: клик — эта панель · средняя кнопка — вкл/выкл"
              color: quiz.dim
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.caption
              wrapMode: Text.Wrap
            }
          }

          // ---- страница настроек
          Column {
            id: settingsPage
            visible: quiz.page === "settings"
            width: parent.width
            spacing: Style.space(12)

            RowLayout {
              width: parent.width
              spacing: Style.space(8)

              PanelActionButton {
                iconText: String.fromCodePoint(0xF004D)
                tooltipText: "Назад (Esc)"
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.showPage("main")
              }

              Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: "Настройки"
                color: quiz.foreground
                font.family: quiz.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
              }
            }

            Text {
              textFormat: Text.PlainText
              width: parent.width
              text: "Изменения сохраняются сразу. Кнопки «Пример» внизу показывают, как это выглядит на экране."
              color: quiz.dim
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.caption
              wrapMode: Text.Wrap
            }

            // ---- где показывать
            PanelSectionHeader {
              text: "РАСПОЛОЖЕНИЕ"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingRow {
              label: "Угол"
              ButtonGroup {
                options: [
                  { value: "top-left", label: "↖", tooltip: "Слева сверху" },
                  { value: "top-right", label: "↗", tooltip: "Справа сверху" },
                  { value: "bottom-left", label: "↙", tooltip: "Слева снизу" },
                  { value: "bottom-right", label: "↘", tooltip: "Справа снизу" }
                ]
                value: String(quiz.cfg.position || "top-left")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ position: value }) }
              }
            }

            SettingRow {
              label: "Панель"
              ButtonGroup {
                options: [
                  { value: "under", label: "Не закрывать", tooltip: "Ответ появляется рядом с панелью Omarchy" },
                  { value: "corner", label: "Поверх", tooltip: "Ответ в самом углу экрана, поверх панели" }
                ]
                value: quiz.cfg.over_panels ? "corner" : "under"
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ over_panels: value === "corner" ? "true" : "false" }) }
              }
            }

            NumberSetting {
              label: "Отступ ↔"
              suffix: "пикс. от края"
              value: Number(quiz.cfg.margin_x || 0)
              from: 0
              to: 2000
              onModified: function(v) { quiz.queueSave("margin_x", v) }
            }

            NumberSetting {
              label: "Отступ ↕"
              suffix: "пикс. от края"
              value: Number(quiz.cfg.margin_y || 0)
              from: 0
              to: 2000
              onModified: function(v) { quiz.queueSave("margin_y", v) }
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- цифра
            PanelSectionHeader {
              text: "ЦИФРА"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            NumberSetting {
              label: "Размер"
              suffix: "пикс."
              value: Number(quiz.cfg.font_size || 15)
              from: 6
              to: 120
              onModified: function(v) { quiz.queueSave("font_size", v) }
            }

            NumberSetting {
              label: "Скрывать"
              suffix: "с · 0 — не скрывать"
              value: Number(quiz.cfg.hide_after === undefined ? 5 : quiz.cfg.hide_after)
              from: 0
              to: 600
              onModified: function(v) { quiz.queueSave("hide_after", v) }
            }

            SwitchSetting {
              label: "Ожидание"
              caption: "точка в углу, пока модель думает"
              checked: quiz.cfg.busy_indicator !== false
              onToggled: quiz.save({ busy_indicator: quiz.cfg.busy_indicator === false ? "true" : "false" })
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- полный ответ
            PanelSectionHeader {
              text: "ПОЛНЫЙ ОТВЕТ"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingRow {
              label: "Длина"
              ButtonGroup {
                options: [
                  { value: "short", label: "Короткий", tooltip: "До ~250 символов" },
                  { value: "medium", label: "Средний", tooltip: "До ~500 символов" },
                  { value: "long", label: "Подробный", tooltip: "До ~900 символов, с объяснением" }
                ]
                value: String(quiz.cfg.full_length || "short")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ full_length: value }) }
              }
            }

            SwitchSetting {
              label: "Пояснение"
              caption: "кратко объяснить, почему так"
              checked: quiz.cfg.full_explain !== false
              onToggled: quiz.save({ full_explain: quiz.cfg.full_explain === false ? "true" : "false" })
            }

            SettingRow {
              label: "Язык"
              ButtonGroup {
                options: [
                  { value: "auto", label: "Авто", tooltip: "На языке вопроса" },
                  { value: "ru", label: "Русский" },
                  { value: "en", label: "English" }
                ]
                value: String(quiz.cfg.answer_language || "auto")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ answer_language: value }) }
              }
            }

            SettingField {
              id: instructionsField
              label: "Указания"
              value: String(quiz.cfg.instructions || "")
              placeholder: "например: объясняй как школьнику"
              onCommitted: function(text) { quiz.save({ instructions: text }) }
            }

            NumberSetting {
              label: "Размер"
              suffix: "пикс."
              value: Number(quiz.cfg.full_font_size || 13)
              from: 6
              to: 72
              onModified: function(v) { quiz.queueSave("full_font_size", v) }
            }

            NumberSetting {
              label: "Ширина"
              suffix: "пикс."
              value: Number(quiz.cfg.full_width || 440)
              from: 120
              to: 4000
              step: 20
              onModified: function(v) { quiz.queueSave("full_width", v) }
            }

            NumberSetting {
              label: "Строк"
              suffix: "максимум, дальше «…»"
              value: Number(quiz.cfg.full_max_lines || 10)
              from: 1
              to: 60
              onModified: function(v) { quiz.queueSave("full_max_lines", v) }
            }

            NumberSetting {
              label: "Скрывать"
              suffix: "с · 0 — не скрывать"
              value: Number(quiz.cfg.hide_after_full === undefined ? 20 : quiz.cfg.hide_after_full)
              from: 0
              to: 600
              onModified: function(v) { quiz.queueSave("hide_after_full", v) }
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- цвета и шрифт (общие для цифры и ответа)
            PanelSectionHeader {
              text: "ЦВЕТА И ШРИФТ"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingRow {
              label: "Текст"
              SwatchRow {
                colors: ["#ffffff", "#ffd400", "#86efac", "#7dd3fc", "#f9a8d4", "#fb923c", "#000000"]
                value: quiz.rgbOf(quiz.cfg.color, "#ffffff")
                onPicked: function(color) { quiz.save({ color: color }) }
              }
            }

            SettingRow {
              label: "Фон"
              SwatchRow {
                colors: ["#000000", "#1f2937", "#1e3a8a", "#14532d", "#7f1d1d", "#581c87", "#ffffff"]
                value: quiz.rgbOf(quiz.cfg.background, "#000000")
                onPicked: function(color) { quiz.save({ background: color + quiz.alphaHex(quiz.bgAlpha) }) }
              }
            }

            SettingRow {
              label: "Плотность"
              RowLayout {
                width: parent.width
                spacing: Style.space(8)

                PanelSlider {
                  id: alphaSlider
                  Layout.fillWidth: true
                  Layout.preferredHeight: Style.spacing.controlHeight
                  bar: quiz.bar
                  minimum: 0
                  maximum: 100
                  step: 5
                  integer: true
                  value: Math.round(quiz.bgAlpha * 100)
                  onReleased: function(v) {
                    quiz.save({ background: quiz.rgbOf(quiz.cfg.background, "#000000") + quiz.alphaHex(v / 100) })
                  }
                }

                Text {
                  textFormat: Text.PlainText
                  text: Math.round(alphaSlider.liveValue) + "%"
                  Layout.preferredWidth: Style.space(36)
                  horizontalAlignment: Text.AlignRight
                  color: quiz.dim
                  font.family: quiz.fontFamily
                  font.pixelSize: Style.font.caption
                }
              }
            }

            SettingField {
              id: fontField
              label: "Шрифт"
              value: String(quiz.cfg.font || "")
              placeholder: "sans-serif, Inter, JetBrains Mono…"
              onCommitted: function(text) { quiz.save({ font: text === "" ? "sans-serif" : text }) }
            }

            PanelSeparator { foreground: quiz.foreground }

            // ---- модель и снимок
            PanelSectionHeader {
              text: "МОДЕЛЬ И СНИМОК"
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
            }

            SettingRow {
              label: "Снимать"
              ButtonGroup {
                options: [
                  { value: "monitor", label: "Весь экран" },
                  { value: "window", label: "Активное окно" }
                ]
                value: String(quiz.cfg.capture || "monitor")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ capture: value }) }
              }
            }

            SettingRow {
              visible: quiz.provider !== "anthropic"
              label: "Рассужд."
              ButtonGroup {
                options: [
                  { value: "minimal", label: "Мин.", tooltip: "Быстрее всего (не все модели поддерживают)" },
                  { value: "low", label: "Низк." },
                  { value: "medium", label: "Сред." },
                  { value: "high", label: "Выс.", tooltip: "Точнее на сложных задачах, но медленнее" }
                ]
                value: String(quiz.cfg.thinking || "")
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ thinking: value }) }
              }
            }

            SettingRow {
              label: "Качество"
              ButtonGroup {
                options: [
                  { value: "1024", label: "Быстрее", tooltip: "Снимок до 1024 пикс. — меньше, быстрее отправка" },
                  { value: "1568", label: "Обычное" },
                  { value: "2048", label: "Чётче", tooltip: "Снимок до 2048 пикс. — для мелкого текста" }
                ]
                value: String(quiz.cfg.max_size || 1568)
                focusable: false
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onChanged: function(value) { quiz.save({ max_size: value }) }
              }
            }

            NumberSetting {
              label: "Таймаут"
              suffix: "с ожидания ответа"
              value: Number(quiz.cfg.timeout || 30)
              from: 5
              to: 300
              onModified: function(v) { quiz.queueSave("timeout", v) }
            }

            Text {
              textFormat: Text.PlainText
              readonly property string message: quiz.svc ? quiz.svc.saveError : ""
              visible: message !== ""
              width: parent.width
              text: message
              color: quiz.urgent
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.bodySmall
              wrapMode: Text.Wrap
            }

            PanelSeparator { foreground: quiz.foreground }

            Flow {
              width: parent.width
              spacing: Style.space(8)

              Button {
                text: "Пример цифры"
                iconText: String.fromCodePoint(0xF0208)
                tooltipText: "Показать цифру на экране с этими настройками"
                bordered: true
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.preview("number")
              }

              Button {
                text: "Пример ответа"
                iconText: String.fromCodePoint(0xF0208)
                tooltipText: "Показать полный ответ на экране с этими настройками"
                bordered: true
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.preview("full")
              }

              Button {
                text: "Сбросить вид"
                iconText: String.fromCodePoint(0xF0450)
                tooltipText: "Вернуть расположение, размеры и цвета по умолчанию"
                bordered: true
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: quiz.resetAppearance()
              }

              Button {
                text: "Открыть конфиг"
                iconText: String.fromCodePoint(0xF0DC9)
                tooltipText: quiz.svc ? quiz.svc.configPath : ""
                bordered: true
                enabled: !!quiz.svc && !quiz.svc.missingBinary
                opacity: enabled ? 1.0 : 0.45
                foreground: quiz.foreground
                fontFamily: quiz.fontFamily
                onClicked: {
                  quiz.close()
                  quiz.svc.openConfig()
                }
              }
            }
          }
        }
      }
    }
  }

  // Подпись слева, элемент управления справа.
  component SettingRow: RowLayout {
    property string label: ""
    default property alias content: holder.data

    width: parent ? parent.width : implicitWidth
    spacing: Style.space(10)

    Text {
      textFormat: Text.PlainText
      text: parent.label
      Layout.preferredWidth: Style.space(78)
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }

    Item {
      id: holder
      Layout.fillWidth: true
      implicitHeight: children.length > 0 ? children[0].implicitHeight : 0
    }
  }

  // Число со стрелками: подпись слева, пояснение справа.
  component NumberSetting: RowLayout {
    id: numberSetting

    property string label: ""
    property string suffix: ""
    property int value: 0
    property int from: 0
    property int to: 100
    property int step: 1

    signal modified(int value)

    width: parent ? parent.width : implicitWidth
    spacing: Style.space(10)

    Text {
      textFormat: Text.PlainText
      text: numberSetting.label
      Layout.preferredWidth: Style.space(78)
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }

    NumberField {
      value: numberSetting.value
      from: numberSetting.from
      to: numberSetting.to
      stepSize: numberSetting.step
      fieldWidth: Style.space(112)
      foreground: quiz.foreground
      fontFamily: quiz.fontFamily
      fontSize: Style.font.bodySmall
      onModified: function(v) { numberSetting.modified(v) }
    }

    Text {
      textFormat: Text.PlainText
      text: numberSetting.suffix
      Layout.fillWidth: true
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.caption
      elide: Text.ElideRight
    }
  }

  // Переключатель с пояснением.
  component SwitchSetting: RowLayout {
    id: switchSetting

    property string label: ""
    property string caption: ""
    property bool checked: false

    signal toggled()

    width: parent ? parent.width : implicitWidth
    spacing: Style.space(10)

    Text {
      textFormat: Text.PlainText
      text: switchSetting.label
      Layout.preferredWidth: Style.space(78)
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }

    ToggleSwitch {
      checked: switchSetting.checked
      cursorRing: false
      foreground: quiz.foreground
      onToggled: switchSetting.toggled()
    }

    Text {
      textFormat: Text.PlainText
      text: switchSetting.caption
      Layout.fillWidth: true
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.caption
      wrapMode: Text.Wrap
    }
  }

  // Готовые цвета кружками; выбранный обведён.
  component SwatchRow: Row {
    id: swatches

    property var colors: []
    property string value: ""

    signal picked(string color)

    spacing: Style.space(6)

    Repeater {
      model: swatches.colors

      Rectangle {
        id: swatch
        required property var modelData
        readonly property bool selected: swatches.value === modelData

        width: Style.space(24)
        height: width
        radius: width / 2
        color: "transparent"
        border.width: selected ? 2 : 0
        border.color: quiz.foreground

        Rectangle {
          anchors.centerIn: parent
          width: parent.width - Style.space(6)
          height: width
          radius: width / 2
          color: swatch.modelData
          border.width: 1
          border.color: Qt.rgba(quiz.foreground.r, quiz.foreground.g, quiz.foreground.b, 0.35)
        }

        MouseArea {
          anchors.fill: parent
          cursorShape: Qt.PointingHandCursor
          onClicked: swatches.picked(swatch.modelData)
        }
      }
    }
  }

  // Поле ввода, которое сохраняет значение по Enter или при уходе фокуса.
  // Esc отменяет правку. Ключ API после сохранения стирается из поля.
  component SettingField: RowLayout {
    id: field

    property string label: ""
    property string value: ""
    property string placeholder: ""
    property bool password: false
    readonly property bool editing: input.activeFocus

    signal committed(string text)

    width: parent ? parent.width : implicitWidth
    spacing: Style.space(10)

    Text {
      textFormat: Text.PlainText
      text: field.label
      Layout.preferredWidth: Style.space(78)
      Layout.alignment: Qt.AlignVCenter
      color: quiz.dim
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.bodySmall
      elide: Text.ElideRight
    }

    TextField {
      id: input
      property bool cancelled: false

      Layout.fillWidth: true
      password: field.password
      placeholderText: field.placeholder
      text: field.password ? "" : field.value
      foreground: quiz.foreground
      font.family: quiz.fontFamily
      font.pixelSize: Style.font.bodySmall
      verticalPadding: Style.space(5)

      Keys.onEscapePressed: {
        input.cancelled = true
        keyCatcher.forceActiveFocus()
      }
      onEditingFinished: {
        if (input.cancelled) {
          input.cancelled = false
          input.text = Qt.binding(function() { return field.password ? "" : field.value })
          return
        }
        var v = input.text.trim()
        if (field.password) {
          input.text = ""
          if (v !== "") field.committed(v)
        } else if (v !== field.value) {
          field.committed(v)
        }
      }
    }
  }
}
