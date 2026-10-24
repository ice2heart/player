#include "net/md5.h"

#include <gtest/gtest.h>

using oap::md5Hex;

TEST(Md5, Rfc1321Vectors) {
    EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5Hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    EXPECT_EQ(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a"); // more than one 64-byte block
}

TEST(Md5, SubsonicDocumentationToken) {
    // The example in the Subsonic API docs: password "sesame", salt "c19b2d".
    EXPECT_EQ(md5Hex("sesamec19b2d"), "26719a1196d2a940705a59634eb18eab");
}
