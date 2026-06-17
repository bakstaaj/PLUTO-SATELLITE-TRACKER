/* APP_ASSET_SPLIT_V1: extracted from web/index.html */
    let lastRadioHardware = null;
    let lastTimeStatus = null;
    let lastTrackState = null;
    let passFilter = "actionable";
    let currentObserverConfig = null;
    let currentSelectedPass = null;
    let currentSelectedPassKey = "";
    let pendingMapLocation = null;
    let mapLocationPickEnabled = false; // MAP_UI_CONTROLS_V1
    let pendingObserverDraft = null;
    let observerEditLockUntil = 0;
    let observerEditingActive = false;
    let currentMapFocusTime = "";
    let currentLeafletMap = null;
    let currentLeafletPassKey = "";
    let currentLeafletView = null;
    let refreshTimerId = 0;
    let refreshInFlight = false;
    let analogAudioSession = null;
    let autoSyncInFlight = false;
    let lastAutoSyncMs = 0;
    let lastAutoPassRefreshMs = 0;

    const AUTO_TIME_SYNC_COOLDOWN_MS = 10 * 60 * 1000;
    const AUTO_PASS_REFRESH_COOLDOWN_MS = 10 * 60 * 1000;
    const AUTO_TIME_SYNC_DRIFT_SECONDS = 45;

    async function parseJsonResponse(response, url) {
      const text = await response.text();
      if (!response.ok) {
        throw new Error(`${url}: ${response.status}`);
      }
      try {
        return JSON.parse(text);
      } catch (error) {
        const snippet = text.slice(0, 180).replace(/\s+/g, " ").trim();
        throw new Error(`${url}: invalid JSON (${error.message})${snippet ? ` | ${snippet}` : ""}`);
      }
    }

    async function getJson(url) {
      const response = await fetch(url, { cache: "no-store" });
      return parseJsonResponse(response, url);
    }

    async function postJson(url, payload) {
      const response = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      return parseJsonResponse(response, url);
    }

    function currentBrowserEpochSeconds() {
      return Math.floor(Date.now() / 1000);
    }

    function currentPlutoDriftSeconds(statusPayload) {
      const plutoEpoch = Number(statusPayload && statusPayload.time_epoch ? statusPayload.time_epoch : 0);
      if (!plutoEpoch) return Infinity;
      return Math.abs(currentBrowserEpochSeconds() - plutoEpoch);
    }

    async function autoSyncTimeAndPasses(force = false) {
      if (autoSyncInFlight) return false;

      const [status, timeStatus] = await Promise.all([
        getJson("/api/status"),
        getJson("/api/time/status")
      ]);

      const nowMs = Date.now();
      const driftSeconds = currentPlutoDriftSeconds(status);
      const needsSync =
        force ||
        (timeStatus.state !== "synced") ||
        (driftSeconds > AUTO_TIME_SYNC_DRIFT_SECONDS);

      if (!needsSync) {
        return false;
      }

      if (!force && (nowMs - lastAutoSyncMs) < AUTO_TIME_SYNC_COOLDOWN_MS) {
        return false;
      }

      autoSyncInFlight = true;
      try {
        await getJson(`/api/time/sync?epoch=${currentBrowserEpochSeconds()}`);
        lastAutoSyncMs = Date.now();

        const shouldRefreshPasses =
          force ||
          (timeStatus.state !== "synced") ||
          (driftSeconds > AUTO_TIME_SYNC_DRIFT_SECONDS) ||
          ((Date.now() - lastAutoPassRefreshMs) >= AUTO_PASS_REFRESH_COOLDOWN_MS);

        if (shouldRefreshPasses) {
          await postJson("/api/refresh/passes", {});
          lastAutoPassRefreshMs = Date.now();
        }

        return true;
      } finally {
        autoSyncInFlight = false;
      }
    }

    function setPillState(id, text, state) {
      const node = document.getElementById(id);
      if (!node) return;
      node.textContent = text;
      node.className = `pill ${state || "pending"}`;
      if (id === "status") {
        node.classList.add("status");
      }
    }

    function openDrawer() {
      document.getElementById("appDrawer").classList.add("open");
      document.getElementById("appDrawer").setAttribute("aria-hidden", "false");
      document.getElementById("drawerBackdrop").hidden = false;
      document.getElementById("menuToggleButton").setAttribute("aria-expanded", "true");
    }

    function closeDrawer() {
      document.getElementById("appDrawer").classList.remove("open");
      document.getElementById("appDrawer").setAttribute("aria-hidden", "true");
      document.getElementById("drawerBackdrop").hidden = true;
      document.getElementById("menuToggleButton").setAttribute("aria-expanded", "false");
    }

    function toggleDrawer() {
      const drawer = document.getElementById("appDrawer");
      if (drawer.classList.contains("open")) {
        closeDrawer();
      } else {
        openDrawer();
      }
    }

    function observerDraftFromInputs() {
      return {
        name: document.getElementById("observerNameInput").value,
        latitude_deg: document.getElementById("observerLatitudeInput").value,
        longitude_deg: document.getElementById("observerLongitudeInput").value,
        altitude_m: document.getElementById("observerAltitudeInput").value,
        minimum_elevation_deg: document.getElementById("observerMinElevationInput").value
      };
    }

    function applyObserverInputs(source) {
      document.getElementById("observerNameInput").value = source.name ?? "";
      document.getElementById("observerLatitudeInput").value = source.latitude_deg ?? "";
      document.getElementById("observerLongitudeInput").value = source.longitude_deg ?? "";
      document.getElementById("observerAltitudeInput").value = source.altitude_m ?? 0;
      document.getElementById("observerMinElevationInput").value = source.minimum_elevation_deg ?? 10;
    }

    function updatePendingObserverDraft(partial = {}) {
      pendingObserverDraft = {
        ...(pendingObserverDraft || observerDraftFromInputs()),
        ...partial
      };
    }

    function markObserverEditing(ms = 4000) {
      observerEditLockUntil = Date.now() + ms;
    }

    function observerInputsLocked() {
      return observerEditingActive || Date.now() < observerEditLockUntil;
    }

    function setDl(id, entries) {
      const node = document.getElementById(id);
      node.innerHTML = "";
      for (const [label, value] of entries) {
        const dt = document.createElement("dt");
        const dd = document.createElement("dd");
        dt.textContent = label;
        dd.textContent = value ?? "";
        node.append(dt, dd);
      }
    }

    function formatHz(hz) {
      if (!Number.isFinite(Number(hz))) return "";
      return `${(Number(hz) / 1000000).toFixed(3)} MHz`;
    }

    function summarizeDownlinks(sat) {
      const seen = new Set();
      const rows = [];
      for (const tx of sat.transmitters || []) {
        if (!tx.downlink_hz) continue;
        const label = formatHz(tx.downlink_hz);
        if (seen.has(label)) continue;
        seen.add(label);
        rows.push(label);
      }
      return rows.slice(0, 4);
    }

    function formatTime(iso) {
      if (!iso) return "";
      return new Date(iso).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    }

    function formatDuration(seconds) {
      const minutes = Math.round(Number(seconds || 0) / 60);
      return `${minutes} min`;
    }

    function formatCountdown(seconds) {
      const total = Math.max(0, Math.round(Number(seconds || 0)));
      const hours = Math.floor(total / 3600);
      const minutes = Math.floor((total % 3600) / 60);
      const secs = total % 60;
      if (hours > 0) return `${hours}h ${minutes}m ${secs}s`;
      if (minutes > 0) return `${minutes}m ${secs}s`;
      return `${secs}s`;
    }

    function formatDateTime(iso) {
      if (!iso) return "";
      return new Date(iso).toLocaleString([], {
        month: "short",
        day: "numeric",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit"
      });
    }

    function formatEpoch(epoch) {
      if (!Number.isFinite(Number(epoch)) || Number(epoch) <= 0) return "-";
      return new Date(Number(epoch) * 1000).toISOString().replace(".000Z", "Z");
    }

    function appendRefreshSummary(entries, refreshStatus) {
      const summary = refreshStatus.summary || {};
      if ((refreshStatus.target || "") === "passes") {
        if (summary.generated_utc) entries.push(["Passes generated", summary.generated_utc]);
        if (summary.start_utc) entries.push(["Prediction start", summary.start_utc]);
        if (summary.prediction_hours !== undefined) entries.push(["Prediction window", `${summary.prediction_hours} h`]);
        if (summary.pass_count !== undefined) entries.push(["Pass count", String(summary.pass_count)]);
        if (summary.satellite_count !== undefined) entries.push(["Satellites used", String(summary.satellite_count)]);
      } else if ((refreshStatus.target || "") === "catalog") {
        if (summary.catalog_updated_utc) entries.push(["Catalog updated", summary.catalog_updated_utc]);
        if (summary.celestrak_retrieved_utc) entries.push(["CelesTrak fetched", summary.celestrak_retrieved_utc]);
        if (summary.satnogs_status) entries.push(["SatNOGS status", summary.satnogs_status]);
        if (summary.satnogs_retrieved_utc) entries.push(["SatNOGS fetched", summary.satnogs_retrieved_utc]);
        if (summary.satellite_count !== undefined) entries.push(["Catalog satellites", String(summary.satellite_count)]);
        if (summary.satellites_with_transmitters !== undefined) entries.push(["With transmitters", String(summary.satellites_with_transmitters)]);
        if (summary.transmitter_count !== undefined) entries.push(["Transmitters", String(summary.transmitter_count)]);
        if (summary.satnogs_warning) entries.push(["SatNOGS note", summary.satnogs_warning]);
      }
    }

        /* MAP_FINAL_POLISH_V2 */
    function passTimingState(pass) {
      if (!pass) return "unknown";
      const now = Date.now();
      const aos = Date.parse(pass.aos_utc || "");
      const los = Date.parse(pass.los_utc || "");
      if (Number.isFinite(los) && now > los) return "stale";
      if (Number.isFinite(aos) && now < aos) return "upcoming";
      if (Number.isFinite(los) && now <= los) return "active";
      return "unknown";
    }
    function passReadiness(pass) {
      const timing = passTimingState(pass);
      if (timing === "stale") return { state: timing, label: "Stale", actionable: false };
      if (!isPassTunable(pass)) return { state: timing, label: "Out of range", actionable: false };
      if (!pass.doppler_plan || !(pass.doppler_plan.points || []).length) {
        return { state: timing, label: "No Doppler", actionable: false };
      }
      if (timing === "active") return { state: timing, label: "Active now", actionable: true };
      if (timing === "upcoming") return { state: timing, label: "Ready", actionable: true };
      return { state: timing, label: "Check time", actionable: false };
    }

    function renderRadioTarget(pass) {
      const radio = pass.radio || {};
      const downlink = radio.downlink_hz || (pass.downlinks_hz || [])[0];
      if (!downlink) return "No downlink";
      const mode = radio.mode || (pass.modes || [])[0] || "";
      const prefix = isPassTunable(pass) ? "Tune" : "Out of range";
      return `${prefix} ${formatHz(downlink)} ${mode}`.trim();
    }

    function formatOffsetHz(value) {
      if (!Number.isFinite(value)) return "-";
      const rounded = Math.round(value);
      return `${rounded >= 0 ? "+" : ""}${rounded} Hz`;
    }

    function escapeHtml(value) {
      return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#39;");
    }

    function renderDopplerRows(plan) {
      const points = (plan && plan.points) || [];
      if (!points.length) {
        return `<p class="doppler-note">No Doppler schedule for this pass.</p>`;
      }

      const maxRows = 10;
      const stride = Math.max(1, Math.ceil(points.length / maxRows));
      const rows = points
        .filter((_, index) => index % stride === 0 || index === points.length - 1)
        .map((point) => `
          <tr>
            <td>${escapeHtml(formatTime(point.time_utc))}</td>
            <td>${escapeHtml(`${point.elevation_deg} deg`)}</td>
            <td>${escapeHtml(formatHz(point.rx_hz))}</td>
            <td>${escapeHtml(formatOffsetHz(point.rx_offset_hz))}</td>
            <td>${escapeHtml(point.tx_hz ? formatHz(point.tx_hz) : "-")}</td>
            <td>${escapeHtml(point.tx_offset_hz !== undefined ? formatOffsetHz(point.tx_offset_hz) : "-")}</td>
          </tr>
        `)
        .join("");

      return `
        <table class="doppler-table">
          <thead>
            <tr>
              <th>Time</th>
              <th>El</th>
              <th>RX tune</th>
              <th>RX offset</th>
              <th>TX tune</th>
              <th>TX offset</th>
            </tr>
          </thead>
          <tbody>${rows}</tbody>
        </table>
        <p class="doppler-note">Range-rate model. Positive range rate means the satellite is moving away.</p>
      `;
    }

    function normalizeLongitude(lon) {
      let value = Number(lon || 0);
      while (value < -180) value += 360;
      while (value > 180) value -= 360;
      return value;
    }

    function splitTrackSegments(points) {
      const segments = [];
      let current = [];
      for (const point of points || []) {
        const lon = normalizeLongitude(point.longitude_deg);
        if (current.length) {
          const previous = current[current.length - 1];
          if (Math.abs(lon - previous.longitude_deg) > 180) {
            if (current.length > 1) segments.push(current);
            current = [];
          }
        }
        current.push({ ...point, longitude_deg: lon });
      }
      if (current.length > 1) segments.push(current);
      return segments;
    }

    function passKey(pass) {
      if (!pass) return "";
      return `${pass.norad_id || ""}:${pass.aos_utc || ""}`;
    }

    function findNearestTrackPoint(pass, isoTime) {
      if (!pass || !isoTime) return null;
      const target = Date.parse(isoTime);
      const points = pass.ground_track || [];
      if (!Number.isFinite(target) || !points.length) return null;

      let bestPoint = null;
      let bestDelta = Infinity;
      for (const point of points) {
        const pointTime = Date.parse(point.time_utc || "");
        if (!Number.isFinite(pointTime)) continue;
        const delta = Math.abs(pointTime - target);
        if (delta < bestDelta) {
          bestDelta = delta;
          bestPoint = point;
        }
      }
      return bestPoint;
    }

    function trackedPointForPass(pass, trackState) {
      if (!pass || !trackState || !trackState.point_time_utc) return null;
      if (trackState.state === "idle" || trackState.state === "complete") return null;
      return findNearestTrackPoint(pass, trackState.point_time_utc);
    }

    function focusedMapPoint(pass, trackState) {
      if (!pass) return null;
      if (currentMapFocusTime) {
        return findNearestTrackPoint(pass, currentMapFocusTime);
      }
      return trackedPointForPass(pass, trackState) ||
        findNearestTrackPoint(pass, pass.tca_utc) ||
        ((pass.ground_track || [])[0] || null);
    }

    function projectSkyPoint(azimuthDeg, elevationDeg, size) {
      const az = (Number(azimuthDeg || 0) * Math.PI) / 180;
      const elevation = Math.max(0, Math.min(90, Number(elevationDeg || 0)));
      const radius = (size / 2) * (1 - (elevation / 90));
      const center = size / 2;
      return {
        x: center + radius * Math.sin(az),
        y: center - radius * Math.cos(az)
      };
    }

    function formatMapPopup(point, label) {
      if (!point) return "";
      const details = [
        `<strong>${escapeHtml(label)}</strong>`,
        escapeHtml(formatDateTime(point.time_utc))
      ];
      if (Number.isFinite(Number(point.latitude_deg)) && Number.isFinite(Number(point.longitude_deg))) {
        details.push(escapeHtml(`Lat ${Number(point.latitude_deg).toFixed(3)} deg, Lon ${Number(point.longitude_deg).toFixed(3)} deg`));
      }
      if (Number.isFinite(Number(point.azimuth_deg)) && Number.isFinite(Number(point.elevation_deg))) {
        details.push(escapeHtml(`Az ${point.azimuth_deg} deg, El ${point.elevation_deg} deg`));
      }
      if (Number.isFinite(Number(point.rx_hz))) {
        details.push(escapeHtml(`RX ${formatHz(point.rx_hz)}`));
      }
      return `<div class="map-popup">${details.join("<br>")}</div>`;
    }


    function resetMapView() {
      currentLeafletView = null;
      renderMapPanel(currentSelectedPass, currentObserverConfig);
    }

        /* MAP_PICK_CONTROL_SKY_LABELS_V1 */
        /* MAP_PICK_BUTTON_CONNECTED_ICON_V2 */
    function setMapLocationPickEnabled(enabled) {
      mapLocationPickEnabled = Boolean(enabled);

      const applyButtonState = (button) => {
        if (!button) return;

        /*
         * Do not set textContent on the Leaflet map button. That button is an
         * SVG-only control, and textContent="" deletes the crosshair markup.
         */
        if (button.id === "mapPickLocationButton") {
          button.textContent = mapLocationPickEnabled ? "Map Pick: On" : "Map Pick: Off";
        }

        button.classList.toggle("map-pick-enabled", mapLocationPickEnabled);
        button.setAttribute("aria-pressed", mapLocationPickEnabled ? "true" : "false");
        button.title = mapLocationPickEnabled
          ? "Map Pick is ON. Click the map once to set receiver location."
          : "Turn on Map Pick, then click the map once to set receiver location.";
        if (button.id === "mapPickMapButton") {
          button.setAttribute("aria-label", mapLocationPickEnabled ? "Map Pick on" : "Map Pick off");
        }
      };

      applyButtonState(document.getElementById("mapPickLocationButton"));
      applyButtonState(document.getElementById("mapPickMapButton"));

      const hint = document.getElementById("mapPickHint");
      if (hint) {
        hint.textContent = mapLocationPickEnabled
          ? "Map Pick is ON: click the map once to prefill receiver latitude and longitude."
          : "Map Pick is OFF: map clicks will not change receiver location.";
      }
    }
    function toggleMapLocationPick() {
      setMapLocationPickEnabled(!mapLocationPickEnabled);
    }

    function trackPointIndexForPoint(points, point) {
      if (!point || !(points || []).length) return -1;
      if (point.time_utc) {
        const exactIndex = points.findIndex((candidate) => candidate.time_utc === point.time_utc);
        if (exactIndex >= 0) return exactIndex;
      }
      const target = Date.parse(point.time_utc || "");
      if (!Number.isFinite(target)) return -1;
      let bestIndex = -1;
      let bestDelta = Infinity;
      points.forEach((candidate, index) => {
        const candidateTime = Date.parse(candidate.time_utc || "");
        if (!Number.isFinite(candidateTime)) return;
        const delta = Math.abs(candidateTime - target);
        if (delta < bestDelta) {
          bestDelta = delta;
          bestIndex = index;
        }
      });
      return bestIndex;
    }

        /* MAP_NULL_PASS_STARTUP_FIX_V1 */
        /* MAP_FINAL_POLISH_V2 */
    function liveLookPointForPass(pass, activeTrackPoint, focusPoint) {
      if (activeTrackPoint) return activeTrackPoint;
      if (!pass) return null;

      const trackPoints = (pass && pass.ground_track) || [];
      if (!trackPoints.length) return null;

      const timing = passTimingState(pass);
      if (timing === "active") {
        return findNearestTrackPoint(pass, new Date().toISOString()) || trackPoints[0] || null;
      }
      if (timing === "stale") {
        return trackPoints[trackPoints.length - 1] || null;
      }

      /*
       * Upcoming/unknown passes should not show an AOS-to-TCA progress line or
       * a misleading "Now" marker. The full visible pass path still auto-fits.
       */
      return null;
    }
    function pathThroughPoint(points, point) {
      const index = trackPointIndexForPoint(points, point);
      if (index < 0) return [];
      return points.slice(0, index + 1);
    }

    function formatLookAngleValue(value) {
      if (!Number.isFinite(Number(value))) return "-";
      return `${Number(value).toFixed(1)} deg`;
    }

    /* MAP_LIVE_SATELLITE_FIT_ICON_V3 */
    function satelliteLiveIconV3() {
      if (!window.L) return null;
      return L.divIcon({
        className: "satellite-live-icon",
        html: `
          <svg viewBox="0 0 64 64" aria-hidden="true" focusable="false">
            <g transform="rotate(-25 32 32)">
              <rect x="25" y="24" width="14" height="16" rx="3" fill="#00a7c7" stroke="#ffffff" stroke-width="3"/>
              <rect x="7" y="25" width="16" height="14" rx="2" fill="#17435b" stroke="#ffffff" stroke-width="2"/>
              <rect x="41" y="25" width="16" height="14" rx="2" fill="#17435b" stroke="#ffffff" stroke-width="2"/>
              <line x1="23" y1="32" x2="25" y2="32" stroke="#ffffff" stroke-width="3"/>
              <line x1="39" y1="32" x2="41" y2="32" stroke="#ffffff" stroke-width="3"/>
              <circle cx="32" cy="32" r="3" fill="#ffffff"/>
              <path d="M32 18 L36 9" stroke="#ffffff" stroke-width="3" stroke-linecap="round"/>
              <circle cx="37" cy="7" r="3" fill="#ffcc33" stroke="#ffffff" stroke-width="1.5"/>
            </g>
          </svg>
        `,
        iconSize: [28, 28],
        iconAnchor: [14, 14],
        popupAnchor: [0, -14]
      });
    }

    function fitLeafletToCurrentPassV3(map, bounds) {
      if (!map || !(bounds || []).length) return;

      const validBounds = bounds.filter((latLng) =>
        Array.isArray(latLng) &&
        Number.isFinite(Number(latLng[0])) &&
        Number.isFinite(Number(latLng[1]))
      );

      if (validBounds.length > 1) {
        const applyFit = () => {
          try {
            map.invalidateSize(false);
            map.fitBounds(validBounds, {
              paddingTopLeft: [28, 28],
              paddingBottomRight: [28, 42],
              maxZoom: 7,
              animate: false
            });
          } catch (_error) {
          }
        };
        applyFit();
        window.setTimeout(applyFit, 80);
        window.setTimeout(applyFit, 350);
      } else if (validBounds.length === 1) {
        map.setView(validBounds[0], 4, { animate: false });
        window.setTimeout(() => map.invalidateSize(false), 80);
      }
    }


    /* MAP_PICK_CONTROL_SKY_LABELS_V1 */
    function addLeafletMapPickControlV1(map) {
      if (!map || !window.L) return;

      const PickControl = L.Control.extend({
        options: { position: "topright" },
        onAdd: function onAdd() {
          const container = L.DomUtil.create("div", "leaflet-bar leaflet-control map-pick-leaflet-control");
          const button = L.DomUtil.create("button", "map-pick-map-button", container);
          button.type = "button";
          button.id = "mapPickMapButton";
          button.setAttribute("aria-pressed", mapLocationPickEnabled ? "true" : "false");
          button.setAttribute("aria-label", mapLocationPickEnabled ? "Map Pick on" : "Map Pick off");
          button.title = mapLocationPickEnabled
            ? "Map Pick is ON. Click the map once to set receiver location."
            : "Turn on Map Pick, then click the map once to set receiver location.";
          button.innerHTML = `
            <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
              <circle cx="12" cy="12" r="7.5" fill="none" stroke="currentColor" stroke-width="2"/>
              <line x1="12" y1="2.5" x2="12" y2="7" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
              <line x1="12" y1="17" x2="12" y2="21.5" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
              <line x1="2.5" y1="12" x2="7" y2="12" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
              <line x1="17" y1="12" x2="21.5" y2="12" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
              <circle class="map-pick-center-dot" cx="12" cy="12" r="2" fill="currentColor"/>
            </svg>
          `;

          L.DomEvent.disableClickPropagation(container);
          L.DomEvent.disableScrollPropagation(container);
          L.DomEvent.on(button, "click", (event) => {
            L.DomEvent.stop(event);
            toggleMapLocationPick();
          });
          return container;
        }
      });

      new PickControl().addTo(map);
      window.setTimeout(() => setMapLocationPickEnabled(mapLocationPickEnabled), 0);
    }

    function renderLeafletMap(pass, config, focusPoint, activeTrackPoint) {
      const mapNode = document.getElementById("trackerLeafletMap");
      if (!mapNode) return;
      if (!window.L) {
        mapNode.textContent = "Basemap library did not load. Check browser internet access and reload the page.";
        return;
      }

      if (currentLeafletMap) {
        currentLeafletMap.remove();
        currentLeafletMap = null;
      }

      const passKeyValue = passKey(pass);
      const observerLatLng = [Number(config.latitude_deg || 0), normalizeLongitude(config.longitude_deg)];
      const overlayBounds = [observerLatLng];
      currentLeafletMap = L.map(mapNode, {
        zoomControl: false,
        scrollWheelZoom: false,
        doubleClickZoom: false,
        worldCopyJump: true
      });

      currentLeafletMap.scrollWheelZoom.disable();
      currentLeafletMap.doubleClickZoom.disable();

      L.control.zoom({ position: "topright" }).addTo(currentLeafletMap);
      addLeafletMapPickControlV1(currentLeafletMap);
      L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
        minZoom: 2,
        maxZoom: 19,
        attribution: "&copy; OpenStreetMap contributors"
      }).addTo(currentLeafletMap);

      L.circleMarker(observerLatLng, {
        radius: 7,
        color: "#ffffff",
        weight: 2,
        fillColor: "#006f9f",
        fillOpacity: 1,
        bubblingMouseEvents: false
      }).addTo(currentLeafletMap)
        .bindPopup(`<div class="map-popup"><strong>${escapeHtml(config.name || "Observer")}</strong><br>${escapeHtml(`${Number(config.latitude_deg).toFixed(5)} deg, ${Number(config.longitude_deg).toFixed(5)} deg`)}</div>`);

      if (pendingMapLocation) {
        L.circleMarker([pendingMapLocation.latitude_deg, normalizeLongitude(pendingMapLocation.longitude_deg)], {
          radius: 8,
          color: "#006f9f",
          weight: 2,
          dashArray: "4 3",
          fillColor: "#006f9f",
          fillOpacity: 0.35,
          bubblingMouseEvents: false
        }).addTo(currentLeafletMap)
          .bindPopup(`<div class="map-popup"><strong>Pending observer location</strong><br>${escapeHtml(`${pendingMapLocation.latitude_deg.toFixed(5)} deg, ${pendingMapLocation.longitude_deg.toFixed(5)} deg`)}</div>`);
      }

      const trackPoints = (pass && pass.ground_track) || [];
      const livePoint = liveLookPointForPass(pass, activeTrackPoint, focusPoint);
      const progressPoints = pathThroughPoint(trackPoints, livePoint);
      const segments = splitTrackSegments(trackPoints);
      segments.forEach((segment) => {
        const latLngs = segment.map((point) => {
          const latLng = [Number(point.latitude_deg || 0), normalizeLongitude(point.longitude_deg)];
          overlayBounds.push(latLng);
          return latLng;
        });
        if (latLngs.length > 1) {
          L.polyline(latLngs, {
            color: "#c4471c",
            weight: 4,
            opacity: 0.9,
            bubblingMouseEvents: false
          }).addTo(currentLeafletMap);
        }
      });

      const progressSegments = splitTrackSegments(progressPoints);
      progressSegments.forEach((segment) => {
        const latLngs = segment.map((point) => [Number(point.latitude_deg || 0), normalizeLongitude(point.longitude_deg)]);
        if (latLngs.length > 1) {
          L.polyline(latLngs, {
            color: "#00a7c7",
            weight: 5,
            opacity: 0.95,
            dashArray: "8 6",
            lineCap: "round",
            lineJoin: "round",
            bubblingMouseEvents: false
          }).addTo(currentLeafletMap);
        }
      });

      trackPoints.forEach((point) => {
        const latLng = [Number(point.latitude_deg || 0), normalizeLongitude(point.longitude_deg)];
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        L.circleMarker(latLng, {
          radius: isFocus ? 7 : 4,
          color: "#ffffff",
          weight: isFocus ? 2 : 1,
          fillColor: isFocus ? "#102030" : "#c4471c",
          fillOpacity: 1,
          bubblingMouseEvents: false
        }).addTo(currentLeafletMap)
          .bindPopup(formatMapPopup(point, isFocus ? "Focus" : "Pass sample"))
          .on("mouseover", () => {
            currentMapFocusTime = point.time_utc || "";
            renderMapPanel(currentSelectedPass, currentObserverConfig);
          })
          .on("click", () => {
            currentMapFocusTime = point.time_utc || "";
            renderMapPanel(currentSelectedPass, currentObserverConfig);
          });
      });

      [
        { point: trackPoints[0], label: "AOS", color: "#1a7f37" },
        { point: trackPoints.length ? trackPoints[Math.floor(trackPoints.length / 2)] : null, label: "TCA", color: "#a15c00" },
        { point: trackPoints.length ? trackPoints[trackPoints.length - 1] : null, label: "LOS", color: "#7d3ad3" }
      ].forEach(({ point, label, color }) => {
        if (!point) return;
        const latLng = [Number(point.latitude_deg || 0), normalizeLongitude(point.longitude_deg)];
        L.circleMarker(latLng, {
          radius: 7,
          color: "#ffffff",
          weight: 2,
          fillColor: color,
          fillOpacity: 1,
          bubblingMouseEvents: false
        }).addTo(currentLeafletMap).bindPopup(formatMapPopup(point, label));
      });

      if (livePoint) {
        const liveLatLng = [Number(livePoint.latitude_deg || 0), normalizeLongitude(livePoint.longitude_deg)];
        const liveIcon = satelliteLiveIconV3();
        if (liveIcon) {
          L.marker(liveLatLng, {
            icon: liveIcon,
            zIndexOffset: 1000,
            bubblingMouseEvents: false,
            title: "Satellite position"
          }).addTo(currentLeafletMap).bindPopup(formatMapPopup(livePoint, "Satellite position"));
        } else {
          L.circleMarker(liveLatLng, {
            radius: 8,
            color: "#ffffff",
            weight: 2,
            fillColor: "#00a7c7",
            fillOpacity: 1,
            bubblingMouseEvents: false
          }).addTo(currentLeafletMap).bindPopup(formatMapPopup(livePoint, "Satellite position"));
        }
      }

      if (livePoint) {
        const focusLatLng = [Number(livePoint.latitude_deg || 0), normalizeLongitude(livePoint.longitude_deg)];
        L.polyline([observerLatLng, focusLatLng], {
          color: "#3e6b85",
          weight: 2,
          opacity: 0.85,
          dashArray: "6 5",
          bubblingMouseEvents: false
        }).addTo(currentLeafletMap);
      }

      currentLeafletMap.on("click", (event) => {
        if (!mapLocationPickEnabled) {
          const status = document.getElementById("status");
          if (status) status.textContent = "Map Pick is off. Toggle Map Pick On before clicking the map to change receiver location.";
          return;
        }

        pendingMapLocation = {
          latitude_deg: event.latlng.lat,
          longitude_deg: event.latlng.lng
        };
        updatePendingObserverDraft({
          latitude_deg: event.latlng.lat.toFixed(5),
          longitude_deg: event.latlng.lng.toFixed(5)
        });
        applyObserverInputs(pendingObserverDraft);
        setMapLocationPickEnabled(false);
        renderMapPanel(currentSelectedPass, currentObserverConfig);
        document.getElementById("status").textContent = `Map selected ${event.latlng.lat.toFixed(5)}, ${event.latlng.lng.toFixed(5)}. Save Location to apply it.`;
      });

      currentLeafletPassKey = passKeyValue;
      currentLeafletView = null;
      fitLeafletToCurrentPassV3(currentLeafletMap, overlayBounds);
    }


    /* SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1 */
    function renderSkySatelliteIcon(point) {
      if (!point) return "";
      const x = Number(point.x || 0).toFixed(1);
      const y = Number(point.y || 0).toFixed(1);
      return `
        <g class="sky-satellite-icon" transform="translate(${x} ${y}) rotate(-25)">
          <line class="satellite-boom" x1="-10" y1="0" x2="10" y2="0" />
          <rect class="satellite-panel" x="-17" y="-5" width="8" height="10" rx="1.2" />
          <rect class="satellite-panel" x="9" y="-5" width="8" height="10" rx="1.2" />
          <rect class="satellite-body" x="-6" y="-6" width="12" height="12" rx="2" />
          <line class="satellite-boom" x1="0" y1="-8" x2="0" y2="-13" />
        </g>
      `;
    }


    /* SKY_PLOT_AOS_TCA_LOS_COLORS_V1 */
    function skySpecialMarkerClassForPoint(pass, point, index, total) {
      if (!point || !pass) return "";
      const pointTime = point.time_utc || "";

      if (index === 0 || pointTime === pass.aos_utc) {
        return "sky-aos-dot sky-special-dot";
      }

      if (index === total - 1 || pointTime === pass.los_utc) {
        return "sky-los-dot sky-special-dot";
      }

      if (pass.tca_utc && pointTime === pass.tca_utc) {
        return "sky-tca-dot sky-special-dot";
      }

      /*
       * High-resolution sample generation can mean the exact TCA timestamp is
       * not one of the sample times. In that case, make the sample nearest TCA
       * carry the TCA color.
       */
      const target = Date.parse(pass.tca_utc || "");
      if (Number.isFinite(target)) {
        const points = (pass.ground_track || []);
        let bestIndex = -1;
        let bestDelta = Infinity;
        points.forEach((candidate, candidateIndex) => {
          const candidateTime = Date.parse(candidate.time_utc || "");
          if (!Number.isFinite(candidateTime)) return;
          const delta = Math.abs(candidateTime - target);
          if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = candidateIndex;
          }
        });
        if (index === bestIndex) {
          return "sky-tca-dot sky-special-dot";
        }
      }

      return "";
    }

    function renderMapPanel(pass, config) {
      const node = document.getElementById("mapPanel");
      if (!config) {
        node.className = "empty";
        node.textContent = "Map unavailable until observer config loads.";
        return;
      }

      const trackPoints = (pass && pass.ground_track) || [];
      const activeTrackPoint = trackedPointForPass(pass, lastTrackState);
      const focusPoint = focusedMapPoint(pass, lastTrackState);
      const livePoint = liveLookPointForPass(pass, activeTrackPoint, focusPoint);
      const progressPoints = pathThroughPoint(trackPoints, livePoint);
      const skySize = 340;
      const skyCenter = skySize / 2;
      const skyPath = trackPoints.map((point) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        return `${xy.x.toFixed(1)},${xy.y.toFixed(1)}`;
      }).join(" ");
      const skyProgressPath = progressPoints.map((point) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        return `${xy.x.toFixed(1)},${xy.y.toFixed(1)}`;
      }).join(" ");
      const skyTrackDots = trackPoints.map((point, index) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        const specialClass = skySpecialMarkerClassForPoint(pass, point, index, trackPoints.length);
        const className = specialClass || (isFocus ? "sky-focus-dot" : "sky-sample-dot");
        const radius = specialClass ? "5.2" : (isFocus ? "5.5" : "3.2");
        const strokeWidth = specialClass || isFocus ? "2" : "1.2";
        return `<circle class="${className} track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${radius}" fill="${isFocus && !specialClass ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${strokeWidth}" />`;
      }).join("");
      const focusSky = focusPoint ? projectSkyPoint(focusPoint.azimuth_deg, focusPoint.elevation_deg, skySize) : null;
      const liveSky = livePoint ? projectSkyPoint(livePoint.azimuth_deg, livePoint.elevation_deg, skySize) : null;
      const readoutPoint = livePoint || focusPoint;
      const readoutTitle = livePoint ? "Live sample" : "Selected sample look angle";
      const liveLabel = readoutPoint
        ? `${formatDateTime(readoutPoint.time_utc)} | Az ${formatLookAngleValue(readoutPoint.azimuth_deg)} | El ${formatLookAngleValue(readoutPoint.elevation_deg)}`
        : "No look angle available.";
      const title = pass
        ? `${pass.name || "Selected pass"} visible ground track`
        : `${config.name || "Observer"} location`;

      node.className = "";
      node.innerHTML = `
        <div class="map-shell">
          <div class="map-views">
            <div class="map-frame">
              <div id="trackerLeafletMap" class="map-canvas" role="img" aria-label="${escapeHtml(title)}"></div>
              <div class="map-legend">
                <span class="legend-item"><span class="legend-swatch" style="background:#006f9f"></span>Observer</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#c4471c"></span>Path</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#00a7c7"></span>Progress</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#3e6b85"></span>Look</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#102030"></span>Focus</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#00a7c7"></span>Satellite</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#1a7f37"></span>AOS</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#a15c00"></span>TCA</span>
                <span class="legend-item"><span class="legend-swatch" style="background:#7d3ad3"></span>LOS</span>
              </div>
            </div>
            <div class="sky-frame">
              <svg viewBox="0 0 ${skySize} ${skySize}" role="img" aria-label="Polar sky plot">
                <rect x="0" y="0" width="${skySize}" height="${skySize}" fill="#fbfcfe" />
                <circle cx="${skyCenter}" cy="${skyCenter}" r="${(skySize / 2) - 12}" fill="#f3f7fb" stroke="#8ea7b9" stroke-width="1.5" />
                <circle cx="${skyCenter}" cy="${skyCenter}" r="${((skySize / 2) - 12) * (2 / 3)}" fill="none" stroke="#c7d4df" stroke-width="1" />
                <circle cx="${skyCenter}" cy="${skyCenter}" r="${((skySize / 2) - 12) * (1 / 3)}" fill="none" stroke="#c7d4df" stroke-width="1" />
                <line x1="${skyCenter}" y1="12" x2="${skyCenter}" y2="${skySize - 12}" stroke="#c7d4df" stroke-width="1" />
                <line x1="12" y1="${skyCenter}" x2="${skySize - 12}" y2="${skyCenter}" stroke="#c7d4df" stroke-width="1" />
                <text x="${skyCenter}" y="24" text-anchor="middle" font-size="13" fill="#526173">N</text>
                <text x="${skySize - 24}" y="${skyCenter + 5}" text-anchor="middle" font-size="13" fill="#526173">E</text>
                <text x="${skyCenter}" y="${skySize - 18}" text-anchor="middle" font-size="13" fill="#526173">S</text>
                <text x="24" y="${skyCenter + 5}" text-anchor="middle" font-size="13" fill="#526173">W</text>
                <text x="${skyCenter + 6}" y="${skyCenter - (((skySize / 2) - 12) * (2 / 3)) + 2}" font-size="9.5" fill="#7b8797">30° el</text>
                <text x="${skyCenter + 6}" y="${skyCenter - (((skySize / 2) - 12) * (1 / 3)) + 2}" font-size="9.5" fill="#7b8797">60°</text>
                <text x="${skyCenter + 6}" y="${skyCenter + 12}" font-size="9.5" fill="#7b8797">90°</text>
                ${trackPoints.length ? `<polyline class="sky-pass-path" fill="none" stroke="#c4471c" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" points="${skyPath}" />` : ""}
                ${skyProgressPath ? `<polyline class="sky-pass-progress" fill="none" stroke="#00a7c7" stroke-width="4" stroke-linecap="round" stroke-linejoin="round" stroke-dasharray="8 6" points="${skyProgressPath}" />` : ""}
                ${liveSky ? `<line class="sky-look-line" x1="${skyCenter}" y1="${skyCenter}" x2="${liveSky.x.toFixed(1)}" y2="${liveSky.y.toFixed(1)}" stroke="#3e6b85" stroke-width="2" stroke-dasharray="5 4" />` : ""}
                ${skyTrackDots}
                ${focusSky ? `<circle class="sky-focus-dot" cx="${focusSky.x.toFixed(1)}" cy="${focusSky.y.toFixed(1)}" r="6" fill="#102030" stroke="#fff" stroke-width="2" />` : ""}
                ${renderSkySatelliteIcon(liveSky)}
              </svg>
              <div class="listen-panel">
                <button id="analogAudioToggleButton" type="button">Listen</button>
                <span id="analogAudioStatus">Select a pass to listen.</span>
              </div>
            </div>
          </div>
          ${livePoint ? `
            <div class="map-info">
              <div><strong>Sample</strong>${escapeHtml(formatDateTime(livePoint.time_utc))}</div>
              <div><strong>Ground Point</strong>${escapeHtml(`${livePoint.latitude_deg.toFixed(3)} deg, ${livePoint.longitude_deg.toFixed(3)} deg`)}</div>
              <div><strong>Look Angles</strong>${escapeHtml(`Az ${livePoint.azimuth_deg} deg, El ${livePoint.elevation_deg} deg`)}</div>
              <div><strong>Altitude</strong>${escapeHtml(`${livePoint.altitude_km} km`)}</div>
            </div>
          ` : ""}
        </div>
      `;

      renderLeafletMap(pass, config, focusPoint, activeTrackPoint);
      bindAnalogAudio(pass, node);
      setMapLocationPickEnabled(mapLocationPickEnabled);
    }

    async function tuneFocusedSample(pass, focusPoint, button) {
      if (!focusPoint || !focusPoint.time_utc) {
        throw new Error("Focus does not have a usable time.");
      }
      await ensureSelectedTrack(pass);
      const params = new URLSearchParams({ time_utc: focusPoint.time_utc });

      button.disabled = true;
      const originalText = button.textContent;
      button.textContent = "Tuning...";
      try {
        await getJson(`/api/radio/track/tune_point?${params.toString()}`);
        button.textContent = "Tuned";
        await refresh();
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = originalText;
        }, 1200);
      }
    }

    function preferredRefreshMs() {
      if (document.hidden) return 0;
      const trackState = lastTrackState || {};
      if (trackState.state && trackState.state !== "idle" && trackState.state !== "stopped" && trackState.state !== "complete") {
        return 4000;
      }
      if (currentSelectedPass) {
        const state = passTimingState(currentSelectedPass);
        if (state === "active") return 5000;
        if (state === "upcoming") {
          const aos = Date.parse(currentSelectedPass.aos_utc || "");
          if (Number.isFinite(aos) && (aos - Date.now()) <= (15 * 60 * 1000)) {
            return 10000;
          }
        }
      }
      return 30000;
    }

    async function timedRefresh() {
      if (observerInputsLocked()) {
        scheduleRefreshLoop();
        return;
      }
      if (refreshInFlight) return;
      refreshInFlight = true;
      try {
        await autoSyncTimeAndPasses(false);
        await refresh();
      } catch (_error) {
      } finally {
        refreshInFlight = false;
        scheduleRefreshLoop();
      }
    }

    function scheduleRefreshLoop() {
      if (refreshTimerId) {
        clearTimeout(refreshTimerId);
        refreshTimerId = 0;
      }
      const delay = preferredRefreshMs();
      if (!delay) return;
      refreshTimerId = window.setTimeout(() => {
        timedRefresh();
      }, delay);
    }

    async function stepFocusedSample(pass, focusPoint, button) {
      if (!focusPoint || !focusPoint.time_utc) {
        throw new Error("Focus does not have a usable time.");
      }
      await ensureSelectedTrack(pass);
      button.disabled = true;
      const originalText = button.textContent;
      button.textContent = "Stepping...";
      try {
        await getJson(`/api/radio/track/tune_point?time_utc=${encodeURIComponent(focusPoint.time_utc)}`);
        button.textContent = "Stepped";
        currentMapFocusTime = focusPoint.time_utc;
        await refresh();
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = originalText;
        }, 1200);
      }
    }

    function radioHardwareEntries(hardware) {
      return [
        ["Radio profile", hardware.profile_label || hardware.profile_id || "-"],
        ["Firmware mode", hardware.firmware_mode || "-"],
        ["IIO device", hardware.iio_device_name || "-"],
        ["Capability range", `${formatHz(hardware.capability_min_hz)}-${formatHz(hardware.capability_max_hz)}`],
        ["VHF tunable", hardware.vhf_tunable ? "Yes" : "No"],
        ["UHF tunable", hardware.uhf_tunable ? "Yes" : "No"],
        ["RX LO", hardware.current_rx_lo_hz ? formatHz(hardware.current_rx_lo_hz) : "-"],
        ["RX LO path", hardware.rx_lo_path || "-"],
        ["App range", `${formatHz(hardware.software_min_hz)}-${formatHz(hardware.software_max_hz)}`]
      ];
    }

    function isHardwareTunable(hz) {
      const value = Number(hz || 0);
      if (!value) return false;
      const hardware = lastRadioHardware || {};
      const min = Number(hardware.capability_min_hz || hardware.software_min_hz || 70000000);
      const max = Number(hardware.capability_max_hz || hardware.software_max_hz || 6000000000);
      return value >= min && value <= max;
    }

    function isPassTunable(pass) {
      const radio = pass.radio || {};
      const downlink = radio.downlink_hz || (pass.downlinks_hz || [])[0];
      return radio.pluto_tunable !== false && isHardwareTunable(downlink);
    }


    async function refreshRadioStatus() {
      const [status, hardware, track, trackState] = await Promise.all([
        getJson("/api/radio/status"),
        getJson("/api/radio/hardware"),
        getJson("/api/radio/track/status"),
        getJson("/api/radio/track/state")
      ]);
      const entries = [
        ["State", status.state || "idle"]
      ];
      if (status.name) entries.push(["Target", status.name]);
      if (status.downlink_hz) entries.push(["Downlink", formatHz(status.downlink_hz)]);
      if (status.uplink_hz) entries.push(["Uplink", formatHz(status.uplink_hz)]);
      if (status.mode) entries.push(["Mode", status.mode]);
      if (track.state && track.state !== "idle") entries.push(["Track", track.name || track.state]);
      if (track.point_count) entries.push(["Track points", track.point_count]);
      if (trackState.state && trackState.state !== "idle") entries.push(["Tracking", trackState.state]);
      if (trackState.rx_hz) entries.push(["Track RX", formatHz(trackState.rx_hz)]);
      if (trackState.point_time_utc) entries.push(["Track point", formatTime(trackState.point_time_utc)]);
      if (trackState.seconds_until_point > 0) entries.push(["Point in", formatCountdown(trackState.seconds_until_point)]);
      if (trackState.seconds_until_aos > 0) entries.push(["AOS in", formatCountdown(trackState.seconds_until_aos)]);
      if (trackState.seconds_until_los > 0) entries.push(["LOS in", formatCountdown(trackState.seconds_until_los)]);
      if (trackState.seconds_since_los > 0) entries.push(["LOS ago", formatCountdown(trackState.seconds_since_los)]);
      if (trackState.lo_write_result) entries.push(["LO write", trackState.lo_write_result]);
      if (trackState.message) entries.push(["Track message", trackState.message]);
      entries.push(...radioHardwareEntries(hardware));
setDl("radioStatus", entries);
    }

    async function planRadioTarget(pass, button) {
      const radio = pass.radio || {};
      const downlink = radio.downlink_hz || (pass.downlinks_hz || [])[0];
      if (!downlink) return;

      const params = new URLSearchParams({
        name: pass.name || "",
        norad: String(pass.norad_id || ""),
        aos: pass.aos_utc || "",
        downlink: String(downlink),
        mode: radio.mode || (pass.modes || [])[0] || "",
        description: radio.description || ""
      });
      if (radio.uplink_hz) params.set("uplink", String(radio.uplink_hz));

      button.disabled = true;
      button.textContent = "Planning...";
      try {
        await getJson(`/api/radio/plan?${params.toString()}`);
        button.textContent = "Planned";
        await refreshRadioStatus();
      } catch (error) {
        button.textContent = "Plan failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = "Details";
        }, 1200);
      }
    }

    async function tuneRadioTarget(pass, button) {
      const radio = pass.radio || {};
      const downlink = radio.downlink_hz || (pass.downlinks_hz || [])[0];
      if (!downlink) return;

      const params = new URLSearchParams({
        name: pass.name || "",
        norad: String(pass.norad_id || ""),
        aos: pass.aos_utc || "",
        downlink: String(downlink),
        mode: radio.mode || (pass.modes || [])[0] || "",
        description: radio.description || ""
      });
      if (radio.uplink_hz) params.set("uplink", String(radio.uplink_hz));

      button.disabled = true;
      button.textContent = "Tuning...";
      try {
        await getJson(`/api/radio/tune?${params.toString()}`);
        button.textContent = "Tuned";
        await refreshRadioStatus();
      } catch (error) {
        button.textContent = "Tune failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = "Listen";
        }, 1200);
      }
    }

    /* DENSE_DOPPLER_RADIO_PLAN_V1 */
    const RADIO_DOPPLER_STEP_SECONDS_V1 = 5;

    function interpolateNumberDopplerV1(a, b, ratio) {
      const av = Number(a);
      const bv = Number(b);
      if (!Number.isFinite(av) && !Number.isFinite(bv)) return undefined;
      if (!Number.isFinite(av)) return bv;
      if (!Number.isFinite(bv)) return av;
      return av + ((bv - av) * ratio);
    }

    function roundHzDopplerV1(value) {
      if (!Number.isFinite(Number(value))) return undefined;
      return Math.round(Number(value));
    }

    function isoWithoutMillisDopplerV1(epochMs) {
      return new Date(epochMs).toISOString().replace(".000Z", "Z");
    }

    function interpolateDopplerPointV1(a, b, epochMs) {
      const aMs = Date.parse(a.time_utc || "");
      const bMs = Date.parse(b.time_utc || "");
      const span = bMs - aMs;
      const ratio = span > 0 ? Math.max(0, Math.min(1, (epochMs - aMs) / span)) : 0;
      const point = {
        ...a,
        time_utc: isoWithoutMillisDopplerV1(epochMs),
        interpolated: true
      };

      for (const key of [
        "rx_hz",
        "tx_hz",
        "rx_offset_hz",
        "tx_offset_hz",
        "range_rate_m_s",
        "range_km",
        "azimuth_deg",
        "elevation_deg"
      ]) {
        const value = interpolateNumberDopplerV1(a[key], b[key], ratio);
        if (value === undefined) continue;
        point[key] = key.endsWith("_hz") ? roundHzDopplerV1(value) : Number(value.toFixed(6));
      }

      return point;
    }

    function densifyDopplerPlanForRadioV1(plan, stepSeconds = RADIO_DOPPLER_STEP_SECONDS_V1) {
      const points = (plan && plan.points) || [];
      if (points.length < 2) return plan || { points: [] };

      const sorted = points
        .map((point) => ({ point, epochMs: Date.parse(point.time_utc || "") }))
        .filter((item) => Number.isFinite(item.epochMs))
        .sort((a, b) => a.epochMs - b.epochMs);

      if (sorted.length < 2) return plan;

      const stepMs = Math.max(1000, Number(stepSeconds || 5) * 1000);
      const densePoints = [];
      const pushUnique = (point) => {
        if (!point || !point.time_utc) return;
        if (densePoints.length && densePoints[densePoints.length - 1].time_utc === point.time_utc) return;
        densePoints.push(point);
      };

      for (let index = 0; index < sorted.length - 1; index += 1) {
        const current = sorted[index];
        const next = sorted[index + 1];
        pushUnique({ ...current.point, interpolated: false });

        for (let epochMs = current.epochMs + stepMs; epochMs < next.epochMs; epochMs += stepMs) {
          pushUnique(interpolateDopplerPointV1(current.point, next.point, epochMs));
        }
      }

      pushUnique({ ...sorted[sorted.length - 1].point, interpolated: false });

      return {
        ...(plan || {}),
        points: densePoints,
        original_point_count: points.length,
        dense_point_count: densePoints.length,
        dense_step_seconds: Math.round(stepMs / 1000),
        dense_generated_by: "browser"
      };
    }

    function denseDopplerPointCountTextV1(plan) {
      if (!plan || !plan.dense_point_count) return "";
      return ` | radio plan ${plan.dense_point_count} pts @ ${plan.dense_step_seconds || RADIO_DOPPLER_STEP_SECONDS_V1}s`;
    }


    async function planDopplerTrack(pass, button) {
      const radio = pass.radio || {};
      const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);
      if (!plan || !(plan.points || []).length) return;

      const payload = {
        ok: true,
        state: "track_planned",
        name: pass.name || "",
        norad_id: pass.norad_id || null,
        aos_utc: pass.aos_utc || "",
        tca_utc: pass.tca_utc || "",
        los_utc: pass.los_utc || "",
        mode: radio.mode || (pass.modes || [])[0] || "",
        description: radio.description || "",
        point_count: plan.points.length,
        doppler_plan: plan
      };

      button.disabled = true;
      button.textContent = "Planning...";
      try {
        await postJson("/api/radio/track/plan", payload);
        button.textContent = `Track planned${denseDopplerPointCountTextV1(plan)}`;
        await refreshRadioStatus();
      } catch (error) {
        button.textContent = "Plan failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = "Doppler Preview";
        }, 1200);
      }
    }

    async function controlDopplerTrack(action, button) {
      button.disabled = true;
      const originalText = button.textContent;
      button.textContent = action.includes("stop") ? "Stopping..." : "Tuning...";
      try {
        await getJson(`/api/radio/track/${action}`);
        button.textContent = action.includes("stop") ? "Stopped" : (action === "step" ? "Tuned" : "Started");
        await refreshRadioStatus();
      } catch (error) {
        button.textContent = "Failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = originalText;
        }, 1200);
      }
    }

    function dopplerTrackPayload(pass) {
      const radio = pass.radio || {};
      const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);
      return {
        ok: true,
        state: "track_planned",
        name: pass.name || "",
        norad_id: pass.norad_id || null,
        aos_utc: pass.aos_utc || "",
        tca_utc: pass.tca_utc || "",
        los_utc: pass.los_utc || "",
        mode: radio.mode || (pass.modes || [])[0] || "",
        description: radio.description || "",
        point_count: (plan && plan.points ? plan.points.length : 0),
        doppler_plan: plan
      };
    }

    async function ensureSelectedTrack(pass) {
      const plan = pass && pass.doppler_plan;
      if (!plan || !(plan.points || []).length) {
        throw new Error("Selected pass does not have a Doppler plan.");
      }
      await postJson("/api/radio/track/plan", dopplerTrackPayload(pass));
    }

    async function runSelectedTrackAction(pass, action, button) {
      if (action !== "stop" && action !== "auto/stop") {
        await ensureSelectedTrack(pass);
      }
      await controlDopplerTrack(action, button);
    }

    async function syncPlutoTime(button) {
      const epoch = currentBrowserEpochSeconds();
      button.disabled = true;
      button.textContent = "Syncing...";
      try {
        await getJson(`/api/time/sync?epoch=${epoch}`);
        await postJson("/api/refresh/passes", {});
        lastAutoSyncMs = Date.now();
        lastAutoPassRefreshMs = Date.now();
        button.textContent = "Synced";
        await refresh();
      } catch (error) {
        button.textContent = "Sync failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = "Sync Time";
        }, 1200);
      }
    }

    async function saveObserverConfig(button) {
      const payload = {
        name: document.getElementById("observerNameInput").value.trim(),
        latitude_deg: Number(document.getElementById("observerLatitudeInput").value),
        longitude_deg: Number(document.getElementById("observerLongitudeInput").value),
        altitude_m: Number(document.getElementById("observerAltitudeInput").value || 0),
        minimum_elevation_deg: Number(document.getElementById("observerMinElevationInput").value || 10),
        grid: ""
      };

      if (!payload.name || !Number.isFinite(payload.latitude_deg) || !Number.isFinite(payload.longitude_deg)) {
        throw new Error("Observer name, latitude, and longitude are required.");
      }

      button.disabled = true;
      button.textContent = "Saving...";
      try {
        await postJson("/api/config", payload);
        pendingMapLocation = null;
        pendingObserverDraft = null;
        button.textContent = "Rebuilding passes...";
        await postJson("/api/refresh/passes", {});
        button.textContent = "Saved";
        await refresh();
      } catch (error) {
        button.textContent = "Save failed";
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = "Save Location";
        }, 1200);
      }
    }

    async function runDataRefresh(target, button) {
      const originalText = button.textContent;
      button.disabled = true;
      button.textContent = "Running...";
      try {
        await postJson(`/api/refresh/${target}`, {});
        button.textContent = "Done";
        await refresh();
      } catch (error) {
        button.textContent = "Failed";
        await refresh().catch(() => {});
        throw error;
      } finally {
        setTimeout(() => {
          button.disabled = false;
          button.textContent = originalText;
        }, 1200);
      }
    }
    /* UI_BACKEND_AUDIO_PLAYER_V1
     * Backend-owned audio streaming.
     *
     * The old player pulled short WAV blocks and scheduled them with WebAudio.
     * That caused staccato playback when network/browser timing drifted.
     *
     * New model:
     *   selected pass -> backend DSP start -> browser <audio> streams live.wav?stream=1
     * The backend owns tuning/DSP/buffering. The browser only plays a single stream.
     */
        /* UI_STREAMING_PCM_PLAYER_V2
     * Backend-owned streaming audio with browser PCM playback.
     *
     * Native <audio> rejects some endless WAV streams. This player fetches the
     * single backend live.wav?stream=1 stream, skips the WAV header, converts
     * signed 16-bit little-endian PCM to Float32, and schedules it through
     * WebAudio. The browser does not tune, demodulate, or poll blocks; it only
     * plays the backend decoded stream.
     */
        /* UI_STREAMING_PCM_RECONNECT_V3
     * Backend-owned streaming audio with browser PCM playback and reconnect.
     *
     * Some backend WAV streams close after a short segment. While the session is
     * active, the browser reopens /api/radio/audio/live.wav?stream=1 instead of
     * stopping playback. The backend still owns tuning, DSP, and PCM generation;
     * the browser only plays decoded PCM.
     */
    function analogAudioUrl(pass) {
      const radio = pass && pass.radio ? pass.radio : {};
      const downlink = radio.downlink_hz || (pass && pass.downlinks_hz ? pass.downlinks_hz[0] : 0);
      if (!downlink) return "";
      const params = new URLSearchParams({ downlink_hz: String(downlink) });
      return {
        downlink,
        startUrl: `/api/radio/audio/live/start?${params.toString()}`,
        streamUrl: `/api/radio/audio/live.wav?stream=1&${params.toString()}`,
        stopUrl: `/api/radio/audio/live/stop`
      };
    }

    function concatUint8Arrays(a, b) {
      if (!a || !a.length) return b || new Uint8Array();
      if (!b || !b.length) return a;
      const out = new Uint8Array(a.length + b.length);
      out.set(a, 0);
      out.set(b, a.length);
      return out;
    }

    function pcm16ToAudioBuffer(context, bytes, sampleRate) {
      const sampleCount = Math.floor(bytes.length / 2);
      const buffer = context.createBuffer(1, sampleCount, sampleRate);
      const channel = buffer.getChannelData(0);
      const view = new DataView(bytes.buffer, bytes.byteOffset, sampleCount * 2);
      for (let i = 0; i < sampleCount; i += 1) {
        const sample = view.getInt16(i * 2, true);
        channel[i] = Math.max(-1, Math.min(1, sample / 32768));
      }
      return buffer;
    }

    async function stopAnalogAudio(reason = "Analog monitor stopped.") {
      const session = analogAudioSession;
      analogAudioSession = null;
      if (!session) return;

      session.stopped = true;
      if (session.reconnectTimer) {
        window.clearTimeout(session.reconnectTimer);
        session.reconnectTimer = 0;
      }
      try {
        if (session.controller) session.controller.abort();
      } catch (_error) {
      }

      try {
        for (const source of session.sources || []) {
          try { source.stop(); } catch (_error) {}
        }
      } catch (_error) {
      }

      try {
        if (session.stopUrl) {
          await postJson(session.stopUrl, {});
        }
      } catch (_error) {
      }

      try {
        if (session.context) await session.context.close();
      } catch (_error) {
      }

      if (session.button?.isConnected) {
        session.button.textContent = "Listen";
      }
      if (session.statusNode?.isConnected) {
        session.statusNode.textContent = reason;
      }
    }

    async function startAnalogAudio(pass, button, statusNode) {
      const audioUrls = analogAudioUrl(pass);
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      const sampleRate = 24000;
      const targetChunkBytes = 4096 * 2;
      if (!AudioCtx) {
        throw new Error("Web Audio is not available in this browser.");
      }
      if (!audioUrls) {
        throw new Error("No usable downlink is available for this pass.");
      }

      await stopAnalogAudio();

      statusNode.textContent = "Planning backend tuning and Doppler tracking...";
      try {
        await getJson(`/api/radio/plan?${new URLSearchParams({
          name: pass.name || "",
          norad: String(pass.norad_id || ""),
          aos: pass.aos_utc || "",
          downlink: String(audioUrls.downlink),
          mode: (pass.radio && pass.radio.mode) || (pass.modes || [])[0] || "",
          description: (pass.radio && pass.radio.description) || ""
        }).toString()}`);
      } catch (_error) {
        // Continue: live/start can still tune directly by downlink_hz.
      }

      if (pass.doppler_plan && (pass.doppler_plan.points || []).length) {
        try {
          await postJson("/api/radio/track/plan", dopplerTrackPayload(pass));
          await getJson("/api/radio/track/auto/start");
        } catch (_error) {
          // Continue with fixed-frequency audio if auto tracking cannot start.
        }
      }

      statusNode.textContent = "Starting backend audio DSP...";
      await postJson(audioUrls.startUrl, {});

      const context = new AudioCtx({ sampleRate });
      await context.resume();

      const session = {
        button,
        context,
        controller: null,
        nextTime: context.currentTime + 0.35,
        passKey: passKey(pass),
        reconnectCount: 0,
        reconnectTimer: 0,
        sources: [],
        statusNode,
        stopUrl: audioUrls.stopUrl,
        stopped: false,
        totalBytes: 0,
        totalBuffers: 0
      };
      analogAudioSession = session;

      button.textContent = "Stop";
      statusNode.textContent = "Opening backend decoded audio stream...";

      const schedulePcmBytes = (pcmBytes) => {
        if (session.stopped || !pcmBytes.length) return;
        const evenLength = pcmBytes.length - (pcmBytes.length % 2);
        if (evenLength <= 0) return;

        const audioBuffer = pcm16ToAudioBuffer(context, pcmBytes.slice(0, evenLength), sampleRate);
        const source = context.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(context.destination);
        source.onended = () => {
          const index = session.sources.indexOf(source);
          if (index >= 0) session.sources.splice(index, 1);
        };

        const startAt = Math.max(context.currentTime + 0.05, session.nextTime);
        source.start(startAt);
        session.nextTime = startAt + audioBuffer.duration;
        session.sources.push(source);
        session.totalBuffers += 1;

        const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
        if (statusNode?.isConnected) {
          statusNode.textContent = `Playing backend decoded audio from Pluto (${bufferedSeconds.toFixed(1)}s buffered, reconnects ${session.reconnectCount}).`;
        }
      };

      const readOneStreamSegment = async () => {
        const controller = new AbortController();
        session.controller = controller;
        const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=${session.reconnectCount}`;
        const response = await fetch(streamUrl, {
          cache: "no-store",
          signal: controller.signal
        });

        if (!response.ok) {
          throw new Error(`${streamUrl}: ${response.status}`);
        }
        if (!response.body || !response.body.getReader) {
          throw new Error("Browser streaming fetch is not available.");
        }

        const reader = response.body.getReader();
        let pending = new Uint8Array();
        let headerSkipped = false;
        let segmentBytes = 0;

        const processPending = (force = false) => {
          if (!headerSkipped) {
            if (pending.length < 44) return;
            pending = pending.slice(44);
            headerSkipped = true;
          }

          while (pending.length >= targetChunkBytes || (force && pending.length >= 2)) {
            const take = force ? pending.length - (pending.length % 2) : targetChunkBytes;
            const chunk = pending.slice(0, take);
            pending = pending.slice(take);
            schedulePcmBytes(chunk);
          }
        };

        while (!session.stopped) {
          const { value, done } = await reader.read();
          if (done) break;
          if (!value || !value.length) continue;

          segmentBytes += value.length;
          session.totalBytes += value.length;
          pending = concatUint8Arrays(pending, value);
          processPending(false);

          if (statusNode?.isConnected && session.totalBuffers === 0) {
            statusNode.textContent = `Receiving backend audio stream (${session.totalBytes} bytes)...`;
          }
        }

        processPending(true);
        return segmentBytes;
      };

      /* NOAA_AUDIO_DIAGNOSTIC_RECONNECT_UNTIL_STOP_V6: reconnect NOAA stream segments until user clicks Stop. */
      const reconnectLoop = async () => {
        while (!session.stopped && analogAudioSession === session) {
          try {
            const segmentBytes = await readOneStreamSegment();
            if (session.stopped || analogAudioSession !== session) return;

            session.reconnectCount += 1;
            const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
            if (statusNode?.isConnected) {
              statusNode.textContent =
                `Backend stream segment ended after ${segmentBytes} bytes; reconnecting (${bufferedSeconds.toFixed(1)}s buffered).`;
            }

            // Reconnect quickly. If the buffer is empty, reconnect immediately.
            const delayMs = bufferedSeconds > 0.8 ? 200 : 20;
            await new Promise((resolve) => {
              session.reconnectTimer = window.setTimeout(resolve, delayMs);
            });
            session.reconnectTimer = 0;
          } catch (error) {
            if (session.stopped || analogAudioSession !== session) return;
            const message = error && error.message ? error.message : "Backend audio stream failed.";
            if (statusNode?.isConnected) statusNode.textContent = message;
            const statusBar = document.getElementById("status");
            if (statusBar) statusBar.textContent = message;
            await stopAnalogAudio(message);
            return;
          }
        }
      };

      reconnectLoop();
    }


    /* NOAA_AUDIO_DIAGNOSTIC_V3
     * Fixed-frequency audio path test. This intentionally bypasses Doppler
     * planning/tracking but uses the same backend live audio start/stream/stop
     * endpoints and the same browser PCM scheduling model as satellite Listen.
     */
    function noaaDiagnosticFrequencyHz() {
      const select = document.getElementById("noaaDiagnosticFrequencySelect");
      const value = Number(select ? select.value : 0);
      return Number.isFinite(value) && value > 0 ? value : 162500000;
    }

    function noaaDiagnosticAudioUrls(frequencyHz) {
      const params = new URLSearchParams({ downlink_hz: String(frequencyHz) });
      return {
        downlink: frequencyHz,
        startUrl: `/api/radio/audio/live/start?${params.toString()}`,
        streamUrl: `/api/radio/audio/live.wav?stream=1&${params.toString()}`,
        stopUrl: `/api/radio/audio/live/stop`
      };
    }

    function formatNoaaDiagnosticFrequency(frequencyHz) {
      return `${(Number(frequencyHz) / 1000000).toFixed(3)} MHz`;
    }

    async function startNoaaDiagnosticAudio(button, statusNode) {
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      const sampleRate = 24000;
      const targetChunkBytes = 4096 * 2;
      const frequencyHz = noaaDiagnosticFrequencyHz();
      const audioUrls = noaaDiagnosticAudioUrls(frequencyHz);
      const sessionKey = `noaa:${frequencyHz}`;

      if (!AudioCtx) {
        throw new Error("Web Audio is not available in this browser.");
      }

      await stopAnalogAudio("Audio diagnostic stopped.");

      if (statusNode?.isConnected) {
        statusNode.textContent = `Tuning NOAA ${formatNoaaDiagnosticFrequency(frequencyHz)}...`;
      }

      try {
        await getJson(`/api/radio/plan?${new URLSearchParams({
          name: "NOAA Audio Diagnostic",
          downlink: String(frequencyHz),
          mode: "NFM",
          description: "NOAA Weather Radio audio path diagnostic"
        }).toString()}`);
      } catch (_error) {
        // Continue: live/start can tune directly by downlink_hz.
      }

      await postJson(audioUrls.startUrl, {});

      const context = new AudioCtx({ sampleRate });
      await context.resume();

      const session = {
        button,
        context,
        controller: null,
        nextTime: context.currentTime + 0.35,
        passKey: sessionKey,
        reconnectCount: 0,
        reconnectTimer: 0,
        sources: [],
        statusNode,
        stopUrl: audioUrls.stopUrl,
        stopped: false,
        totalBytes: 0,
        totalBuffers: 0
      };
      analogAudioSession = session;

      button.textContent = "Stop";
      if (statusNode?.isConnected) {
        statusNode.textContent = `Opening NOAA ${formatNoaaDiagnosticFrequency(frequencyHz)} backend audio stream...`;
      }

      const schedulePcmBytes = (pcmBytes) => {
        if (session.stopped || !pcmBytes.length) return;
        const evenLength = pcmBytes.length - (pcmBytes.length % 2);
        if (evenLength <= 0) return;

        const audioBuffer = pcm16ToAudioBuffer(context, pcmBytes.slice(0, evenLength), sampleRate);
        const source = context.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(context.destination);
        source.onended = () => {
          const index = session.sources.indexOf(source);
          if (index >= 0) session.sources.splice(index, 1);
        };

        const startAt = Math.max(context.currentTime + 0.05, session.nextTime);
        source.start(startAt);
        session.nextTime = startAt + audioBuffer.duration;
        session.sources.push(source);
        session.totalBuffers += 1;

        const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
        if (statusNode?.isConnected) {
          statusNode.textContent = `Playing NOAA ${formatNoaaDiagnosticFrequency(frequencyHz)} (${bufferedSeconds.toFixed(1)}s buffered, reconnects ${session.reconnectCount}).`;
        }
      };

      const readOneStreamSegment = async () => {
        const controller = new AbortController();
        session.controller = controller;
        const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=${session.reconnectCount}`;
        const response = await fetch(streamUrl, {
          cache: "no-store",
          signal: controller.signal
        });

        if (!response.ok) {
          throw new Error(`${streamUrl}: ${response.status}`);
        }
        if (!response.body || !response.body.getReader) {
          throw new Error("Browser streaming fetch is not available.");
        }

        const reader = response.body.getReader();
        let pending = new Uint8Array();
        let headerSkipped = false;
        let segmentBytes = 0;

        const processPending = (force = false) => {
          if (!headerSkipped) {
            if (pending.length < 44) return;
            pending = pending.slice(44);
            headerSkipped = true;
          }

          while (pending.length >= targetChunkBytes || (force && pending.length >= 2)) {
            const take = force ? pending.length - (pending.length % 2) : targetChunkBytes;
            const chunk = pending.slice(0, take);
            pending = pending.slice(take);
            schedulePcmBytes(chunk);
          }
        };

        while (!session.stopped) {
          const { value, done } = await reader.read();
          if (done) break;
          if (!value || !value.length) continue;

          segmentBytes += value.length;
          session.totalBytes += value.length;
          pending = concatUint8Arrays(pending, value);
          processPending(false);

          if (statusNode?.isConnected && session.totalBuffers === 0) {
            statusNode.textContent = `Receiving NOAA diagnostic stream (${session.totalBytes} bytes)...`;
          }
        }

        processPending(true);
        return segmentBytes;
      };

      const reconnectLoop = async () => {
        while (!session.stopped && analogAudioSession === session) {
          try {
            const segmentBytes = await readOneStreamSegment();
            if (session.stopped || analogAudioSession !== session) return;

            session.reconnectCount += 1;
            const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
            if (statusNode?.isConnected) {
              statusNode.textContent =
                `NOAA stream segment ended after ${segmentBytes} bytes; reconnecting (${bufferedSeconds.toFixed(1)}s buffered).`;
            }

            const delayMs = bufferedSeconds > 0.8 ? 200 : 20;
            await new Promise((resolve) => {
              session.reconnectTimer = window.setTimeout(resolve, delayMs);
            });
            session.reconnectTimer = 0;
          } catch (error) {
            if (session.stopped || analogAudioSession !== session) return;
            const message = error && error.message ? error.message : "NOAA audio diagnostic failed.";
            if (statusNode?.isConnected) statusNode.textContent = message;
            const statusBar = document.getElementById("status");
            if (statusBar) statusBar.textContent = message;
            await stopAnalogAudio(message);
            return;
          }
        }
      };

      reconnectLoop();
    }

    function bindNoaaAudioDiagnostic() {
      const button = document.getElementById("noaaDiagnosticListenButton");
      const statusNode = document.getElementById("noaaDiagnosticStatus");
      const select = document.getElementById("noaaDiagnosticFrequencySelect");
      if (!button || !statusNode || !select) return;

      button.addEventListener("click", async () => {
        const sessionKey = `noaa:${noaaDiagnosticFrequencyHz()}`;
        if (analogAudioSession && analogAudioSession.passKey === sessionKey) {
          await stopAnalogAudio("Audio diagnostic stopped.");
          return;
        }

        button.disabled = true;
        try {
          statusNode.textContent = "Starting NOAA audio diagnostic...";
          await startNoaaDiagnosticAudio(button, statusNode);
        } catch (error) {
          await stopAnalogAudio();
          button.textContent = "Listen";
          statusNode.textContent = error.message || "Unable to start NOAA audio diagnostic.";
          const statusBar = document.getElementById("status");
          if (statusBar) statusBar.textContent = statusNode.textContent;
        } finally {
          button.disabled = false;
        }
      });

      select.addEventListener("change", async () => {
        if (analogAudioSession && String(analogAudioSession.passKey || "").startsWith("noaa:")) {
          await stopAnalogAudio(`Selected NOAA ${formatNoaaDiagnosticFrequency(noaaDiagnosticFrequencyHz())}. Press Listen to test.`);
        } else {
          statusNode.textContent = `Selected NOAA ${formatNoaaDiagnosticFrequency(noaaDiagnosticFrequencyHz())}. Press Listen to test.`;
        }
      });
    }

function bindAnalogAudio(pass, node) {
      const button = node.querySelector("#analogAudioToggleButton");
      const status = node.querySelector("#analogAudioStatus");
      const audioUrls = analogAudioUrl(pass);
      if (!button || !status) return;

      button.disabled = !audioUrls;
      if (!audioUrls) {
        status.textContent = "No downlink is available for this pass.";
        return;
      }

      const setIdle = (message) => {
        button.textContent = "Listen";
        status.textContent = message;
      };

      if (analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
        analogAudioSession.button = button;
        analogAudioSession.statusNode = status;
        button.textContent = "Stop";
        status.textContent = "Streaming live analog FM audio from Pluto...";
      } else {
        setIdle("Ready to listen to this pass.");
      }

      button.addEventListener("click", async () => {
        if (analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
          await stopAnalogAudio("Analog monitor stopped.");
          return;
        }

        try {
          status.textContent = "Connecting to Pluto FM audio stream...";
          await startAnalogAudio(pass, button, status);
        } catch (error) {
          await stopAnalogAudio();
          setIdle(error.message || "Unable to start analog audio.");
          document.getElementById("status").textContent = error.message || "Unable to start analog audio.";
        }
      });
    }


        /* PASS_DETAIL_MODAL_LISTEN_V1B */

    /* PASS_DETAIL_MODAL_LISTEN_V1B */
    function openPassDetailModal(pass) {
      renderPassDetail(pass);
      const modal = document.getElementById("passDetailModal");
      if (modal) modal.hidden = false;
    }

    function closePassDetailModal() {
      const modal = document.getElementById("passDetailModal");
      if (modal) modal.hidden = true;
    }

    function renderPassDetail(pass) {
      const node = document.getElementById("passDetail");
      /* NOAA_AUDIO_DIAGNOSTIC_KEEP_RUNNING_V6

       * Do not let normal pass-detail refreshes stop the fixed-frequency

       * NOAA audio diagnostic. NOAA uses a noaa:<hz> session key and

       * should run until the user clicks Stop. Satellite Listen sessions

       * still stop when the selected pass changes.

       */

      if (analogAudioSession &&

          !String(analogAudioSession.passKey || "").startsWith("noaa:") &&

          analogAudioSession.passKey !== passKey(pass)) {

        stopAnalogAudio();

      }

      currentSelectedPass = pass || null;
      currentSelectedPassKey = passKey(pass);
      currentMapFocusTime = "";
      renderMapPanel(currentSelectedPass, currentObserverConfig);

      if (!node) return;
      if (!pass) {
        node.className = "pass-detail-modal-body empty";
        node.textContent = "Select Details from Next Passes.";
        return;
      }

      const radio = pass.radio || {};
      const uplink = radio.uplink_hz ? formatHz(radio.uplink_hz) : "-";
      const downlink = radio.downlink_hz ? formatHz(radio.downlink_hz) : "-";
      const tunable = isPassTunable(pass) ? "Yes" : "No";
      const description = radio.description || "-";
      const readiness = passReadiness(pass);

      node.className = "pass-detail-modal-body pass-detail-compact";
      node.innerHTML = `
        <div class="detail-title"></div>
        <div class="detail-subtitle"></div>
        <div class="detail-grid">
          <div>
            <h3>Timing</h3>
            <dl>
              <dt>AOS</dt><dd></dd>
              <dt>TCA</dt><dd></dd>
              <dt>LOS</dt><dd></dd>
              <dt>Duration</dt><dd></dd>
            </dl>
          </div>
          <div>
            <h3>Geometry</h3>
            <dl>
              <dt>Max el</dt><dd></dd>
              <dt>AOS az</dt><dd></dd>
              <dt>LOS az</dt><dd></dd>
              <dt>TCA range</dt><dd></dd>
            </dl>
          </div>
          <div class="full-width">
            <h3>Radio</h3>
            <dl>
              <dt>Downlink</dt><dd></dd>
              <dt>Uplink</dt><dd></dd>
              <dt>Mode</dt><dd></dd>
              <dt>Type</dt><dd></dd>
              <dt>Status</dt><dd></dd>
              <dt>Tunable</dt><dd></dd>
              <dt>Pass state</dt><dd></dd>
              <dt>Description</dt><dd></dd>
            </dl>
          </div>
          <div class="full-width">
            <h3>Doppler Preview</h3>
            <div id="dopplerPreview"></div>
          </div>
        </div>
      `;

      node.querySelector(".detail-title").textContent = pass.name || "";
      node.querySelector(".detail-subtitle").textContent = `NORAD ${pass.norad_id || ""}`;
      const dds = node.querySelectorAll("dd");
      const values = [
        formatDateTime(pass.aos_utc),
        formatDateTime(pass.tca_utc),
        formatDateTime(pass.los_utc),
        formatDuration(pass.duration_s),
        `${pass.max_elevation_deg} deg`,
        `${pass.aos_azimuth_deg} deg`,
        `${pass.los_azimuth_deg} deg`,
        `${pass.range_at_tca_km} km`,
        downlink,
        uplink,
        radio.mode || "-",
        radio.type || "-",
        radio.status || "-",
        tunable,
        readiness.label,
        description
      ];
      values.forEach((value, index) => {
        if (dds[index]) dds[index].textContent = value;
      });

      const preview = node.querySelector("#dopplerPreview");
      if (preview) preview.innerHTML = renderDopplerRows(pass.doppler_plan);
    }
    /* PASS_LOADING_WATCH_V2 */
    function passPayloadNeedsQuickLoadV2(payload) {
      const passes = (payload && payload.passes) || [];
      if (!passes.length) return true;
      const startMs = Date.parse((payload && payload.start_utc) || "");
      if (!Number.isFinite(startMs)) return true;
      return Math.abs(Date.now() - startMs) > (15 * 60 * 1000);
    }

    function showPassLoadingFeedbackV2(title, detail) {
      window.__plutoPassLoadingWatchV2 = true;
      const passesNode = document.getElementById("passes");
      if (!passesNode) return;
      passesNode.className = "pass-loading";
      passesNode.innerHTML = `
        <div class="pass-loading-spinner" aria-hidden="true"></div>
        <div class="pass-loading-text">
          <div class="pass-loading-title">${escapeHtml(title || "Loading passes...")}</div>
          <div class="pass-loading-detail">${escapeHtml(detail || "Watching Pluto for the quick pass preview.")}</div>
        </div>
      `;
    }

    function shouldKeepPassLoadingSpinnerV2(passesPayload, refreshStatus) {
      if (!window.__plutoPassLoadingWatchV2) return false;
      const target = (refreshStatus && refreshStatus.target) || "";
      const state = (refreshStatus && refreshStatus.state) || "";
      const running = target === "passes" && !["", "idle", "ok", "failed"].includes(state);
      return running && passPayloadNeedsQuickLoadV2(passesPayload);
    }


    /* PASS_LOADING_FILE_WATCH_V3 */
    function passPayloadTimestampV3(payload) {
      return String((payload && payload.generated_utc) || "");
    }

    function passPayloadStartMsV3(payload) {
      const startMs = Date.parse((payload && payload.start_utc) || "");
      return Number.isFinite(startMs) ? startMs : 0;
    }

    function rememberPassPayloadStampV3(payload) {
      const generated = passPayloadTimestampV3(payload);
      const startUtc = String((payload && payload.start_utc) || "");
      if (generated) window.__plutoLastPassGeneratedUtcV3 = generated;
      if (startUtc) window.__plutoLastPassStartUtcV3 = startUtc;
    }

    function startPassFileWatchV3(title, detail) {
      const existing = window.__plutoPassFileWatchV3 || {};
      window.__plutoPassFileWatchV3 = {
        active: true,
        startedAt: Date.now(),
        previousGeneratedUtc: existing.previousGeneratedUtc || window.__plutoLastPassGeneratedUtcV3 || "",
        previousStartUtc: existing.previousStartUtc || window.__plutoLastPassStartUtcV3 || ""
      };
      if (typeof showPassLoadingFeedbackV2 === "function") {
        showPassLoadingFeedbackV2(title, detail);
      } else if (typeof showPassLoadingFeedbackV1 === "function") {
        showPassLoadingFeedbackV1(title, detail);
      } else {
        const passesNode = document.getElementById("passes");
        if (passesNode) {
          passesNode.className = "empty";
          passesNode.textContent = title || "Loading passes...";
        }
      }
    }

    function passPayloadLooksCurrentForBrowserV3(payload) {
      const passes = (payload && payload.passes) || [];
      if (!passes.length) return false;
      const startMs = passPayloadStartMsV3(payload);
      if (!startMs) return false;
      return Math.abs(Date.now() - startMs) <= (15 * 60 * 1000);
    }

    function passPayloadLooksNewEnoughV3(payload) {
      if (!passPayloadLooksCurrentForBrowserV3(payload)) return false;
      const watch = window.__plutoPassFileWatchV3 || {};
      const generated = passPayloadTimestampV3(payload);
      if (!watch.active) return true;

      /*
       * During a manual or startup refresh, do not render the old pass list
       * simply because it has some rows. Wait for generated_utc to change.
       * If no previous stamp is known, current-enough rows are acceptable.
       */
      if (watch.previousGeneratedUtc && generated && generated === watch.previousGeneratedUtc) {
        return false;
      }
      return true;
    }

    function shouldKeepPassFileSpinnerV3(passesPayload) {
      const watch = window.__plutoPassFileWatchV3 || {};
      if (!watch.active) return false;
      if (passPayloadLooksNewEnoughV3(passesPayload)) {
        window.__plutoPassFileWatchV3.active = false;
        rememberPassPayloadStampV3(passesPayload);
        return false;
      }

      /*
       * Keep watching for up to 2 minutes. If the backend never publishes a new
       * file, let the normal error/empty state appear instead of spinning forever.
       */
      if (Date.now() - Number(watch.startedAt || 0) > (2 * 60 * 1000)) {
        window.__plutoPassFileWatchV3.active = false;
        return false;
      }
      return true;
    }


        /* PASS_DETAIL_MODAL_LISTEN_V1B */
    function renderPasses(payload) {
      const node = document.getElementById("passes");
      const passes = payload.passes || [];
      if (!passes.length) {
        node.className = "empty";
        node.textContent = payload.message || "No upcoming passes in the prediction window.";
        renderPassDetail(null);
        return;
      }

      node.className = "pass-list";
      node.innerHTML = "";
      const filteredPasses = passes.filter((pass) => {
        const readiness = passReadiness(pass);
        if (passFilter === "all") return true;
        if (passFilter === "ready") return readiness.actionable;
        if (passFilter === "active") return readiness.state === "active";
        return readiness.state !== "stale";
      });
      const visiblePasses = filteredPasses.slice(0, 12);
      const filters = document.createElement("div");
      filters.className = "filter-row";
      for (const [value, label] of [["actionable", "Upcoming"], ["ready", "Ready"], ["active", "Active"], ["all", "All"]]) {
        const filterButton = document.createElement("button");
        filterButton.type = "button";
        filterButton.textContent = label;
        filterButton.className = passFilter === value ? "active" : "";
        filterButton.addEventListener("click", () => {
          passFilter = value;
          renderPasses(payload);
        });
        filters.appendChild(filterButton);
      }
      node.appendChild(filters);
      if (!visiblePasses.length) {
        const empty = document.createElement("div");
        empty.className = "empty";
        empty.textContent = "No passes match the current filter.";
        node.appendChild(empty);
        renderPassDetail(null);
        return;
      }

      const foundIndex = visiblePasses.findIndex((pass) => passKey(pass) === currentSelectedPassKey);
      const selectedIndex = foundIndex >= 0 ? foundIndex : 0;

      const selectPass = (pass, row) => {
        node.querySelectorAll(".pass-row").forEach((item) => item.classList.remove("selected"));
        if (row) row.classList.add("selected");
        renderPassDetail(pass);
      };

      for (const [index, pass] of visiblePasses.entries()) {
        const row = document.createElement("div");
        const readiness = passReadiness(pass);
        row.className = index === selectedIndex ? "pass-row selected" : "pass-row";
        row.tabIndex = 0;
        const downlink = (pass.downlinks_hz || [])[0];
        row.innerHTML = `
          <div><strong></strong><span></span></div>
          <div></div>
          <div></div>
          <div><button class="pass-detail-button" type="button">Details</button></div>
        `;
        row.children[0].querySelector("strong").textContent = pass.name || "";
        row.children[0].querySelector("span").textContent =
          downlink ? `${formatHz(downlink)} ${((pass.modes || [])[0] || "")}` : ((pass.modes || [])[0] || "");
        row.children[1].textContent =
          `${formatTime(pass.aos_utc)}-${formatTime(pass.los_utc)} (${formatDuration(pass.duration_s)})`;
        row.children[2].className = isPassTunable(pass) ? "radio-ok" : "radio-warn";
        row.children[2].textContent = `${readiness.label}: ${renderRadioTarget(pass)}`;
        row.title = `Max ${pass.max_elevation_deg} deg, az ${pass.aos_azimuth_deg}-${pass.los_azimuth_deg}, ${readiness.label}`;

        const detailButton = row.querySelector(".pass-detail-button");
        detailButton.addEventListener("click", (event) => {
          event.preventDefault();
          event.stopPropagation();
          selectPass(pass, row);
          openPassDetailModal(pass);
        });

        row.addEventListener("click", () => {
          selectPass(pass, row);
        });
        row.addEventListener("keydown", (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            selectPass(pass, row);
          }
        });
        node.appendChild(row);
      }
      renderPassDetail(visiblePasses[selectedIndex] || visiblePasses[0]);
    }
    async function refresh() {
      const [status, config, catalog, passes, radioStatus, radioHardware, radioTrack, radioTrackState, timeStatus, refreshStatus] = await Promise.all([
        getJson("/api/status"),
        getJson("/api/config"),
        getJson("/api/satellites"),
        getJson("/api/passes"),
        getJson("/api/radio/status"),
        getJson("/api/radio/hardware"),
        getJson("/api/radio/track/status"),
        getJson("/api/radio/track/state"),
        getJson("/api/time/status"),
        getJson("/api/refresh/status")
      ]);
      lastRadioHardware = radioHardware;
      lastTimeStatus = timeStatus;
      lastTrackState = radioTrackState;
      currentObserverConfig = config;

      setPillState("status", status.ok ? "Backend online" : "Backend warning", status.ok ? "running" : "stopped");
      setPillState(
        "timePill",
        (timeStatus.state || "unknown") === "synced" ? "Time synced" : "Time unsynced",
        (timeStatus.state || "unknown") === "synced" ? "running" : "pending"
      );
      setPillState(
        "trackPill",
        radioTrackState.state && radioTrackState.state !== "idle" ? `Track ${radioTrackState.state}` : "Track idle",
        radioTrackState.state && radioTrackState.state !== "idle" ? "running" : "pending"
      );

      setDl("observer", [
        ["Name", config.name],
        ["Latitude", `${config.latitude_deg} deg`],
        ["Longitude", `${config.longitude_deg} deg`],
        ["Altitude", `${config.altitude_m} m`],
        ["Minimum elevation", `${config.minimum_elevation_deg} deg`]
      ]);
      if (pendingObserverDraft) {
        applyObserverInputs(pendingObserverDraft);
      } else {
        applyObserverInputs({
          name: config.name || "",
          latitude_deg: config.latitude_deg ?? "",
          longitude_deg: config.longitude_deg ?? "",
          altitude_m: config.altitude_m ?? 0,
          minimum_elevation_deg: config.minimum_elevation_deg ?? 10
        });
      }
      renderMapPanel(currentSelectedPass, currentObserverConfig);
setDl("storage", [
        ["Web", status.web_dir],
        ["Config", status.config_dir],
        ["Data", status.data_dir]
      ]);

      setDl("timeStatus", [
        ["Pluto UTC", formatEpoch(status.time_epoch)],
        ["Browser UTC", new Date().toISOString().replace(".000Z", "Z")],
        ["Sync state", timeStatus.state || "unknown"],
        ["Last synced", timeStatus.synced_epoch ? formatEpoch(timeStatus.synced_epoch) : "-"]
      ]);
setDl("radioStatus", [
        ["State", radioStatus.state || "idle"],
        ["Target", radioStatus.name || "-"],
        ["Downlink", radioStatus.downlink_hz ? formatHz(radioStatus.downlink_hz) : "-"],
        ["Mode", radioStatus.mode || "-"],
        ["LO path", radioStatus.lo_path || "-"],
        ["Track", radioTrack.state && radioTrack.state !== "idle" ? (radioTrack.name || radioTrack.state) : "-"],
        ["Track points", radioTrack.point_count || "-"],
        ["Tracking", radioTrackState.state || "-"],
        ["Track RX", radioTrackState.rx_hz ? formatHz(radioTrackState.rx_hz) : "-"],
        ["Track point", radioTrackState.point_time_utc ? formatTime(radioTrackState.point_time_utc) : "-"],
        ["Point in", radioTrackState.seconds_until_point > 0 ? formatCountdown(radioTrackState.seconds_until_point) : "-"],
        ["AOS in", radioTrackState.seconds_until_aos > 0 ? formatCountdown(radioTrackState.seconds_until_aos) : "-"],
        ["LOS in", radioTrackState.seconds_until_los > 0 ? formatCountdown(radioTrackState.seconds_until_los) : "-"],
        ["LOS ago", radioTrackState.seconds_since_los > 0 ? formatCountdown(radioTrackState.seconds_since_los) : "-"],
        ["LO write", radioTrackState.lo_write_result || "-"],
        ["Track message", radioTrackState.message || "-"],
        ...radioHardwareEntries(radioHardware)
      ]);
const refreshEntries = [
        ["State", refreshStatus.state || "idle"],
        ["Target", refreshStatus.target || "-"],
        ["Updated", refreshStatus.updated_utc || "-"],
        ["Message", refreshStatus.message || "-"]
      ];
      appendRefreshSummary(refreshEntries, refreshStatus);
      setDl("refreshStatus", refreshEntries);

      if (shouldKeepPassFileSpinnerV3(passes)) {
        const refreshState = (refreshStatus && refreshStatus.state) || "waiting";
        startPassFileWatchV3(
          "Loading quick pass preview...",
          `Watching /api/passes for a new pass file. Refresh state: ${refreshState}.`
        );
      } else {
        rememberPassPayloadStampV3(passes);
        renderPasses(passes);
      }
      scheduleRefreshLoop();
const tbody = document.getElementById("satellites");
      const satellites = catalog.satellites || [];
      const meta = catalog.metadata || {};
      document.getElementById("catalogMeta").textContent =
        `${meta.satellite_count ?? satellites.length} satellites, ` +
        `${meta.satellites_with_transmitters ?? 0} with transmitter metadata` +
        (catalog.updated_utc ? `, updated ${catalog.updated_utc}` : "");

      if (!satellites.length) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty">Catalog is ready for TLE and repeater metadata import.</td></tr>';
        return;
      }

      tbody.innerHTML = "";
      for (const sat of satellites) {
        const tr = document.createElement("tr");
        const downlinks = summarizeDownlinks(sat);
        tr.innerHTML = `<td></td><td></td><td class="freq"></td><td></td>`;
        tr.children[0].textContent = sat.name || "";
        tr.children[1].textContent = sat.norad_id || "";
        tr.children[2].textContent = downlinks.length ? downlinks.join(", ") : "-";
        tr.children[3].textContent = (sat.modes || []).join(", ") || "-";
        tbody.appendChild(tr);
      }
    }

    document.getElementById("syncTimeButton").addEventListener("click", (event) => {
      syncPlutoTime(event.currentTarget).catch((error) => {
        document.getElementById("status").textContent = error.message;
      });
    });

    document.getElementById("refreshPassesButton").addEventListener("click", (event) => {
      runDataRefresh("passes", event.currentTarget).catch((error) => {
        document.getElementById("status").textContent = error.message;
      });
    });

    document.getElementById("refreshCatalogButton").addEventListener("click", (event) => {
      runDataRefresh("catalog", event.currentTarget).catch((error) => {
        document.getElementById("status").textContent = error.message;
      });
    });

    document.getElementById("saveObserverButton").addEventListener("click", (event) => {
      saveObserverConfig(event.currentTarget).catch((error) => {
        document.getElementById("status").textContent = error.message;
      });
    });


    const noaaDiagnosticListenButton = document.getElementById("noaaDiagnosticListenButton");
    if (noaaDiagnosticListenButton) {
      bindNoaaAudioDiagnostic();
    }

    document.getElementById("menuToggleButton").addEventListener("click", () => {
      toggleDrawer();
    });

    document.getElementById("menuCloseButton").addEventListener("click", () => {
      closeDrawer();
    });

    document.getElementById("drawerBackdrop").addEventListener("click", () => {
      closeDrawer();
    });

    const passDetailCloseButton = document.getElementById("passDetailCloseButton");
    if (passDetailCloseButton) {
      passDetailCloseButton.addEventListener("click", () => closePassDetailModal());
    }

    const passDetailModal = document.getElementById("passDetailModal");
    if (passDetailModal) {
      passDetailModal.addEventListener("click", (event) => {
        if (event.target === passDetailModal) closePassDetailModal();
      });
    }

    for (const id of [
      "observerNameInput",
      "observerLatitudeInput",
      "observerLongitudeInput",
      "observerAltitudeInput",
      "observerMinElevationInput"
    ]) {
      const input = document.getElementById(id);
      input.addEventListener("input", () => {
        markObserverEditing();
        updatePendingObserverDraft();
      });
      input.addEventListener("focus", () => {
        observerEditingActive = true;
        markObserverEditing(15000);
      });
      input.addEventListener("blur", () => {
        observerEditingActive = false;
        markObserverEditing(1500);
        scheduleRefreshLoop();
      });
    }

    document.addEventListener("visibilitychange", () => {
      scheduleRefreshLoop();
    });

    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        closePassDetailModal();
        closeDrawer();
      }
    });

    (async () => {
      try {
        await autoSyncTimeAndPasses(true);
      } catch (error) {
        const statusNode = document.getElementById("status");
        if (statusNode) {
          statusNode.textContent = `Auto time sync failed: ${error.message}`;
        }
      }

      try {
        await refresh();
      } catch (error) {
        setPillState("status", "Backend unavailable", "stopped");
        document.getElementById("passes").textContent = error.message;
      }
    })();

    // UI_BROWSER_TIME_QUICK_FULL_REFRESH_V3_APPEND
    // Browser-owned startup recovery for standalone Pluto operation.
    // The browser is the time source: sync Pluto UTC, publish a quick pass
    // preview, and let the full 24-hour pass rebuild complete in the background.
    (function installBrowserTimeQuickFullRefreshV3() {
      if (window.__plutoBrowserTimeQuickFullRefreshV3Installed) return;
      window.__plutoBrowserTimeQuickFullRefreshV3Installed = true;

      let browserRefreshPollTimerId = 0;
      let browserBootstrapStarted = false;
      let originalPostJson = null;

      function browserEpochSeconds() {
        return Math.floor(Date.now() / 1000);
      }

      function isoEpochSeconds(value) {
        const millis = Date.parse(value || "");
        return Number.isFinite(millis) ? Math.floor(millis / 1000) : 0;
      }

      function passPayloadNeedsBrowserRefresh(payload) {
        const passes = (payload && payload.passes) || [];
        const startEpoch = isoEpochSeconds(payload && payload.start_utc);
        const nowEpoch = browserEpochSeconds();
        if (!passes.length || !startEpoch) return true;
        return Math.abs(nowEpoch - startEpoch) > 15 * 60;
      }

      function setBrowserStatus(text, state) {
        try {
          if (typeof setPillState === "function") {
            setPillState("status", text, state || "pending");
          } else {
            const status = document.getElementById("status");
            if (status) status.textContent = text;
          }
        } catch (_error) {
        }
      }


      /* PASS_LOADING_SPINNER_V1 */
      function showPassLoadingFeedbackV2(title, detail) {
        const passesNode = document.getElementById("passes");
        if (!passesNode) return;
        passesNode.className = "pass-loading";
        passesNode.innerHTML = `
          <div class="pass-loading-spinner" aria-hidden="true"></div>
          <div class="pass-loading-text">
            <div class="pass-loading-title">${escapeHtml(title || "Loading passes...")}</div>
            <div class="pass-loading-detail">${escapeHtml(detail || "The quick preview will appear first; the full 24-hour rebuild continues in the background.")}</div>
          </div>
        `;
      }

      async function syncPlutoTimeFromBrowserV3() {
        const result = await getJson(`/api/time/sync?epoch=${browserEpochSeconds()}`);
        try {
          if (typeof setPillState === "function") setPillState("timePill", "Time synced", "running");
        } catch (_error) {
        }
        return result;
      }

      function scheduleFullRefreshPollV3() {
        if (browserRefreshPollTimerId) {
          window.clearInterval(browserRefreshPollTimerId);
        }
        const startedAt = Date.now();
        browserRefreshPollTimerId = window.setInterval(async () => {
          try {
            await refresh();
            const passesPayload = await getJson("/api/passes");
            const refreshStatus = await getJson("/api/refresh/status");
            const hours = Number((passesPayload && passesPayload.hours) ||
              (refreshStatus && refreshStatus.summary && refreshStatus.summary.prediction_hours) || 0);
            const count = Number((passesPayload && passesPayload.metadata && passesPayload.metadata.pass_count) ||
              ((passesPayload && passesPayload.passes) || []).length || 0);
            if ((refreshStatus.state || "") === "ok" && hours >= 23 && count >= 20 && !passPayloadNeedsBrowserRefresh(passesPayload)) {
              window.clearInterval(browserRefreshPollTimerId);
              browserRefreshPollTimerId = 0;
              setBrowserStatus("Backend online", "running");
            }
            if (Date.now() - startedAt > 15 * 60 * 1000) {
              window.clearInterval(browserRefreshPollTimerId);
              browserRefreshPollTimerId = 0;
            }
          } catch (_error) {
          }
        }, 6000);
      }

      async function triggerBrowserOwnedPassRefreshV3() {
        await syncPlutoTimeFromBrowserV3();
        const poster = originalPostJson || postJson;
        startPassFileWatchV3("Requesting quick pass preview...", "Waiting for Pluto to publish the first pass results.");
        const result = await poster("/api/refresh/passes", {});
        scheduleFullRefreshPollV3();
        return result;
      }

      async function bootstrapBrowserTimeRefreshV3() {
        if (browserBootstrapStarted) return;
        browserBootstrapStarted = true;
        try {
          setBrowserStatus("Syncing browser time...", "pending");
          startPassFileWatchV3("Syncing Pluto time...", "Using browser time before requesting pass predictions.");
          await syncPlutoTimeFromBrowserV3();
          await refresh();
          const passesPayload = await getJson("/api/passes");
          if (passPayloadNeedsBrowserRefresh(passesPayload)) {
            setBrowserStatus("Loading quick pass preview...", "pending");
            startPassFileWatchV3("Loading quick pass preview...", "A short pass list will appear first; the full 24-hour pass rebuild continues after that.");
            await triggerBrowserOwnedPassRefreshV3();
            await refresh();
          }
        } catch (error) {
          setBrowserStatus("Startup refresh failed", "stopped");
          const passesNode = document.getElementById("passes");
          if (passesNode) {
          passesNode.className = "empty";
          passesNode.textContent = error.message || String(error);
        }
        } finally {
          browserBootstrapStarted = false;
        }
      }

      try {
        originalPostJson = postJson;
        postJson = async function browserTimePostJsonWrapperV3(url, payload) {
          if (url === "/api/refresh/passes" || url === "/api/refresh/all") {
            await syncPlutoTimeFromBrowserV3();
            const result = await originalPostJson(url, payload);
            scheduleFullRefreshPollV3();
            return result;
          }
          return originalPostJson(url, payload);
        };
      } catch (_error) {
      }

      try {
        syncPlutoTime = async function syncPlutoTime(button) {
          button.disabled = true;
          button.textContent = "Syncing...";
          try {
            await syncPlutoTimeFromBrowserV3();
            button.textContent = "Synced";
            await refresh();
          } catch (error) {
            button.textContent = "Sync failed";
            throw error;
          } finally {
            setTimeout(() => {
              button.disabled = false;
              button.textContent = "Sync Time";
            }, 1200);
          }
        };
      } catch (_error) {
      }

      try {
        const previousRunDataRefresh = runDataRefresh;
        runDataRefresh = async function runDataRefresh(target, button) {
          if (target !== "passes" && target !== "all") {
            return previousRunDataRefresh(target, button);
          }
          const originalText = button.textContent;
          button.disabled = true;
          button.textContent = "Syncing time...";
          try {
            await syncPlutoTimeFromBrowserV3();
            button.textContent = "Loading preview...";
            startPassFileWatchV3("Regenerating pass preview...", "The quick pass list will update first; full results continue in the background.");
            await (originalPostJson || postJson)(`/api/refresh/${target}`, {});
            scheduleFullRefreshPollV3();
            button.textContent = "Done";
            await refresh();
          } catch (error) {
            button.textContent = "Failed";
            await refresh().catch(() => {});
            throw error;
          } finally {
            setTimeout(() => {
              button.disabled = false;
              button.textContent = originalText;
            }, 1200);
          }
        };
      } catch (_error) {
      }

      window.plutoBrowserTimeQuickFullRefreshV3 = {
        syncTime: syncPlutoTimeFromBrowserV3,
        triggerPassRefresh: triggerBrowserOwnedPassRefreshV3,
        bootstrap: bootstrapBrowserTimeRefreshV3,
        scheduleFullPoll: scheduleFullRefreshPollV3
      };

      window.setTimeout(() => {
        bootstrapBrowserTimeRefreshV3();
      }, 300);
    })();


/* ROTATOR_UI_CONTROLS_V2_4_0C */
(function () {
  "use strict";

  const ROTATOR_TYPES = [
    ["simulation", "Simulation"],
    ["hamlib_rotctld", "Hamlib rotctld TCP"],
    ["satran", "SATRAN MK2/MK3"],
    ["easycomm2", "EasyComm II"],
    ["yaesu_gs232", "Yaesu GS-232"]
  ];

  function rotatorApi(path, options) {
    return fetch(path, Object.assign({ cache: "no-store" }, options || {})).then(async (response) => {
      const text = await response.text();
      let data = {};
      try {
        data = text ? JSON.parse(text) : {};
      } catch (err) {
        data = { ok: false, error: text || String(err) };
      }
      data.http_status = response.status;
      data.http_ok = response.ok;
      if (!response.ok && !data.error) {
        data.error = response.status + " " + response.statusText;
      }
      return data;
    });
  }

  function numberValue(id, fallback) {
    const el = document.getElementById(id);
    if (!el) return fallback;
    const n = Number(el.value);
    return Number.isFinite(n) ? n : fallback;
  }

  function textValue(id, fallback) {
    const el = document.getElementById(id);
    if (!el) return fallback;
    return el.value || fallback;
  }

  function boolValue(id) {
    const el = document.getElementById(id);
    return !!(el && el.checked);
  }

  function setValue(id, value) {
    const el = document.getElementById(id);
    if (!el || value === undefined || value === null) return;
    if (el.type === "checkbox") {
      el.checked = !!value;
    } else {
      el.value = value;
    }
  }

  function setRotatorStatus(message, data, isError) {
    const status = document.getElementById("rotatorStatusText");
    if (!status) return;

    const prefix = message ? message + "\n" : "";
    const body = data ? JSON.stringify(data, null, 2) : "";
    status.textContent = prefix + body;
    status.classList.toggle("rotator-status-error", !!isError);
  }

  function rotatorConfigFromForm() {
    return {
      enabled: boolValue("rotatorEnabled"),
      type: textValue("rotatorType", "simulation"),
      host: textValue("rotatorHost", "127.0.0.1"),
      port: numberValue("rotatorPort", 4533),
      update_interval_sec: numberValue("rotatorUpdateInterval", 2),
      min_move_deg: numberValue("rotatorMinMove", 1.0),
      az_offset_deg: numberValue("rotatorAzOffset", 0.0),
      el_offset_deg: numberValue("rotatorElOffset", 0.0),
      min_el_deg: numberValue("rotatorMinEl", 0.0),
      max_el_deg: numberValue("rotatorMaxEl", 90.0),
      park_on_los: boolValue("rotatorParkOnLos"),
      park_az_deg: numberValue("rotatorParkAz", 0.0),
      park_el_deg: numberValue("rotatorParkEl", 0.0)
    };
  }

  function fillRotatorForm(cfg) {
    if (!cfg) return;

    setValue("rotatorEnabled", !!cfg.enabled);
    setValue("rotatorType", cfg.type || "simulation");

    const connection = cfg.connection || {};
    setValue("rotatorHost", cfg.host || connection.host || "127.0.0.1");
    setValue("rotatorPort", cfg.port || connection.port || 4533);

    setValue("rotatorUpdateInterval", cfg.update_interval_sec ?? 2);
    setValue("rotatorMinMove", cfg.min_move_deg ?? 1.0);
    setValue("rotatorAzOffset", cfg.az_offset_deg ?? 0.0);
    setValue("rotatorElOffset", cfg.el_offset_deg ?? 0.0);
    setValue("rotatorMinEl", cfg.min_el_deg ?? 0.0);
    setValue("rotatorMaxEl", cfg.max_el_deg ?? 90.0);
    setValue("rotatorParkOnLos", !!cfg.park_on_los);
    setValue("rotatorParkAz", cfg.park_az_deg ?? 0.0);
    setValue("rotatorParkEl", cfg.park_el_deg ?? 0.0);
  }

  async function loadRotatorConfig() {
    const data = await rotatorApi("/api/rotator/config");
    if (data.http_ok) {
      fillRotatorForm(data);
      setRotatorStatus("Rotator config loaded.", data, false);
    } else {
      setRotatorStatus("Failed to load rotator config.", data, true);
    }
    return data;
  }

  async function saveRotatorConfig() {
    const cfg = rotatorConfigFromForm();
    const data = await rotatorApi("/api/rotator/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(cfg)
    });

    setRotatorStatus(data.http_ok ? "Rotator config saved." : "Failed to save rotator config.", data, !data.http_ok);
    if (data.http_ok) {
      await loadRotatorState();
    }
    return data;
  }


// ROTATOR_UI_LIVE_TARGET_V2_4_2
let rotatorLiveTargetTimerV242 = null;

function rotatorLiveTargetValueV242(data, keys) {
  for (const key of keys) {
    const value = data && data[key];
    if (value !== undefined && value !== null && value !== "" && Number.isFinite(Number(value))) {
      return Number(value);
    }
  }
  return null;
}

function rotatorLiveTargetFormatV242(value) {
  return value === null ? "--" : `${value.toFixed(1)}°`;
}

function updateRotatorLiveTargetV242(data) {
  // ROTATOR_UI_LIVE_TARGET_WAITING_FIX_V2_4_2B
  const box = document.getElementById("rotatorLiveTarget");
  if (!box) {
    return;
  }

  const stateText = data && data.state ? String(data.state) : "unknown";
  const stateLower = stateText.toLowerCase();
  const sourceText = data && data.source ? ` · ${data.source}` : "";
  const targetOnlyStates = new Set(["waiting", "idle", "stopped", "parked", "error", "unknown"]);
  let targetAz = rotatorLiveTargetValueV242(data, ["target_az_deg"]);
  let targetEl = rotatorLiveTargetValueV242(data, ["target_el_deg"]);

  if ((targetAz === null || targetEl === null) && !targetOnlyStates.has(stateLower)) {
    targetAz = targetAz === null ? rotatorLiveTargetValueV242(data, ["az_deg", "azimuth_deg"]) : targetAz;
    targetEl = targetEl === null ? rotatorLiveTargetValueV242(data, ["el_deg", "elevation_deg"]) : targetEl;
  }

  box.textContent =
    `Target Az ${rotatorLiveTargetFormatV242(targetAz)} / ` +
    `El ${rotatorLiveTargetFormatV242(targetEl)} · ${stateText}${sourceText}`;
  box.classList.toggle("rotator-live-target-active", targetAz !== null && targetEl !== null);
}

function startRotatorLiveTargetPollingV242() {
  if (rotatorLiveTargetTimerV242) {
    return;
  }

  const refresh = () => {
    if (!document.getElementById("rotatorLiveTarget")) {
      return;
    }
    if (typeof loadRotatorState === "function") {
      loadRotatorState().catch(() => {});
    }
  };

  refresh();
  rotatorLiveTargetTimerV242 = window.setInterval(refresh, 5000);
}


async function loadRotatorState() {
    const data = await rotatorApi("/api/rotator/state");
    updateRotatorLiveTargetV242(data);
    setRotatorStatus(data.http_ok ? "Rotator state refreshed." : "Failed to refresh rotator state.", data, !data.http_ok);
    return data;
  }

  async function rotatorPost(path, okMessage, failMessage) {
    const data = await rotatorApi(path, { method: "POST", body: "" });
    setRotatorStatus(data.http_ok ? okMessage : failMessage, data, !data.http_ok);
    return data;
  }

  async function testRotator() {
    const az = numberValue("rotatorTestAz", 180);
    const el = numberValue("rotatorTestEl", 45);
    return rotatorPost(`/api/rotator/test?az=${encodeURIComponent(az)}&el=${encodeURIComponent(el)}`,
      "Rotator test command sent.",
      "Rotator test failed.");
  }

  // ROTATOR_UI_PROTOCOL_PREVIEW_V2_4_4
  async function previewRotatorProtocolCommandV244() {
    const type = textValue("rotatorType", "simulation");
    const az = numberValue("rotatorTestAz", 180);
    const el = numberValue("rotatorTestEl", 45);
    const path =
      `/api/rotator/protocol/preview?type=${encodeURIComponent(type)}` +
      `&az=${encodeURIComponent(az)}&el=${encodeURIComponent(el)}`;
    const data = await rotatorApi(path);
    const ok = !!(data && data.http_ok && data.ok !== false);
    setRotatorStatus(
      ok ? "Rotator command preview. This did not move hardware." : "Rotator command preview failed.",
      data,
      !ok
    );
    return data;
  }

  function createField(labelText, inputHtml) {
    return `<label class="rotator-field"><span>${labelText}</span>${inputHtml}</label>`;
  }

  function createRotatorPanel() {
    if (document.getElementById("rotatorControlPanel")) {
      return;
    }

    const typeOptions = ROTATOR_TYPES.map(([value, label]) => `<option value="${value}">${label}</option>`).join("");

    const panel = document.createElement("section");
    panel.id = "rotatorControlPanel";
    panel.className = "rotator-card";
    panel.innerHTML = `
      <div class="rotator-card-header">
        <div>
          <h2>Rotator Control</h2>
          <div id="rotatorLiveTarget" class="rotator-live-target">Target Az -- / El -- · waiting</div>
          <p>Configure and test satellite antenna rotator control. Simulation is safe for development.</p>
        </div>
        <button type="button" id="rotatorRefreshStateBtn" class="rotator-small-button">Refresh</button>
      </div>

      <div class="rotator-grid">
        ${createField("Enabled", '<input id="rotatorEnabled" type="checkbox" />')}
        ${createField("Type", `<select id="rotatorType">${typeOptions}</select>`)}
        ${createField("Host", '<input id="rotatorHost" type="text" value="127.0.0.1" />')}
        ${createField("Port", '<input id="rotatorPort" type="number" value="4533" min="1" max="65535" />')}
        ${createField("Update sec", '<input id="rotatorUpdateInterval" type="number" value="2" min="1" max="60" />')}
        ${createField("Min move deg", '<input id="rotatorMinMove" type="number" value="1.0" step="0.1" />')}
        ${createField("Az offset", '<input id="rotatorAzOffset" type="number" value="0.0" step="0.1" />')}
        ${createField("El offset", '<input id="rotatorElOffset" type="number" value="0.0" step="0.1" />')}
        ${createField("Min el", '<input id="rotatorMinEl" type="number" value="0.0" step="0.1" />')}
        ${createField("Max el", '<input id="rotatorMaxEl" type="number" value="90.0" step="0.1" />')}
        ${createField("Park on LOS", '<input id="rotatorParkOnLos" type="checkbox" />')}
        ${createField("Park az", '<input id="rotatorParkAz" type="number" value="0.0" step="0.1" />')}
        ${createField("Park el", '<input id="rotatorParkEl" type="number" value="0.0" step="0.1" />')}
      </div>

      <div class="rotator-actions">
        <button type="button" id="rotatorLoadConfigBtn">Load Config</button>
        <button type="button" id="rotatorSaveConfigBtn">Save Config</button>
      </div>

      <div class="rotator-test-row">
        ${createField("Test az", '<input id="rotatorTestAz" type="number" value="180" step="0.1" />')}
        ${createField("Test el", '<input id="rotatorTestEl" type="number" value="45" step="0.1" />')}
        <button type="button" id="rotatorPreviewCommandBtn">Preview Command</button>
        <button type="button" id="rotatorTestBtn">Test Move</button>
      </div>

      <div class="rotator-actions">
        <button type="button" id="rotatorTrackStartBtn">Start Rotator Tracking</button>
        <button type="button" id="rotatorTrackStepBtn">Step Once</button>
        <button type="button" id="rotatorTrackStopBtn">Stop Tracking</button>
        <button type="button" id="rotatorParkBtn">Park</button>
        <button type="button" id="rotatorStopBtn">Stop</button>
      </div>

      <pre id="rotatorStatusText" class="rotator-status">Rotator UI ready. Load config to begin.</pre>
    `;

    const preferredHost =
      document.querySelector("#configPanel") ||
      document.querySelector("[data-config-panel]") ||
      document.querySelector(".config-panel") ||
      document.querySelector("main") ||
      document.querySelector("#app") ||
      document.body;

    preferredHost.appendChild(panel);

    document.getElementById("rotatorLoadConfigBtn")?.addEventListener("click", loadRotatorConfig);
    document.getElementById("rotatorSaveConfigBtn")?.addEventListener("click", saveRotatorConfig);
    document.getElementById("rotatorRefreshStateBtn")?.addEventListener("click", loadRotatorState);
    document.getElementById("rotatorPreviewCommandBtn")?.addEventListener("click", previewRotatorProtocolCommandV244);
    document.getElementById("rotatorTestBtn")?.addEventListener("click", testRotator);
    document.getElementById("rotatorParkBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/park", "Rotator park command sent.", "Rotator park failed."));
    document.getElementById("rotatorStopBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/stop", "Rotator stop command sent.", "Rotator stop failed."));
    document.getElementById("rotatorTrackStartBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/track/start", "Rotator tracking started.", "Rotator tracking start failed."));
    document.getElementById("rotatorTrackStopBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/track/stop", "Rotator tracking stopped.", "Rotator tracking stop failed."));
    document.getElementById("rotatorTrackStepBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/track/step", "Rotator tracking step complete.", "Rotator tracking step failed."));
    startRotatorLiveTargetPollingV242();

    loadRotatorConfig().catch((err) => setRotatorStatus("Rotator UI load failed.", { ok: false, error: String(err) }, true));

    window.setInterval(() => {
      if (document.getElementById("rotatorControlPanel")) {
        loadRotatorState().catch(() => {});
      }
    }, 10000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", createRotatorPanel);
  } else {
    createRotatorPanel();
  }

  window.plutoRotatorUi = {
    loadConfig: loadRotatorConfig,
    saveConfig: saveRotatorConfig,
    loadState: loadRotatorState,
    previewCommand: previewRotatorProtocolCommandV244,
    test: testRotator
  };
})();

