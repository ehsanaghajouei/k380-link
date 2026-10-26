/*
 * k380-link: keeps one Bluetooth BR/EDR keyboard link out of sniff mode, logs the
 * link health once per second, and asks bluetoothd to reconnect after a radio drop.
 *
 * Talks to the controller through a raw HCI socket and to bluetoothd over sd-bus.
 * Needs CAP_NET_RAW to send HCI commands.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

/* Kernel Bluetooth socket ABI (include/net/bluetooth/hci_sock.h), defined here to avoid libbluetooth. */
#define BTPROTO_HCI     1
#define SOL_HCI         0
#define HCI_FILTER      2
#define HCI_CHANNEL_RAW 0
#define HCIGETCONNLIST  _IOR('H', 212, int)
#define BT_CONNECTED    1

#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT   0x04
#define ACL_LINK        0x01

#define EVT_CONN_COMPLETE    0x03
#define EVT_CONN_REQUEST     0x04
#define EVT_DISCONN_COMPLETE 0x05
#define EVT_CMD_COMPLETE     0x0e
#define EVT_CMD_STATUS       0x0f
#define EVT_HW_ERROR         0x10
#define EVT_ROLE_CHANGE      0x12
#define EVT_MODE_CHANGE      0x14
#define EVT_LSTO_CHANGE      0x38

#define OP(ogf, ocf)           ((uint16_t)((ogf) << 10 | (ocf)))
#define OP_EXIT_SNIFF          OP(0x02, 0x0004)
#define OP_READ_LINK_POLICY    OP(0x02, 0x000c)
#define OP_WRITE_LINK_POLICY   OP(0x02, 0x000d)
#define OP_READ_FAILED_CONTACT OP(0x05, 0x0001)
#define OP_READ_LINK_QUALITY   OP(0x05, 0x0003)
#define OP_READ_RSSI           OP(0x05, 0x0005)

#define LP_SNIFF           0x0004
#define MODE_SNIFF         2
#define ERR_UNKNOWN_CONN   0x02
#define ERR_CONN_TIMEOUT   0x08
#define ERR_CMD_DISALLOWED 0x0c
#define ERR_LMP_TIMEOUT    0x22

struct sockaddr_hci {
	sa_family_t hci_family;
	uint16_t hci_dev;
	uint16_t hci_channel;
};

struct hci_ufilter {
	uint32_t type_mask;
	uint32_t event_mask[2];
	uint16_t opcode;
};

struct hci_conn_info {
	uint16_t handle;
	uint8_t bdaddr[6];
	uint8_t type;
	uint8_t out;
	uint16_t state;
	uint32_t link_mode;
};

struct hci_conn_list_req {
	uint16_t dev_id;
	uint16_t conn_num;
	struct hci_conn_info conn_info[16];
};

static const char *const mode_names[] = { "active", "hold", "sniff", "park" };

static volatile sig_atomic_t quit;

