// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <linux/serial.h>
#include <errno.h>
#include <sys/file.h>
#include <signal.h>
#include <limits.h>
#ifndef SERIAL_TEST_VERSION
#define SERIAL_TEST_VERSION "development"
#endif

/*
 * glibc for MIPS has its own bits/termios.h which does not define
 * CMSPAR, so we vampirise the value from the generic bits/termios.h
 */
#ifndef CMSPAR
#define CMSPAR 010000000000
#endif

/*
 * Define modem line bits
 */
#ifndef TIOCM_LOOP
#define TIOCM_LOOP	0x8000
#endif

// command line args
int _cl_baud = 0;
char *_cl_port = NULL;
int _cl_divisor = 0;
int _cl_rx_dump = 0;
int _cl_rx_dump_ascii = 0;
int _cl_tx_detailed = 0;
int _cl_rx_detailed = 0;
int _cl_stats = 0;
int _cl_stop_on_error = 0;
int _cl_single_byte = -1;
int _cl_another_byte = -1;
int _cl_rts_cts = 0;
int _cl_2_stop_bit = 0;
int _cl_parity = 0;
int _cl_odd_parity = 0;
int _cl_stick_parity = 0;
int _cl_loopback = 0;
int _cl_dump_err = 0;
int _cl_no_rx = 0;
int _cl_no_tx = 0;
int _cl_rx_delay = 0;
int _cl_tx_delay = 0;
int _cl_tx_bytes = 0;
int _cl_rx_bytes_threash = 0;
int _cl_rs485_after_delay = -1;
int _cl_rs485_before_delay = 0;
int _cl_rs485_rts_after_send = 0;
int _cl_do_not_touch_modem_lines = 0;
int _cl_tx_time = 0;
int _cl_rx_time = 0;
int _cl_tx_wait = 0;
int _cl_ascii_range = 0;
int _cl_write_after_read = 0;
int _cl_rx_timeout_ms = 2000;
int _cl_tx_timeout_ms = 2000;
int _cl_error_on_timeout = 0;
int _cl_no_icount = 0;
int _cl_flush_buffers = 0;
int _cl_initiate_tx = 0;
long long _cl_expected_rx = -1;
int _cl_drain_timeout = 10000;
int _failed = 0;
int _locked = 0;
int _saved_termios_valid = 0, _saved_rs485_valid = 0;
struct termios _saved_termios;
struct serial_rs485 _saved_rs485;
struct serial_icounter_struct _initial_icount;
int _initial_icount_valid = 0;
int _rx_requested = 0;
int _cl_turnaround_delay = 0;
int _saved_serial_valid = 0, _serial_changed = 0;
struct serial_struct _saved_serial;
int _saved_modem_valid = 0, _saved_modem_status = 0;



// Module variables
unsigned char _write_count_value = 0;
unsigned char _read_count_value = 0;
int _fd = -1;
unsigned char * _write_data;
ssize_t _write_size;

// keep our own counts for cases where the driver stats don't work
long long int _write_count = 0;
long long int _read_count = 0;
long long int _error_count = 0;

volatile sig_atomic_t sigint_received = 0;
void sigint_handler(int s)
{
    sigint_received = s;
}

static void exit_handler(void)
{
    /* A failed lock acquisition must never modify another owner's device. */
    if (_fd >= 0) {
        if (_locked) {
            if (_saved_modem_valid) {
                int status;
                if (ioctl(_fd, TIOCMGET, &status) == 0) {
                    status = (status & ~TIOCM_LOOP) | (_saved_modem_status & TIOCM_LOOP);
                    if (ioctl(_fd, TIOCMSET, &status) < 0) perror("restoring modem lines");
                }
            }
            if (_saved_serial_valid && _serial_changed && ioctl(_fd, TIOCSSERIAL, &_saved_serial) < 0)
                perror("restoring serial configuration");
            if (_saved_rs485_valid && ioctl(_fd, TIOCSRS485, &_saved_rs485) < 0)
                perror("restoring RS485 configuration");
            if (_saved_termios_valid && tcsetattr(_fd, TCSANOW, &_saved_termios) < 0)
                perror("restoring terminal configuration");
            flock(_fd, LOCK_UN);
        }
        close(_fd);
    }
    free(_cl_port);
    free(_write_data);
}

