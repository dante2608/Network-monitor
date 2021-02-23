#ifndef NETWORK_MONITOR_STOMP_CLIENT_H
#define NETWORK_MONITOR_STOMP_CLIENT_H

#include <network-monitor/stomp-frame.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

#include <iomanip>
#include <iostream>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace NetworkMonitor {

/*! \brief Error codes for the STOMP client.
 */
enum class StompClientError {
    kOk = 0,
    kUndefinedError,
    kCouldNotCloseWebSocketsConnection,
    kCouldNotConnectToWebSocketsServer,
    kCouldNotSendStompFrame,
    kCouldNotSendSubscribeFrame,
    kUnexpectedCouldNotCreateValidFrame,
    kUnexpectedMessageContentType,
    kUnexpectedSubscriptionMismatch,
    kWebSocketsServerDisconnected,
};

/*! \brief Print operator for the `StompClientError` class.
 */
std::ostream& operator<<(std::ostream& os, const StompClientError& m);

/*! \brief Convert `StompClientError` to string.
 */
std::string ToString(const StompClientError& m);

/*! \brief STOMP client implementing the subset of commands needed by the
 *         network-events service.
 */
template <typename WsClient>
class StompClient {
public:
    /*! \brief Construct a STOMP client connecting to a remote URL/port through
     *         a secure WebSockets connection.
     */
    StompClient(
        const std::string& url,
        const std::string& endpoint,
        const std::string& port,
        boost::asio::io_context& ioc,
        boost::asio::ssl::context& ctx
    ) : ws_ {url, endpoint, port, ioc, ctx},
        url_ {url},
        context_ {boost::asio::make_strand(ioc)}
    {
    }

    /*! \brief The copy constructor is deleted.
     */
    StompClient(const StompClient& other) = delete;

    /*! \brief Move constructor.
     */
    StompClient(StompClient&& other) = default;

    /*! \brief The copy assignment operator is deleted.
     */
    StompClient& operator=(const StompClient& other) = delete;

    /*! \brief Move assignment operator.
     */
    StompClient& operator=(StompClient&& other) = default;

    /*! \brief Connect to the STOMP server.
     *
     *  This method first connects to the WebSockets server, then tries to
     *  establish a STOMP connection with the user credentials.
     *
     *  \param username     Username
     *  \param password     Password
     *  \param onConnect    This handler is called when the STOMP connection
     *                      is setup correctly. It will also be called with an
     *                      error if there is any failure before a successful
     *                      connection.
     *  \param onDisconnect This handler is called when the STOMP or the
     *                      WebSockets connection is suddenly closed. In the
     *                      STOMP protocol, this may happen also in response to
     *                      bad inputs (authentication, subscription).
     *
     *  All handlers run in a separate I/O execution context from the WebSockets
     *  one.
     */
    void Connect(
        const std::string& username,
        const std::string& password,
        std::function<void (StompClientError)> onConnect = nullptr,
        std::function<void (StompClientError)> onDisconnect = nullptr
    )
    {
        username_ = username;
        password_ = password;
        onConnect_ = onConnect;
        onDisconnect_ = onDisconnect;
        ws_.Connect(
            [this](auto ec) {
                OnWsConnect(ec);
            },
            [this](auto ec, auto&& msg) {
                OnWsMessage(ec, std::move(msg));
            },
            [this](auto ec) {
                OnWsDisconnect(ec);
            }
        );
    }

    /*! \brief Close the STOMP and WebSockets connection.
     *
     *  \param onClose This handler is called when the connection has been
     *                 closed.
     *
     *  All handlers run in a separate I/O execution context from the WebSockets
     *  one.
     */
    void Close(
        std::function<void (StompClientError)> onClose = nullptr
    )
    {
        subscriptions_.clear();
        ws_.Close(
            [this, onClose](auto ec) {
                OnWsClose(ec, onClose);
            }
        );
    }

