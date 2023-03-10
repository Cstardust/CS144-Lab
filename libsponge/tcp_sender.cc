#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>

void Timer::start(uint64_t initial_alarm) {
    _active = true;
    _initial_alarm = initial_alarm;
    _alarm = _initial_alarm;
}

//  tip : Timer eplapse到期之后(return true) user一定要调用reset
bool Timer::elapse(uint64_t elapsed) {
    if (!_active)
        throw "unstart";
    if (_alarm > elapsed) {
        _alarm -= elapsed;
        return false;
    } else {
        _alarm = 0;
        return true;
    }
}

void Timer::reset() {
    _active = false;
    _alarm = 0;
    _initial_alarm = 0;
}

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _send_window{}
    , _receive_window_size(1)  //  >??
    , _initial_retransmission_timeout{retx_timeout}
    , _timer{}
    , _stream(capacity)
    , _consecutive_retransmissions_cnt(0) {}

uint64_t TCPSender::bytes_in_flight() const {
    uint64_t send_window_size = 0;
    for (const auto &seg : _send_window) {
        send_window_size += seg.length_in_sequence_space();
    }
    return send_window_size;
}

size_t TCPSender::send_segment(size_t remaining_recv_window_sz) {
    //  1. build tcpsegment
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    //  syn
    if (state() == State::CLOSED && remaining_recv_window_sz >= 1)
        seg.header().syn = true;

    //  payload
    size_t payload_sz =
        min({TCPConfig::MAX_PAYLOAD_SIZE, remaining_recv_window_sz - seg.header().syn, _stream.buffer_size()});
    seg.payload() =
        _stream.read(payload_sz);  //  bytestream中读取出来的是tcp payload。至于tcp header 是由sender自己填写。

    //  fin
    if (state() == SYN_ACKED_2 && remaining_recv_window_sz > payload_sz + seg.header().syn)
        seg.header().fin = true;

    //  2. send the seg
    if (seg.length_in_sequence_space() != 0) {
        _segments_out.push(seg);
        _send_window.push_back(seg);
    }

    //  3.  return length in seq space
    return seg.length_in_sequence_space();
}

//    (5.1) Every time a packet containing data is sent (including a
//          retransmission), if the timer is not running, start it running
//          so that it will expire after RTO seconds (for the current value
//          of RTO).
void TCPSender::timer_when_filling() {
    if (!_timer.active()) {
        _timer.reset();
        _timer.start(_initial_retransmission_timeout);
    }
}

void TCPSender::update_when_filling(size_t seg_len_in_seq_space, size_t &remaining_recv_window_sz) {
    //  update _next_seq
    _next_seqno += seg_len_in_seq_space;
    //  update receive_window_size. useful for filling loop
    remaining_recv_window_sz -= seg_len_in_seq_space;
}

