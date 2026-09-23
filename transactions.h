/* SPDX-License-Identifier: MIT
 * Opt-in framed half-duplex transactions. No retries conceal a failed exchange.
 * Network byte order: LST1/type/role/length/run-id/sequence/payload/CRC32.
 */
static const char *transaction_error = "none";
static unsigned completed, payload_rx, payload_tx;
static long long latency_total, latency_max;
static int transaction_fail(const char *reason)
{
    if (!strcmp(transaction_error, "none")) transaction_error = reason;
    _failed = 1;
    return -1;
}
static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0]<<24 | (unsigned)p[1]<<16 | (unsigned)p[2]<<8 | p[3];
}
static void put32(unsigned char *p, unsigned value)
{
    p[0]=value>>24; p[1]=value>>16; p[2]=value>>8; p[3]=value;
}
static unsigned frame_crc(const unsigned char *p, unsigned size)
{
    unsigned crc = ~0u;
    for (unsigned i=0;i<size;++i) {
        crc ^= p[i];
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
static int transaction_io(unsigned char *buffer, unsigned size, int sending, long long deadline, unsigned prior)
{
    unsigned offset=0;
    while (offset<size) {
        if (sigint_received) return transaction_fail("interrupted");
        long long left=deadline-monotonic_ms();
        if (left<=0) return transaction_fail(sending ? "tx_timeout" : (offset || prior ? "truncated_frame" : "timeout"));
        struct pollfd port={.fd=_fd,.events=sending?POLLOUT:POLLIN,.revents=0};
        int ret=poll(&port,1,left>20?20:(int)left);
        if (ret<0) { if(errno==EINTR) continue; return transaction_fail("io"); }
        if(port.revents&(POLLERR|POLLHUP|POLLNVAL)) return transaction_fail("disconnected");
        if(!(port.revents&port.events)) continue;
        ssize_t count=sending?write(_fd,buffer+offset,size-offset):read(_fd,buffer+offset,size-offset);
        if(count<0) {
            if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK) continue;
            return transaction_fail("io");
        }
        if(!count) continue;
        offset+=count;
        if(sending) _write_count+=count; else _read_count+=count;
    }
    return 0;
}
static int transaction_send(unsigned type, unsigned seq, const unsigned char *payload, unsigned length, long long deadline)
{
    unsigned char data[4116];
    memcpy(data,"LST1",4); data[4]=type; data[5]=_transaction_role==2;
    data[6]=length>>8; data[7]=length; put32(data+8,_run_id); put32(data+12,seq);
    if(length) memcpy(data+16,payload,length);
    put32(data+16+length,frame_crc(data,16+length));
    if(transaction_io(data,20+length,1,deadline,0)) return -1;
    long long remaining=deadline-monotonic_ms();
    if(remaining<=0) return transaction_fail("tx_timeout");
    int saved=_cl_drain_timeout;
    if(remaining<_cl_drain_timeout) _cl_drain_timeout=remaining;
    drain_output(); _cl_drain_timeout=saved;
    if(_failed) return transaction_fail("drain");
    if(sigint_received) return transaction_fail("interrupted");
    return 0;
}
static int transaction_receive(unsigned type, unsigned seq, unsigned char *payload, unsigned length, long long deadline)
{
    unsigned char data[4116];
    if(transaction_io(data,16,0,deadline,0)) return -1;
    if(memcmp(data,"LST1",4)) return transaction_fail("framing");
    unsigned size=((unsigned)data[6]<<8)|data[7];
    if(size>4096) return transaction_fail("length");
    if(transaction_io(data+16,size+4,0,deadline,16)) return -1;
    if(get32(data+16+size)!=frame_crc(data,16+size)) return transaction_fail("crc");
    if(get32(data+8)!=_run_id) return transaction_fail("run_id");
    if(data[5]==(_transaction_role==2)) return transaction_fail("local_echo");
    if(data[5]>1) return transaction_fail("role");
    if(get32(data+12)!=seq) return transaction_fail("sequence");
    if(data[4]!=type) return transaction_fail("frame_type");
    if(size!=length) return transaction_fail("length");
    if(size) memcpy(payload,data+16,size);
    /* The guard applies only after a complete, validated peer frame. */
    long long guard_end=monotonic_ms()+_cl_turnaround_delay;
    while(monotonic_ms()<guard_end) {
        if(sigint_received) return transaction_fail("interrupted");
        if(monotonic_ms()>=deadline) return transaction_fail("timeout");
        poll(NULL,0,1);
    }
    return 0;
}
static void transaction_payload(unsigned char *data, unsigned seq, int response)
{
    unsigned state=_run_id^seq;
    for(unsigned i=0;i<_payload_bytes;++i) {
        state=state*1664525u+1013904223u;
        data[i]=(state>>24)^(response?0xa5:0);
    }
}
static void run_transactions(void)
{
    unsigned char config[8], received[4096], expected[4096];
    put32(config,_transactions); put32(config+4,_payload_bytes);
    long long deadline=monotonic_ms()+(_startup_timeout?_startup_timeout:_transaction_timeout);
    int initiator=_transaction_role==1;
    if(initiator) {
        if(transaction_send(1,0,config,8,deadline)||transaction_receive(2,0,received,8,deadline)) return;
    } else {
        if(transaction_receive(1,0,received,8,deadline)) return;
    }
    if(memcmp(config,received,8)) { transaction_fail("configuration"); return; }
    if(!initiator && transaction_send(2,0,config,8,deadline)) return;
    for(unsigned seq=1;seq<=_transactions;++seq) {
        long long start=monotonic_ms(); deadline=start+_transaction_timeout;
        if(initiator) {
            transaction_payload(expected,seq,0);
            if(transaction_send(3,seq,expected,_payload_bytes,deadline)) return;
            payload_tx+=_payload_bytes;
            if(transaction_receive(4,seq,received,_payload_bytes,deadline)) return;
            transaction_payload(expected,seq,1);
            if(memcmp(expected,received,_payload_bytes)) { transaction_fail("payload"); return; }
            payload_rx+=_payload_bytes;
        } else {
            if(transaction_receive(3,seq,received,_payload_bytes,deadline)) return;
            transaction_payload(expected,seq,0);
            if(memcmp(expected,received,_payload_bytes)) { transaction_fail("payload"); return; }
            payload_rx+=_payload_bytes;
            transaction_payload(expected,seq,1);
            if(transaction_send(4,seq,expected,_payload_bytes,deadline)) return;
            payload_tx+=_payload_bytes;
        }
        ++completed;
        long long duration=monotonic_ms()-start;
        latency_total+=duration;
        if(duration>latency_max) latency_max=duration;
    }
    deadline=monotonic_ms()+_transaction_timeout;
    if(initiator) {
        if(transaction_send(5,_transactions+1,config,8,deadline)||transaction_receive(6,_transactions+1,received,8,deadline)) return;
    } else {
        if(transaction_receive(5,_transactions+1,received,8,deadline)) return;
    }
    if(memcmp(config,received,8)) { transaction_fail("accounting"); return; }
    if(!initiator) transaction_send(6,_transactions+1,config,8,deadline);
}
static void transaction_report(void)
{
    printf("Transaction result: {\"version\":1,\"requested_baud\":%d,\"role\":\"%s\",\"run_id\":%u,\"completed\":%u,\"payload_rx\":%u,\"payload_tx\":%u,\"error\":\"%s\",\"drain\":\"%s\",\"latency_mean_ms\":%lld,\"latency_max_ms\":%lld}\n",
           _cl_baud?_cl_baud:115200,_transaction_role==1?"initiator":"responder",_run_id,completed,payload_rx,payload_tx,transaction_error,
           _physical_empty?"physical_empty":"driver_queue_only",completed?latency_total/completed:0,latency_max);
}
