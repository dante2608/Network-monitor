#include "websocket-client-mock.h"

#include <network-monitor/stomp-client.h>
#include <network-monitor/websocket-client.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <string>

using NetworkMonitor::BoostWebSocketClient;
using NetworkMonitor::MockWebSocketClientForStomp;
using NetworkMonitor::StompClient;
using NetworkMonitor::StompClientError;
using NetworkMonitor::StompCommand;
using NetworkMonitor::StompError;
using NetworkMonitor::StompFrame;

using namespace std::string_literals;

// Use this to set a timeout on tests that may hang or suffer from a slow
// connection.
static boost::unit_test::timeout gTimeout {3};

// This fixture is used to re-initialize all mock properties before a test.
struct StompClientTestFixture {
    StompClientTestFixture()
    {
        MockWebSocketClientForStomp::endpoint = "/passengers";
        MockWebSocketClientForStomp::username = "some_username";
        MockWebSocketClientForStomp::password = "some_password_123";
        MockWebSocketClientForStomp::connectEc = {};
        MockWebSocketClientForStomp::sendEc = {};
        MockWebSocketClientForStomp::closeEc = {};
        MockWebSocketClientForStomp::triggerDisconnection = false;
        MockWebSocketClientForStomp::messageQueue = {};
        MockWebSocketClientForStomp::subscriptionMessages = {};
    }
};

BOOST_AUTO_TEST_SUITE(network_monitor);

BOOST_AUTO_TEST_SUITE(stomp_client);

BOOST_AUTO_TEST_SUITE(enum_class_StompClientError);

BOOST_AUTO_TEST_CASE(ostream)
{
    std::stringstream invalidSs;
    invalidSs << StompClientError::kUndefinedError;
    auto invalid {invalidSs.str()};
    for (const auto& error: {
        StompClientError::kOk,
        StompClientError::kCouldNotCloseWebSocketsConnection,
        StompClientError::kCouldNotConnectToWebSocketsServer,
        StompClientError::kCouldNotSendStompFrame,
        StompClientError::kCouldNotSendSubscribeFrame,
        StompClientError::kUnexpectedCouldNotCreateValidFrame,
        StompClientError::kUnexpectedMessageContentType,
        StompClientError::kUnexpectedSubscriptionMismatch,
        StompClientError::kWebSocketsServerDisconnected,
    }) {
        std::stringstream ss {};
        ss << error;
        BOOST_CHECK(ss.str() != invalid);
    }
}

BOOST_AUTO_TEST_SUITE_END(); // enum_class_StompClientError

BOOST_FIXTURE_TEST_SUITE(class_StompClient, StompClientTestFixture);