static long long number(const char *text, long long min, long long max)
{
    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 0);
    if (errno || end == text || *end || value < min || value > max) {
        fprintf(stderr, "Invalid numeric argument: %s\n", text);
        exit(2);
    }
    return value;
}

static void dump_data(unsigned char * b, int count)
{
	printf("%i bytes: ", count);
	int i;
	for (i=0; i < count; i++) {
		printf("%02x ", b[i]);
	}

	printf("\n");
}

static void dump_data_ascii(unsigned char * b, int count)
{
	int i;
	for (i=0; i < count; i++) {
		printf("%c", b[i]);
	}
}

static void set_baud_divisor(int speed, int custom_divisor)
{
	// default baud was not found, so try to set a custom divisor
	struct serial_struct ss;
	int ret;

	if (ioctl(_fd, TIOCGSERIAL, &ss) < 0) {
		ret = -errno;
		perror("TIOCGSERIAL failed");
		exit(ret);
	}

	_serial_changed = 1;
	ss.flags = (ss.flags & ~ASYNC_SPD_MASK) | ASYNC_SPD_CUST;
	if (custom_divisor) {
		ss.custom_divisor = custom_divisor;
	} else {
		if (speed <= 0 || ss.baud_base <= 0) exit(2);
        ss.custom_divisor = ((long long)ss.baud_base + speed / 2) / speed;
        if (ss.custom_divisor <= 0) {
            fprintf(stderr, "Baud exceeds supported divisor range\n");
            exit(2);
        }
		int closest_speed = ss.baud_base / ss.custom_divisor;

		if (closest_speed < (long long)speed * 98 / 100 || closest_speed > (long long)speed * 102 / 100) {
			fprintf(stderr, "Cannot set speed to %d, closest is %d\n", speed, closest_speed);
			exit(-EINVAL);
		}

		printf("closest baud = %i, base = %i, divisor = %i\n", closest_speed, ss.baud_base,
				ss.custom_divisor);
	}

	if (ioctl(_fd, TIOCSSERIAL, &ss) < 0) {
		ret = -errno;
		perror("TIOCSSERIAL failed");
		exit(ret);
	}
}

static void clear_custom_speed_flag()
{
	struct serial_struct ss;
	int ret;

	if (ioctl(_fd, TIOCGSERIAL, &ss) < 0) {
		// return silently as some devices do not support TIOCGSERIAL
		return;
	}

	if ((ss.flags & ASYNC_SPD_MASK) != ASYNC_SPD_CUST)
		return;

	_serial_changed = 1;
	ss.flags &= ~ASYNC_SPD_MASK;

	if (ioctl(_fd, TIOCSSERIAL, &ss) < 0) {
		ret = -errno;
		perror("TIOCSSERIAL failed");
		exit(ret);
	}
}

// converts integer baud to Linux define
static int get_baud(int baud)
{
	switch (baud) {
	case 1200:
		return B1200;
	case 2400:
		return B2400;
	case 4800:
		return B4800;
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 500000:
		return B500000;
	case 576000:
		return B576000;
	case 921600:
		return B921600;
#ifdef B1000000
	case 1000000:
		return B1000000;
#endif
#ifdef B1152000
	case 1152000:
		return B1152000;
#endif
#ifdef B1500000
	case 1500000:
		return B1500000;
#endif
#ifdef B2000000
	case 2000000:
		return B2000000;
#endif
#ifdef B2500000
	case 2500000:
		return B2500000;
#endif
#ifdef B3000000
	case 3000000:
		return B3000000;
#endif
#ifdef B3500000
	case 3500000:
		return B3500000;
#endif
#ifdef B4000000
	case 4000000:
		return B4000000;
#endif
	default:
		return -1;
	}
}

void set_modem_lines(int fd, int bits, int mask)
{
	if (_cl_do_not_touch_modem_lines)
		return;

	int status, ret;

	if (ioctl(fd, TIOCMGET, &status) < 0) {
		ret = -errno;
		perror("TIOCMGET failed");
		exit(ret);
	}

	if (!_saved_modem_valid) { _saved_modem_status = status; _saved_modem_valid = 1; }
	status = (status & ~mask) | (bits & mask);

	if (ioctl(fd, TIOCMSET, &status) < 0) {
		ret = -errno;
		perror("TIOCMSET failed");
		exit(ret);
	}
}

