#include <gtest/gtest.h>
#include "backend/psc_power_sensor_provider.hpp"

// ---------------------------------------------------------------------------
// PscPowerSensorProvider::Handles()
// ---------------------------------------------------------------------------

TEST(PscPowerSensorHandles, ExactLeafPath) {
    PscPowerSensorProvider p;
    EXPECT_TRUE(p.Handles("/components/component[name=PSC-0]/power-supply/state/output-voltage"));
}

TEST(PscPowerSensorHandles, SubtreePath) {
    PscPowerSensorProvider p;
    EXPECT_TRUE(p.Handles("/components/component[name=PSC-0]/power-supply/state"));
}

TEST(PscPowerSensorHandles, ComponentWithoutKey) {
    PscPowerSensorProvider p;
    EXPECT_TRUE(p.Handles("/components/component"));
}

TEST(PscPowerSensorHandles, NonPscComponentIsAccepted) {
    PscPowerSensorProvider p;
    EXPECT_TRUE(p.Handles("/components/component[name=FAN-0]/state"));
}

TEST(PscPowerSensorHandles, DoesNotHandleSystemPath) {
    PscPowerSensorProvider p;
    EXPECT_FALSE(p.Handles("/system/alarms"));
}

TEST(PscPowerSensorHandles, DoesNotHandleComponentsRoot) {
    PscPowerSensorProvider p;
    EXPECT_FALSE(p.Handles("/components"));
}

TEST(PscPowerSensorHandles, DoesNotHandleEmptyPath) {
    PscPowerSensorProvider p;
    EXPECT_FALSE(p.Handles(""));
}
