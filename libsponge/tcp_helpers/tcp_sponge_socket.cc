#include "tcp_sponge_socket.hh"

#include "network_interface.hh"
#include "parser.hh"
#include "tun.hh"
#include "util.hh"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

using namespace std;

static constexpr size_t TCP_TICK_MS = 10;

//  Eventloop while(condition) { poll(); handleEvents(); }
//! \param[in] condition is a function returning true if loop should continue
template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::_tcp_loop(const function<bool()> &condition) {
    auto base_time = timestamp_ms();
    while (condition()) {   //  while (true)
        // poll(); handleEvents();
        auto ret = _eventloop.wait_next_event(TCP_TICK_MS);
        if (ret == EventLoop::Result::Exit or _abort) {
            break;
        }
        //  passes time since last handling segs;  
            //  tcpconnection 和 network interface 距离上次tick过去的时间 ; 告知他们. 做出相应变化。
            //  tcp connection tick
            //  network interface tick 
        if (_tcp.value().active()) {
            const auto next_time = timestamp_ms();
            _tcp.value().tick(next_time - base_time);
            _datagram_adapter.tick(next_time - base_time);
            base_time = next_time;
        }
    }
}

//! \param[in] data_socket_pair is a pair of connected AF_UNIX SOCK_STREAM sockets
//! \param[in] datagram_interface is the interface for reading and writing datagrams
template <typename AdaptT>
TCPSpongeSocket<AdaptT>::TCPSpongeSocket(pair<FileDescriptor, FileDescriptor> data_socket_pair,
                                         AdaptT &&datagram_interface)
    : LocalStreamSocket(move(data_socket_pair.first))
    , _thread_data(move(data_socket_pair.second))
    , _datagram_adapter(move(datagram_interface)) {
    _thread_data.set_blocking(false);
}

