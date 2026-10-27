#include "http_downloader.h"

#include <curl/curl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

namespace fs = std::filesystem;

namespace oap {

namespace {

constexpr int kPollMs = 1000; // curl_multi_poll() also returns for curl_multi_wakeup() (cancel, shutdown)
constexpr long kMaxRedirects = 5;
constexpr long kLowSpeedBytesPerSecond = 1;
constexpr long kHttpOk = 200, kHttpPartial = 206, kHttpRangeNotSatisfiable = 416;
constexpr const char* kPartSuffix = ".part";

void initCurlOnce() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

CURLM* multiOf(void* multi) { return static_cast<CURLM*>(multi); }

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// A Subsonic server answers an error (a wrong id, a bad token) with status 200
// and a document, not audio.
bool isErrorDocument(const std::string& contentType) {
    return startsWith(contentType, "text/") || startsWith(contentType, "application/json") ||
           startsWith(contentType, "application/xml");
}

long long existingSize(const fs::path& file) {
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    return ec ? 0 : static_cast<long long>(size);
}

} // namespace

// One try to fetch a file with curl, on the worker thread.
struct HttpDownloader::Fetch {
    HttpDownloader& owner;
    RequestId id;
    fs::path part;
    long long offset; // bytes of `part` kept; the request asks for the rest

    Fetch(HttpDownloader& owner, RequestId id, fs::path part, long long offset)
        : owner(owner), id(id), part(std::move(part)), offset(offset) {}

    CURL* easy = nullptr;
    std::ofstream out;
    bool begun = false;     // the answer's status is known and the file is open (if it is audio)
    bool accepted = false;  // audio that is written
    bool diskError = false;
    long status = 0;
    long long written = 0; // bytes of the file so far
    long long total = 0;

    // The first body bytes (or the end of an empty body) have come: look at the status and open the file.
    void begin() {
        begun = true;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        char* type = nullptr;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type);
        if (status != kHttpOk && status != kHttpPartial) return;
        if (type && isErrorDocument(type)) return;
        curl_off_t length = -1;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        bool resumed = status == kHttpPartial && offset > 0;
        // 200 to a Range request: the server sends the whole file again.
        written = resumed ? offset : 0;
        total = length >= 0 ? written + length : 0;
        out.open(part, std::ios::binary | (resumed ? std::ios::app : std::ios::trunc));
        if (!out) {
            diskError = true;
            return;
        }
        accepted = true;
    }

    std::size_t onData(const char* data, std::size_t bytes) {
        if (!begun) begin();
        if (diskError) return 0; // curl fails the transfer with a write error
        if (!accepted) return bytes; // an error body: dropped
        out.write(data, static_cast<std::streamsize>(bytes));
        out.flush(); // what arrived is on disk, whatever happens next
        if (!out) {
            diskError = true;
            return 0;
        }
        written += static_cast<long long>(bytes);
        std::lock_guard lock(owner.mutex_);
        owner.progress_[id] = {written, total};
        return bytes;
    }
};

HttpDownloader::HttpDownloader(UrlSource urls) : HttpDownloader(std::move(urls), Options{}) {}

HttpDownloader::HttpDownloader(UrlSource urls, Options options) : urls_(std::move(urls)), options_(options) {
    initCurlOnce();
    multi_ = curl_multi_init();
    worker_ = std::thread([this] { run(); });
}

HttpDownloader::~HttpDownloader() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    curl_multi_wakeup(multiOf(multi_));
    worker_.join();
    curl_multi_cleanup(multiOf(multi_));
}

Downloader::RequestId HttpDownloader::download(const std::string& trackId, const std::string& dest, Progress progress,
                                               Done done) {
    RequestId id = ++lastId_;
    waiting_.emplace(id, Callbacks{std::move(progress), std::move(done)});
    std::string url = urls_(trackId);
    {
        std::lock_guard lock(mutex_);
        if (url.empty()) finished_.emplace_back(id, Result::NotConfigured);
        else queue_.push_back({id, std::move(url), dest});
    }
    wake_.notify_all();
    return id;
}

void HttpDownloader::cancel(RequestId id) {
    if (!waiting_.erase(id)) return;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(queue_, [id](const Job& job) { return job.id == id; });
        progress_.erase(id);
        if (current_ == id) abort_ = true;
    }
    curl_multi_wakeup(multiOf(multi_));
}

