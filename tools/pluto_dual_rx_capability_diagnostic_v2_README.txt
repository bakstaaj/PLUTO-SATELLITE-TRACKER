# Pluto Dual RX Capability Diagnostic v2

This version does not use SSH. It talks to Pluto with host-side libiio:

```bash
iio_info -u ip:192.168.2.1
iio_readdev -u ip:192.168.2.1 ...
```

## Run

```bash
cd ~/sdrdev/PLUTO-SATELLITE-TRACKER
./tools/diagnose_pluto_dual_rx_capability_v2.sh .
```

Output:

```text
C:/Users/jim/Downloads/pluto_dual_rx_capability_report_v2.txt
```

Upload that report before backend patching.

## What we need to see

The important PASS lines are:

- `cf-ad9361-lpc present`
- `voltage0 mentioned`
- `voltage1 mentioned`
- `RX0 capture bytes`
- `RX1 capture bytes`
- `RX0+RX1 capture bytes`

If all pass, the next backend patch can add `rx_channel=0|1|best`, where `best` compares RX0 and RX1 signal quality during audio playback and selects the better stream while one shared Doppler-tracked LO follows the pass.