void TCPSender::fill_window()  //  try to send segment to fill the receive window
{
    size_t remaining_recv_window_sz = _receive_window_size == 0 ? 1 : _receive_window_size;
    if (bytes_in_flight() > remaining_recv_window_sz) {
        // "the recv_window should == bytes_in_flight"
        // "In fact , the receive_window is full now , but because of our sender implementation , we can't acked part of
        // the segment , so we can't bascially remaining_recv_window_sz -= bytes_in_flight() to get 0 . Instead, we
        // should return now"
        return;
    }

    // sender目前已知的 receive_window_size 减去 bytes sen but not acked。
    remaining_recv_window_sz -= bytes_in_flight();

    //  发送结束 : remaining_recv_window_sz == 0 or 没有报文可以发送(payload empty and flag is empty)
    while (remaining_recv_window_sz > 0) {
        //  build and send tcpsegment
        size_t seg_len_in_seq_space = send_segment(remaining_recv_window_sz);

        // nothing to send : no syn or fin flag , no payload
        if (seg_len_in_seq_space == 0)
            break;

        //  start timer if it not start
        timer_when_filling();

        //  update _next_seq ; update receive_window_size ; update _send_window_size
        update_when_filling(seg_len_in_seq_space, remaining_recv_window_sz);
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
//  robust enough to deal with any ackno
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    _receive_window_size = window_size;

    uint64_t abs_ackno = unwrap(ackno, _isn, _next_seqno);

    // should never happened ! the byte after _next_seqno hasn't been sent !
    if (abs_ackno > _next_seqno)  // ack: receiver 期待 sender发送的下一个字节索引 ，_next_seqno :
                                  // sender顺序将要发送的下一个字节索引
        return;

    //  if send_window is empty , then ack is useless. (nothing is unacked in TCPSender)
    if (_send_window.empty())
        return;

    bool seg_acked = false;  //  whether the seg in send_window is acked。(可以顺带排除ack < left edge of the send_window)
    //  remove acked seg from the send_window
    for (deque<TCPSegment>::iterator iter = _send_window.begin(); iter != _send_window.end();) {
        uint64_t abs_idx = unwrap(iter->header().seqno, _isn, _next_seqno);
        uint64_t len = iter->length_in_sequence_space();
        //  _send_window中得seqno一定都是增序排列(由fill_window可知，是按照发送的顺序push到send_window中的)
        if (abs_idx >= abs_ackno)
            break;
        //  remove seg from send_window
        if (abs_idx + len <= abs_ackno)  
        {
            seg_acked = true;
            iter = _send_window.erase(iter);
        } else {
        //  not remove seg from send_window
            ++iter;
        }
    }

    //  send_window中没有字节被ack，故不需要重启 / 关闭定时器 ，也不需要发送数据
    if (!seg_acked) {
        fill_window();
        return;
    }
    
    //  上一个计时重传的分组被移除 故 下一个重新计数
    _consecutive_retransmissions_cnt = 0;

    //    (5.3) When an ACK is received that acknowledges new data, restart the
    //      retransmission timer so that it will expire after RTO seconds
    //      (for the current value of RTO).
    if (!_send_window.empty()) {
        _timer.reset();
        _timer.start(_initial_retransmission_timeout);
    }
    //    (5.2) When all outstanding data has been acknowledged, turn off the retransmission timer. 即 send_window is empty , close the timer
    else {
        _timer.reset();
    }

    //  send new data
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    // timer not active but want to use
    if (!_timer.active())  //  send_extra.cc 最后在alarm不工作的时候又调用了一次tick
        return;

    //    When the retransmission timer expires, do the following: 5.4 , 5.5 , 5.6 , 5.7
    if (_timer.elapse(ms_since_last_tick)) {
        TCPSegment &oldest_seg = _send_window.front();

        uint64_t timeout = _timer.initial_alarm();
        //  (why ?) only when window size > 0 , double alarm and ++ cnt
        if (_receive_window_size > 0) {
            //    (5.5) The host MUST set RTO <- RTO * 2 ("back off the timer").  The
            //          maximum value discussed in (2.5) above may be used to provide
            //          an upper bound to this doubling operation.
            timeout <<= 1;
            ++_consecutive_retransmissions_cnt;  
            //    (5.7) If the timer expires awaiting the ACK of a SYN segment and the
            //          TCP implementation is using an RTO less than 3 seconds, the RTO
            //          MUST be re-initialized to 3 seconds when data transmission
            //          begins (i.e., after the three-way handshake completes).
        }
        //    (5.6) Start the retransmission timer, such that it expires after RTO
        //  seconds (for the value of RTO after the doubling operation
        //  outlined in 5.5).
        _timer.reset();
        _timer.start(timeout);

        //  (5.4) Retransmit the earliest segment that has not been acknowledged by the TCP receiver.
        _segments_out.push(oldest_seg);     //  即 超时重传
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions_cnt; }

void TCPSender::send_empty_segment(bool rst /*= false*/) {
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    if (rst)
        seg.header().rst = true; 
    _next_seqno += 0;
    _segments_out.push(seg);
}