//  construct 本端 TCPConnection
//  设置eventloop应该监听并如何处理的事情  ; set up what should the eventloop to poll and handle. 
template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::_initialize_TCP(const TCPConfig &config) {
    _tcp.emplace(config);

    // Set up the event loop

    // There are four possible events to handle:
    //
    // 1) Incoming datagram received (needs to be given to
    //    TCPConnection::segment_received method)
    //
    // 2) Outbound bytes received from local application via a write()
    //    call (needs to be read from the local stream socket and
    //    given to TCPConnection::data_written method)
    //
    // 3) Incoming bytes reassembled by the TCPConnection
    //    (needs to be read from the inbound_stream and written
    //    to the local stream socket back to the application)
    //
    // 4) Outbound segment generated by TCP (needs to be
    //    given to underlying datagram socket)

    // rule 1: read from filtered packet stream and dump into TCPConnection
    //  event : adapter(网卡)的读事件(网卡有数据可读)
    _eventloop.add_rule(_datagram_adapter,      //  实际上注册的是底层Tapfd
                        Direction::In,
                        //  handler : 读出adaper的数据 向上交付给local tcp
                        [&] {
                            auto seg = _datagram_adapter.read();        //  read and unwrap from ip to tcp  TCPOverIPv4OverEthernetAdapter::read()
                            // cerr<<"read from filtered packet stream and dump into TCPConnection "<<endl;
                            //  tcp接收来自adapter的数据. 进行运输层的处理.
                            if (seg) {
                                _tcp->segment_received(move(seg.value()));
                            }

                            // debugging output:
                            if (_thread_data.eof() and _tcp.value().bytes_in_flight() == 0 and not _fully_acked) {
                                cerr << "DEBUG: Outbound stream to "
                                     << _datagram_adapter.config().destination.to_string()
                                     << " has been fully acknowledged.\n";
                                _fully_acked = true;
                            }
                        },
                        //  只有local tcp存活时 才监听并处理该事件
                        [&] { return _tcp->active(); });

    //  user 写 _socket. 由于socket_pair,数据传送给 socket_thread_data ; tcp_thread 负责读出 _thread_data的数据 ，然受写入tcp 送入协议栈处理并从adapter发送出去
    //  rule 2: read from pipe into outbound buffer
    //  event : _thread_data的读事件. 即_thread_data接收到了_socket写入pipe的数据
    _eventloop.add_rule(
        _thread_data,           //  sockfd
        Direction::In,
        //  handler : tcp_thread 负责读出 _thread_data接收到的数据 ，然受写入tcp 送入协议栈处理并从adapter发送出去
        [&] {
            // cerr<<"read from pipe into outbound buffer"<<endl;
            const auto data = _thread_data.read(_tcp->remaining_outbound_capacity());
            const auto len = data.size();
            const auto amount_written = _tcp->write(move(data));
            if (amount_written != len) {
                throw runtime_error("TCPConnection::write() accepted less than advertised length");
            }
            //  管道已空 且 _socket没有数据可写入_thread+data了.
            //  _thread_data没有数据可送入协议栈
            if (_thread_data.eof()) {
                _tcp->end_input_stream();
                _outbound_shutdown = true;

                // debugging output:
                cerr << "DEBUG: Outbound stream to " << _datagram_adapter.config().destination.to_string()
                     << " finished (" << _tcp.value().bytes_in_flight() << " byte"
                     << (_tcp.value().bytes_in_flight() == 1 ? "" : "s") << " still in flight).\n";
            }
        },
        //  在local tcp存活 && 写不关闭 && local tcp outbound_buffer仍有空闲空间时 可监听并处理此事件
        [&] { return (_tcp->active()) and (not _outbound_shutdown) and (_tcp->remaining_outbound_capacity() > 0); },
        [&] {
        //  移除事件时 关闭app写入tcp的bytestream ; shutdown = true
            _tcp->end_input_stream();
            _outbound_shutdown = true;
        });

    // rule 3: read from inbound buffer into pipe
    //  event : _thread_data和_socket的管道不满即可触发
    _eventloop.add_rule(
        _thread_data,           //  sockfd
        Direction::Out,
        //  handler : 从tcp inbound buffer中弹出数据 并交由_thread_data写给_socket. 适时(inbound buffer读到eof）关闭_thread_data
        [&] {
            // cerr<<"read from inbound buffer into pipe"<<endl;
            ByteStream &inbound = _tcp->inbound_stream();
            // Write from the inbound_stream into
            // the pipe, handling the possibility of a partial
            // write (i.e., only pop what was actually written).
            const size_t amount_to_write = min(size_t(65536), inbound.buffer_size());
            const std::string buffer = inbound.peek_output(amount_to_write);
            const auto bytes_written = _thread_data.write(move(buffer), false);
            inbound.pop_output(bytes_written);

            if (inbound.eof() or inbound.error()) {
                _thread_data.shutdown(SHUT_WR);
                _inbound_shutdown = true;

                // debugging output:
                cerr << "DEBUG: Inbound stream from " << _datagram_adapter.config().destination.to_string()
                     << " finished " << (inbound.error() ? "with an error/reset.\n" : "cleanly.\n");
                if (_tcp.value().state() == TCPState::State::TIME_WAIT) {
                    cerr << "DEBUG: Waiting for lingering segments (e.g. retransmissions of FIN) from peer...\n";
                }
            }
        },
        //  interest : 如果注册了该_data_socket的写事件到epoll上，那么只要pipe不满就会触发，则会陷入死循环。那么该如何正确的在poll上注册写事件 使得写事件可以在有数据写的时候发生，没有的时候就不发生？
        //  解决方案如下：设置好注册写事件的先决条件. 也即 只有在有数据的时候才写(有数据时才把该写事件注册在poll上). 
        //  并且 在不满足该条件时，就立刻将该写事件从poll上拿下来.
        [&] {
            //  tcp的inbound buffer不空(有数据可交付给上层app) || tcp的inbound buffer读到eof亦或者出错error (那就可以返回给上层eof或是error) 
            return (not _tcp->inbound_stream().buffer_empty()) or
                   ((_tcp->inbound_stream().eof() or _tcp->inbound_stream().error()) and not _inbound_shutdown);
        });

    // rule 4: read outbound segments from TCPConnection and send as datagrams
    //  event : adpater可写. 同rule3 注册不及时移除会死循环.
    _eventloop.add_rule(_datagram_adapter,      //  TapFd
                        Direction::Out,
                        [&] {
                            // cerr<<"read outbound segments from TCPConnection and send as datagrams"<<endl;
                            while (not _tcp->segments_out().empty()) {
                                _datagram_adapter.write(_tcp->segments_out().front());
                                _tcp->segments_out().pop();
                            }
                        },
                        //  interest : 如果tcp的outbound buffer有数据要可发 才注册 ; 不符合该条件时立刻移除
                        [&] { return not _tcp->segments_out().empty(); });
}