static void display_help(void)
{
	printf("Usage: linux-serial-test [OPTION]\n"
			"\n"
			"  -h, --help\n"
			"  -b, --baud               Baud rate, 115200, etc (115200 is default)\n"
			"  -p, --port               Port (/dev/ttyS0, etc) (must be specified)\n"
			"  -d, --divisor            UART Baud rate divisor (can be used to set custom baud rates)\n"
			"  -D, --rx_dump            Dump Rx data (ascii, raw)\n"
			"  -T, --detailed_tx        Detailed Tx data\n"
			"  -R, --detailed_rx        Detailed Rx data\n"
			"  -s, --stats              Dump serial port stats every 5s\n"
			"  -S, --stop-on-err        Stop program if we encounter an error\n"
			"  -y, --single-byte        Send specified byte to the serial port\n"
			"  -z, --second-byte        Send another specified byte to the serial port\n"
			"  -c, --rts-cts            Enable RTS/CTS flow control\n"
			"  -B, --2-stop-bit         Use two stop bits per character\n"
			"  -P, --parity             Use parity bit (odd, even, mark, space)\n"
			"  -k, --loopback           Use internal hardware loop back\n"
			"  -K, --write-follow       Write follows the read count (can be used for multi-serial loopback)\n"
			"  -F, --initiate-tx	    Initiate the first transmission when write-follow is used. Required if write-follow\n"
			"			    is used on both devices in a point-to-point exchange\n"
			"  -e, --dump-err           Display errors\n"
			"  -r, --no-rx              Don't receive data (can be used to test flow control)\n"
			"                           when serial driver buffer is full\n"
			"  -t, --no-tx              Don't transmit data\n"
			"  -l, --rx-delay           Delay between reading data (ms) (can be used to test flow control)\n"
			"  -a, --tx-delay           Delay between writing data (ms)\n"
			"  -w, --tx-bytes           Number of bytes for each write (default is to repeatedly write 1024 bytes\n"
			"                           until no more are accepted)\n"
			"  -M, --rx-bytes-threash   Legacy read threshold hint; partial reads never block progress\n"
			"  -q, --rs485              Enable RS485 direction control on port, and set delay from when TX is\n"
			"                           finished and RS485 driver enable is de-asserted. Delay is specified in\n"
			"                           milliseconds. To optionally specify a delay from when the driver is enabled\n"
			"                           to start of TX use 'after_delay.before_delay' (-q 1.1)\n"
			"  -Q, --rs485_rts          Deassert RTS on send, assert after send. Omitting -Q inverts this logic.\n"
			"  -m, --no-modem           Do not clobber against any modem lines.\n"
			"  -o, --tx-time            Number of seconds to transmit for (defaults to 0, meaning no limit)\n"
			"  -i, --rx-time            Number of seconds to receive for (defaults to 0, meaning no limit)\n"
			"  -A, --ascii              Output bytes range from 32 to 126 (default is 0 to 255)\n"
			"  -I, --rx-timeout         Receive timeout\n"
			"  -O, --tx-timeout         Transmission timeout\n"
			"  -W, --tx-wait            Number of seconds to wait before to transmit (defaults to 0, meaning no wait)\n"
			"  -Z, --error-on-timeout   Treat timeouts as errors\n"
			"  -n, --no-icount          Do not request driver for counts of input serial line interrupts (TIOCGICOUNT)\n"
			"      --version            Show source revision\n"
            "      --expected-rx N      Require exactly N received bytes (detects whole-pattern loss)\n"
            "      --turnaround-delay MS  Guard interval after receiving before replying\n"
            "      --drain-timeout MS   Bound output drain (default 10000 milliseconds)\n"
            "  -f, --flush-buffers      Flush RX and TX buffers before starting\n"
			"\n"
		);
}

