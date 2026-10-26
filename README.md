# k380-link

A small Linux daemon that tries to stop a Bluetooth keyboard (written for the Logitech K380) from dropping its connection while you type.

It was written for an Intel AX211 laptop, where the K380 kept disconnecting with reason `0x08` (Connection Timeout). In a btmon capture of one drop, the laptop's request to wake the link from sniff (low-power) mode timed out about 5 seconds after the last keystroke, and then the link dropped.

> **Status: experimental.** Keeping the link out of sniff mode has not been shown to prevent these drops. The automatic reconnect has not yet run after a real drop.

## What it does

For one keyboard, identified by its Bluetooth address:

- **Keeps the link awake.** Each time the keyboard connects, it turns off sniff mode for that link. If the link enters sniff anyway, it pulls it back out and logs a warning. On exit, it puts the original link policy back.
- **Logs link health.** Once a second it logs RSSI, link quality, the failed contact counter and the link mode. It also logs connects, disconnects with their reason, role changes and controller hardware errors.
- **Reconnects faster.** After a drop with reason `0x08` (Connection Timeout) or `0x22` (LMP Response Timeout), it waits a few seconds. If the keyboard hasn't come back by then, it asks bluetoothd to reconnect (`org.bluez.Device1.Connect`), up to a set number of tries. It doesn't reconnect after any other kind of disconnect, such as the keyboard going to sleep or being switched off.

It sends HCI commands over a raw HCI socket and talks to bluetoothd over sd-bus. It doesn't need libbluetooth.

**Downside:** without sniff mode the keyboard's radio stays fully on, so its batteries run down faster. How much faster hasn't been measured.

## Requirements

- Linux with BlueZ, and a keyboard that connects over Bluetooth Classic (BR/EDR)
- `gcc`, `make`, `pkg-config`, and the systemd development headers (`systemd-devel` on Fedora, `libsystemd-dev` on Debian/Ubuntu)
- `CAP_NET_RAW`, or root, to send HCI commands

## Build and install

1. Find your keyboard's address:

   ```sh
   bluetoothctl devices
   ```

2. In `k380-link.service`, replace `XX:XX:XX:XX:XX:XX` with that address.

3. Build, install and start it:

   ```sh
   make
   sudo make install
   sudo systemctl daemon-reload
   sudo systemctl enable --now k380-link.service
   ```

## Usage

```
k380-link -a BDADDR [-i HCI_INDEX] [-w RECONNECT_WAIT_S] [-t RECONNECT_TRIES]
```

| Option | Default | Meaning |
|---|---|---|
| `-a` | (required) | The keyboard's Bluetooth address |
| `-i` | `0` | Controller index (`hci0`) |
| `-w` | `3` | Seconds to wait after a drop before each reconnect attempt |
| `-t` | `3` | Number of reconnect attempts before giving up |

To try it in the foreground first: `sudo ./k380-link -a XX:XX:XX:XX:XX:XX`

## Commands

```sh
# Status
sudo systemctl status k380-link.service

# Drops and warnings only
journalctl -u k380-link -p warning

# Full log
journalctl -u k380-link -f

# Stop it
sudo systemctl disable --now k380-link

# Remove it
sudo make uninstall
```

## License

[MIT](LICENSE)
