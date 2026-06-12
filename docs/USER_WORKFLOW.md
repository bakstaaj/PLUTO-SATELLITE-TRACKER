# Pluto Satellite Tracker Workflow

This guide walks through the app in the order most operators will use it.

## 1. Open the UI

Open the Pluto app in a browser:

- `http://192.168.2.1:8080/` when connected over Pluto USB Ethernet
- `http://192.168.3.1:8080/` when connected over the alternate network path

If the page loads, the top-right status should show `Backend online`.

## 2. Set the observer location

Use the `Observer` section first.

1. Enter a station name.
2. Enter latitude and longitude manually, or click directly on the map to prefill them.
3. Set altitude in meters.
4. Set minimum elevation in degrees.
5. Click `Save Location`.

What happens next:

- The observer config is saved on the Pluto.
- The app regenerates pass predictions for that location.
- The map updates around the new observer position.

## 3. Sync Pluto time

Before starting any real tracking, click `Sync Time`.

This is important because:

- pass timing depends on correct UTC time
- `Start Track` and `Auto Track` expect the Pluto clock to be synced

Check the `Time` panel after syncing:

- `Sync state` should read `synced`
- `Last synced` should show a recent UTC timestamp

## 4. Refresh satellite data

Use the `Data Refresh` section when you want fresher orbital data.

- `Refresh Catalog/TLE` updates the local satellite catalog and TLE data
- `Regenerate Passes` rebuilds predicted passes from the current observer location and local catalog

Typical order:

1. `Refresh Catalog/TLE`
2. `Regenerate Passes`

The `Refresh Status` panel shows the last refresh result and summary counts.

## 5. Pick a pass

Use the `Next Passes` list.

- `Upcoming` shows current/future passes that are not stale
- `Ready` narrows to actionable passes
- `Active` shows passes currently in progress
- `All` shows everything in the prediction window

Click a pass row to load:

- pass timing and geometry
- radio target details
- Doppler preview
- map path and sky plot

## 6. Understand the map

The `Map` section now uses a real OpenStreetMap basemap.

What you can do there:

- pan and zoom like a normal web map
- click the map to prefill a new observer location
- inspect the visible ground track for the selected pass
- see AOS, TCA, LOS, and current/focused samples

Notes:

- the right-side sky plot is still the fastest way to understand azimuth/elevation geometry
- basemap tiles require browser internet access
- the `Reset` button clears the remembered map view for the current pass

## 7. Use the focused sample controls

When a pass is selected, the map and sky plot highlight one sample point.

The focused sample is used by these buttons:

- `Tune Focused RX`: tune the receiver to the exact Doppler-corrected frequency for that sample
- `Step Track Now`: apply the exact Doppler point immediately and update tracking state
- `Follow Live Point`: stop pinning a manual sample and return focus to the live/current point

The `Radio Status` panel will show:

- current target
- tuned frequency
- tracking state
- current Doppler point time
- LO write result

## 8. Manual radio workflow

For a selected pass, use the `Selected Pass` section.

Recommended order for manual tuning:

1. `Plan Radio Target`
2. `Tune Receiver`

Use this when you mainly want to park the Pluto on the satellite downlink without automatic stepping.

## 9. Doppler track workflow

For tracked tuning, use this order:

1. `Plan Doppler Track`
2. Check the `Doppler Preview`
3. Use one of the tracking controls

Tracking controls:

- `Start Track`: start tracked tuning using the nearest live Doppler point
- `Step Now`: apply one tracking step right now
- `Stop Track`: stop manual track stepping state
- `Auto Track`: keep stepping the Doppler track automatically
- `Stop Auto`: stop the automatic loop

Good operating pattern:

1. Sync time
2. Select a future or active pass
3. Plan the Doppler track
4. Inspect the map and focused sample
5. Start tracking close to AOS, or use Auto Track if you want the Pluto to keep stepping on its own

## 10. Read the status panels

Three status areas matter most during operation:

- `Time`: confirms the Pluto clock is synced
- `Radio Status`: shows tuned target, LO path, track state, and current Doppler point
- `Refresh Status`: shows whether the catalog or pass data was updated successfully

If something feels off, look at `Radio Status` first.

## 11. Common first-time workflow

If you are starting from a fresh reboot, this is the shortest safe path:

1. Open the UI
2. Confirm `Backend online`
3. Set or verify observer location
4. Click `Save Location`
5. Click `Sync Time`
6. Click `Refresh Catalog/TLE`
7. Click `Regenerate Passes`
8. Select a pass from `Next Passes`
9. Review the map and Doppler preview
10. Use `Tune Receiver` or `Plan Doppler Track` depending on whether you want fixed or tracked tuning

## 12. What to do if nothing seems to happen

Check these in order:

1. `Backend online` is visible
2. `Sync state` is `synced`
3. the selected pass is not stale
4. the pass is Pluto-tunable for the active radio profile
5. `Radio Status` shows a valid `LO path`
6. `Track message` or `Refresh Status` does not show a backend error

If the UI loads but pass or radio behavior looks stale, refresh the page and compare the `Radio Status` values to the currently selected pass.
