#include "ESP8266WiFi.h"
#include "arduino-mock/Arduino.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <math.h>

#include "../../src/wifi_module/wifi_module.ino"

using ::testing::AtLeast;
using ::testing::AtMost;
using ::testing::InSequence;
using ::testing::InvokeWithoutArgs;
using ::testing::Return;
using ::testing::_;

class WifiModuleFixture : public ::testing::Test
{
public:
    ArduinoMock* arduinoMock;
    ESP8266Mock* esp8266Mock;

    unsigned long incrementByOneSecond()
    {
        arduinoMock->addMillisSecs(1);
        return arduinoMock->getMillis();
    }

    // Run this before the tests
    virtual void SetUp()
    {
        arduinoMock             = arduinoMockInstance();
        esp8266Mock             = esp8266MockInstance();
        currentTransmissionSize = DEFAULT_TRANSMISSION_SIZE;
        // Workaround for millis() not really returning the mock value.
        // TO-DO: Consider whether arduino-mock should be changed to accomodate this
        ON_CALL(*arduinoMock, millis())
            .WillByDefault(InvokeWithoutArgs(this, &WifiModuleFixture::incrementByOneSecond));
    }
    // Run this after the tests
    virtual void TearDown()
    {
        releaseArduinoMock();
        releaseEsp8266Mock();
    }
};

TEST_F(WifiModuleFixture, setup_whenCalled_willSetPins)
{
    // Pins are initialized as inputs or outputs
    EXPECT_CALL(*arduinoMock, pinMode(RX_PIN, INPUT));
    EXPECT_CALL(*arduinoMock, pinMode(TX_PIN, OUTPUT));

    // Output pin initial states are set
    EXPECT_CALL(*arduinoMock, digitalWrite(TX_PIN, LOW)).Times(AtLeast(1));

    // We might go to sleep, so expect a signal to the power controller
    EXPECT_CALL(*arduinoMock, digitalWrite(TX_PIN, HIGH)).Times(AtMost(1));

    setup();
}

TEST_F(WifiModuleFixture, setup_whenCalled_willSetStationMode)
{
    EXPECT_CALL(*esp8266Mock, mode(WIFI_STA));

    setup();
}

TEST_F(WifiModuleFixture, setup_whenConnected_willNotSleep)
{
    EXPECT_CALL(*esp8266Mock, begin(_, _));
    EXPECT_CALL(*esp8266Mock, status()).WillOnce(Return(WL_CONNECTED));
    EXPECT_CALL(*esp8266Mock, deepSleep(_)).Times(0);

    setup();
}

TEST_F(WifiModuleFixture, setup_whenConnectionFailed_willSleep)
{
    EXPECT_CALL(*esp8266Mock, begin(_, _));
    EXPECT_CALL(*esp8266Mock, status()).Times(AtMost(CONNECTION_RETRIES)).WillRepeatedly(Return(WL_CONNECT_FAILED));

    EXPECT_CALL(*esp8266Mock, deepSleep(_));

    setup();
}

TEST_F(WifiModuleFixture, setup_whenConnectionFailedFewTimesBeforeSuccess_willNotSleep)
{
    EXPECT_CALL(*esp8266Mock, begin(_, _));
    InSequence s;
    // First fail a few times
    EXPECT_CALL(*esp8266Mock, status()).Times(CONNECTION_RETRIES - 1).WillRepeatedly(Return(WL_CONNECT_FAILED));
    // But then succeed
    EXPECT_CALL(*esp8266Mock, status()).WillOnce(Return(WL_CONNECTED));
    EXPECT_CALL(*esp8266Mock, deepSleep(_)).Times(0);

    setup();
}

TEST_F(WifiModuleFixture, getNetworkContext_whenCalled_willReturnValidPair)
{
    String  expectedMAC    = "00-14-22-01-23-45";
    int32_t expectedRSSI   = 32;
    auto    expectedResult = std::make_pair(expectedMAC, expectedRSSI);

    EXPECT_CALL(*esp8266Mock, BSSIDstr(_)).WillOnce(Return(expectedMAC));
    EXPECT_CALL(*esp8266Mock, RSSI(_)).WillOnce(Return(expectedRSSI));

    ASSERT_EQ(getNetworkContext(0), expectedResult);
}

