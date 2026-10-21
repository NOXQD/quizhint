// quizhint: по сигналу снимает экран, спрашивает vision-модель и показывает
// номер правильного варианта маленькой цифрой в левом верхнем углу.

#include "capture.hpp"
#include "config.hpp"
#include "display.hpp"
#include "http.hpp"
#include "hyprland.hpp"
#include "image.hpp"
#include "json.hpp"
#include "llm.hpp"
#include "log.hpp"
#include "overlay.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <iterator>
#include <optional>
#include <spawn.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

constexpr const char* kVersion = "1.4.0";

// Сигнал «выделить область и ответить» (pkill -RTMIN+1 -x quizhint).
int region_signal() { return SIGRTMIN + 1; }

struct Options {
    std::string config_path;
    bool once = false;
    bool show = false;
    std::string show_text;
    std::string image_path;
    std::string save_path;
    bool verbose = false;
    bool events = false;
    bool print_config = false;
    bool set_stdin = false;
    ConfigValues set_values;
    std::string mode;     // number | full — вместо answer_mode из конфига
    bool region = false;  // --once: выделить область вместо снимка экрана
};

void usage(FILE* out) {
    std::fputs(
        "Использование: quizhint [ПАРАМЕТРЫ]\n"
        "\n"
        "Без параметров работает в фоне: по SIGUSR1 снимает активный монитор, спрашивает\n"
        "модель и показывает номер правильного ответа в левом верхнем углу.\n"
        "  pkill -USR1 -x quizhint     спросить про весь экран (повесьте на горячую клавишу)\n"
        "  pkill -RTMIN+1 -x quizhint  выделить область мышью и спросить про неё\n"
        "  pkill -USR2 -x quizhint     спрятать ответ / отменить запрос\n"
        "\n"
        "  -c, --config ФАЙЛ    конфиг (по умолчанию ~/.config/quizhint/config)\n"
        "      --once           один раз: снимок → ответ (печатается в stdout) → выход\n"
        "      --image ФАЙЛ     отправить готовую картинку вместо снимка (подразумевает --once)\n"
        "      --save ФАЙЛ      сохранить JPEG, который уходит модели\n"
        "      --show ТЕКСТ     только показать ТЕКСТ в углу — проверка внешнего вида\n"
        "      --mode РЕЖИМ     number — номер варианта, full — короткий полный ответ\n"
        "      --region         с --once: выделить область мышью (нужен slurp)\n"
        "  -v, --verbose        подробный лог с замерами времени\n"
        "\n"
        "Для плагина Omarchy и скриптов:\n"
        "      --events         писать события в stdout, по JSON-объекту на строку\n"
        "      --print-config   вывести настройки в JSON (ключ API не выводится)\n"
        "      --set КЛЮЧ=ЗНАЧ  изменить параметр в конфиге (можно несколько раз)\n"
        "      --set-stdin      то же, но строки КЛЮЧ=ЗНАЧ читаются из stdin\n"
        "      --version        версия\n"
        "  -h, --help           эта справка\n",
        out);
}

// Событие для плагина: одна строка JSON в stdout.
void emit_event(bool enabled, const std::string& name, const std::string& fields = {}) {
    if (!enabled) return;
    std::printf("{\"event\":%s%s%s}\n", json::quote(name).c_str(), fields.empty() ? "" : ",", fields.c_str());
    std::fflush(stdout);
}

std::string json_field(const char* key, const std::string& value) {
    return json::quote(key) + ":" + json::quote(value);
}

bool parse_assignment(const std::string& line, ConfigValues& out) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    std::string key = line.substr(0, eq);
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    out.emplace_back(key, line.substr(eq + 1));
    return true;
}

void arm_timer(int fd, double ms) {
    const auto ns = static_cast<long long>(std::max(ms, 0.001) * 1e6);
    itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(ns / 1000000000LL);
    its.it_value.tv_nsec = static_cast<long>(ns % 1000000000LL);
    timerfd_settime(fd, 0, &its, nullptr);
}