static struct {
	int sock;
	uint16_t dev;
	uint8_t addr[6]; /* wire order (little-endian) */
	char addr_str[18];
	char dbus_path[64];
	sd_bus *bus;
	int handle; /* -1 while not connected */
	int mode;   /* -1 while unknown */
	int orig_policy;
	int rssi, quality, failed;
	bool send_failed;
	unsigned wait_s, max_tries, tries;
	uint64_t reconnect_at; /* CLOCK_MONOTONIC ms, 0 = none */
	bool connect_pending;
} st = { .sock = -1, .handle = -1, .mode = -1, .orig_policy = -1, .wait_s = 3, .max_tries = 3 };

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The "<N>" prefix sets the journal priority (sd-daemon(3)). */
static void logp(int prio, const char *fmt, ...)
{
	va_list ap;

	printf("<%d>", prio);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static const char *reason_str(uint8_t code)
{
	switch (code) {
	case 0x04: return "page timeout";
	case 0x05: return "authentication failure";
	case 0x06: return "PIN or key missing (keyboard needs re-pairing)";
	case 0x08: return "connection timeout (radio link lost)";
	case 0x13: return "remote user terminated";
	case 0x14: return "remote low resources";
	case 0x15: return "remote power off";
	case 0x16: return "terminated by local host";
	case 0x22: return "LMP response timeout";
	default:   return "other";
	}
}

static void send_cmd(uint16_t op, const uint8_t *params, uint8_t len)
{
	uint8_t buf[4 + 255] = { HCI_COMMAND_PKT, op & 0xff, op >> 8, len };

	memcpy(buf + 4, params, len);
	if (write(st.sock, buf, 4 + len) < 0) {
		if (!st.send_failed)
			logp(LOG_WARNING, "HCI command %#06x failed: %s", op, strerror(errno));
		st.send_failed = true;
		return;
	}
	st.send_failed = false;
}

static void cmd_handle(uint16_t op)
{
	uint8_t p[2] = { st.handle & 0xff, st.handle >> 8 };

	send_cmd(op, p, sizeof(p));
}

static void write_policy(uint16_t policy)
{
	uint8_t p[4] = { st.handle & 0xff, st.handle >> 8, policy & 0xff, policy >> 8 };

	send_cmd(OP_WRITE_LINK_POLICY, p, sizeof(p));
}

static int find_handle(void)
{
	struct hci_conn_list_req req = { .dev_id = st.dev, .conn_num = 16 };

	if (ioctl(st.sock, HCIGETCONNLIST, &req) < 0)
		return -1;
	for (int i = 0; i < req.conn_num; i++) {
		struct hci_conn_info *ci = &req.conn_info[i];

		if (ci->type == ACL_LINK && ci->state == BT_CONNECTED && !memcmp(ci->bdaddr, st.addr, 6))
			return ci->handle;
	}
	return -1;
}

static void on_connected(int handle, const char *how)
{
	st.handle = handle;
	st.mode = -1;
	st.orig_policy = -1;
	st.reconnect_at = 0;
	st.tries = 0;
	logp(LOG_NOTICE, "%s %s (handle %#06x)", st.addr_str, how, handle);
	cmd_handle(OP_READ_LINK_POLICY); /* the reply triggers the no-sniff policy write */
}

static void on_disconnected(uint8_t reason)
{
	logp(LOG_WARNING, "%s disconnected: %s (%#04x)", st.addr_str, reason_str(reason), reason);
	st.handle = -1;
	st.mode = -1;
	/* Reconnect only after radio-loss drops, not after sleep, power-off or a local disconnect. */
	if (reason == ERR_CONN_TIMEOUT || reason == ERR_LMP_TIMEOUT) {
		st.tries = 0;
		st.reconnect_at = now_ms() + st.wait_s * 1000ULL;
	}
}

static void retry_or_give_up(void)
{
	if (st.handle >= 0)
		return;
	if (st.tries < st.max_tries)
		st.reconnect_at = now_ms() + st.wait_s * 1000ULL;
	else
		logp(LOG_WARNING, "giving up after %u attempts; waiting for the keyboard to reconnect by itself",
		     st.tries);
}

static int on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const sd_bus_error *e = sd_bus_message_get_error(m);

	(void)userdata;
	(void)ret_error;
	st.connect_pending = false;
	if (e) {
		logp(LOG_WARNING, "reconnect attempt %u/%u failed: %s", st.tries, st.max_tries,
		     e->message ? e->message : e->name);
		retry_or_give_up();
	} else {
		logp(LOG_NOTICE, "reconnect attempt %u/%u succeeded", st.tries, st.max_tries);
	}
	return 0;
}

static void start_reconnect(void)
{
	int r;

	st.reconnect_at = 0;
	st.tries++;
	logp(LOG_NOTICE, "keyboard not back; reconnect attempt %u/%u", st.tries, st.max_tries);
	r = sd_bus_call_method_async(st.bus, NULL, "org.bluez", st.dbus_path, "org.bluez.Device1", "Connect",
				     on_connect_reply, NULL, "");
	if (r < 0) {
		logp(LOG_WARNING, "reconnect call failed: %s", strerror(-r));
		retry_or_give_up();
		return;
	}
	st.connect_pending = true;
}

static void log_sample(void)
{
	char rssi[12] = "?", quality[12] = "?", failed[12] = "?";

	if (st.rssi != INT_MIN)
		snprintf(rssi, sizeof(rssi), "%d", st.rssi);
	if (st.quality >= 0)
		snprintf(quality, sizeof(quality), "%d", st.quality);
	if (st.failed >= 0)
		snprintf(failed, sizeof(failed), "%d", st.failed);
	logp(LOG_INFO, "rssi=%s quality=%s/255 failed_contacts=%s mode=%s", rssi, quality, failed,
	     st.mode < 0 ? "unknown" : mode_names[st.mode & 3]);
}