TEST_F(WifiModuleFixture, getDatapoints_whenNoNetworkFound_willReturnEmpty)
{
    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(0));

    ASSERT_EQ(getDatapoints().size(), 0);
}

TEST_F(WifiModuleFixture, getDatapoints_whenNoSSIDMatch_willReturnEmpty)
{
    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(10));
    EXPECT_CALL(*esp8266Mock, SSID(_)).WillRepeatedly(Return("SomeOtherSSID"));

    ASSERT_EQ(getDatapoints().size(), 0);
}

TEST_F(WifiModuleFixture, getDatapoints_whenSSIDMatch_willReturnValid)
{
    const int8_t                           networksFound     = 10;
    const uint8_t                          accessPointsFound = 2;
    std::array<String, accessPointsFound>  expectedMAC       = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI      = { 12, 2 };

    std::vector<std::pair<String, int32_t>> expectedDatapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(networksFound));
    {
        InSequence s;
        EXPECT_CALL(*esp8266Mock, SSID(_))
            .Times(networksFound - accessPointsFound)
            .WillRepeatedly(Return("SomeOtherSSID"));
        // Return one of the SSID's used for positioning
        EXPECT_CALL(*esp8266Mock, SSID(_)).Times(accessPointsFound).WillRepeatedly(Return(*positioningNetworks.begin()));
    }
    EXPECT_CALL(*esp8266Mock, BSSIDstr(_)).WillOnce(Return(expectedMAC[0])).WillOnce(Return(expectedMAC[1]));
    EXPECT_CALL(*esp8266Mock, RSSI(_)).WillOnce(Return(expectedRSSI[0])).WillOnce(Return(expectedRSSI[1]));

    ASSERT_EQ(getDatapoints(), expectedDatapoints);
}

TEST_F(WifiModuleFixture, transmitData_whenNoDatapoints_willReturnFalse)
{
    std::vector<std::pair<String, int32_t>> datapoints;

    ASSERT_FALSE(transmitData(datapoints));
}

TEST_F(WifiModuleFixture, transmitData_whenConnectionFails_willReturnFalse)
{
    const uint8_t                          accessPointsFound = 2;
    std::array<String, accessPointsFound>  expectedMAC       = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI      = { 12, 2 };

    std::vector<std::pair<String, int32_t>> datapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, connect(_, _)).WillOnce(Return(false));

    ASSERT_FALSE(transmitData(datapoints));
}

TEST_F(WifiModuleFixture, transmitData_whenConnectionTimeout_willReturnFalse)
{
    const uint8_t                          accessPointsFound = 2;
    std::array<String, accessPointsFound>  expectedMAC       = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI      = { 12, 2 };

    std::vector<std::pair<String, int32_t>> datapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, connect(_, _)).WillOnce(Return(true));
    EXPECT_CALL(*esp8266Mock, stop()); // If we connected we should also stop

    EXPECT_CALL(*esp8266Mock, available()).WillRepeatedly(Return(0));

    ASSERT_FALSE(transmitData(datapoints));
}

TEST_F(WifiModuleFixture, transmitData_whenConnectionNoTimeout_willReturnTrue)
{
    const uint8_t                          accessPointsFound = 2;
    std::array<String, accessPointsFound>  expectedMAC       = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI      = { 12, 2 };

    std::vector<std::pair<String, int32_t>> datapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, connect(_, _)).WillOnce(Return(true));
    EXPECT_CALL(*esp8266Mock, stop()); // If we connected we should also stop

    EXPECT_CALL(*esp8266Mock, print(_));
    EXPECT_CALL(*esp8266Mock, available()).WillOnce(Return(1));

    ASSERT_TRUE(transmitData(datapoints));
}

TEST_F(WifiModuleFixture, loop_whenNoDatapointsFound_willNotConnect)
{
    // No datapoints were found
    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(10));
    EXPECT_CALL(*esp8266Mock, SSID(_)).WillRepeatedly(Return("SomeOtherSSID"));
    // No connection or transmission
    EXPECT_CALL(*esp8266Mock, connect(_, _)).Times(0);

    loop();
}