void disarm_timer(int fd) {
    const itimerspec its{};
    timerfd_settime(fd, 0, &its, nullptr);
}

void drain(int fd) {
    uint64_t value;
    while (read(fd, &value, sizeof value) == sizeof value) {
    }
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// Один демон на сессию: иначе pkill разбудит несколько копий сразу.
// Возвращает fd блокировки, -2 если уже запущен, -1 если взять её не удалось.
int acquire_lock() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/quizhint.lock";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
    const bool running = errno == EWOULDBLOCK;
    close(fd);
    return running ? -2 : -1;
}

class App {
public:
    App(Config cfg, Options opt, int sig_fd)
        : cfg_(std::move(cfg)),
          opt_(std::move(opt)),
          sig_fd_(sig_fd),
          overlay_(display_, cfg_),
          capturer_(display_, cfg_.capture_protocol) {}

    ~App() {
        stop_select();
        if (hide_fd_ >= 0) close(hide_fd_);
        if (delay_fd_ >= 0) close(delay_fd_);
    }

    int run();

private:
    enum class Phase {
        Idle,
        Hiding,     // ждём, пока композитор уберёт старую цифру с экрана
        Selecting,  // пользователь выделяет область (slurp)
        Settling,   // рамка выделения закрылась — ждём кадр без неё
        Capturing,  // снимаем экран
        Asking,     // ждём ответ модели
    };

    Output* pick_output();
    void trigger(bool region);
    void begin_capture();
    void start_select();
    void on_select_readable();
    void finish_select(int status);
    void stop_select();
    void on_frame(const RawFrame* frame, const std::string& error);
    std::optional<PixelRect> crop_rect(const RawFrame& frame) const;
    void send(const std::vector<uint8_t>& image, const std::string& media_type);
    void on_response(const HttpResult& result);
    void show(const std::string& text, bool full = false);
    void fail(const std::string& message);
    void cancel();
    void handle_signals();
    void on_hide_timer();
    int fatal(const std::string& message);

    Config cfg_;
    Options opt_;
    int sig_fd_;
    int hide_fd_ = -1;
    int delay_fd_ = -1;
    Display display_;
    Overlay overlay_;
    Capturer capturer_;
    Http http_;

    Phase phase_ = Phase::Idle;
    Output* target_ = nullptr;
    std::optional<hypr::Monitor> monitor_;
    std::optional<hypr::Rect> window_;  // capture = window: активное окно
    std::optional<hypr::Rect> region_;  // выделенная область
    llm::Kind kind_ = llm::Kind::Number;
    pid_t select_pid_ = -1;
    int select_fd_ = -1;
    std::string select_out_;
    Clock::time_point t_trigger_{}, t_request_{};
    bool quit_ = false;
    int exit_code_ = 0;
};

int App::fatal(const std::string& message) {
    log_info("%s", message.c_str());
    emit_event(opt_.events, "fatal", json_field("message", message));
    return 1;
}