static void process_options(int argc, char * argv[])
{
	for (;;) {
		int option_index = 0;
		static const char *short_options = "hb:p:d:D:TRsSy:z:cBertq:Qml:a:w:o:i:P:kKFM:AI:O:W:Znf";
		static const struct option long_options[] = {
			{"help", no_argument, 0, 0},
			{"baud", required_argument, 0, 'b'},
			{"port", required_argument, 0, 'p'},
			{"divisor", required_argument, 0, 'd'},
			{"rx_dump", required_argument, 0, 'D'},
			{"detailed_tx", no_argument, 0, 'T'},
			{"detailed_rx", no_argument, 0, 'R'},
			{"stats", no_argument, 0, 's'},
			{"stop-on-err", no_argument, 0, 'S'},
			{"single-byte", required_argument, 0, 'y'},
			{"second-byte", required_argument, 0, 'z'},
			{"rts-cts", no_argument, 0, 'c'},
			{"2-stop-bit", no_argument, 0, 'B'},
			{"parity", required_argument, 0, 'P'},
			{"loopback", no_argument, 0, 'k'},
			{"write-follow", no_argument, 0, 'K'},
            {"write-follows", no_argument, 0, 'K'},
			{"initiate-tx", no_argument, 0, 'F'},
			{"dump-err", no_argument, 0, 'e'},
			{"no-rx", no_argument, 0, 'r'},
			{"no-tx", no_argument, 0, 't'},
			{"rx-delay", required_argument, 0, 'l'},
			{"tx-delay", required_argument, 0, 'a'},
			{"tx-bytes", required_argument, 0, 'w'},
			{"rx-bytes-threash", required_argument, 0, 'M'},
			{"rs485", required_argument, 0, 'q'},
			{"rs485_rts", no_argument, 0, 'Q'},
			{"no-modem", no_argument, 0, 'm'},
			{"tx-time", required_argument, 0, 'o'},
			{"rx-time", required_argument, 0, 'i'},
			{"tx-wait", required_argument, 0, 'W'},
			{"ascii", no_argument, 0, 'A'},
			{"rx-timeout", required_argument, 0, 'I'},
			{"tx-timeout", required_argument, 0, 'O'},
			{"error-on-timeout", no_argument, 0, 'Z'},
			{"no-icount", no_argument, 0, 'n'},
			{"flush-buffers", no_argument, 0, 'f'},
			{"version", no_argument, 0, 1003},
            {"turnaround-delay", required_argument, 0, 1002},
            {"expected-rx", required_argument, 0, 1000},
            {"drain-timeout", required_argument, 0, 1001},
            {0,0,0,0},
		};

		int c = getopt_long(argc, argv, short_options,
				long_options, &option_index);

		if (c == EOF) {
			break;
		}

		switch (c) {
		case 0:
		case 'h':
			display_help();
			exit(0);
			break;
		case 'b':
			_cl_baud = number(optarg, 1, INT_MAX);
			break;
		case 'p':
			free(_cl_port);
            _cl_port = strdup(optarg);
            if (!_cl_port) { perror("strdup"); exit(2); }
			break;
		case 'd':
			_cl_divisor = number(optarg, 1, INT_MAX);
			break;
		case 'D':
			_cl_rx_dump = 1;
			_cl_rx_dump_ascii = !strcmp(optarg, "ascii");
			break;
		case 'T':
			_cl_tx_detailed = 1;
			break;
		case 'R':
			_cl_rx_detailed = 1;
			break;
		case 's':
			_cl_stats = 1;
			break;
		case 'S':
			_cl_stop_on_error = 1;
			break;
		case 'y': {
			_cl_single_byte = number(optarg, 0, 255);
			break;
		}
		case 'z': {
			_cl_another_byte = number(optarg, 0, 255);
			break;
		}
		case 'c':
			_cl_rts_cts = 1;
			break;
		case 'B':
			_cl_2_stop_bit = 1;
			break;
		case 'P':
            if (strcmp(optarg, "odd") && strcmp(optarg, "even") && strcmp(optarg, "mark") && strcmp(optarg, "space")) {
                fprintf(stderr, "Invalid parity: %s\n", optarg); exit(2);
            }
			_cl_parity = 1;
			_cl_odd_parity = (!strcmp(optarg, "mark")||!strcmp(optarg, "odd"));
			_cl_stick_parity = (!strcmp(optarg, "mark")||!strcmp(optarg, "space"));
			break;
		case 'k':
			_cl_loopback = 1;
			break;
		case 'K':
			_cl_write_after_read = 1;
			break;
		case 'F':
			_cl_initiate_tx = 1;
			break;
		case 'e':
			_cl_dump_err = 1;
			break;
		case 'r':
			_cl_no_rx = 1;
			break;
		case 't':
			_cl_no_tx = 1;
			break;
		case 'l': {
			_cl_rx_delay = number(optarg, 0, INT_MAX);
			break;
		}
		case 'a': {
			_cl_tx_delay = number(optarg, 0, INT_MAX);
			break;
		}
		case 'w': {
			_cl_tx_bytes = number(optarg, 0, 1048576);
			break;
		}
		case 'M': {
			_cl_rx_bytes_threash = number(optarg, 0, 1048576);
			break;
		}
        case 'q': {
            char *argument = strdup(optarg);
            if (!argument) { perror("strdup"); exit(2); }
            char *dot = strchr(argument, '.');
            if (dot) *dot++ = '\0';
            _cl_rs485_after_delay = number(argument, 0, INT_MAX);
            _cl_rs485_before_delay = dot ? number(dot, 0, INT_MAX) : 0;
            free(argument);
            break;
        }
		case 'Q':
			_cl_rs485_rts_after_send = 1;
			break;
		case 'm':
			_cl_do_not_touch_modem_lines = 1;
			break;
		case 'o': {
			_cl_tx_time = number(optarg, 0, INT_MAX);
			break;
		}
		case 'i': {
			_cl_rx_time = number(optarg, 0, INT_MAX);
			break;
		}
		case 'W': {
			_cl_tx_wait = number(optarg, 0, INT_MAX);
			break;
		}
		case 'A':
			_cl_ascii_range = 1;
			break;
		case 'I':
			_cl_rx_timeout_ms = number(optarg, 1, INT_MAX);
			break;
		case 'O':
			_cl_tx_timeout_ms = number(optarg, 1, INT_MAX);
			break;
		case 'Z':
			_cl_error_on_timeout = 1;
			break;
		case 'n':
			_cl_no_icount = 1;
			break;
        case 1003:
            printf("linux-serial-test %s\n", SERIAL_TEST_VERSION); exit(0);
        case 1002:
            _cl_turnaround_delay = number(optarg, 0, INT_MAX); break;
        case 1000:
            _cl_expected_rx = number(optarg, 0, LLONG_MAX); break;
        case 1001:
            _cl_drain_timeout = number(optarg, 1, INT_MAX); break;
        case '?':
        default:
            exit(2);
        case 'f':
			_cl_flush_buffers = 1;
			break;
		}
	}
    if (optind != argc) { fprintf(stderr, "Unexpected positional argument\n"); exit(2); }
}

