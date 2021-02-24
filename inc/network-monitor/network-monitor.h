#ifndef NETWORK_MONITOR_NETWORK_MONITOR_H
#define NETWORK_MONITOR_NETWORK_MONITOR_H

#include <network-monitor/file-downloader.h>
#include <network-monitor/stomp-client.h>
#include <network-monitor/transport-network.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <spdlog/fmt/chrono.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>

namespace NetworkMonitor {

/*! \brief Configuration structure for the Live Transport Network Monitor
 *         process.
 */
struct NetworkMonitorConfig {
    std::string url;
    std::string port;
    std::string username;
    std::string password;
    std::filesystem::path caCertFile;
    std::filesystem::path networkLayoutFile;
};

/*! \brief Error codes for the Live Transport Network Monitor process.
 */
enum class NetworkMonitorError {
    kOk = 0,
    kUndefinedError,
    kCouldNotConnectToStompClient,
    kCouldNotParsePassengerEvent,
    kCouldNotRecordPassengerEvent,
    kCouldNotSubscribeToPassengerEvents,
    kFailedNetworkLayoutFileDownload,
    kFailedNetworkLayoutFileParsing,
    kFailedTransportNetworkConstruction,
    kMissingCaCertFile,
    kMissingNetworkLayoutFile,
    kStompClientDisconnected,
};

/*! \brief Print operator for the `NetworkMonitorError` class.
 */
std::ostream& operator<<(
    std::ostream& os,
    const NetworkMonitorError& m
);

/*! \brief Convert `NetworkMonitorError` to string.
 */
std::string ToString(
    const NetworkMonitorError& m
);

/*! \brief Live Transport Network Monitor
 */
template <typename WsClient>
class NetworkMonitor {
public:
    /*! \brief Default constructor.
     */
    NetworkMonitor() = default;

    /*! \brief Destructor.
     */
    ~NetworkMonitor() = default;

    /*! \brief Setup the Live Transport Network Monitor.
     *
     *  This function only sets up the connection and performs error checks.
     *  It does not run the STOMP client.
     */
    NetworkMonitorError Configure(
        const NetworkMonitorConfig& config
    )
    {
        spdlog::info("NetworkMonitor::Configure");

        // Sanity checks
        spdlog::info("Running sanity checks");
        if (!std::filesystem::exists(config.caCertFile)) {
            spdlog::error("Could not find {}. Exiting", config.caCertFile);
            return NetworkMonitorError::kMissingCaCertFile;
        }
        if (!config.networkLayoutFile.empty() &&
                !std::filesystem::exists(config.networkLayoutFile)) {
            spdlog::error("Could not find {}. Exiting",
                          config.networkLayoutFile);
            return NetworkMonitorError::kMissingNetworkLayoutFile;
        }

        // Download the network-layout.json file if the config does not contain
        // a local filename, then parse the file.
        auto networkLayoutFile {config.networkLayoutFile.empty() ?
            std::filesystem::temp_directory_path() / "network-layout.json" :
            config.networkLayoutFile
        };
        if (config.networkLayoutFile.empty()) {
            spdlog::info("Downloading the network layout file to {}",
                         networkLayoutFile);
            const std::string fileUrl {
                "https://" + config.url + networkLayoutEndpoint_
            };
            bool downloaded {DownloadFile(
                fileUrl,
                networkLayoutFile,
                config.caCertFile
            )};
            if (!downloaded) {
                spdlog::error("Could not download {}. Exiting", fileUrl);
                return NetworkMonitorError::kFailedNetworkLayoutFileDownload;
            }
        }
        spdlog::info("Loading the network layout file");
        auto parsed = ParseJsonFile(networkLayoutFile);
        if (parsed.empty()) {
            spdlog::error("Could not parse {}. Exiting", networkLayoutFile);
            return NetworkMonitorError::kFailedNetworkLayoutFileParsing;
        }

        // Network representation
        spdlog::info("Constructing the network representation");
        try {
            bool networkLoaded {network_.FromJson(std::move(parsed))};
            if (!networkLoaded) {
                spdlog::error("Could not construct the TransportNetwork. "
                              "Exiting");
                return NetworkMonitorError::kFailedTransportNetworkConstruction;
            }
        } catch (const std::exception& e) {
            spdlog::error("Exception while constructing the TransportNetwork: "
                          "{}. Exiting", e.what());
            return NetworkMonitorError::kFailedTransportNetworkConstruction;
        }

        // STOMP client
        spdlog::info("Constructing the STOMP client");
        boost::asio::ssl::context ctx {
            boost::asio::ssl::context::tlsv12_client
        };
        ctx.load_verify_file(config.caCertFile.string());
        client_ = std::make_shared<StompClient<WsClient>>(
            config.url,
            networkEventsEndpoint_,
            config.port,
            ioc_,
            ctx
        );
        client_->Connect(
            config.username,
            config.password,
            [this](auto ec) {
                OnConnect(ec);
            },
            [this](auto ec) {
                OnDisconnect(ec);
            }
        );

        // Note: At this stage nothing runs until someone calls the run()
        //       function on the I/O context object.
        spdlog::info("NetworkMonitor successfully configured");
        return NetworkMonitorError::kOk;
    }