TEST_F(WifiModuleFixture, loop_whenDatapointsFound_willTransmit)
{
    // Change the current transmission size so to make it easier to test that
    // the correct number of transmissions will be made.
    currentTransmissionSize = 1;
    // Some valid datapoints found
    const int8_t  networksFound         = 10;
    const uint8_t accessPointsFound     = 2;
    const uint8_t numberOfTransmissions = ceil(accessPointsFound / static_cast<double>(currentTransmissionSize));

    std::array<String, accessPointsFound>  expectedMAC  = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI = { 12, 2 };

    std::vector<std::pair<String, int32_t>> expectedDatapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(networksFound));
    {
        InSequence s;
        EXPECT_CALL(*esp8266Mock, SSID(_))
            .Times(networksFound - accessPointsFound)
            .WillRepeatedly(Return("SomeOtherSSID"));
        // Return one of the SSID's used for positioning
        EXPECT_CALL(*esp8266Mock, SSID(_)).Times(accessPointsFound).WillRepeatedly(Return(*positioningNetworks.begin()));
    }
    EXPECT_CALL(*esp8266Mock, BSSIDstr(_)).WillOnce(Return(expectedMAC[0])).WillOnce(Return(expectedMAC[1]));
    EXPECT_CALL(*esp8266Mock, RSSI(_)).WillOnce(Return(expectedRSSI[0])).WillOnce(Return(expectedRSSI[1]));

    // Transmit datapoints
    EXPECT_CALL(*esp8266Mock, connect(_, _)).Times(numberOfTransmissions).WillRepeatedly(Return(true));
    EXPECT_CALL(*esp8266Mock, print(_)).Times(numberOfTransmissions);
    EXPECT_CALL(*esp8266Mock, available()).Times(numberOfTransmissions).WillRepeatedly(Return(1));

    loop();
}

TEST_F(WifiModuleFixture, loop_whenTransmissionFails_willStopTransmitting)
{
    // Change the transmission size to make it apparent that the transmission
    // will stop after the first failure.
    currentTransmissionSize = 1;
    // Some valid datapoints found
    const int8_t                           networksFound     = 10;
    const uint8_t                          accessPointsFound = 2;
    std::array<String, accessPointsFound>  expectedMAC       = { "00-14-22-01-23-45", "01-24-22-AA-23-FF" };
    std::array<int32_t, accessPointsFound> expectedRSSI      = { 12, 2 };

    std::vector<std::pair<String, int32_t>> expectedDatapoints
        = { std::make_pair(expectedMAC[0], expectedRSSI[0]), std::make_pair(expectedMAC[1], expectedRSSI[1]) };

    EXPECT_CALL(*esp8266Mock, scanNetworks()).WillOnce(Return(networksFound));
    {
        InSequence s;
        EXPECT_CALL(*esp8266Mock, SSID(_))
            .Times(networksFound - accessPointsFound)
            .WillRepeatedly(Return("SomeOtherSSID"));
        // Return one of the SSID's used for positioning
        EXPECT_CALL(*esp8266Mock, SSID(_)).Times(accessPointsFound).WillRepeatedly(Return(*positioningNetworks.begin()));
    }
    EXPECT_CALL(*esp8266Mock, BSSIDstr(_)).WillOnce(Return(expectedMAC[0])).WillOnce(Return(expectedMAC[1]));
    EXPECT_CALL(*esp8266Mock, RSSI(_)).WillOnce(Return(expectedRSSI[0])).WillOnce(Return(expectedRSSI[1]));

    // Transmit datapoints only once
    EXPECT_CALL(*esp8266Mock, connect(_, _)).WillOnce(Return(true));
    EXPECT_CALL(*esp8266Mock, print(_)).Times(1);
    // Make sure that the transmission will always fail
    EXPECT_CALL(*esp8266Mock, available()).WillRepeatedly(Return(0));

    loop();
}

int main(int argc, char* argv[])
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
