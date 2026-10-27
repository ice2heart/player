#include "frontend/no_downloader.h"

#include <gtest/gtest.h>

using namespace oap;
using Result = Downloader::Result;

TEST(NoDownloader, IsNotConfiguredAndFinishesWithNotConfiguredOnTheNextPoll) {
    NoDownloader downloader;
    EXPECT_FALSE(downloader.configured());
    std::vector<Result> results;
    int progressCalls = 0;
    auto first = downloader.download("t1", "/nowhere/a.flac", [&](long long, long long) { ++progressCalls; },
                                     [&](Result r) { results.push_back(r); });
    auto second = downloader.download("t2", "/nowhere/b.flac", nullptr, [&](Result r) { results.push_back(r); });
    EXPECT_NE(first, 0u);
    EXPECT_NE(first, second);
    EXPECT_TRUE(results.empty()) << "callbacks run only from poll()";
    downloader.poll();
    EXPECT_EQ(results, (std::vector<Result>{Result::NotConfigured, Result::NotConfigured}));
    downloader.poll();
    EXPECT_EQ(results.size(), 2u) << "each callback runs once";
    EXPECT_EQ(progressCalls, 0);
}

TEST(NoDownloader, ACancelledDownloadNeverCallsBack) {
    NoDownloader downloader;
    int calls = 0;
    auto cancelled = downloader.download("t1", "/x/a", nullptr, [&](Result) { calls += 10; });
    downloader.download("t2", "/x/b", nullptr, [&](Result) { calls += 1; });
    downloader.cancel(cancelled);
    downloader.cancel(12345); // unknown: ignored
    downloader.poll();
    EXPECT_EQ(calls, 1);
}

TEST(NoDownloader, ADownloadCancelledByAnEarlierCallbackInTheSamePollNeverCallsBack) {
    NoDownloader downloader;
    int calls = 0;
    Downloader::RequestId second = 0;
    downloader.download("t1", "/x/a", nullptr, [&](Result) { downloader.cancel(second); });
    second = downloader.download("t2", "/x/b", nullptr, [&](Result) { ++calls; });
    downloader.poll();
    EXPECT_EQ(calls, 0);
}