int App::run() {
    std::string error;
    if (!display_.connect(error)) return fatal(error);
    display_.on_output_removed = [this](Output* output) {
        overlay_.output_removed(output);
        capturer_.output_removed(output);
        if (target_ == output) target_ = nullptr;
    };
    hide_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    delay_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (hide_fd_ < 0 || delay_fd_ < 0) return fatal(std::string("timerfd_create: ") + std::strerror(errno));

    for (const auto& o : display_.outputs()) log_debug("монитор %s, масштаб %d", o->name.c_str(), o->scale);
    log_debug("протоколы: ext-image-copy-capture %s, wlr-screencopy %s, fractional-scale %s",
              display_.ext_copy_capture && display_.ext_output_source ? "есть" : "нет",
              display_.wlr_screencopy ? "есть" : "нет",
              display_.fractional_scale && display_.viewporter ? "есть" : "нет");

    Output* output = pick_output();
    if (!output) return fatal("не найдено ни одного монитора");
    // Поверхность создаём сразу (прозрачной), чтобы первая цифра появилась без задержки.
    overlay_.move_to(output);

    if (opt_.show) {
        // --mode full: показать текст так, как выглядит полный ответ.
        const bool full = cfg_.answer_mode == "full";
        overlay_.set(opt_.show_text, full ? Overlay::Kind::Text : Overlay::Kind::Badge);
        // Это проверка внешнего вида — дольше нескольких секунд держать незачем.
        const double seconds = full ? cfg_.hide_after_full : cfg_.hide_after;
        arm_timer(hide_fd_, std::clamp(seconds > 0 ? seconds : 4.0, 2.0, 6.0) * 1000);
    } else if (opt_.once) {
        trigger(opt_.region && opt_.image_path.empty());
    } else {
        log_info("работаю: модель %s, режим %s; весь экран — pkill -USR1 -x quizhint, область — pkill -RTMIN+1 -x quizhint",
                 cfg_.model.c_str(), cfg_.answer_mode == "full" ? "полный ответ" : "цифра");
        emit_event(opt_.events, "ready",
                   json_field("provider", cfg_.provider) + "," + json_field("model", cfg_.model) + "," +
                       json_field("capture", cfg_.capture) + "," + json_field("version", kVersion));
    }

    while (!quit_) {
        if (!display_.prepare_poll()) {
            log_info("соединение с композитором потеряно");
            exit_code_ = 1;
            break;
        }
        curl_waitfd fds[] = {
            {display_.fd(), CURL_WAIT_POLLIN, 0},
            {sig_fd_, CURL_WAIT_POLLIN, 0},
            {hide_fd_, CURL_WAIT_POLLIN, 0},
            {delay_fd_, CURL_WAIT_POLLIN, 0},
            {select_fd_, CURL_WAIT_POLLIN, 0},
        };
        const unsigned nfds = select_fd_ >= 0 ? 5 : 4;
        // curl ждёт и свои сокеты, и наши дескрипторы — один цикл без потоков.
        const CURLMcode mc = curl_multi_poll(http_.multi(), fds, nfds, 60000, nullptr);
        if (mc != CURLM_OK) {
            display_.cancel_poll();
            log_info("curl_multi_poll: %s", curl_multi_strerror(mc));
            exit_code_ = 1;
            break;
        }
        if (!display_.finish_poll(fds[0].revents & CURL_WAIT_POLLIN)) {
            log_info("соединение с композитором потеряно");
            exit_code_ = 1;
            break;
        }
        if (fds[1].revents) handle_signals();
        if (fds[2].revents) {
            drain(hide_fd_);
            on_hide_timer();
        }
        if (fds[3].revents) {
            drain(delay_fd_);
            if (phase_ == Phase::Hiding || phase_ == Phase::Settling) begin_capture();
        }
        if (nfds > 4 && fds[4].revents && select_fd_ >= 0) on_select_readable();
        HttpResult result;
        if (http_.active() && http_.perform(result)) on_response(result);
    }
    return exit_code_;
}

Output* App::pick_output() {
    monitor_ = hypr::focused_monitor();
    if (monitor_) {
        if (Output* o = display_.output_by_name(monitor_->name)) return o;
        monitor_.reset();
    }
    if (overlay_.output()) return overlay_.output();
    return display_.first_output();
}

void App::trigger(bool region) {
    // Повторное нажатие клавиши выделения, пока рамка на экране, — отмена.
    if (region && phase_ == Phase::Selecting) {
        cancel();
        emit_event(opt_.events, "idle");
        return;
    }
    cancel();
    disarm_timer(hide_fd_);
    t_trigger_ = Clock::now();
    region_.reset();
    window_.reset();
    // В режиме «цифра» выделенная область без вариантов ответа получает короткий ответ.
    kind_ = cfg_.answer_mode == "full" ? llm::Kind::Full : (region ? llm::Kind::Auto : llm::Kind::Number);
    if (region) {
        start_select();
        return;
    }
    emit_event(opt_.events, "busy");
    target_ = pick_output();
    if (!target_) {
        fail("не найден монитор");
        return;
    }
    if (cfg_.capture == "window" && monitor_) window_ = hypr::active_window();
    overlay_.move_to(target_);

    if (!opt_.image_path.empty()) {
        std::vector<uint8_t> data;
        if (!read_file(opt_.image_path, data)) {
            fail("не удалось прочитать " + opt_.image_path);
            return;
        }
        const std::string type = sniff_image_type(data);
        if (type.empty()) {
            fail(opt_.image_path + ": нужен JPEG, PNG, GIF или WebP");
            return;
        }
        send(data, type);
        return;
    }

    if (overlay_.shown()) {
        // Старая цифра не должна попасть на снимок: прячем и ждём кадр без неё
        // (с запасным таймером на случай, если frame callback не придёт).
        phase_ = Phase::Hiding;
        arm_timer(delay_fd_, 200);
        overlay_.hide([this] {
            if (phase_ == Phase::Hiding) begin_capture();
        });
    } else {
        begin_capture();
    }
}

