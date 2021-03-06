#include "websocket-client-mock.h"

#include <network-monitor/env.h>
#include <network-monitor/file-downloader.h>
#include <network-monitor/network-monitor.h>
#include <network-monitor/websocket-client.h>

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using NetworkMonitor::BoostWebSocketClient;
using NetworkMonitor::GetEnvVar;
using NetworkMonitor::MockWebSocketClientForStomp;
using NetworkMonitor::NetworkMonitorConfig;
using NetworkMonitor::NetworkMonitorError;
using NetworkMonitor::ParseJsonFile;

// Use this to set a timeout on tests that may hang or suffer from a slow
// connection.
static boost::unit_test::timeout gTimeout {3};

// This fixture is used to re-initialize all mock properties before a test.
struct NetworkMonitorTestFixture {
    NetworkMonitorTestFixture()
    {
        MockWebSocketClientForStomp::endpoint = "/passengers";
        MockWebSocketClientForStomp::username = "some_username";
        MockWebSocketClientForStomp::password = "some_password_123";
        MockWebSocketClientForStomp::connectEc = {};
        MockWebSocketClientForStomp::sendEc = {};
        MockWebSocketClientForStomp::closeEc = {};
        MockWebSocketClientForStomp::triggerDisconnection = false;
        MockWebSocketClientForStomp::subscriptionMessages = {};
    }
};

BOOST_AUTO_TEST_SUITE(network_monitor);

BOOST_AUTO_TEST_SUITE(network_monitor);

BOOST_AUTO_TEST_SUITE(enum_class_NetworkMonitorError);

BOOST_AUTO_TEST_CASE(ostream)
{
    std::stringstream invalidSs;
    invalidSs << NetworkMonitorError::kUndefinedError;
    auto invalid {invalidSs.str()};
    for (const auto& error: {
        NetworkMonitorError::kOk,
        NetworkMonitorError::kCouldNotConnectToStompClient,
        NetworkMonitorError::kCouldNotParsePassengerEvent,
        NetworkMonitorError::kCouldNotRecordPassengerEvent,
        NetworkMonitorError::kCouldNotSubscribeToPassengerEvents,
        NetworkMonitorError::kFailedNetworkLayoutFileDownload,
        NetworkMonitorError::kFailedNetworkLayoutFileParsing,
        NetworkMonitorError::kFailedTransportNetworkConstruction,
        NetworkMonitorError::kMissingCaCertFile,
        NetworkMonitorError::kMissingNetworkLayoutFile,
        NetworkMonitorError::kStompClientDisconnected,
    }) {
        std::stringstream ss;
        ss << error;
        BOOST_CHECK(ss.str() != invalid);
    }
}

BOOST_AUTO_TEST_SUITE_END(); // enum_class_NetworkMonitorError

BOOST_FIXTURE_TEST_SUITE(class_NetworkMonitor, NetworkMonitorTestFixture);

BOOST_AUTO_TEST_SUITE(Configure);

BOOST_AUTO_TEST_CASE(ok)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kOk);
}

BOOST_AUTO_TEST_CASE(ok_download_file, *gTimeout)
{
    // Note: In this test we use a mock but we download the file for real.
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        "", // Empty network layout file path. Will download
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kOk);
}

BOOST_AUTO_TEST_CASE(missing_cacert_file)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        std::filesystem::temp_directory_path() / "nonexistent_cacert.pem",
        TESTS_NETWORK_LAYOUT_JSON,
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kMissingCaCertFile);
}

BOOST_AUTO_TEST_CASE(missing_network_layout_file)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::temp_directory_path() / "nonexistent_nw_file.json",
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kMissingNetworkLayoutFile);
}

BOOST_AUTO_TEST_CASE(download_file_fail)
{
    NetworkMonitorConfig config {
        "ltnm-fail.learncppthroughprojects.com", // It will fail to download
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        "", // Empty network layout file path. Will try to download
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kFailedNetworkLayoutFileDownload);
}