BOOST_AUTO_TEST_CASE(connect, *gTimeout)
{
    // Since we use the mock, we do not actually connect to this remote.
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool connected {false};
    auto onConnect {[&client, &connected](auto ec) {
        connected = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
        client.Close([](auto ec) {});
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(connected);
}

BOOST_AUTO_TEST_CASE(connect_nullptr, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    // Because in this test we do not use the onConnect handler, we need to
    // close the connection after some time.
    // Note: This test does not actually check that we connect, only that we do
    //       not fail because of the nullptr callback.
    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    client.Connect(username, password, nullptr);
    bool didTimeout {false};
    boost::asio::high_resolution_timer timer {ioc};
    timer.expires_after(std::chrono::milliseconds {250});
    timer.async_wait([&didTimeout, &client](auto ec) {
        didTimeout = true;
        BOOST_CHECK(!ec);
        client.Close([](auto ec) {});
    });
    ioc.run();
    BOOST_CHECK(didTimeout);
}

BOOST_AUTO_TEST_CASE(fail_to_connect_ws, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    // Setup the mock.
    namespace error = boost::asio::ssl::error;
    MockWebSocketClientForStomp::connectEc = error::stream_truncated;

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool calledOnConnect {false};
    auto onConnect {[&calledOnConnect](auto ec) {
        calledOnConnect = true;
        BOOST_CHECK_EQUAL(
            ec,
            StompClientError::kCouldNotConnectToWebSocketsServer
        );
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(calledOnConnect);
}

BOOST_AUTO_TEST_CASE(fail_to_connect_auth, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_bad_password_123"}; // Bad password
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    // When we fail to authenticate, the serve closes our connection.
    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    auto onConnect {[](auto ec) {
        // We should never get here.
        BOOST_CHECK(false);
    }};
    bool calledOnDisconnect {false};
    auto onDisconnect {[&calledOnDisconnect](auto ec) {
        calledOnDisconnect = true;
        BOOST_CHECK_EQUAL(
            ec,
            StompClientError::kWebSocketsServerDisconnected
        );
    }};
    client.Connect(username, password, onConnect, onDisconnect);
    ioc.run();
    BOOST_CHECK(calledOnDisconnect);
}

BOOST_AUTO_TEST_CASE(close, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool closed {false};
    auto onClose {[&closed](auto ec) {
        closed = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
    }};
    auto onConnect {[&client, &onClose](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Close(onClose);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(close_nullptr, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool connected {false};
    auto onConnect {[&client, &connected](auto ec) {
        connected = true;
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Close(nullptr);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    // If we got here the Close() worked.
    BOOST_CHECK(connected);
}

BOOST_AUTO_TEST_CASE(close_before_connect, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool closed {false};
    auto onClose {[&closed](auto ec) {
        closed = true;
        BOOST_CHECK_EQUAL(
            ec,
            StompClientError::kCouldNotCloseWebSocketsConnection
        );
    }};
    client.Close(onClose);
    ioc.run();
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(subscribe, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool calledOnSubscribe {false};
    auto onSubscribe {[&calledOnSubscribe, &client](auto ec, auto&& id) {
        calledOnSubscribe = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
        BOOST_CHECK(id != "");
        client.Close([](auto ec) {});
    }};
    auto onMessage {[](auto ec, auto&& msg) {
    }};
    auto onConnect {[&client, &onSubscribe, &onMessage](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        auto id {client.Subscribe("/passengers", onSubscribe, onMessage)};
        BOOST_REQUIRE(id != "");
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(calledOnSubscribe);
}

BOOST_AUTO_TEST_CASE(subscribe_onSubscribe_nullptr, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    // Setup the mock.
    MockWebSocketClientForStomp::subscriptionMessages = {
        "{counter: 1}",
    };

    // This test requires the subscription to send a valid message for us to
    // say: "yes, we did subscribe".
    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool subscribed {false};
    auto onMessage {[&subscribed, &client](auto ec, auto&& msg) {
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
        subscribed = true;
        client.Close([](auto ec) {});
    }};
    auto onConnect {[&client, &onMessage](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Subscribe("/passengers", nullptr, onMessage);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(subscribed);
}

BOOST_AUTO_TEST_CASE(subscribe_onMessage_nullptr, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool calledOnSubscribe {false};
    auto onSubscribe {[&calledOnSubscribe, &client](auto ec, auto&& id) {
        calledOnSubscribe = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
        BOOST_CHECK(id != "");
        client.Close([](auto ec) {});
    }};
    auto onConnect {[&client, &onSubscribe](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Subscribe("/passengers", onSubscribe, nullptr);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(calledOnSubscribe);
}

BOOST_AUTO_TEST_CASE(subscribe_get_message, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    // Setup the mock.
    MockWebSocketClientForStomp::subscriptionMessages = {
        "{counter: 1}",
    };

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool messageReceived {false};
    auto onMessage {[&messageReceived, &client](auto ec, auto&& msg) {
        messageReceived = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
        client.Close([](auto ec) {});
    }};
    auto onConnect {[&client, &onMessage](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Subscribe("/passengers", nullptr, onMessage);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(messageReceived);
}

BOOST_AUTO_TEST_CASE(subscribe_before_connect, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool calledOnSubscribe {false};
    auto onSubscribe {[&calledOnSubscribe, &client](auto ec, auto&& id) {
        calledOnSubscribe = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kCouldNotSendSubscribeFrame);
        BOOST_CHECK_EQUAL(id, "");
        client.Close([](auto ec) {});
    }};
    auto onMessage {[](auto ec, auto&& msg) {
        // We should never get here.
        BOOST_CHECK(false);
    }};
    client.Subscribe("/passengers", onSubscribe, onMessage);
    ioc.run();
    BOOST_CHECK(calledOnSubscribe);
}

BOOST_AUTO_TEST_CASE(subscribe_after_close, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    bool calledOnSubscribe {false};
    auto onSubscribe {[&calledOnSubscribe](auto ec, auto&& id) {
        calledOnSubscribe = true;
        BOOST_CHECK_EQUAL(ec, StompClientError::kCouldNotSendSubscribeFrame);
        BOOST_CHECK_EQUAL(id, "");
    }};
    auto onClose {[&client, &onSubscribe](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Subscribe("/passengers", onSubscribe, nullptr);
    }};
    auto onConnect {[&client, &onClose](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Close(onClose);
    }};
    client.Connect(username, password, onConnect);
    ioc.run();
    BOOST_CHECK(calledOnSubscribe);
}

BOOST_AUTO_TEST_CASE(subscribe_to_invalid_endpoint, *gTimeout)
{
    const std::string url {"ltnm.learncppthroughprojects.com"};
    const std::string endpoint {"/network-events"};
    const std::string port {"443"};
    const std::string username {"some_username"};
    const std::string password {"some_password_123"};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);

    StompClient<MockWebSocketClientForStomp> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };
    auto onSubscribe {[](auto ec, auto&& id) {
        // We should never get here.
        BOOST_CHECK(false);
    }};
    bool calledOnDisconnect {false};
    auto onDisconnect {[&calledOnDisconnect](auto ec) {
        calledOnDisconnect = true;
        BOOST_CHECK_EQUAL(
            ec,
            StompClientError::kWebSocketsServerDisconnected
        );
    }};
    auto onConnect {[&client, &onSubscribe](auto ec) {
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        client.Subscribe("/invalid", onSubscribe, nullptr);
    }};
    client.Connect(username, password, onConnect, onDisconnect);
    ioc.run();
    BOOST_CHECK(calledOnDisconnect);
}

static std::string GetEnvVar(
    const std::string& envVar,
    const std::string& defaultValue = ""
)
{
    const char* value {std::getenv(envVar.c_str())};
    if (defaultValue == "") {
        BOOST_REQUIRE(value != nullptr);
    }
    return value != nullptr ? value : defaultValue;
}

BOOST_AUTO_TEST_CASE(live, *gTimeout)
{
    const std::string url {GetEnvVar(
        "LTNM_SERVER_URL",
        "ltnm.learncppthroughprojects.com"
    )};
    const std::string endpoint {"/network-events"};
    const std::string port {GetEnvVar("LTNM_SERVER_PORT", "443")};
    boost::asio::io_context ioc {};
    boost::asio::ssl::context ctx {boost::asio::ssl::context::tlsv12_client};
    ctx.load_verify_file(TESTS_CACERT_PEM);
    const std::string username {GetEnvVar("LTNM_USERNAME")};
    const std::string password {GetEnvVar("LTNM_PASSWORD")};

    StompClient<BoostWebSocketClient> client {
        url,
        endpoint,
        port,
        ioc,
        ctx
    };

    bool calledOnClose {false};
    auto onClose {[&calledOnClose](auto ec) {
        calledOnClose = true;
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
    }};

    // We cannot guarantee that we will get a message, so we close the
    // connection on a successful subscription.
    bool calledOnSubscribe {false};
    auto onSubscribe {[
        &calledOnSubscribe,
        &client,
        &onClose
    ](auto ec, auto&& id) {
        calledOnSubscribe = true;
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        BOOST_REQUIRE(id != "");
        client.Close(onClose);
    }};

    // Receiving messages from the live service is not guaranteed, as it depends
    // on the time of the day. If we do receive a message, we check that it is
    // valid.
    auto onMessage {[](auto ec, auto&& msg) {
        BOOST_CHECK_EQUAL(ec, StompClientError::kOk);
    }};

    bool calledOnConnect {false};
    auto onConnect {[
        &calledOnConnect,
        &client,
        &onSubscribe,
        &onMessage
    ](auto ec) {
        calledOnConnect = true;
        BOOST_REQUIRE_EQUAL(ec, StompClientError::kOk);
        auto id {client.Subscribe("/passengers", onSubscribe, onMessage)};
        BOOST_REQUIRE(id != "");
    }};

    client.Connect(username, password, onConnect);

    ioc.run();

    BOOST_CHECK(calledOnConnect);
    BOOST_CHECK(calledOnSubscribe);
    BOOST_CHECK(calledOnClose);
}

BOOST_AUTO_TEST_SUITE_END(); // class_StompClient

BOOST_AUTO_TEST_SUITE_END(); // stomp_client

BOOST_AUTO_TEST_SUITE_END(); // network_monitor