static void on_cmd_complete(uint16_t op, const uint8_t *r, int len)
{
	uint8_t status;

	/* Every command we send returns status + connection handle first. */
	if (len < 3 || st.handle < 0 || ((r[1] | r[2] << 8) & 0x0fff) != st.handle)
		return;
	status = r[0];

	switch (op) {
	case OP_READ_LINK_POLICY:
		if (status || len < 5) {
			logp(LOG_WARNING, "reading link policy failed: %#04x", status);
			break;
		}
		st.orig_policy = r[3] | r[4] << 8;
		write_policy(st.orig_policy & ~LP_SNIFF);
		break;
	case OP_WRITE_LINK_POLICY:
		if (status) {
			logp(LOG_WARNING, "disabling sniff mode failed: %#04x", status);
			break;
		}
		logp(LOG_NOTICE, "sniff mode disabled on this link (policy %#06x -> %#06x)", st.orig_policy,
		     st.orig_policy & ~LP_SNIFF);
		cmd_handle(OP_EXIT_SNIFF);
		break;
	case OP_READ_RSSI:
		if (status == ERR_UNKNOWN_CONN) {
			logp(LOG_WARNING, "%s link vanished without a disconnect event (adapter reset?)", st.addr_str);
			st.handle = -1;
			st.mode = -1;
			break;
		}
		st.rssi = status || len < 4 ? INT_MIN : (int8_t)r[3];
		break;
	case OP_READ_LINK_QUALITY:
		st.quality = status || len < 4 ? -1 : r[3];
		break;
	case OP_READ_FAILED_CONTACT:
		st.failed = status || len < 5 ? -1 : r[3] | r[4] << 8;
		log_sample(); /* last of the three per-second reads */
		break;
	}
}

static void on_cmd_status(uint8_t status, uint16_t op)
{
	if (op != OP_EXIT_SNIFF || !status)
		return;
	if (status == ERR_CMD_DISALLOWED) {
		if (st.handle >= 0 && st.mode < 0)
			st.mode = 0; /* not in sniff mode, so active */
	} else {
		logp(LOG_WARNING, "exit sniff failed: %#04x", status);
	}
}

static void handle_event(uint8_t evt, const uint8_t *p, int plen)
{
	int handle;

	switch (evt) {
	case EVT_CONN_COMPLETE: /* status, handle, bdaddr, link type, encryption */
		if (plen < 11 || p[9] != ACL_LINK || memcmp(p + 3, st.addr, 6))
			break;
		handle = (p[1] | p[2] << 8) & 0x0fff;
		if (p[0])
			logp(LOG_WARNING, "connection to %s failed: %s (%#04x)", st.addr_str, reason_str(p[0]), p[0]);
		else if (st.handle != handle)
			on_connected(handle, "connected");
		break;
	case EVT_CONN_REQUEST: /* bdaddr, class, link type */
		if (plen >= 10 && !memcmp(p, st.addr, 6))
			logp(LOG_NOTICE, "%s is connecting to the laptop", st.addr_str);
		break;
	case EVT_DISCONN_COMPLETE: /* status, handle, reason */
		if (plen >= 4 && !p[0] && st.handle >= 0 && ((p[1] | p[2] << 8) & 0x0fff) == st.handle)
			on_disconnected(p[3]);
		break;
	case EVT_CMD_COMPLETE: /* ncmd, opcode, return parameters */
		if (plen >= 3)
			on_cmd_complete(p[1] | p[2] << 8, p + 3, plen - 3);
		break;
	case EVT_CMD_STATUS: /* status, ncmd, opcode */
		if (plen >= 4)
			on_cmd_status(p[0], p[2] | p[3] << 8);
		break;
	case EVT_HW_ERROR: /* code */
		if (plen >= 1)
			logp(LOG_ERR, "controller hardware error %#04x", p[0]);
		break;
	case EVT_ROLE_CHANGE: /* status, bdaddr, role */
		if (plen >= 8 && !p[0] && !memcmp(p + 1, st.addr, 6))
			logp(LOG_INFO, "laptop role on this link: %s", p[7] ? "peripheral" : "central");
		break;
	case EVT_MODE_CHANGE: /* status, handle, mode, interval */
		if (plen < 6 || st.handle < 0 || ((p[1] | p[2] << 8) & 0x0fff) != st.handle)
			break;
		if (p[0]) {
			logp(LOG_WARNING, "mode change failed: %s (%#04x)", reason_str(p[0]), p[0]);
			break;
		}
		if (p[3] != st.mode) {
			if (p[3] == MODE_SNIFF)
				logp(LOG_WARNING, "link entered sniff mode (interval %.1f ms); pulling it back",
				     (p[4] | p[5] << 8) * 0.625);
			else
				logp(LOG_NOTICE, "link mode -> %s", mode_names[p[3] & 3]);
		}
		st.mode = p[3];
		break;
	case EVT_LSTO_CHANGE: /* handle, timeout */
		if (plen >= 4 && st.handle >= 0 && ((p[0] | p[1] << 8) & 0x0fff) == st.handle)
			logp(LOG_NOTICE, "link supervision timeout -> %.2f s", (p[2] | p[3] << 8) * 0.625 / 1000);
		break;
	}
}