//! \brief Call [socketpair](\ref man2::socketpair) and return connected Unix-domain sockets of specified type
//! \param[in] type is the type of AF_UNIX sockets to create (e.g., SOCK_SEQPACKET)
//! \returns a std::pair of connected sockets
//  创建一对socketpair
static inline pair<FileDescriptor, FileDescriptor> socket_pair_helper(const int type) {
    int fds[2];
    SystemCall("socketpair", ::socketpair(AF_UNIX, type, 0, static_cast<int *>(fds)));
    return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

//! \param[in] datagram_interface is the underlying interface (e.g. to UDP, IP, or Ethernet)
template <typename AdaptT>
TCPSpongeSocket<AdaptT>::TCPSpongeSocket(AdaptT &&datagram_interface)
    : TCPSpongeSocket(socket_pair_helper(SOCK_STREAM), move(datagram_interface)) {}


//  main thread join tcp thread
template <typename AdaptT>
TCPSpongeSocket<AdaptT>::~TCPSpongeSocket() {
    try {
        if (_tcp_thread.joinable()) {
            cerr << "Warning: unclean shutdown of TCPSpongeSocket\n";
            // force the other side to exit
            _abort.store(true);
            _tcp_thread.join();     //  main thread 等待 eventloop thread结束
        }
    } catch (const exception &e) {
        cerr << "Exception destructing TCPSpongeSocket: " << e.what() << endl;
    }
}

template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::wait_until_closed() {
    //  关闭user看到的_socketfd的读写   (对tcp的影响感觉是 _socket 关闭读写 -> _thread_data关闭读写 -> tcp关闭读写 发送fin? 日后再说 该睡觉了)
    shutdown(SHUT_RDWR);
    if (_tcp_thread.joinable()) {
        cerr << "DEBUG: Waiting for clean shutdown... ";    //  ? 这还clean ? 
        _tcp_thread.join();     //  等待tcp thread结束
        cerr << "done.\n";
    }
}

//! \param[in] c_tcp is the TCPConfig for the TCPConnection
//! \param[in] c_ad is the FdAdapterConfig for the FdAdapter
template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::connect(const TCPConfig &c_tcp, const FdAdapterConfig &c_ad) {
    if (_tcp) {
        throw runtime_error("connect() with TCPConnection already initialized");
    }

    _initialize_TCP(c_tcp);

    //  将local socket的{ip,port}告知_datagram_adapter
    _datagram_adapter.config_mut() = c_ad;
    //  local tcpcnnection 主动发送 tcp层的syn
    cerr << "DEBUG: Connecting to " << c_ad.destination.to_string() << "...\n";
    _tcp->connect();

    const TCPState expected_state = TCPState::State::SYN_SENT;

    if (_tcp->state() != expected_state) {
        throw runtime_error("After TCPConnection::connect(), state was " + _tcp->state().name() + " but expected " +
                            expected_state.name());
    }
    //  main thread等待建立连接
    _tcp_loop([&] { return _tcp->state() == TCPState::State::SYN_SENT; });
    cerr << "Successfully connected to " << c_ad.destination.to_string() << ".\n";
    //  连接建立
    //  tcp_thread负责处理 _thread_data以及tcp协议栈以及tapFd 的数据处理 并传送给_socket
    _tcp_thread = thread(&TCPSpongeSocket::_tcp_main, this);
}

//! \param[in] c_tcp is the TCPConfig for the TCPConnection
//! \param[in] c_ad is the FdAdapterConfig for the FdAdapter
template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::listen_and_accept(const TCPConfig &c_tcp, const FdAdapterConfig &c_ad) {
    if (_tcp) {
        throw runtime_error("listen_and_accept() with TCPConnection already initialized");
    }

    _initialize_TCP(c_tcp);
    //  将local socket的{ip,port}告知_datagram_adapter
    _datagram_adapter.config_mut() = c_ad;
    _datagram_adapter.set_listening(true);

    //  main thread 监听等待peer连接local socket
    cerr << "DEBUG: Listening for incoming connection...\n";
    _tcp_loop([&] {
        //  当local TCPConnection处于ESTABLISHED之前的状态 : LISTEN , SYN_RCVD or SYN_SENT(应该可以去掉) 
        const auto s = _tcp->state();
        return (s == TCPState::State::LISTEN or s == TCPState::State::SYN_RCVD or s == TCPState::State::SYN_SENT);
    });
    //  local tcp socket 和 peer tcp socket 成功建立连接
    cerr << "New connection from " << _datagram_adapter.config().destination.to_string() << ".\n";

    //  开启tcp_thread , 用于处理_thread_data以及tcp协议栈以及tapFd
    _tcp_thread = thread(&TCPSpongeSocket::_tcp_main, this);

    //  return to the main thread
}


//  根本就没有所谓的TCPSocket
//  只有Socket 底层是 TCP
//  thread_data 是给 tcp_thread操作的。巧妙地将tcp thread 和 main thread分离开 不必操作同一socket 而是通过一个管道传输数据
//  这是否也符合所谓的 通过传递信息来共享内存 而不是通过共享内存来传递信息
//  非要说的话 继承自LocalStreamSocket的fd是用户看到的TCPSocket . 这个fd并不是从网卡读数据。从网卡读数据的fd在最底层 是TAPfd
//  socket是站在应用层和传输层之间的！代码就能体现出来！
//  然后_thread_data 这个socketfd和_socket是一对 通过管道读写互相传递数据. (大概来讲就是tcp_thread操作_thraed_dtaa main_thread操作_socket)
//  所以总共有3个fd

template <typename AdaptT>
void TCPSpongeSocket<AdaptT>::_tcp_main() {
    try {
        if (not _tcp.has_value()) {
            throw runtime_error("no TCP");
        }
        //  while(true) {poll();(TCPSocket相关事件) ; handleEvents();}
        _tcp_loop([] { return true; });
        //  关闭TCPSocket对应的fd
        shutdown(SHUT_RDWR);
        if (not _tcp.value().active()) {
            cerr << "DEBUG: TCP connection finished "
                 << (_tcp.value().state() == TCPState::State::RESET ? "uncleanly" : "cleanly.\n");
        }
        //  清空tcpconnection
        _tcp.reset();
    } catch (const exception &e) {
        cerr << "Exception in TCPConnection runner thread: " << e.what() << "\n";
        throw e;
    }
}

//! Specialization of TCPSpongeSocket for TCPOverUDPSocketAdapter
template class TCPSpongeSocket<TCPOverUDPSocketAdapter>;

//! Specialization of TCPSpongeSocket for TCPOverIPv4OverTunFdAdapter
template class TCPSpongeSocket<TCPOverIPv4OverTunFdAdapter>;

//! Specialization of TCPSpongeSocket for TCPOverIPv4OverEthernetAdapter
template class TCPSpongeSocket<TCPOverIPv4OverEthernetAdapter>;

//! Specialization of TCPSpongeSocket for LossyTCPOverUDPSocketAdapter
template class TCPSpongeSocket<LossyTCPOverUDPSocketAdapter>;

//! Specialization of TCPSpongeSocket for LossyTCPOverIPv4OverTunFdAdapter
template class TCPSpongeSocket<LossyTCPOverIPv4OverTunFdAdapter>;

CS144TCPSocket::CS144TCPSocket() : TCPOverIPv4SpongeSocket(TCPOverIPv4OverTunFdAdapter(TunFD("tun144"))) {}

void CS144TCPSocket::connect(const Address &address) {
    TCPConfig tcp_config;
    tcp_config.rt_timeout = 100;

    FdAdapterConfig multiplexer_config;
    multiplexer_config.source = {"169.254.144.9", to_string(uint16_t(random_device()()))};
    multiplexer_config.destination = address;

    TCPOverIPv4SpongeSocket::connect(tcp_config, multiplexer_config);
}

static const string LOCAL_TAP_IP_ADDRESS = "169.254.10.9";
static const string LOCAL_TAP_NEXT_HOP_ADDRESS = "169.254.10.1";

EthernetAddress random_private_ethernet_address() {
    EthernetAddress addr;
    for (auto &byte : addr) {
        byte = random_device()();  // use a random local Ethernet address
    }
    addr.at(0) |= 0x02;  // "10" in last two binary digits marks a private Ethernet address
    addr.at(0) &= 0xfe;

    return addr;
}

FullStackSocket::FullStackSocket()
    : TCPOverIPv4OverEthernetSpongeSocket(TCPOverIPv4OverEthernetAdapter(TapFD("tap10"),
                                                                         random_private_ethernet_address(),
                                                                         Address(LOCAL_TAP_IP_ADDRESS, "0"),
                                                                         Address(LOCAL_TAP_NEXT_HOP_ADDRESS, "0"))) {}

void FullStackSocket::connect(const Address &address) {
    TCPConfig tcp_config;
    tcp_config.rt_timeout = 100;

    //  socket {ip , port} : 自动配置好ip 并 随机分配好本socket的port.
    //  peer {ip , port} : 要连接的ip和port是user指定的
        //  multiplexer_config
        //  我们的实现中最终会传给本socket的TCPOverIPv4Adapter的_datagram_adapter，
        //  他会在unwrap_tcp_in_ip时帮我们检验接收到的segment是否是给本socket的。不是则直接丢掉。是则提交给tcp层
    FdAdapterConfig multiplexer_config;
    multiplexer_config.source = {LOCAL_TAP_IP_ADDRESS, to_string(uint16_t(random_device()()))};
    multiplexer_config.destination = address;

    TCPOverIPv4OverEthernetSpongeSocket::connect(tcp_config, multiplexer_config);
}