void App::begin_capture() {
    disarm_timer(delay_fd_);
    phase_ = Phase::Capturing;
    capturer_.start(target_, [this](const RawFrame* frame, const std::string& error) { on_frame(frame, error); });
}

// Рамку выделения рисует slurp: он печатает «x y w h монитор» в логических
// координатах и выходит (код 1, если выделение отменили клавишей Esc).
void App::start_select() {
    if (overlay_.shown()) overlay_.set("");  // старый ответ не должен мешать выделять
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        fail(std::string("pipe: ") + std::strerror(errno));
        return;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // slurp читает варианты рамок из stdin, если это не терминал, — даём пустой.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);

    // Наши заблокированные сигналы наследуются — дочернему процессу их надо вернуть.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (const int sig : {SIGUSR1, SIGUSR2, SIGINT, SIGTERM, SIGHUP, SIGPIPE, region_signal()}) sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    const char* argv[] = {"slurp", "-b", "00000055", "-c", "ffffffdd", "-s", "ffffff12", "-w", "2",
                          "-f", "%x %y %w %h %o", nullptr};
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, "slurp", &actions, &attr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        fail(rc == ENOENT ? "для выделения области нужна программа slurp (sudo pacman -S slurp)"
                          : std::string("не удалось запустить slurp: ") + std::strerror(rc));
        return;
    }
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    select_pid_ = pid;
    select_fd_ = pipefd[0];
    select_out_.clear();
    phase_ = Phase::Selecting;
    emit_event(opt_.events, "selecting");
}

void App::on_select_readable() {
    char buf[256];
    ssize_t n;
    while ((n = read(select_fd_, buf, sizeof buf)) > 0) select_out_.append(buf, static_cast<size_t>(n));
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    // Конец вывода: slurp завершился.
    close(select_fd_);
    select_fd_ = -1;
    int status = 0;
    while (waitpid(select_pid_, &status, 0) < 0 && errno == EINTR) {
    }
    select_pid_ = -1;
    finish_select(status);
}

void App::finish_select(int status) {
    phase_ = Phase::Idle;
    int x = 0, y = 0, w = 0, h = 0;
    char name[128] = {};
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                    std::sscanf(select_out_.c_str(), "%d %d %d %d %127s", &x, &y, &w, &h, name) >= 4;
    select_out_.clear();
    if (!ok || w < 8 || h < 8) {
        log_debug("выделение отменено");
        emit_event(opt_.events, "idle");
        if (opt_.once) {
            exit_code_ = 1;
            quit_ = true;
        }
        return;
    }
    // Монитор: по имени от slurp, иначе тот, где центр области.
    std::optional<hypr::Monitor> mon;
    for (auto& m : hypr::monitors()) {
        const bool by_name = name[0] && m.name == name;
        const double lw = m.scale > 0 ? m.width / m.scale : m.width, lh = m.scale > 0 ? m.height / m.scale : m.height;
        const bool by_center = x + w / 2 >= m.x && x + w / 2 < m.x + (m.transform % 2 ? lh : lw) &&
                               y + h / 2 >= m.y && y + h / 2 < m.y + (m.transform % 2 ? lw : lh);
        if (by_name || (!mon && by_center)) mon = m;
        if (by_name) break;
    }
    target_ = mon ? display_.output_by_name(mon->name) : nullptr;
    if (!target_) {
        fail("не удалось определить монитор выделенной области (нужен Hyprland)");
        return;
    }
    monitor_ = mon;
    region_ = hypr::Rect{x, y, w, h};
    log_debug("область %dx%d+%d+%d на %s", w, h, x, y, mon->name.c_str());
    overlay_.move_to(target_);
    t_trigger_ = Clock::now();
    emit_event(opt_.events, "busy");
    // Снимаем чуть позже: рамка slurp должна успеть исчезнуть с экрана.
    phase_ = Phase::Settling;
    arm_timer(delay_fd_, 150);
}