static void dump_serial_port_stats(void)
{
	struct serial_icounter_struct icount = { 0 };

	printf("%s: count for this session: rx=%lld, tx=%lld, rx err=%lld\n", _cl_port, _read_count, _write_count, _error_count);

	if (!_cl_no_icount) {
		int ret = ioctl(_fd, TIOCGICOUNT, &icount);
		if (ret < 0) {
			perror("Error getting TIOCGICOUNT");
            _failed = 1;
		} else {
			printf("%s: TIOCGICOUNT: ret=%i, rx=%i, tx=%i, frame = %i, overrun = %i, parity = %i, brk = %i, buf_overrun = %i\n",
					_cl_port, ret, icount.rx, icount.tx, icount.frame, icount.overrun, icount.parity, icount.brk,
					icount.buf_overrun);
            if (_initial_icount_valid) {
                unsigned int overrun = (unsigned)icount.overrun - (unsigned)_initial_icount.overrun;
                unsigned int frame = (unsigned)icount.frame - (unsigned)_initial_icount.frame;
                unsigned int parity = (unsigned)icount.parity - (unsigned)_initial_icount.parity;
                unsigned int brk = (unsigned)icount.brk - (unsigned)_initial_icount.brk;
                unsigned int buf = (unsigned)icount.buf_overrun - (unsigned)_initial_icount.buf_overrun;
                printf("Driver error deltas: overrun=%u frame=%u parity=%u brk=%u buf_overrun=%u\n", overrun, frame, parity, brk, buf);
                if (overrun || frame || parity || brk || buf) _failed = 1;
            }
		}
	}
}

