#include "http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace oap {

namespace {

// curl_multi_poll() returns earlier for curl's own timers (timeouts) and for
// curl_multi_wakeup() (new request, cancel, shutdown), so an idle client sleeps.
constexpr int kIdleWaitMs = 60'000;
constexpr long kMaxRedirects = 5;
// Less than this many bytes per second for readTimeout ends a request.
constexpr long kLowSpeedBytesPerSecond = 1;

struct Transfer {
    CURL* easy = nullptr;
    Http::Id id = 0;
    Http::Handler handler;
    std::string body;
    std::size_t maxBody = 0;
};

std::size_t onData(char* data, std::size_t size, std::size_t count, void* user) {
    auto* transfer = static_cast<Transfer*>(user);
    std::size_t bytes = size * count;
    if (transfer->body.size() + bytes > transfer->maxBody) return 0; // curl fails the request
    transfer->body.append(data, bytes);
    return bytes;
}

void initCurlOnce() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

CURLM* multiOf(void* multi) { return static_cast<CURLM*>(multi); }

} // namespace

HttpClient::HttpClient() : HttpClient(Options{}) {}

HttpClient::HttpClient(Options options) : options_(options) {
    initCurlOnce();
    multi_ = curl_multi_init();
    worker_ = std::thread([this] { run(); });
}

HttpClient::~HttpClient() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    curl_multi_wakeup(multiOf(multi_));
    worker_.join();
    curl_multi_cleanup(multiOf(multi_));
}

void HttpClient::get(Id id, std::string url, Handler handler) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({id, std::move(url), std::move(handler)});
    }
    curl_multi_wakeup(multiOf(multi_));
}

void HttpClient::cancel(Id id) {
    {
        std::lock_guard lock(mutex_);
        auto queued = std::find_if(queue_.begin(), queue_.end(), [id](const Job& job) { return job.id == id; });
        if (queued != queue_.end()) {
            queue_.erase(queued);
            return;
        }
        cancels_.push_back(id);
    }
    curl_multi_wakeup(multiOf(multi_));
}

void HttpClient::run() {
    CURLM* multi = multiOf(multi_);
    std::map<Id, std::unique_ptr<Transfer>> active;
    auto drop = [&](Id id) {
        auto it = active.find(id);
        if (it == active.end()) return;
        curl_multi_remove_handle(multi, it->second->easy);
        curl_easy_cleanup(it->second->easy);
        active.erase(it);
    };
    auto start = [&](Job& job) {
        drop(job.id); // the same id again replaces the old request
        auto transfer = std::make_unique<Transfer>();
        transfer->easy = curl_easy_init();
        transfer->id = job.id;
        transfer->handler = std::move(job.handler);
        transfer->maxBody = options_.maxBody;
        if (!transfer->easy) {
            transfer->handler(Response{});
            return;
        }
        CURL* easy = transfer->easy;
        curl_easy_setopt(easy, CURLOPT_URL, job.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onData);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, long(options_.connectTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, long(options_.readTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, long(options_.totalTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, kMaxRedirects);
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85.0
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, ""); // whatever curl can decode, e.g. gzip
#if LIBCURL_VERSION_NUM >= 0x075700 // 7.87.0
        // Removing a request that is still resolving its host name must not wait for the resolver.
        curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);
#endif
        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            curl_easy_cleanup(easy);
            transfer->handler(Response{});
            return;
        }
        active.emplace(job.id, std::move(transfer));
    };

    for (;;) {
        std::deque<Job> jobs;
        std::vector<Id> cancels;
        {
            std::lock_guard lock(mutex_);
            if (stop_) break;
            jobs.swap(queue_);
            cancels.swap(cancels_);
        }
        for (Id id : cancels) drop(id);
        for (Job& job : jobs) start(job);

        int running = 0;
        curl_multi_perform(multi, &running);
        int left = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
            if (msg->msg != CURLMSG_DONE) continue;
            Transfer* transfer = nullptr;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &transfer);
            Response response;
            response.reached = msg->data.result == CURLE_OK;
            if (response.reached) {
                curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &response.status);
                response.body = std::move(transfer->body);
            }
            Id id = transfer->id;
            Handler handler = std::move(transfer->handler);
            drop(id); // invalidates msg and transfer
            handler(std::move(response));
        }
        curl_multi_poll(multi, nullptr, 0, kIdleWaitMs, nullptr);
    }
    for (auto& [id, transfer] : active) {
        curl_multi_remove_handle(multi, transfer->easy);
        curl_easy_cleanup(transfer->easy);
    }
}

} // namespace oap