    /*! \brief Run the I/O context.
     *
     *  This function runs the I/O context in the current thread.
     */
    void Run()
    {
        spdlog::info("Running the Live Transport Network Monitor");
        lastErrorCode_ = NetworkMonitorError::kOk;
        ioc_.run();
    }

    /*! \brief Run the I/O context for a maximum amount of time.
     *
     *  This function runs the I/O context in the current thread.
     *
     *  \param runFor   A time duration after which the I/O context stops, even
     *                  if it has outstanding work to dispatch.
     */
    template <typename DurationRep, typename DurationRatio>
    void Run(
        std::chrono::duration<DurationRep, DurationRatio> runFor
    )
    {
        spdlog::info(
            "Running the Live Transport Network Monitor for {}", runFor
        );
        lastErrorCode_ = NetworkMonitorError::kOk;
        ioc_.run_for(runFor);
    }

    /*! \brief Stop any computation.
     *
     *  This function causes the I/O context run function to cancel any
     *  outstanding work. This may leave some connections dangling or some
     *  messages not completely sent or parsed.
     */
    void Stop()
    {
        // Here we do not set lastErrorCode_ because the caller may want to
        // know what was the last error code before the network monitor was
        // stoppped.
        spdlog::info("Stopping the Live Transport Network Monitor");
        ioc_.stop();
    }

    /*! \brief Get the latest recorder error.
     *
     *  This is the last error code recorded before the I/O context run function
     *  run out of work to do.
     */
    NetworkMonitorError GetLastErrorCode() const
    {
        return lastErrorCode_;
    }

    /*! \brief Access the internal network representation.
     *
     *  \returns a reference to the internal `TransportNetwork` object instance.
     *           The object has the same lifetime as the `NetworkMonitor` class.
     */
    const TransportNetwork& GetNetworkRepresentation() const
    {
        return network_;
    }

private:
    // We maintain our own instance of the I/O context.
    boost::asio::io_context ioc_ {};

    // We keep the client as a shared pointer to allow a default constructor.
    std::shared_ptr<StompClient<WsClient>> client_ {nullptr};

    TransportNetwork network_ {};

    NetworkMonitorError lastErrorCode_ {NetworkMonitorError::kUndefinedError};

    // Remote endpoints
    const std::string networkEventsEndpoint_ {"/network-events"};
    const std::string networkLayoutEndpoint_ {"/network-layout.json"};
    const std::string subscriptionEndpoint_ {"/passengers"};

    // Handlers

    void OnConnect(
        StompClientError ec
    )
    {
        using Error = NetworkMonitorError;
        if (ec != StompClientError::kOk) {
            spdlog::error(
                "NetworkMonitor: STOMP client connection failed: {}", ec
            );
            lastErrorCode_ = Error::kCouldNotConnectToStompClient;
            client_->Close();
            return;
        }
        spdlog::info("NetworkMonitor: STOMP client connected");

        // Subscribe to the passenger events
        spdlog::info(
            "NetworkMonitor: Subscribing to {}", subscriptionEndpoint_
        );
        auto id {client_->Subscribe(
            subscriptionEndpoint_,
            [this](auto ec, auto&& id) {
                OnSubscribe(ec, std::move(id));
            },
            [this](auto ec, auto&& msg) {
                OnMessage(ec, std::move(msg));
            }
        )};
        if (id.empty()) {
            spdlog::error(
                "NetworkMonitor: STOMP client subscription failed: {}", ec
            );
            lastErrorCode_ = Error::kCouldNotSubscribeToPassengerEvents;
            client_->Close();
        }
    }

    void OnDisconnect(
        StompClientError ec
    )
    {
        spdlog::error("NetworkMonitor: STOMP client disconnected: {}", ec);
        lastErrorCode_ = NetworkMonitorError::kStompClientDisconnected;
    }

    void OnSubscribe(
        StompClientError ec,
        std::string&& subscriptionId
    )
    {
        using Error = NetworkMonitorError;
        if (ec != StompClientError::kOk) {
            spdlog::error(
                "NetworkMonitor: Unable to subscribe to {}",
                subscriptionEndpoint_
            );
            lastErrorCode_ = Error::kCouldNotSubscribeToPassengerEvents;
        } else {
            spdlog::info(
                "NetworkMonitor: STOMP client subscribed to {}",
                subscriptionEndpoint_
            );
        }
    }

    void OnMessage(
        StompClientError ec,
        std::string&& msg
    )
    {
        using Error = NetworkMonitorError;
        PassengerEvent event {};
        try {
            event = nlohmann::json::parse(msg);
        } catch (...) {
            spdlog::error(
                "NetworkMonitor: Could not parse passenger event: {}{}",
                std::setw(4), msg
            );
            lastErrorCode_ = Error::kCouldNotParsePassengerEvent;
            return;
        }
        auto ok {network_.RecordPassengerEvent(event)};
        spdlog::debug("msg {} {}", std::setw(4), msg);
        if (!ok) {
            spdlog::error(
                "NetworkMonitor: Could not record new passenger event: {}{}",
                std::setw(4), msg
            );
            lastErrorCode_ = Error::kCouldNotRecordPassengerEvent;
        } else {
            spdlog::debug(
                "NetworkMonitor: New event: {}",
                boost::posix_time::to_iso_extended_string(event.timestamp)
            );
        }
    }
};

} // namespace NetworkMonitor

#endif // NETWORK_MONITOR_NETWORK_MONITOR_H