static unsigned char next_count_value(unsigned char c)
{
	c++;
	if (_cl_ascii_range && c == 127)
		c = 32;
	return c;
}

static void process_read_data(void)
{
    unsigned char rb[1024];
    ssize_t c = read(_fd, rb, sizeof(rb));
    if (c < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("read"); _failed = 1;
        }
        return;
    }
    if (!c) return;
    if (_cl_rx_dump) {
        if (_cl_rx_dump_ascii) dump_data_ascii(rb, c);
        else dump_data(rb, c);
    }
    for (ssize_t i = 0; i < c; ++i) {
        if (rb[i] != _read_count_value) {
            if (_cl_dump_err && _error_count < 32)
                printf("Error, count: %lld, expected %02x, got %02x\n",
                       _read_count + i, _read_count_value, rb[i]);
            ++_error_count;
            if (_cl_stop_on_error) _failed = 1;
            _read_count_value = rb[i];
        }
        _read_count_value = next_count_value(_read_count_value);
    }
    _read_count += c;
    if (_cl_rx_detailed) printf("Read %zd bytes\n", c);
}

static ssize_t write_credit(void)
{
    if (!_cl_write_after_read) return _write_size;
    long long credit = _read_count - _write_count;
    if (_cl_initiate_tx) credit += _write_size;
    if (credit <= 0) return 0;
    return credit > _write_size ? _write_size : credit;
}

static void process_write_data(void)
{
    ssize_t size = write_credit();
    if (!size) return;
    unsigned char next = _write_count_value;
    for (ssize_t i = 0; i < size; ++i) {
        _write_data[i] = next;
        next = next_count_value(next);
    }
    ssize_t count = write(_fd, _write_data, size);
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("write"); _failed = 1;
        }
        return;
    }
    if (count) {
        _write_count_value = count < size ? _write_data[count] : next;
        _write_count += count;
    }
    if (_cl_tx_detailed) printf("wrote %zd bytes\n", count);
}

