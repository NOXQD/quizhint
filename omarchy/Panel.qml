import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
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

  // Где взять ключ для выбранного провайдера.
  readonly property var keyPage: provider === "gemini"
    ? { label: "aistudio.google.com/apikey", url: "https://aistudio.google.com/apikey" }
    : provider === "anthropic"
      ? { label: "console.anthropic.com", url: "https://console.anthropic.com/settings/keys" }
      : null
  readonly property bool editing: modelField.editing || keyField.editing || urlField.editing || hotkeyField.editing

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
    return barForeground
  }

  readonly property string answerText: svc && svc.lastAnswer !== "" ? svc.lastAnswer : "–"
  readonly property string answerMeta: {
    if (!svc || svc.lastAnswerAt === 0)
      return hotkeyText !== "" ? "Откройте вопрос и нажмите " + hotkeyText : "Назначьте клавишу ниже"
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
    contentWidth: panel.fittedContentWidth(Style.space(372))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(660))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      blocked: quiz.editing
      onCloseRequested: quiz.close()
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
                text: quiz.lastFailed ? "Запрос не удался" : "Последний ответ"
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

          RowLayout {
            width: parent.width
            spacing: Style.space(10)

            Text {
              textFormat: Text.PlainText
              Layout.fillWidth: true
              text: quiz.hotkeyText !== ""
                ? String.fromCodePoint(0xF0100) + "  Снимок экрана только по " + quiz.hotkeyText
                : String.fromCodePoint(0xF0100) + "  Клавиша не назначена — снимков нет"
              color: quiz.hotkeyText !== "" ? quiz.dim : quiz.urgent
              font.family: quiz.fontFamily
              font.pixelSize: Style.font.caption
              wrapMode: Text.Wrap
            }

            Button {
              text: "Спрятать"
              iconText: String.fromCodePoint(0xF0209)
              tooltipText: "Убрать цифру с экрана"
              bordered: true
              enabled: quiz.running && !!quiz.svc && (quiz.svc.showingAnswer || quiz.busy)
              opacity: enabled ? 1.0 : 0.45
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
              onClicked: quiz.svc.hide()
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
            text: "СНИМОК И ЦИФРА"
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
            label: "Цифра"
            ButtonGroup {
              options: [
                { value: "under", label: "Под панелью" },
                { value: "corner", label: "В самом углу" }
              ]
              value: quiz.cfg.over_panels ? "corner" : "under"
              focusable: false
              foreground: quiz.foreground
              fontFamily: quiz.fontFamily
              onChanged: function(value) { quiz.save({ over_panels: value === "corner" ? "true" : "false" }) }
            }
          }

          SettingField {
            id: hotkeyField
            label: "Клавиша"
            value: quiz.hotkeyText
            placeholder: "например CTRL + SHIFT + Q"
            onCommitted: function(text) { quiz.save({ hotkey: text.toUpperCase() }) }
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