BOOST_AUTO_TEST_CASE(fail_to_parse_network_layout_file)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "bad_json_file.json"
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(ec, NetworkMonitorError::kFailedNetworkLayoutFileParsing);
}

BOOST_AUTO_TEST_CASE(fail_to_construct_transport_network)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "bad_network_layout_file.json"
    };
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_CHECK_EQUAL(
        ec,
        NetworkMonitorError::kFailedTransportNetworkConstruction
    );
}

BOOST_AUTO_TEST_SUITE_END(); // Configure

BOOST_AUTO_TEST_SUITE(Run);

BOOST_AUTO_TEST_CASE(fail_to_connect_ws, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };

    // Setup the mock.
    namespace error = boost::asio::ssl::error;
    MockWebSocketClientForStomp::connectEc = error::stream_truncated;

    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run();

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(
        monitor.GetLastErrorCode(),
        NetworkMonitorError::kCouldNotConnectToStompClient
    );
}

BOOST_AUTO_TEST_CASE(fail_to_connect_auth, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "wrong_password_123", // We will fail to authenticate
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };

    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run();

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(
        monitor.GetLastErrorCode(),
        NetworkMonitorError::kStompClientDisconnected
    );
}

BOOST_AUTO_TEST_CASE(fail_to_subscribe, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };

    // Setup the mock.
    // Note: Our mock does not support a random subscription failure, so we
    //       trigger it by expecting a different subscription endpoint.
    MockWebSocketClientForStomp::endpoint = "/not-passengers";

    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run();

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(
        monitor.GetLastErrorCode(),
        NetworkMonitorError::kStompClientDisconnected
    );
}

BOOST_AUTO_TEST_CASE(fail_to_parse_passenger_event, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };

    // Setup the mock.
    MockWebSocketClientForStomp::subscriptionMessages = {
        "Not a valid JSON payload {}[]--.",
    };

    // We need to set a timeout as the network monitor is resilient to bad
    // message and would otherwise keep running indefinitely.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(150));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(
        monitor.GetLastErrorCode(),
        NetworkMonitorError::kCouldNotParsePassengerEvent
    );
}

BOOST_AUTO_TEST_CASE(fail_to_record_passenger_event, *gTimeout)
{
    // In this test we load a very simple network and then try to process a
    // passenger event for a station outside of the network.
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "from_json_1line_1route.json",
    };

    // Setup the mock.
    nlohmann::json event {
        {"datetime", "2020-11-01T07:18:50.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_42"}, // This station is not in the network
    };
    MockWebSocketClientForStomp::subscriptionMessages = {
        event.dump(),
    };

    // We need to set a timeout as the network monitor is resilient to bad
    // message and would otherwise keep running indefinitely.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(150));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(
        monitor.GetLastErrorCode(),
        NetworkMonitorError::kCouldNotRecordPassengerEvent
    );
}

BOOST_AUTO_TEST_CASE(record_1_passenger_event, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "from_json_1line_1route.json",
    };

    // Setup the mock.
    nlohmann::json event {
        {"datetime", "2020-11-01T07:18:50.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_0"},
    };
    MockWebSocketClientForStomp::subscriptionMessages = {
        event.dump(),
    };

    // We need to set a timeout otherwise the network monitor will run forever.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(150));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(monitor.GetLastErrorCode(), NetworkMonitorError::kOk);
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_0"),
        1
    );
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_1"),
        0
    );
}

BOOST_AUTO_TEST_CASE(record_2_passenger_events_same_station, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "from_json_1line_1route.json",
    };

    // Setup the mock.
    nlohmann::json event0 {
        {"datetime", "2020-11-01T07:18:50.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_0"},
    };
    nlohmann::json event1 {
        {"datetime", "2020-11-01T07:18:51.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_0"},
    };
    MockWebSocketClientForStomp::subscriptionMessages = {
        event0.dump(),
        event1.dump(),
    };

    // We need to set a timeout otherwise the network monitor will run forever.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(150));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(monitor.GetLastErrorCode(), NetworkMonitorError::kOk);
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_0"),
        2
    );
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_1"),
        0
    );
}

