#define main serial_main
#define ioctl mock_ioctl
#define write mock_write
#include "../linux-serial-test.c"
#undef main
#undef ioctl
#undef write
#include <assert.h>
#include <pty.h>
#include <stdarg.h>

static struct serial_rs485 config;
static int set_count, reject_set, wrong_readback, queued;
static ssize_t write_limit = 3;
int mock_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    switch (request) {
    case TIOCGRS485: *(struct serial_rs485 *)arg = config; return 0;
    case TIOCSRS485:
        if (reject_set) { errno = EIO; return -1; }
        config = *(struct serial_rs485 *)arg;
        if (wrong_readback) config.delay_rts_after_send = 999;
        ++set_count; return 0;
    case TIOCOUTQ: *(int *)arg = queued; return 0;
    default: errno = ENOTTY; return -1;
    }
}
ssize_t mock_write(int fd, const void *data, size_t size)
{
    (void)fd; (void)data;
    return size > (size_t)write_limit ? write_limit : (ssize_t)size;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "short-write")) {
        _write_size = 16; _write_data = malloc(16);
        process_write_data();
        assert(_write_count == 3 && _write_count_value == 3);
        process_write_data();
        assert(_write_count == 6 && _write_count_value == 6);
        return 0;
    }
    if (!strcmp(argv[1], "credit")) {
        _write_size = 16; _cl_write_after_read = 1; _cl_initiate_tx = 1;
        assert(write_credit() == 16);
        _write_count = 3; assert(write_credit() == 13);
        _write_count = 16; assert(write_credit() == 0);
        _read_count = 5; assert(write_credit() == 5);
        return 0;
    }
    if (!strcmp(argv[1], "drain")) {
        queued = 16; _cl_drain_timeout = 20;
        long long start = monotonic_ms(); drain_output();
        assert(_failed && monotonic_ms() - start < 500); return 0;
    }
    if (!strcmp(argv[1], "read-error")) {
        _fd = -1; process_read_data(); assert(_failed); return 0;
    }
    int master, slave; char name[128];
    assert(openpty(&master, &slave, name, NULL, NULL) == 0);
    _cl_port = strdup(name);
    config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    config.delay_rts_after_send = 99;
    if (!strcmp(argv[1], "preserve")) {
        setup_serial_port(B9600); assert(set_count == 0); return 0;
    }
    _cl_rs485_after_delay = 2; _cl_rs485_before_delay = 3;
    _cl_rs485_rts_after_send = 1;
    reject_set = !strcmp(argv[1], "reject");
    wrong_readback = !strcmp(argv[1], "readback");
    setup_serial_port(B9600);
    assert(!reject_set && !wrong_readback);
    assert(config.delay_rts_after_send == 2 && config.delay_rts_before_send == 3);
    assert(config.flags & SER_RS485_RTS_AFTER_SEND);
    assert(!(config.flags & (SER_RS485_RTS_ON_SEND | SER_RS485_RX_DURING_TX)));
    exit_handler();
    assert(config.delay_rts_after_send == 99);
    return 0;
}