static void setup_serial_port(int baud)
{
	struct termios newtio;
	struct serial_rs485 rs485;
	int ret;

	_fd = open(_cl_port, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);

	if (_fd < 0) {
		ret = -errno;
		perror("Error opening serial port");
		exit(ret);
	}

	/* Lock device file */
	if (flock(_fd, LOCK_EX | LOCK_NB) < 0) {
		ret = -errno;
		perror("Error failed to lock device file");
		exit(ret);
	}

	_locked = 1;
    if (tcgetattr(_fd, &_saved_termios) < 0) { perror("tcgetattr"); exit(2); }
    _saved_termios_valid = 1;
    _saved_serial_valid = ioctl(_fd, TIOCGSERIAL, &_saved_serial) == 0;
    bzero(&newtio, sizeof(newtio)); /* clear struct for new port settings */

	/* man termios get more info on below settings */
	newtio.c_cflag = baud | CS8 | CLOCAL | CREAD;

	if (_cl_rts_cts) {
		newtio.c_cflag |= CRTSCTS;
	}

	if (_cl_2_stop_bit) {
		newtio.c_cflag |= CSTOPB;
	}

	if (_cl_parity) {
		newtio.c_cflag |= PARENB;
		if (_cl_odd_parity) {
			newtio.c_cflag |= PARODD;
		}
		if (_cl_stick_parity) {
			newtio.c_cflag |= CMSPAR;
		}
	}

	newtio.c_iflag = 0;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	// Read available data; poll and monotonic deadlines control waiting.
	newtio.c_cc[VMIN] = 1;

	// No termios timer: the descriptor is nonblocking.
	newtio.c_cc[VTIME] = 0;

    if (tcsetattr(_fd, TCSANOW, &newtio) < 0) { perror("tcsetattr"); exit(2); }
    struct termios effective;
    tcflag_t mask = CSIZE | PARENB | PARODD | CMSPAR | CSTOPB | CRTSCTS;
    if (tcgetattr(_fd, &effective) < 0 || cfgetospeed(&effective) != cfgetospeed(&newtio) ||
        (effective.c_cflag & mask) != (newtio.c_cflag & mask)) {
        fprintf(stderr, "Driver did not accept requested terminal settings\n"); exit(2);
    }
    /* Preserve boot/application RS485 state unless explicitly requested. */
    if (_cl_rs485_after_delay >= 0) {
        if (ioctl(_fd, TIOCGRS485, &rs485) < 0) { perror("TIOCGRS485"); exit(2); }
        _saved_rs485 = rs485;
        _saved_rs485_valid = 1;
        rs485.flags |= SER_RS485_ENABLED;
        rs485.flags &= ~(SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND | SER_RS485_RX_DURING_TX);
        rs485.flags |= _cl_rs485_rts_after_send ? SER_RS485_RTS_AFTER_SEND : SER_RS485_RTS_ON_SEND;
        rs485.delay_rts_after_send = _cl_rs485_after_delay;
        rs485.delay_rts_before_send = _cl_rs485_before_delay;
        unsigned int wanted_flags = rs485.flags;
        if (ioctl(_fd, TIOCSRS485, &rs485) < 0 || ioctl(_fd, TIOCGRS485, &rs485) < 0) {
            perror("configuring RS485"); exit(2);
        }
        unsigned int mask = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND | SER_RS485_RX_DURING_TX;
        if ((rs485.flags & mask) != (wanted_flags & mask) ||
            rs485.delay_rts_after_send != (unsigned)_cl_rs485_after_delay ||
            rs485.delay_rts_before_send != (unsigned)_cl_rs485_before_delay) {
            fprintf(stderr, "Driver did not accept requested RS485 configuration\n"); exit(2);
        }
    }
}

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { perror("clock_gettime"); exit(2); }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void drain_output(void)
{
    long long deadline = monotonic_ms() + _cl_drain_timeout;
    int queued;
    while (!sigint_received) {
        if (ioctl(_fd, TIOCOUTQ, &queued) < 0) { perror("TIOCOUTQ"); _failed = 1; return; }
        if (!queued) {
            int state;
            /* Physical UARTs can expose the shift-register state. USB drivers
             * may not; accepted/drained bytes are not peer delivery proof. */
            if (ioctl(_fd, TIOCSERGETLSR, &state) < 0) {
                if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP) return;
                perror("TIOCSERGETLSR"); _failed = 1; return;
            }
            if (state & TIOCSER_TEMT) return;
        }
        if (monotonic_ms() >= deadline) {
            fprintf(stderr, "Output drain deadline exceeded\n"); _failed = 1; return;
        }
        poll(NULL, 0, 10);
    }
}

static int compute_error_count(void)
{
    if (_failed || sigint_received) return 125;
    long long result = _error_count;
    if (_cl_expected_rx >= 0 && _read_count != _cl_expected_rx) return 125;
    if (_rx_requested && !_read_count && _cl_expected_rx != 0) return 125;
    if (_rx_requested && _write_count && _cl_expected_rx < 0)
        result += llabs(_write_count - _read_count);
    return result > 125 ? 125 : (int)result;
}