BOOST_AUTO_TEST_CASE(record_2_passenger_events_different_station, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        std::filesystem::path(TEST_DATA) / "from_json_1line_1route.json",
    };

    // Setup the mock.
    nlohmann::json event0 {
        {"datetime", "2020-11-01T07:18:50.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_0"},
    };
    nlohmann::json event1 {
        {"datetime", "2020-11-01T07:18:51.234000Z"},
        {"passenger_event", "in"},
        {"station_id", "station_1"},
    };
    MockWebSocketClientForStomp::subscriptionMessages = {
        event0.dump(),
        event1.dump(),
    };

    // We need to set a timeout otherwise the network monitor will run forever.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(150));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(monitor.GetLastErrorCode(), NetworkMonitorError::kOk);
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_0"),
        1
    );
    BOOST_CHECK_EQUAL(
        monitor.GetNetworkRepresentation().GetPassengerCount("station_1"),
        1
    );
}

BOOST_AUTO_TEST_CASE(record_passenger_events_from_file, *gTimeout)
{
    NetworkMonitorConfig config {
        "ltnm.learncppthroughprojects.com",
        "443",
        "some_username",
        "some_password_123",
        TESTS_CACERT_PEM,
        TESTS_NETWORK_LAYOUT_JSON,
    };

    // Setup the mock.
    auto events = ParseJsonFile(
        std::filesystem::path(TEST_DATA) / "passenger_events.json"
    ).get<std::vector<nlohmann::json>>();
    std::vector<std::string> messages;
    messages.reserve(events.size());
    for (const auto& event: events) {
        messages.emplace_back(event.dump());
    }
    MockWebSocketClientForStomp::subscriptionMessages = std::move(messages);

    // Load the expected results.
    auto counts = ParseJsonFile(
        std::filesystem::path(TEST_DATA) / "passenger_events_count.json"
    ).get<std::unordered_map<std::string, long long int>>();

    // We need to set a timeout otherwise the network monitor will run forever.
    NetworkMonitor::NetworkMonitor<MockWebSocketClientForStomp> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(1000));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(monitor.GetLastErrorCode(), NetworkMonitorError::kOk);
    const auto& network {monitor.GetNetworkRepresentation()};
    for (const auto& [stationId, passengerCount]: counts) {
        BOOST_CHECK_EQUAL(network.GetPassengerCount(stationId), passengerCount);
    }
}

BOOST_AUTO_TEST_CASE(live, *gTimeout)
{
    NetworkMonitorConfig config {
        GetEnvVar("LTNM_SERVER_URL", "ltnm.learncppthroughprojects.com"),
        GetEnvVar("LTNM_SERVER_PORT", "443"),
        GetEnvVar("LTNM_USERNAME"),
        GetEnvVar("LTNM_PASSWORD"),
        TESTS_CACERT_PEM,
        GetEnvVar("LTNM_NETWORK_LAYOUT_FILE_PATH", TESTS_NETWORK_LAYOUT_JSON),
    };

    // We simply run the live server for 500ms and check that it did not have
    // any errors while running.
    NetworkMonitor::NetworkMonitor<BoostWebSocketClient> monitor;
    auto ec {monitor.Configure(config)};
    BOOST_REQUIRE_EQUAL(ec, NetworkMonitorError::kOk);
    monitor.Run(std::chrono::milliseconds(1000));

    // When we arrive here, the Run() function ran out of things to do.
    BOOST_CHECK_EQUAL(monitor.GetLastErrorCode(), NetworkMonitorError::kOk);
}

BOOST_AUTO_TEST_SUITE_END(); // Run

BOOST_AUTO_TEST_SUITE_END(); // class_NetworkMonitor

BOOST_AUTO_TEST_SUITE_END(); // network_monitor

BOOST_AUTO_TEST_SUITE_END(); // network_monitor