void App::stop_select() {
    if (select_pid_ > 0) {
        kill(select_pid_, SIGTERM);
        while (waitpid(select_pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
        select_pid_ = -1;
    }
    if (select_fd_ >= 0) {
        close(select_fd_);
        select_fd_ = -1;
    }
    select_out_.clear();
}

void App::on_frame(const RawFrame* frame, const std::string& error) {
    if (phase_ != Phase::Capturing) return;
    if (!frame) {
        fail("не удалось снять экран: " + error);
        return;
    }
    const double capture_ms = ms_since(t_trigger_);
    const auto t0 = Clock::now();
    RgbImage image;
    std::string err;
    if (!frame_to_rgb(*frame, crop_rect(*frame), image, err)) {
        fail(err);
        return;
    }
    const int cw = image.width, ch = image.height;
    downscale(image, cfg_.max_size);
    std::vector<uint8_t> jpeg;
    if (!encode_jpeg(image, cfg_.jpeg_quality, jpeg, err)) {
        fail("JPEG: " + err);
        return;
    }
    log_debug("снимок %dx%d (%s) за %.0f мс; область %dx%d → JPEG %dx%d, %zu КБ за %.0f мс", frame->width,
              frame->height, capturer_.backend_name(), capture_ms, cw, ch, image.width, image.height,
              jpeg.size() / 1024, ms_since(t0));
    if (!opt_.save_path.empty()) {
        if (write_file(opt_.save_path, jpeg)) log_info("снимок сохранён: %s", opt_.save_path.c_str());
        else log_info("не удалось сохранить снимок в %s", opt_.save_path.c_str());
    }
    send(jpeg, "image/jpeg");
}

// Выделенная область или активное окно (capture = window) в пикселях кадра.
std::optional<PixelRect> App::crop_rect(const RawFrame& frame) const {
    const auto& rect = region_ ? region_ : window_;
    if (!rect || !monitor_ || monitor_->scale <= 0 || monitor_->width <= 0 || monitor_->height <= 0)
        return std::nullopt;
    double lw = monitor_->width / monitor_->scale;
    double lh = monitor_->height / monitor_->scale;
    if (monitor_->transform % 2 == 1) std::swap(lw, lh);  // поворот на 90/270
    const double kx = frame.width / lw, ky = frame.height / lh;
    const int x0 = std::clamp(static_cast<int>(std::floor((rect->x - monitor_->x) * kx)), 0, frame.width);
    const int y0 = std::clamp(static_cast<int>(std::floor((rect->y - monitor_->y) * ky)), 0, frame.height);
    const int x1 = std::clamp(static_cast<int>(std::ceil((rect->x + rect->w - monitor_->x) * kx)), 0, frame.width);
    const int y1 = std::clamp(static_cast<int>(std::ceil((rect->y + rect->h - monitor_->y) * ky)), 0, frame.height);
    if (x1 - x0 < 8 || y1 - y0 < 8) return std::nullopt;
    return PixelRect{x0, y0, x1 - x0, y1 - y0};
}

void App::send(const std::vector<uint8_t>& image, const std::string& media_type) {
    auto request = llm::build_request(cfg_, base64(image.data(), image.size()), media_type, kind_, region_.has_value());
    std::string error;
    if (!http_.start(request.url, request.headers, std::move(request.body), cfg_.timeout * 1000L, error)) {
        fail("запрос: " + error);
        return;
    }
    phase_ = Phase::Asking;
    t_request_ = Clock::now();
    if (cfg_.busy_indicator) overlay_.set(Overlay::kBusy);
}

void App::on_response(const HttpResult& result) {
    if (phase_ != Phase::Asking) return;
    phase_ = Phase::Idle;
    if (result.code == CURLE_OPERATION_TIMEDOUT) {
        fail("модель не ответила за " + std::to_string(cfg_.timeout) + " с — выберите модель побыстрее");
        return;
    }
    if (result.code != CURLE_OK) {
        fail("нет связи с сервисом: " + result.error);
        return;
    }
    std::string text, error;
    if (!llm::parse_response(cfg_, result.status, result.body, text, error)) {
        fail(error);
        return;
    }
    const llm::Answer answer = llm::make_answer(text, kind_, llm::answer_limit(cfg_));
    const double seconds = ms_since(t_trigger_) / 1000;
    log_info("ответ за %.2f с (запрос %.2f с): «%s»", seconds, ms_since(t_request_) / 1000,
             llm::snippet(answer.text, 80).c_str());
    log_debug("модель: «%s»", llm::snippet(text, 300).c_str());
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.2f", seconds);
    emit_event(opt_.events, "answer",
               json_field("answer", answer.text) + "," + json_field("mode", answer.full ? "full" : "number") + "," +
                   json_field("region", region_ ? "1" : "") + "," + json_field("text", llm::snippet(text, 600)) +
                   ",\"seconds\":" + secs);
    if (opt_.once) {
        std::printf("%s\n", answer.text.c_str());
        std::fflush(stdout);
    }
    show(answer.text, answer.full);
}

void App::show(const std::string& text, bool full) {
    overlay_.set(text, full ? Overlay::Kind::Text : Overlay::Kind::Badge);
    double seconds = full ? cfg_.hide_after_full : cfg_.hide_after;
    if (opt_.once && seconds <= 0) seconds = 5;
    if (seconds > 0) arm_timer(hide_fd_, seconds * 1000);
}

void App::fail(const std::string& message) {
    log_info("ошибка: %s", message.c_str());
    emit_event(opt_.events, "error", json_field("message", message));
    cancel();
    if (opt_.once) exit_code_ = 1;
    show("!");
}

void App::cancel() {
    stop_select();
    capturer_.cancel();
    http_.cancel();
    disarm_timer(delay_fd_);
    phase_ = Phase::Idle;
}

void App::handle_signals() {
    signalfd_siginfo si;
    while (read(sig_fd_, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
        if (static_cast<int>(si.ssi_signo) == region_signal()) {
            trigger(true);
            continue;
        }
        switch (si.ssi_signo) {
        case SIGUSR1:
            trigger(false);
            break;
        case SIGUSR2:
            cancel();
            disarm_timer(hide_fd_);
            overlay_.set("");
            emit_event(opt_.events, "idle");
            break;
        default:
            quit_ = true;
            break;
        }
    }
}

void App::on_hide_timer() {
    if (phase_ != Phase::Idle) return;  // уже идёт новый запрос
    overlay_.set("");
    emit_event(opt_.events, "idle");
    if (opt_.once || opt_.show) quit_ = true;
}

}  // namespace

int main(int argc, char** argv) {
    // SIGUSR1/SIGUSR2 по умолчанию завершают процесс, а горячая клавиша может
    // прийти сразу после запуска — блокируем их первым делом.
    sigset_t early;
    sigemptyset(&early);
    sigaddset(&early, SIGUSR1);
    sigaddset(&early, SIGUSR2);
    sigaddset(&early, region_signal());
    sigprocmask(SIG_BLOCK, &early, nullptr);

    Options opt;
    enum { kOnce = 1000, kImage, kSave, kShow, kEvents, kPrintConfig, kSet, kSetStdin, kPrintVersion, kMode, kRegion };
    static const option long_options[] = {
        {"config", required_argument, nullptr, 'c'},
        {"once", no_argument, nullptr, kOnce},
        {"image", required_argument, nullptr, kImage},
        {"save", required_argument, nullptr, kSave},
        {"show", required_argument, nullptr, kShow},
        {"verbose", no_argument, nullptr, 'v'},
        {"events", no_argument, nullptr, kEvents},
        {"print-config", no_argument, nullptr, kPrintConfig},
        {"set", required_argument, nullptr, kSet},
        {"set-stdin", no_argument, nullptr, kSetStdin},
        {"version", no_argument, nullptr, kPrintVersion},
        {"mode", required_argument, nullptr, kMode},
        {"region", no_argument, nullptr, kRegion},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    for (int c; (c = getopt_long(argc, argv, "c:vh", long_options, nullptr)) != -1;) {
        switch (c) {
        case 'c': opt.config_path = optarg; break;
        case kOnce: opt.once = true; break;
        case kImage:
            opt.image_path = optarg;
            opt.once = true;
            break;
        case kSave: opt.save_path = optarg; break;
        case kShow:
            opt.show = true;
            opt.show_text = optarg;
            break;
        case 'v': opt.verbose = true; break;
        case kEvents: opt.events = true; break;
        case kPrintConfig: opt.print_config = true; break;
        case kSet:
            if (!parse_assignment(optarg, opt.set_values)) {
                log_info("--set: ожидается КЛЮЧ=ЗНАЧЕНИЕ");
                return 2;
            }
            break;
        case kSetStdin: opt.set_stdin = true; break;
        case kMode:
            opt.mode = optarg;
            if (opt.mode != "number" && opt.mode != "full") {
                log_info("--mode: number или full");
                return 2;
            }
            break;
        case kRegion: opt.region = true; break;
        case kPrintVersion: std::printf("quizhint %s\n", kVersion); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind < argc || (opt.show && opt.once)) {
        usage(stderr);
        return 2;
    }
    g_verbose = opt.verbose;

    const bool explicit_path = !opt.config_path.empty();
    const std::string path = explicit_path ? opt.config_path : default_config_path();

    if (opt.print_config) {
        Config cfg;
        std::string error;
        const bool exists = access(path.c_str(), F_OK) == 0;
        const bool ok = load_config(path, explicit_path, cfg, error) && finalize_config(cfg, false, error);
        std::printf("%s\n", config_json(cfg, path, exists, ok ? "" : error).c_str());
        return ok ? 0 : 1;
    }

    if (opt.set_stdin || !opt.set_values.empty()) {
        if (opt.set_stdin) {
            for (std::string line; std::getline(std::cin, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (!parse_assignment(line, opt.set_values)) {
                    log_info("--set-stdin: ожидаются строки КЛЮЧ=ЗНАЧЕНИЕ");
                    return 2;
                }
            }
        }
        std::string error;
        if (!update_config_file(path, opt.set_values, error)) {
            log_info("%s", error.c_str());
            return 1;
        }
        return 0;
    }

    // Сигналы принимаем через signalfd. Блокируем их до создания любых потоков
    // (резолвер curl), чтобы маска унаследовалась.
    sigset_t mask;
    sigemptyset(&mask);
    for (const int s : {SIGUSR1, SIGUSR2, SIGINT, SIGTERM, SIGHUP, region_signal()}) sigaddset(&mask, s);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    const int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) {
        log_info("signalfd: %s", std::strerror(errno));
        return 1;
    }

    auto startup_error = [&](const std::string& message) {
        log_info("%s", message.c_str());
        emit_event(opt.events, "fatal", json_field("message", message));
        return 1;
    };

    Config cfg;
    std::string error;
    if (!load_config(path, explicit_path, cfg, error) || !finalize_config(cfg, !opt.show, error))
        return startup_error(error);
    if (!opt.mode.empty()) cfg.answer_mode = opt.mode;

    if (!opt.once && !opt.show && acquire_lock() == -2)
        return startup_error("уже запущен (остановить: pkill -x quizhint)");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc;
    {
        App app(std::move(cfg), std::move(opt), sig_fd);
        rc = app.run();
    }
    curl_global_cleanup();
    return rc;
}
