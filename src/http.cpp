#include "http.hpp"

#include <algorithm>

Http::Http() : multi_(curl_multi_init()) {}

Http::~Http() {
    reset();
    if (multi_) curl_multi_cleanup(multi_);
}

size_t Http::on_write(char* data, size_t size, size_t count, void* self) {
    auto& response = static_cast<Http*>(self)->response_;
    const size_t n = size * count;
    if (response.size() + n > kMaxResponse) return 0;  // прерывает передачу
    response.append(data, n);
    return n;
}

bool Http::start(const std::string& url, const std::vector<std::string>& headers, std::string body,
                 long timeout_ms, std::string& error) {
    reset();
    if (!multi_ || !(easy_ = curl_easy_init())) {
        error = "не удалось инициализировать curl";
        reset();
        return false;
    }
    body_ = std::move(body);
    for (const auto& h : headers) headers_ = curl_slist_append(headers_, h.c_str());
    headers_ = curl_slist_append(headers_, "Expect:");
    errbuf_[0] = '\0';

    curl_easy_setopt(easy_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy_, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers_);
    curl_easy_setopt(easy_, CURLOPT_POSTFIELDS, body_.data());
    curl_easy_setopt(easy_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body_.size()));
    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &Http::on_write);
    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(easy_, CURLOPT_ERRORBUFFER, errbuf_);
    curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 10000L));
    curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy_, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy_, CURLOPT_USERAGENT, "quizhint/1.0");

    const CURLMcode mc = curl_multi_add_handle(multi_, easy_);
    if (mc != CURLM_OK) {
        error = curl_multi_strerror(mc);
        reset();
        return false;
    }
    int running = 0;
    curl_multi_perform(multi_, &running);
    return true;
}

bool Http::perform(HttpResult& out) {
    if (!easy_) return false;
    int running = 0;
    curl_multi_perform(multi_, &running);
    int queued = 0;
    while (CURLMsg* msg = curl_multi_info_read(multi_, &queued)) {
        if (msg->msg != CURLMSG_DONE || msg->easy_handle != easy_) continue;
        out.code = msg->data.result;
        curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &out.status);
        out.body = std::move(response_);
        out.error = errbuf_[0] ? errbuf_ : curl_easy_strerror(out.code);
        reset();
        return true;
    }
    return false;
}

void Http::reset() {
    if (easy_) {
        curl_multi_remove_handle(multi_, easy_);
        curl_easy_cleanup(easy_);
        easy_ = nullptr;
    }
    if (headers_) {
        curl_slist_free_all(headers_);
        headers_ = nullptr;
    }
    std::string().swap(body_);  // тело с картинкой большое — освобождаем память
    response_.clear();
}
