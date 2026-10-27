// The FakeDownloader is what App tests use to stand in for downloads, so its
// contract is checked here: callbacks run only from poll() and only after the
// test finished the download, once, never after cancel(); a finished download
// leaves its file, a failed one its .part.

#include "fakes.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using oap::test::FakeDownloader;
using Result = oap::Downloader::Result;
namespace fs = std::filesystem;

namespace {
std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}
} // namespace

TEST(FakeDownloader, CallbackRunsOnlyFromPollAfterFinishAndOnce) {
    FakeDownloader downloader;
    std::vector<Result> results;
    auto id = downloader.download("t1", "/x/a.flac", nullptr, [&](Result r) { results.push_back(r); });
    EXPECT_EQ(downloader.lastRequest().trackId, "t1");
    EXPECT_EQ(downloader.lastRequest().dest, "/x/a.flac");
    downloader.poll();
    EXPECT_TRUE(results.empty()) << "nothing finishes until the test says so";
    ASSERT_TRUE(downloader.finish(id, Result::Server, false));
    EXPECT_TRUE(results.empty()) << "and then only from poll()";
    downloader.poll();
    EXPECT_EQ(results, std::vector<Result>{Result::Server});
    downloader.poll();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_FALSE(downloader.finish(id, Result::Ok, false)) << "already finished";
}

TEST(FakeDownloader, ProgressIsDeliveredFromPoll) {
    FakeDownloader downloader;
    long long bytes = 0, total = 0;
    auto id = downloader.download("t1", "/x/a", [&](long long b, long long t) { bytes = b, total = t; }, nullptr);
    ASSERT_TRUE(downloader.progress(id, 10, 40));
    EXPECT_EQ(bytes, 0);
    downloader.poll();
    EXPECT_EQ(bytes, 10);
    EXPECT_EQ(total, 40);
}

TEST(FakeDownloader, ACancelledDownloadNeverCallsBackAndCannotBeFinished) {
    FakeDownloader downloader;
    int calls = 0;
    auto id = downloader.download("t1", "/x/a", nullptr, [&](Result) { ++calls; });
    downloader.cancel(id);
    EXPECT_EQ(downloader.cancelled, std::vector<FakeDownloader::RequestId>{id});
    EXPECT_FALSE(downloader.finish(id, Result::Ok, false));
    downloader.poll();
    EXPECT_EQ(calls, 0);
}

TEST(FakeDownloader, AFinishedDownloadCancelledBeforeThePollNeverCallsBack) {
    FakeDownloader downloader;
    int calls = 0;
    auto id = downloader.download("t1", "/x/a", nullptr, [&](Result) { ++calls; });
    ASSERT_TRUE(downloader.finish(id, Result::Ok, false));
    downloader.cancel(id);
    downloader.poll();
    EXPECT_EQ(calls, 0);
}

TEST(FakeDownloader, OkWritesTheFileAndAFailureLeavesThePart) {
    fs::path dir = oap::test::testTempDir();
    fs::remove_all(dir);
    FakeDownloader downloader;
    auto ok = downloader.download("t1", (dir / "al1" / "01.flac").string(), nullptr, nullptr);
    auto bad = downloader.download("t2", (dir / "al1" / "02.flac").string(), nullptr, nullptr);
    ASSERT_TRUE(downloader.finish(ok, Result::Ok, true, "song"));
    ASSERT_TRUE(downloader.finish(bad, Result::Unreachable, true, "song", "so"));
    EXPECT_EQ(slurp(dir / "al1" / "01.flac"), "song");
    EXPECT_FALSE(fs::exists(dir / "al1" / "02.flac"));
    EXPECT_EQ(slurp(dir / "al1" / "02.flac.part"), "so");
    fs::remove_all(dir);
}

TEST(FakeDownloader, NotConfiguredFinishesWithNotConfiguredOnTheNextPoll) {
    FakeDownloader downloader;
    downloader.isConfigured = false;
    Result result = Result::Ok;
    downloader.download("t1", "/x/a", nullptr, [&](Result r) { result = r; });
    downloader.poll();
    EXPECT_EQ(result, Result::NotConfigured);
}
