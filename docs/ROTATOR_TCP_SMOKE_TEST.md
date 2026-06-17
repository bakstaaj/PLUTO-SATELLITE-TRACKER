# Rotator TCP Smoke Test

<!-- ROTATOR_TCP_SMOKE_TEST_V2_4_12 -->

This workflow verifies the exact TCP commands sent by the Pluto Satellite Tracker
rotator adapters before connecting real rotator hardware.

Use this before attaching a commercial rotator, SATRAN controller, or any
hardware that can move antennas.

## Purpose

The Pluto app now supports rotator control workflow pieces for:

- Simulation
- Hamlib `rotctld` TCP
- EasyComm II
- Yaesu GS-232
- SATRAN placeholder, pending firmware protocol confirmation

The smoke test lets the Pluto connect to a harmless TCP listener on the
development machine. You can confirm command bytes, host/port routing, and UI
workflow without moving hardware.

## Start the listener

From MSYS2 UCRT64 in the repo:

```bash
cd ~/sdrdev/PLUTO-SATELLITE-TRACKER

python3 tools/rotator_tcp_smoke_listener.py \
  --protocol hamlib \
  --host 0.0.0.0 \
  --port 4533 \
  --log build_logs/rotator_hamlib_smoke.log
```

`0.0.0.0` is correct for the listener bind address because it means “listen on
all local interfaces.”

In the Pluto UI, do **not** use `0.0.0.0` as the rotator Host. Use the LAN IP of
the development machine that is running the listener.

## Find the development machine LAN IP

From MSYS2, one practical option is:

```bash
ipconfig | grep -A 8 -i "Wireless LAN adapter\|Ethernet adapter" | grep -i "IPv4"
```

Use the IPv4 address on the same LAN that the Pluto can reach.

## Configure the Pluto UI

Open:

```text
http://192.168.3.1:8080/SatelliteTracker/
```

Then:

1. Open **Rotator Control** from the menu or the button beside **Listen**.
2. Select the desired rotator Type.
3. Set Host to the development machine LAN IP.
4. Set Port to the listener port, usually `4533`.
5. Click **Apply / Save Config**.
6. Set Test az/el values.
7. Click **Preview Command**.
8. Click **Test Move**.
9. Confirm the listener prints the expected command bytes.

## Expected command examples

For a test move around azimuth `180`, elevation `45`:

```text
Hamlib rotctld:  P 180.00 45.00\n
EasyComm II:     AZ180.0 EL45.0\n
Yaesu GS-232:    W180 045\n
```

The exact number formatting may vary slightly by backend version, but the
protocol prefix should match.

## Protocol reply behavior

The listener sends a simple protocol-shaped success reply by default:

- Hamlib: `RPRT 0\n`
- EasyComm II: newline
- Yaesu GS-232: newline

This allows the Pluto backend to complete a test command when it expects a reply.

To disable replies:

```bash
python3 tools/rotator_tcp_smoke_listener.py --protocol none --host 0.0.0.0 --port 4533
```

To send a custom reply:

```bash
python3 tools/rotator_tcp_smoke_listener.py --protocol hamlib --reply $'RPRT 0\n'
```

## Test matrix

Run one listener session for each adapter:

```bash
python3 tools/rotator_tcp_smoke_listener.py --protocol hamlib --host 0.0.0.0 --port 4533
python3 tools/rotator_tcp_smoke_listener.py --protocol easycomm2 --host 0.0.0.0 --port 4533
python3 tools/rotator_tcp_smoke_listener.py --protocol yaesu_gs232 --host 0.0.0.0 --port 4533
```

For each one, change the Pluto UI Rotator Type, click **Apply / Save Config**,
then click **Preview Command** and **Test Move**.

## Troubleshooting

If the Pluto UI shows a network adapter error:

- Confirm the listener is still running.
- Confirm Windows Firewall allows inbound TCP on the selected port.
- Confirm the Host in the Pluto UI is the development machine LAN IP, not
  `0.0.0.0`, `localhost`, or `127.0.0.1`.
- Confirm the Pluto and development machine are on the same network.
- Confirm the port matches the listener port.
- Try `ping <development-machine-ip>` from the Pluto if shell access is available.

If the listener prints nothing, Pluto did not reach the listener. This is a
network/firewall/host/port issue, not a rotator protocol issue.

If the listener prints unexpected command bytes, do not connect real hardware.
Capture the listener output and fix the adapter formatting first.
