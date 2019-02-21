// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fastrtps/transport/TCPChannelResourceSecure.h>
#include <fastrtps/transport/TCPTransportInterface.h>
#include <fastrtps/utils/IPLocator.h>
#include <fastrtps/utils/eClock.h>

namespace eprosima {
namespace fastrtps {
namespace rtps {

using namespace asio;

TCPChannelResourceSecure::TCPChannelResourceSecure(
        TCPTransportInterface* parent,
        RTCPMessageManager* rtcpManager,
        asio::io_service& service,
        asio::ssl::context& ssl_context,
        const Locator_t& locator,
        uint32_t maxMsgSize)
    : TCPChannelResource(parent, rtcpManager, locator, maxMsgSize)
    , service_(service)
    , ssl_context_(ssl_context)
    , secure_socket_(new asio::ssl::stream<asio::ip::tcp::socket>(service_, ssl_context_))
{
    set_tls_verify_mode(parent->configuration());
}

TCPChannelResourceSecure::TCPChannelResourceSecure(
        TCPTransportInterface* parent,
        RTCPMessageManager* rtcpManager,
        asio::io_service& service,
        asio::ssl::context& ssl_context,
        asio::ssl::stream<asio::ip::tcp::socket>* socket,
        uint32_t maxMsgSize)
    : TCPChannelResource(parent, rtcpManager, maxMsgSize)
    , service_(service)
    , ssl_context_(ssl_context)
    , secure_socket_(socket)
{
    set_tls_verify_mode(parent->configuration());
}

TCPChannelResourceSecure::~TCPChannelResourceSecure()
{
    // Take both mutexes to avoid the situation where
    // A checked alive and was true
    // A took mutex
    // B disables the channel resource
    // B destroy us
    // A tries to perform an operation causing calling a virtual method (our parent still lives).
    std::unique_lock<std::recursive_mutex> read_lock(read_mutex());
    std::unique_lock<std::recursive_mutex> write_lock(write_mutex());
    delete secure_socket_;
}

void TCPChannelResourceSecure::connect()
{
    using asio::ip::tcp;
    using TLSHSRole = TCPTransportDescriptor::TLSConfig::TLSHandShakeRole;
    std::unique_lock<std::mutex> scoped(status_mutex_);
    if (connection_status_ == eConnectionStatus::eDisconnected)
    {
        connection_status_ = eConnectionStatus::eConnecting;
        try
        {
            Locator_t locator = locator_;

            ip::tcp::resolver resolver(service_);

            auto endpoints = resolver.resolve(
                IPLocator::hasWan(locator_) ? IPLocator::toWanstring(locator_) : IPLocator::ip_to_string(locator_),
                std::to_string(IPLocator::getPhysicalPort(locator_)));

            TCPTransportInterface* parent = parent_;
            asio::ssl::stream<asio::ip::tcp::socket>* secure_socket = secure_socket_;

            asio::async_connect(secure_socket_->lowest_layer(), endpoints,
                [secure_socket, locator, parent](const std::error_code& error,
                    const tcp::endpoint& /*endpoint*/)
            {
                if (!error)
                {
                    ssl::stream_base::handshake_type role = ssl::stream_base::client;
                    if (parent->configuration()->tls_config.handshake_role == TLSHSRole::SERVER)
                    {
                        role = ssl::stream_base::server;
                    }

                    logError(RTCP_TLS, "Connected: " << IPLocator::to_string(locator));
                    std::cout << "Connected: " << IPLocator::to_string(locator) << std::endl;

                    secure_socket->async_handshake(role,
                        [locator, parent](const std::error_code& error)
                    {
                        if (!error)
                        {
                            logError(RTCP_TLS, "Handshake OK: " << IPLocator::to_string(locator));
                            std::cout << "Handshake OK: " << IPLocator::to_string(locator) << std::endl;
                            parent->SocketConnected(locator, error);
                        }
                        else
                        {
                            logError(RTCP_TLS, "Handshake failed: " << error.message());
                            std::cout << "Handshake failed: " << error.message() << std::endl;
                            eClock::my_sleep(5000); // Retry, but after a big while
                            parent->SocketConnected(locator, error);
                        }
                    });
                }
                else
                {
                    //logError(RTCP_TLS, "Connect failed: " << error.message());
                    parent->SocketConnected(locator, error); // Manages errors and retries
                }
            });
        }
        catch(const std::system_error &error)
        {
            logError(RTCP, "Openning socket " << error.what());
        }
    }
}

void TCPChannelResourceSecure::disconnect()
{
    if (change_status(eConnectionStatus::eDisconnected))
    {
        try
        {
            asio::error_code ec;
            //secure_socket_->next_layer().cancel();
            //secure_socket_->shutdown();
            secure_socket_->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            secure_socket_->lowest_layer().cancel();

          // This method was added on the version 1.12.0
#if ASIO_VERSION >= 101200 && (!defined(_WIN32_WINNT) || _WIN32_WINNT >= 0x0603)
            secure_socket_->lowest_layer().release();
#endif
        }
        catch (std::exception&)
        {
            // Cancel & shutdown throws exceptions if the socket has been closed ( Test_TCPv4Transport )
        }
        secure_socket_->lowest_layer().close();
    }
}

uint32_t TCPChannelResourceSecure::read(
        octet* buffer,
        std::size_t size,
        asio::error_code& ec)
{
    std::unique_lock<std::recursive_mutex> read_lock(read_mutex());
    return static_cast<uint32_t>(secure_socket_->read_some(asio::buffer(buffer, size), ec));
    /*
    size_t rec = 0;
    size_t readed = static_cast<size_t>(-1);

    while (readed != 0 && rec < size && !ec && alive())
    {
        readed = secure_socket_->read_some(asio::buffer(buffer + rec, size - rec), ec);
        rec += readed;
    }
    return static_cast<uint32_t>(rec);
    */
}

uint32_t TCPChannelResourceSecure::send(
        const octet* data,
        size_t size,
        asio::error_code& ec)
{
    std::unique_lock<std::recursive_mutex> write_lock(write_mutex());
    return static_cast<uint32_t>(secure_socket_->write_some(asio::buffer(data, size), ec));
    /*
    size_t sent = 0;
    size_t aux = static_cast<size_t>(-1);

    while (aux != 0 && sent < size && !ec && alive())
    {
        aux = secure_socket_->write_some(asio::buffer(data + sent, size - sent), ec);
        sent += aux;
    }

    return static_cast<uint32_t>(sent);
    */
}

asio::ip::tcp::endpoint TCPChannelResourceSecure::remote_endpoint() const
{
    return secure_socket_->lowest_layer().remote_endpoint();
}

asio::ip::tcp::endpoint TCPChannelResourceSecure::local_endpoint() const
{
    return secure_socket_->lowest_layer().local_endpoint();
}

void TCPChannelResourceSecure::set_options(const TCPTransportDescriptor* options)
{
    secure_socket_->lowest_layer().set_option(socket_base::receive_buffer_size(options->receiveBufferSize));
    secure_socket_->lowest_layer().set_option(socket_base::send_buffer_size(options->sendBufferSize));
    secure_socket_->lowest_layer().set_option(ip::tcp::no_delay(options->enable_tcp_nodelay));
}

void TCPChannelResourceSecure::set_tls_verify_mode(const TCPTransportDescriptor* options)
{
    using TLSVerifyMode = TCPTransportDescriptor::TLSConfig::TLSVerifyMode;

    if (options->apply_security)
    {
        if (options->tls_config.verify_mode != TLSVerifyMode::UNUSED)
        {
            ssl::verify_mode vm = 0x00;
            if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_NONE))
            {
                vm |= ssl::verify_none;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_PEER))
            {
                vm |= ssl::verify_peer;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_FAIL_IF_NO_PEER_CERT))
            {
                vm |= ssl::verify_fail_if_no_peer_cert;
            }
            else if (options->tls_config.get_verify_mode(TLSVerifyMode::VERIFY_CLIENT_ONCE))
            {
                vm |= ssl::verify_client_once;
            }
            secure_socket_->set_verify_mode(vm);
        }
    }
}

void TCPChannelResourceSecure::cancel()
{
    secure_socket_->lowest_layer().cancel();
}

void TCPChannelResourceSecure::close()
{
    secure_socket_->lowest_layer().close();
}

void TCPChannelResourceSecure::shutdown(asio::socket_base::shutdown_type)
{
    secure_socket_->shutdown();
}

} // namespace rtps
} // namespace fastrtps
} // namespace eprosima