    /*! \brief Subscribe to a STOMP enpoint.
     *
     *  \returns The subscription ID.
     *
     *  \param onSubscribe  This handler is called when the subscription is
     *                      setup correctly. The handler receives an error code
     *                      and the subscription ID. Note: The STOMP server
     *                      closes the WebSocket connection automatically on a
     *                      failed attempt to subscribe. This handler is only
     *                      called when the failure happens at the WebSocket,
     *                      not at the STOMP level.
     *  \param onMessage    This handler is called on every new message from the
     *                      subscription endpoint.
     *
     *  All handlers run in a separate I/O execution context from the WebSockets
     *  one.
     */
    std::string Subscribe(
        const std::string& subscriptionEndpoint,
        std::function<void (StompClientError, std::string&&)> onSubscribe,
        std::function<void (StompClientError, std::string&&)> onMessage
    )
    {
        auto subscriptionId {GenerateId()};
        Subscription subscription {
            subscriptionEndpoint,
            onSubscribe,
            onMessage,
        };

        // Assemble the SUBSCRIBE frame.
        // We use the subscription ID to also request a receipt, so the server
        // will confirm if we are subscribed.
        StompError error;
        StompFrame frame(
            error,
            StompCommand::kSubscribe,
            {
                {StompHeader::kId, subscriptionId},
                {StompHeader::kDestination, subscriptionEndpoint},
                {StompHeader::kAck, "auto"},
                {StompHeader::kReceipt, subscriptionId},
            }
        );
        if (error != StompError::kOk) {
            auto clientError {
                StompClientError::kUnexpectedCouldNotCreateValidFrame
            };
            boost::asio::post(
                context_,
                [
                    onSubscribe,
                    clientError,
                    subscriptionId = std::move(subscriptionId)
                ]() mutable {
                    if (onSubscribe) {
                        onSubscribe(clientError, std::move(subscriptionId));
                    }
                }
            );
            return "";
        }

        // Send the WebSockets message.
        ws_.Send(
            frame.ToString(),
            [
                this,
                subscriptionId,
                subscription = std::move(subscription)
            ](auto ec) mutable {
                OnWsSendSubscribe(
                    ec,
                    std::move(subscriptionId),
                    std::move(subscription)
                );
            }
        );
        return subscriptionId;
    }

private:
    // This strand handles all the STOMP subscription messages. These operations
    // are decoupled from the WebSockets operations.
    // We leave it uninitialized because it does not support a default
    // constructor.
    boost::asio::strand<boost::asio::io_context::executor_type> context_;

    std::string url_ {};

    // We leave this uninitialized because it does not support a default
    // constructor.
    WsClient ws_;

    std::function<void (StompClientError)> onConnect_ {nullptr};
    std::function<void (StompClientError)> onDisconnect_ {nullptr};
    std::string username_ {};
    std::string password_ {};

    struct Subscription {
        std::string endpoint {};
        std::function<void (
            StompClientError,
            std::string&&
        )> onSubscribe {nullptr};
        std::function<void (
            StompClientError,
            std::string&&
        )> onMessage {nullptr};
    };

    // We store subscriptions in a map so we can retrieve the message
    // handler for the right subscription when a message arrives.
    std::unordered_map<std::string, Subscription> subscriptions_ {};


    void OnWsConnect(
        boost::system::error_code ec
    )
    {

        // We cannot continue if the connection was not established correctly.
        if (ec) {
            spdlog::error("OnWsConnect", ec.message());
            boost::asio::post(
                context_,
                [this]() {
                    if (onConnect_) {
                        onConnect_(
                            StompClientError::kCouldNotConnectToWebSocketsServer
                        );
                    }
                }
            );
            return;
        }

        // Assemble and send the STOMP frame.
        StompError error;
        StompFrame frame(
            error,
            StompCommand::kStomp,
            {
                {StompHeader::kAcceptVersion, "1.2"},
                {StompHeader::kHost, url_},
                {StompHeader::kLogin, username_},
                {StompHeader::kPasscode, password_},
            }
        );
        if (error != StompError::kOk) {
            auto clientError {
                StompClientError::kUnexpectedCouldNotCreateValidFrame
            };
            spdlog::error("OnWsConnect: {}: {}", clientError, error);
            boost::asio::post(
                context_,
                [this, clientError]() {
                    if (onConnect_) {
                        onConnect_(clientError);
                    }
                }
            );
            return;
        }
        ws_.Send(
            frame.ToString(),
            [this](auto ec) {
                OnWsSendStomp(ec);
            }
        );
    }

    void OnWsSendStomp(
        boost::system::error_code ec
    )
    {
        // If we got here, it only means that we correctly sent the STOMP
        // frame, not that we are authenticated. OnWsMessage is the function
        // that handles the response from the server.
        if (ec) {
            spdlog::error("OnWsSendStomp: {}", ec.message());
            boost::asio::post(
                context_,
                [this]() {
                    if (onConnect_) {
                        onConnect_(StompClientError::kCouldNotSendStompFrame);
                    }
                }
            );
        }
    }

    void OnWsSendSubscribe(
        boost::system::error_code ec,
        std::string&& subscriptionId,
        Subscription&& subscription
    )
    {
        // At this stage we can only know if we successfully sent the SUBSCRIBE
        // command, but not if our subscription was acknowledged.
        if (!ec) {
            // Save the subscription.
            subscriptions_.emplace(
                subscriptionId,
                std::move(subscription)
            );
        } else {
            // Notify the user.
            spdlog::error("OnWsSendSubscribe: {}", ec.message());
            boost::asio::post(
                context_,
                [onSubscribe = subscription.onSubscribe]() {
                    if (onSubscribe) {
                        onSubscribe(
                            StompClientError::kCouldNotSendSubscribeFrame,
                            ""
                        );
                    }
                }
            );
        }
    }