int main(int argc, char * argv[])
{
	setvbuf(stdout, NULL, _IOLBF, 0);
    printf("Linux serial test app\n");

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
	atexit(&exit_handler); //does not work for SIGINT/SIGTERM without the previous signal handlers

	process_options(argc, argv);

	_rx_requested = !_cl_no_rx;

	if (!_cl_port) {
		fprintf(stderr, "ERROR: Port argument required\n");
		display_help();
		exit(-EINVAL);
	}

	int baud = B115200;

	if (_cl_baud && !_cl_divisor)
		baud = get_baud(_cl_baud);

	if (baud <= 0 || _cl_divisor) {
		printf("NOTE: non standard baud rate, trying custom divisor\n");
		baud = B38400;
		setup_serial_port(B38400);
		set_baud_divisor(_cl_baud, _cl_divisor);
	} else {
		setup_serial_port(baud);
		/*
		 * The flag ASYNC_SPD_CUST might have already been set, so
		 * clear it to avoid confusing the kernel uart dirver.
		 */
		clear_custom_speed_flag();
	}

	if (_cl_loopback) set_modem_lines(_fd, TIOCM_LOOP, TIOCM_LOOP);

	if (_cl_single_byte >= 0) {
		unsigned char data[2];
		int bytes = 1;
		int written;
		data[0] = (unsigned char)_cl_single_byte;
		if (_cl_another_byte >= 0) {
			data[1] = (unsigned char)_cl_another_byte;
			bytes++;
		}
		written = write(_fd, &data, bytes);
		if (written < 0) {
			int ret = errno;
			perror("write()");
			exit(ret);
		} else if (written != bytes) {
			fprintf(stderr, "ERROR: write() returned %d, not %d\n", written, bytes);
			exit(-EIO);
		}
		drain_output();
        return _failed || sigint_received ? 125 : 0;
	}

	_write_size = (_cl_tx_bytes == 0) ? 1024 : _cl_tx_bytes;

	_write_data = malloc(_write_size);
	if (_write_data == NULL) {
		fprintf(stderr, "ERROR: Memory allocation failed\n");
		exit(-ENOMEM);
	}

	if (_cl_ascii_range) {
		_read_count_value = _write_count_value = 32;
	}

    if (_cl_flush_buffers && tcflush(_fd, TCIOFLUSH) < 0) { perror("tcflush"); return 2; }
    if (!_cl_no_icount)
        _initial_icount_valid = ioctl(_fd, TIOCGICOUNT, &_initial_icount) == 0;
    printf("Ready: %s\n", _cl_port);
    long long start = monotonic_ms(), last_read = start, last_write = start;
    long long last_stat = start, last_timeout = start;
    long long tx_start = start + (long long)_cl_tx_wait * 1000;
    long long tx_end = _cl_tx_time ? tx_start + (long long)_cl_tx_time * 1000 : LLONG_MAX;
    long long rx_end = _cl_rx_time ? start + (long long)_cl_rx_time * 1000 : LLONG_MAX;
    while (!_failed && !sigint_received) {
        long long now = monotonic_ms();
        int rx = !_cl_no_rx && now < rx_end;
        int tx = !_cl_no_tx && now < tx_end;
        if (!rx && !tx) break;
        struct pollfd port = { .fd = _fd, .events = 0, .revents = 0 };
        if (rx && now - last_read >= _cl_rx_delay) port.events |= POLLIN;
        if (tx && now >= tx_start && now - last_write >= _cl_tx_delay && (!_read_count || now - last_read >= _cl_turnaround_delay) && write_credit()) port.events |= POLLOUT;
        int status = poll(&port, 1, 10);
        if (status < 0) {
            if (errno == EINTR) continue;
            perror("poll"); _failed = 1; break;
        }
        if (port.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Serial device disconnected or failed (poll revents=0x%x)\n", port.revents);
            _failed = 1; break;
        }
        if (sigint_received) break;
        now = monotonic_ms();
        if ((port.revents & POLLIN) && now < rx_end) {
            long long before = _read_count;
            process_read_data();
            if (_read_count > before) last_read = monotonic_ms();
        }
        if ((port.revents & POLLOUT) && monotonic_ms() < tx_end && (!_read_count || monotonic_ms() - last_read >= _cl_turnaround_delay) && !_failed) {
            long long before = _write_count;
            process_write_data();
            if (_write_count > before) last_write = monotonic_ms();
        }
        now = monotonic_ms();
        if (now - last_timeout >= 1000) {
            int timeout = (rx && now - last_read >= _cl_rx_timeout_ms) ||
                (tx && now >= tx_start && now - (last_write > tx_start ? last_write : tx_start) >= _cl_tx_timeout_ms);
            if (timeout) {
                fprintf(stderr, "Serial transfer inactivity: rx=%lld tx=%lld\n", _read_count, _write_count);
                if (_cl_error_on_timeout) _failed = 1;
            }
            last_timeout = now;
        }
        if (_cl_stats && now - last_stat >= 5000) {
            dump_serial_port_stats(); last_stat = now;
        }
    }
    printf("Terminating ...\n");
    if (!_failed && _write_count) drain_output();
    dump_serial_port_stats();
    return compute_error_count();
}
