# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A single-file C daemon (`k380-link.c`) that keeps one Bluetooth BR/EDR keyboard link (written for the Logitech K380) out of sniff mode, logs link health every second, and asks bluetoothd to reconnect after radio-loss drops. Status is experimental: disabling sniff has not been shown to prevent the drops, and the reconnect path has not yet run after a real drop (see README).

## Commands

```sh
make                                   # build ./k380-link (needs pkg-config + systemd-devel / libsystemd-dev)
sudo ./k380-link -a XX:XX:XX:XX:XX:XX -A YY:YY:YY:YY:YY:YY  # run in the foreground (needs CAP_NET_RAW or root)
sudo make install                      # binary -> /usr/local/sbin, unit -> /etc/systemd/system
sudo systemctl daemon-reload && sudo systemctl restart k380-link
journalctl -u k380-link -f             # full log; add -p warning for drops/warnings only
```

There are no tests and no linter; the only static check is `-Wall -Wextra` in the Makefile.

The repo's `k380-link.service` keeps the placeholder addresses `XX:XX:XX:XX:XX:XX` (keyboard) and `YY:YY:YY:YY:YY:YY` (adapter) on purpose; never commit real addresses. `make install` copies the unit as is, so it overwrites an installed unit that already has real addresses filled in.

## Architecture

- **No libbluetooth.** The kernel HCI socket ABI (`sockaddr_hci`, `hci_ufilter`, `HCIGETCONNLIST`, opcodes, event codes) is redefined at the top of the file. New HCI commands or events get their constants added there.
- **One single-threaded `poll()` loop** over two fds: a raw HCI socket (filter passes all event packets) and the sd-bus fd. The poll timeout is the earliest of the 1 s tick, `st.reconnect_at`, and the sd-bus timeout.
- **All state lives in the global `st`.** `st.handle == -1` means not connected, and `st.mode == -1` means unknown. `st.addr` is in wire order (little-endian); `st.addr_str` is the display form.
- **HCI commands are fire-and-forget.** Replies come back as Command Complete events in `on_cmd_complete()`. It drops any reply whose first two return bytes after the status don't match `st.handle`, because every command used so far returns `status, handle, ...`. A new command that returns something else needs that check changed.
- **On connect:** `on_connected()` sends READ_LINK_POLICY. The reply triggers WRITE_LINK_POLICY (original policy without the sniff bit), and that reply triggers EXIT_SNIFF. If EXIT_SNIFF gets a Command Status of "Command Disallowed", the link is already active.
- **`tick()` (every 1 s):** when disconnected, it polls `HCIGETCONNLIST` to pick up a link that is already up (at startup or after a missed event). When connected, it sends READ_RSSI, READ_LINK_QUALITY and READ_FAILED_CONTACT, and the health line is logged when the FAILED_CONTACT reply arrives (last of the three). If READ_RSSI returns "Unknown Connection ID", the link is treated as gone.
- **Reconnect:** only after disconnect reason `0x08` or `0x22`. After `-w` seconds, `start_reconnect()` calls `org.bluez.Device1.Connect` asynchronously on `st.dbus_path`, up to `-t` tries.
- **Logging:** use `logp()`. It writes `<prio>message` to line-buffered stdout, and journald reads the prefix as the priority (sd-daemon(3)). Don't use syslog().
- **Exit:** on SIGTERM/SIGINT the original link policy is restored. If the HCI socket reports POLLERR/POLLHUP (adapter removed, e.g. on suspend/resume), the process exits with status 1 and relies on `Restart=always` in the unit.
- **Adapter index is fixed for the life of the process.** It is used for the HCI `bind()`, for `HCIGETCONNLIST`, and in the D-Bus path `/org/bluez/hci<N>/dev_...`. Some adapters re-register after suspend/resume under a different index (e.g. `hci0` ↔ `hci1`). With `-A`, `find_adapter()` looks up the index at startup by matching the adapter's address (`HCIGETDEVLIST` + `HCIGETDEVINFO` on the still-unbound socket), so each systemd restart picks up the new index. `-i` (default 0) is the fixed-index fallback.

## systemd unit constraints

The unit is hardened with `CapabilityBoundingSet=CAP_NET_RAW`, `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome` and `RestrictAddressFamilies=AF_BLUETOOTH AF_UNIX` (AF_UNIX is for D-Bus). Code that needs another capability, address family or writable path won't work under the service until the unit is updated too.
