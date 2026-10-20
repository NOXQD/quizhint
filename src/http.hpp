#pragma once

#include <curl/curl.h>

#include <string>
#include <vector>

struct HttpResult {
    CURLcode code = CURLE_OK;
    long status = 0;
    std::string body;
    std::string error;  // описание ошибки curl
};

// Один POST-запрос за раз через curl multi: передача идёт внутри общего
// цикла событий (curl_multi_poll), без отдельных потоков. Соединения
// переиспользуются между запросами.
class Http {
public:
    Http();
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    bool start(const std::string& url, const std::vector<std::string>& headers, std::string body,
               long timeout_ms, std::string& error);
    void cancel() { reset(); }
    bool active() const { return easy_ != nullptr; }
    CURLM* multi() const { return multi_; }

    // Продвигает передачу; true — запрос завершён, результат в out.
    bool perform(HttpResult& out);

private:
    static constexpr size_t kMaxResponse = 4 << 20;

    void reset();
    static size_t on_write(char* data, size_t size, size_t count, void* self);

    CURLM* multi_ = nullptr;
    CURL* easy_ = nullptr;
    curl_slist* headers_ = nullptr;
    std::string body_;
    std::string response_;
    char errbuf_[CURL_ERROR_SIZE] = {};
};
