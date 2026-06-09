#include <gtest/gtest.h>
#include <gnmi.grpc.pb.h>
#include "utils/utils.h"

// ---------------------------------------------------------------------------
// xpath_to_gnmi_path
// ---------------------------------------------------------------------------

TEST(XpathToGnmiPath, SingleElement) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components", &path);

    ASSERT_EQ(path.elem_size(), 1);
    EXPECT_EQ(path.elem(0).name(), "components");
    EXPECT_TRUE(path.elem(0).key().empty());
}

TEST(XpathToGnmiPath, MultiLevel) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component/state", &path);

    ASSERT_EQ(path.elem_size(), 3);
    EXPECT_EQ(path.elem(0).name(), "components");
    EXPECT_EQ(path.elem(1).name(), "component");
    EXPECT_EQ(path.elem(2).name(), "state");
}

TEST(XpathToGnmiPath, KeyWithoutQuotes) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component[name=PSC-0]", &path);

    ASSERT_EQ(path.elem_size(), 2);
    EXPECT_EQ(path.elem(1).name(), "component");
    ASSERT_TRUE(path.elem(1).key().count("name") > 0);
    EXPECT_EQ(path.elem(1).key().at("name"), "PSC-0");
}

TEST(XpathToGnmiPath, KeyWithDoubleQuotes) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component[name=\"PSC-0\"]", &path);

    ASSERT_TRUE(path.elem(1).key().count("name") > 0);
    EXPECT_EQ(path.elem(1).key().at("name"), "PSC-0");
}

TEST(XpathToGnmiPath, KeyWithSingleQuotes) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component[name='PSC-0']", &path);

    ASSERT_TRUE(path.elem(1).key().count("name") > 0);
    EXPECT_EQ(path.elem(1).key().at("name"), "PSC-0");
}

TEST(XpathToGnmiPath, QuotedAndUnquotedProduceSameKey) {
    gnmi::Path unquoted, quoted;
    xpath_to_gnmi_path("/components/component[name=PSC-0]",     &unquoted);
    xpath_to_gnmi_path("/components/component[name=\"PSC-0\"]", &quoted);

    EXPECT_EQ(unquoted.elem(1).key().at("name"),
              quoted.elem(1).key().at("name"));
}

TEST(XpathToGnmiPath, FullPscPath) {
    gnmi::Path path;
    xpath_to_gnmi_path(
        "/components/component[name=PSC-0]/power-supply/state/output-voltage",
        &path);

    ASSERT_EQ(path.elem_size(), 5);
    EXPECT_EQ(path.elem(0).name(), "components");
    EXPECT_EQ(path.elem(1).name(), "component");
    EXPECT_EQ(path.elem(1).key().at("name"), "PSC-0");
    EXPECT_EQ(path.elem(2).name(), "power-supply");
    EXPECT_EQ(path.elem(3).name(), "state");
    EXPECT_EQ(path.elem(4).name(), "output-voltage");
}

TEST(XpathToGnmiPath, EmptyPathProducesNoElems) {
    gnmi::Path path;
    xpath_to_gnmi_path("", &path);

    EXPECT_EQ(path.elem_size(), 0);
}

TEST(XpathToGnmiPath, WithoutLeadingSlash) {
    gnmi::Path path;
    xpath_to_gnmi_path("components/component", &path);

    ASSERT_EQ(path.elem_size(), 2);
    EXPECT_EQ(path.elem(0).name(), "components");
    EXPECT_EQ(path.elem(1).name(), "component");
}

// ---------------------------------------------------------------------------
// gnmi_to_xpath
// ---------------------------------------------------------------------------

TEST(GnmiToXpath, EmptyPathProducesEmptyString) {
    gnmi::Path path;
    EXPECT_EQ(gnmi_to_xpath(path), "");
}

TEST(GnmiToXpath, SingleElement) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components", &path);

    EXPECT_EQ(gnmi_to_xpath(path), "/components");
}

TEST(GnmiToXpath, MultiLevel) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component/state", &path);

    EXPECT_EQ(gnmi_to_xpath(path), "/components/component/state");
}

TEST(GnmiToXpath, KeyAlwaysQuotedInOutput) {
    gnmi::Path path;
    xpath_to_gnmi_path("/components/component[name=PSC-0]", &path);

    // Output always uses double quotes regardless of input quoting
    EXPECT_EQ(gnmi_to_xpath(path), "/components/component[name=\"PSC-0\"]");
}

TEST(GnmiToXpath, RoundTripPreservesKeyValue) {
    const std::string original =
        "/components/component[name=PSC-0]/power-supply/state/output-voltage";

    gnmi::Path path;
    xpath_to_gnmi_path(original, &path);

    // Round-trip output differs in quoting ([name=PSC-0] → [name="PSC-0"])
    // but the key value is preserved
    gnmi::Path path2;
    xpath_to_gnmi_path(gnmi_to_xpath(path), &path2);

    ASSERT_EQ(path.elem_size(), path2.elem_size());
    for (int i = 0; i < path.elem_size(); ++i) {
        EXPECT_EQ(path.elem(i).name(), path2.elem(i).name());
        EXPECT_EQ(path.elem(i).key().size(), path2.elem(i).key().size());
        for (const auto& [k, v] : path.elem(i).key()) {
            ASSERT_TRUE(path2.elem(i).key().count(k) > 0);
            EXPECT_EQ(v, path2.elem(i).key().at(k));
        }
    }
}