void HttpDownloader::poll() {
    std::map<RequestId, std::pair<long long, long long>> progress;
    std::vector<std::pair<RequestId, Result>> finished;
    {
        std::lock_guard lock(mutex_);
        progress.swap(progress_);
        finished.swap(finished_);
    }
    // Callbacks may cancel other downloads or queue new ones: look each id up again.
    for (const auto& [id, bytes] : progress) {
        auto it = waiting_.find(id);
        if (it == waiting_.end() || !it->second.progress) continue;
        if (std::any_of(finished.begin(), finished.end(), [id = id](const auto& f) { return f.first == id; })) continue;
        Progress callback = it->second.progress;
        callback(bytes.first, bytes.second);
    }
    for (const auto& [id, result] : finished) {
        auto it = waiting_.find(id);
        if (it == waiting_.end()) continue;
        Done done = std::move(it->second.done);
        waiting_.erase(it);
        if (done) done(result);
    }
}

bool HttpDownloader::shouldStop() {
    std::lock_guard lock(mutex_);
    return stop_ || abort_;
}

void HttpDownloader::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            current_ = job.id;
            abort_ = false;
        }
        std::optional<Result> result = transfer(job);
        std::lock_guard lock(mutex_);
        current_ = 0;
        if (result && !abort_) finished_.emplace_back(job.id, *result);
    }
}

std::optional<Downloader::Result> HttpDownloader::transfer(const Job& job) {
    fs::path dest(job.dest);
    fs::path part = dest;
    part += kPartSuffix;
    std::error_code ec;
    if (dest.has_parent_path()) {
        fs::create_directories(dest.parent_path(), ec);
        if (ec) return Result::Disk;
    }

    for (bool fromStart = false;; fromStart = true) {
        if (shouldStop()) return std::nullopt;
        Fetch fetch{*this, job.id, part, fromStart ? 0 : existingSize(part)};
        fetch.easy = curl_easy_init();
        if (!fetch.easy) return Result::Unreachable;
        CURL* easy = fetch.easy;
        auto writeCallback = +[](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
            return static_cast<Fetch*>(user)->onData(data, size * count);
        };
        curl_easy_setopt(easy, CURLOPT_URL, job.url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fetch);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, long(options_.connectTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, long(options_.readTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, kMaxRedirects);
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85.0
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#if LIBCURL_VERSION_NUM >= 0x075700 // 7.87.0
        curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);
#endif
        // CURLOPT_RANGE, not CURLOPT_RESUME_FROM: curl would call a 200 answer an error;
        // here it is the server sending the whole file, which begin() handles.
        std::string range = std::to_string(fetch.offset) + "-";
        if (fetch.offset > 0) curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());

        CURLM* multi = multiOf(multi_);
        bool stopped = false;
        CURLcode code = CURLE_FAILED_INIT;
        if (curl_multi_add_handle(multi, easy) == CURLM_OK) {
            int running = 1;
            while (running) {
                curl_multi_perform(multi, &running);
                if (!running) break;
                if (shouldStop()) {
                    stopped = true;
                    break;
                }
                curl_multi_poll(multi, nullptr, 0, kPollMs, nullptr);
            }
            if (!stopped) {
                int left = 0;
                while (CURLMsg* msg = curl_multi_info_read(multi, &left))
                    if (msg->msg == CURLMSG_DONE) code = msg->data.result;
            }
            curl_multi_remove_handle(multi, easy);
        }
        if (!stopped && code == CURLE_OK && !fetch.begun) fetch.begin(); // an empty body
        long status = fetch.status;
        bool diskError = fetch.diskError;
        bool accepted = fetch.accepted;
        curl_easy_cleanup(easy);
        fetch.easy = nullptr;
        if (stopped) return std::nullopt;

        if (diskError) return Result::Disk;
        if (code != CURLE_OK) return Result::Unreachable;
        if (status == kHttpRangeNotSatisfiable && fetch.offset > 0 && !fromStart) continue; // start over
        if (!accepted) return Result::Server;
        fetch.out.close();
        if (!fetch.out) return Result::Disk;
        fs::rename(part, dest, ec);
        return ec ? Result::Disk : Result::Ok;
    }
}

} // namespace oap
