#include <gtest/gtest.h>
#include "backend/psc_power_sensor_provider.hpp"

// ---------------------------------------------------------------------------
// PscPowerSensorProvider::fill()
// ---------------------------------------------------------------------------

TEST(PscPowerSensorFill, ExactLeafProducesOneUpdate) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/components/component[name=PSC-0]/state/temperature/instant");
    EXPECT_EQ(list.size(), 1);
}

TEST(PscPowerSensorFill, SubtreeProducesMultipleUpdates) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/components/component[name=PSC-0]/power-supply/state");
    EXPECT_EQ(list.size(), 6);  // 6 power-supply leaves
}

TEST(PscPowerSensorFill, AllUnitsProducesUpdatesForBoth) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/components/component");
    EXPECT_EQ(list.size(), 14);  // 7 leaves × 2 units
}

TEST(PscPowerSensorFill, NonPscUnitProducesNoUpdates) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/components/component[name=FAN-0]/state");
    EXPECT_EQ(list.size(), 0);
}

TEST(PscPowerSensorFill, UnrecognizedLeafProducesNoUpdates) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> list;
    p.fill(&list, "/components/component[name=PSC-0]/nonexistent/leaf");
    EXPECT_EQ(list.size(), 0);
}

TEST(PscPowerSensorFill, QuotedKeyProducesSameResultAsUnquoted) {
    PscPowerSensorProvider p;
    RepeatedPtrField<Update> unquoted, quoted;
    p.fill(&unquoted, "/components/component[name=PSC-0]/state/temperature/instant");
    p.fill(&quoted,   "/components/component[name=\"PSC-0\"]/state/temperature/instant");
    EXPECT_EQ(unquoted.size(), quoted.size());
}