static void tick(void)
{
	if (st.handle < 0) {
		int h = find_handle();

		if (h >= 0)
			on_connected(h, "found connected");
		return;
	}
	if (st.mode == MODE_SNIFF)
		cmd_handle(OP_EXIT_SNIFF);
	st.rssi = INT_MIN;
	st.quality = -1;
	st.failed = -1;
	cmd_handle(OP_READ_RSSI);
	cmd_handle(OP_READ_LINK_QUALITY);
	cmd_handle(OP_READ_FAILED_CONTACT);
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s -a BDADDR [-i HCI_INDEX] [-w RECONNECT_WAIT_S] [-t RECONNECT_TRIES]\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *addr = NULL;
	unsigned b[6];
	int opt, r, ret = 0;
	struct sigaction sa = { .sa_handler = on_signal };
	struct sockaddr_hci sh = { .hci_family = AF_BLUETOOTH, .hci_channel = HCI_CHANNEL_RAW };
	struct hci_ufilter filter = { .type_mask = 1u << HCI_EVENT_PKT, .event_mask = { ~0u, ~0u } };
	uint64_t next_tick;

	while ((opt = getopt(argc, argv, "a:i:w:t:")) != -1) {
		switch (opt) {
		case 'a': addr = optarg; break;
		case 'i': st.dev = atoi(optarg); break;
		case 'w': st.wait_s = atoi(optarg); break;
		case 't': st.max_tries = atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (!addr || sscanf(addr, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		usage(argv[0]);
	for (int i = 0; i < 6; i++)
		st.addr[5 - i] = b[i];
	snprintf(st.addr_str, sizeof(st.addr_str), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
	snprintf(st.dbus_path, sizeof(st.dbus_path), "/org/bluez/hci%u/dev_%02X_%02X_%02X_%02X_%02X_%02X", st.dev,
		 b[0], b[1], b[2], b[3], b[4], b[5]);

	setvbuf(stdout, NULL, _IOLBF, 0);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	st.sock = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (st.sock < 0) {
		logp(LOG_ERR, "HCI socket: %s", strerror(errno));
		return 1;
	}
	sh.hci_dev = st.dev;
	if (bind(st.sock, (struct sockaddr *)&sh, sizeof(sh)) < 0) {
		logp(LOG_ERR, "bind hci%u: %s", st.dev, strerror(errno));
		return 1;
	}
	if (setsockopt(st.sock, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0) {
		logp(LOG_ERR, "HCI filter: %s", strerror(errno));
		return 1;
	}
	r = sd_bus_open_system(&st.bus);
	if (r < 0) {
		logp(LOG_ERR, "system bus: %s", strerror(-r));
		return 1;
	}

	logp(LOG_INFO, "watching %s on hci%u: sniff off, health log every 1 s, reconnect after %u s (max %u tries)",
	     st.addr_str, st.dev, st.wait_s, st.max_tries);

	next_tick = now_ms();
	while (!quit) {
		uint64_t now = now_ms(), wake, bus_usec;
		struct pollfd fds[2];
		int timeout;

		if (now >= next_tick) {
			tick();
			next_tick = now + 1000;
		}
		if (st.reconnect_at && now >= st.reconnect_at && st.handle < 0 && !st.connect_pending)
			start_reconnect();

		wake = next_tick;
		if (st.reconnect_at && st.reconnect_at < wake)
			wake = st.reconnect_at;
		if (sd_bus_get_timeout(st.bus, &bus_usec) >= 0 && bus_usec != UINT64_MAX && bus_usec / 1000 < wake)
			wake = bus_usec / 1000;
		timeout = wake > now ? (int)(wake - now) : 0;

		fds[0] = (struct pollfd){ .fd = st.sock, .events = POLLIN };
		fds[1] = (struct pollfd){ .fd = sd_bus_get_fd(st.bus), .events = sd_bus_get_events(st.bus) };
		if (poll(fds, 2, timeout) < 0) {
			if (errno == EINTR)
				continue;
			logp(LOG_ERR, "poll: %s", strerror(errno));
			ret = 1;
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			logp(LOG_ERR, "HCI socket closed (adapter removed?)");
			ret = 1;
			break;
		}
		if (fds[0].revents & POLLIN) {
			uint8_t buf[260];
			ssize_t n = recv(st.sock, buf, sizeof(buf), MSG_DONTWAIT);

			if (n < 0 && errno != EAGAIN && errno != EINTR) {
				logp(LOG_ERR, "HCI read: %s", strerror(errno));
				ret = 1;
				break;
			}
			if (n >= 3 && buf[0] == HCI_EVENT_PKT)
				handle_event(buf[1], buf + 3, n - 3);
		}
		while ((r = sd_bus_process(st.bus, NULL)) > 0)
			;
		if (r < 0) {
			logp(LOG_ERR, "D-Bus: %s", strerror(-r));
			ret = 1;
			break;
		}
	}

	if (st.handle >= 0 && st.orig_policy >= 0) {
		write_policy(st.orig_policy);
		logp(LOG_NOTICE, "restored link policy %#06x", st.orig_policy);
	}
	sd_bus_flush_close_unref(st.bus);
	close(st.sock);
	return ret;
}