    void OnWsMessage(
        boost::system::error_code ec,
        std::string&& msg
    )
    {
        // Parse the message.
        StompError error;
        StompFrame frame(error, std::move(msg));
        if (error != StompError::kOk) {
            auto clientError {
                StompClientError::kUnexpectedCouldNotCreateValidFrame
            };
            spdlog::error("OnWsMessage: {}: {}", clientError, error);
            boost::asio::post(
                context_,
                [this, clientError]() {
                    if (onConnect_) {
                        onConnect_(clientError);
                    }
                }
            );
        }

        // Decide what to do based on the STOMP command.
        spdlog::debug("OnWsMessage: Received {}", frame.GetCommand());
        switch (frame.GetCommand()) {
            case StompCommand::kConnected: {
                HandleConnected(std::move(frame));
                break;
            }
            case StompCommand::kError: {
                HandleError(std::move(frame));
                break;
            }
            case StompCommand::kMessage: {
                HandleSubscriptionMessage(std::move(frame));
                break;
            }
            case StompCommand::kReceipt: {
                HandleSubscriptionReceipt(std::move(frame));
                break;
            }
            default: {
                spdlog::error("OnWsMessage: Unexpected STOMP command: {}",
                              frame.GetCommand());
                break;
            }
        }
    }

    void OnWsDisconnect(
        boost::system::error_code ec
    )
    {
        // Notify the user.
        spdlog::error("OnWsDisconnect: {}", ec.message());
        auto error {ec ? StompClientError::kWebSocketsServerDisconnected :
                         StompClientError::kOk};
        boost::asio::post(
            context_,
            [this, error]() {
                if (onDisconnect_) {
                    onDisconnect_(error);
                }
            }
        );
    }

    void OnWsClose(
        boost::system::error_code ec,
        std::function<void (StompClientError)> onClose = nullptr
    )
    {
        // Notify the user.
        auto error {ec ? StompClientError::kCouldNotCloseWebSocketsConnection :
                         StompClientError::kOk};
        boost::asio::post(
            context_,
            [onClose, error]() {
                if (onClose) {
                    onClose(error);
                }
            }
        );
    }

    void HandleConnected(
        StompFrame&& frame
    )
    {
        // Notify the user of the susccessful connection.
        boost::asio::post(
            context_,
            [this]() {
                if (onConnect_) {
                    onConnect_(StompClientError::kOk);
                }
            }
        );
    }

    void HandleError(
        StompFrame&& frame
    )
    {
        // At the moment we do not handle errors, we only print a message.
        spdlog::error("HandleError: The STOMP frame returned an error: {}",
                      frame.GetBody());
    }

    void HandleSubscriptionMessage(
        StompFrame&& frame
    )
    {
        // Find the subscription.
        auto subscriptionId {frame.GetHeaderValue(StompHeader::kSubscription)};
        auto subscriptionIt {subscriptions_.find(std::string(subscriptionId))};
        if (subscriptionIt == subscriptions_.end()) {
            spdlog::error("HandleSubscriptionMessage: Cannot find subscription");
            return;
        }
        const auto& subscription {subscriptionIt->second};

        // Check the endpoint.
        auto endpoint {frame.GetHeaderValue(StompHeader::kDestination)};
        if (endpoint != subscription.endpoint) {
            boost::asio::post(
                context_,
                [onMessage = subscription.onMessage]() {
                    if (onMessage) {
                        onMessage(
                            StompClientError::kUnexpectedSubscriptionMismatch,
                            {}
                        );
                    }
                }
            );
            return;
        }

        // Send the message to the user handler.
        boost::asio::post(
            context_,
            [
                onMessage = subscription.onMessage,
                message = std::string(frame.GetBody())
            ]() mutable {
                if (onMessage) {
                    onMessage(StompClientError::kOk, std::move(message));
                }
            }
        );
    }

    void HandleSubscriptionReceipt(
        StompFrame&& frame
    )
    {
        // Find the subscription.
        // When we send the SUBSCRIBE frame, we request a receipt with the same
        // ID of the subscription so that it's easier to retrieve it here.
        auto subscriptionId {frame.GetHeaderValue(StompHeader::kReceiptId)};
        auto subscriptionIt {subscriptions_.find(std::string(subscriptionId))};
        if (subscriptionIt == subscriptions_.end()) {
            spdlog::error("HandleSubscriptionReceipt: Cannot find subscription");
            return;
        }
        const auto& subscription {subscriptionIt->second};

        // Notify the user of the susccessful subscription.
        boost::asio::post(
            context_,
            [
                onSubscribe = subscription.onSubscribe,
                subscriptionId = std::string(subscriptionId)
            ]() mutable {
                if (onSubscribe) {
                    onSubscribe(
                        StompClientError::kOk,
                        std::move(subscriptionId)
                    );
                }
            }
        );
    }

    std::string GenerateId()
    {
        std::stringstream ss {};
        ss << boost::uuids::random_generator()();
        return ss.str();
    }
};

} // namespace NetworkMonitor

#endif // NETWORK_MONITOR_STOMP_CLIENT_H