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

    function installCollapsibleDrawerSectionsV248() {
      // ROTATOR_MODAL_DIALOG_V2_4_8
      document.querySelectorAll("#appDrawer .drawer-section").forEach((section, index) => {
        if (section.dataset.collapsibleV248 === "1") return;

        const heading = Array.from(section.children).find((child) => child.tagName === "H2");
        if (!heading) return;

        const details = document.createElement("details");
        details.className = "drawer-section-details";
        details.open = index === 0;

        const summary = document.createElement("summary");
        summary.className = "drawer-section-summary";
        summary.textContent = heading.textContent.trim() || "Panel";
        details.appendChild(summary);

        heading.remove();
        while (section.firstChild) {
          details.appendChild(section.firstChild);
        }

        section.appendChild(details);
        section.dataset.collapsibleV248 = "1";
      });
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
        window.setTimeout(() => {
          try {
            if (map && map._container && map._mapPane) map.invalidateSize(false);
          } catch (_error) {
          }
        }, 80);
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

    /* ROTATOR_SELECTED_PASS_PLAN_V2_4_5 */
    let selectedRotatorTrackPlanKeyV245 = "";

    async function publishSelectedPassRotatorPlanV245(pass) {
      if (!pass) return null;
      const key = passKey(pass);
      const plan = pass.doppler_plan || {};
      const points = plan.points || [];
      if (!key || !points.length) return null;
      if (selectedRotatorTrackPlanKeyV245 === key) return null;

      selectedRotatorTrackPlanKeyV245 = key;
      const radio = pass.radio || {};
      const payload = {
        name: pass.name || "",
        norad_id: pass.norad_id || 0,
        aos_utc: pass.aos_utc || "",
        tca_utc: pass.tca_utc || "",
        los_utc: pass.los_utc || "",
        mode: radio.mode || ((pass.modes || [])[0] || ""),
        description: radio.description || "",
        doppler_plan: plan
      };

      try {
        return await postJson("/api/radio/track/plan", payload);
      } catch (error) {
        selectedRotatorTrackPlanKeyV245 = "";
        const status = document.getElementById("status");
        if (status) {
          status.textContent = `Selected pass Doppler plan publish failed: ${error.message || error}`;
        }
        return null;
      }
    }

    function bindRotatorModalOpenButtonsV248() {
      // ROTATOR_MODAL_OPEN_BRIDGE_V2_4_9
      const button = document.getElementById("openRotatorFromListenButton");
      if (!button) return;

      button.onclick = (event) => {
        event.preventDefault();

        if (window.plutoRotatorUi && typeof window.plutoRotatorUi.open === "function") {
          window.plutoRotatorUi.open();
          return;
        }

        const status = document.getElementById("analogAudioStatus") || document.getElementById("status");
        if (status) {
          status.textContent = "Rotator UI is still loading. Try again in a moment.";
        }
      };
    }

    /* SPECTRUM_WATERFALL_UI_V2_5_0 */
    let spectrumWaterfallTimerV250 = 0;
    let spectrumWaterfallRowsV250 = [];
    let spectrumWaterfallFrameV250 = 0;
    let spectrumWaterfallCenterHzV254 = 0; /* SPECTRUM_CENTER_FREQ_LABEL_V2_5_4 */

    /* SPECTRUM_TUNING_CONTROLS_V2_6_5 */
    const SPECTRUM_SAMPLE_RATE_HZ_V265 = 2400000;
    let spectrumWaterfallLastBinsV265 = [];
    let spectrumSquelchUserAdjustedV265 = false;
    /* SPECTRUM_DECODER_BW_CORRECTION_V2_6_6 */
    let spectrumWaterfallSettingsV265 = {
      bandwidthHz: 200000,       // AD9361 / RF front-end filter bandwidth.
      decoderBandwidthHz: 8000,  // Narrow FM decoder/channel bandwidth.
      gainDb: 40,
      squelchDb: -96
    };

    function spectrumWaterfallIsActivePassV250(pass) {
      return !!(pass && passTimingState(pass) === "active");
    }

    function spectrumWaterfallStatusV250(message) {
      const status = document.getElementById("analogAudioStatus") || document.getElementById("status");
      if (status && message) {
        status.textContent = message;
      }
    }

    function spectrumClampV265(value, min, max) {
      return Math.max(min, Math.min(max, value));
    }

    function spectrumFormatBandwidthV265(hz) {
      const value = Number(hz || 0);
      if (!Number.isFinite(value) || value <= 0) return "--";
      if (value >= 1000000) return `${(value / 1000000).toFixed(value >= 2000000 ? 1 : 2)} MHz`;
      return `${Math.round(value / 1000)} kHz`;
    }

    function spectrumFormatGainV265(db) {
      const value = Number(db);
      if (!Number.isFinite(value)) return "--";
      return `${value.toFixed(0)} dB`;
    }

    function spectrumFormatDbV265(db) {
      const value = Number(db);
      if (!Number.isFinite(value)) return "--";
      return `${value.toFixed(0)} dB`;
    }

    function spectrumNoiseFloorDbV265(bins) {
      const values = (bins || []).map((v) => Number(v)).filter((v) => Number.isFinite(v)).sort((a, b) => a - b);
      if (!values.length) return -102;
      const index = Math.max(0, Math.min(values.length - 1, Math.floor(values.length * 0.25)));
      return values[index];
    }

    function spectrumMaybeAutoSetSquelchV265(bins) {
      if (spectrumSquelchUserAdjustedV265) return;
      const noiseFloor = spectrumNoiseFloorDbV265(bins);
      spectrumWaterfallSettingsV265.squelchDb = Math.round(spectrumClampV265(noiseFloor + 6, -118, -45));
      updateSpectrumTuningControlLabelsV265();
    }

    function updateSpectrumTuningControlLabelsV265() {
      const bandwidthValue = document.getElementById("spectrumBandwidthValueV265");
      const decoderBandwidthValue = document.getElementById("spectrumDecoderBandwidthValueV266");
      const gainValue = document.getElementById("spectrumGainValueV265");
      const squelchValue = document.getElementById("spectrumSquelchValueV265");
      const bandwidthInput = document.getElementById("spectrumBandwidthSliderV265");
      const decoderBandwidthInput = document.getElementById("spectrumDecoderBandwidthSliderV266");
      const gainInput = document.getElementById("spectrumGainSliderV265");
      const squelchInput = document.getElementById("spectrumSquelchSliderV265");

      if (bandwidthInput) bandwidthInput.value = String(Math.round(spectrumWaterfallSettingsV265.bandwidthHz));
      if (decoderBandwidthInput) decoderBandwidthInput.value = String(Math.round(spectrumWaterfallSettingsV265.decoderBandwidthHz || 8000));
      if (gainInput) gainInput.value = String(Math.round(spectrumWaterfallSettingsV265.gainDb));
      if (squelchInput) squelchInput.value = String(Math.round(spectrumWaterfallSettingsV265.squelchDb));

      if (bandwidthValue) bandwidthValue.textContent = spectrumFormatBandwidthV265(spectrumWaterfallSettingsV265.bandwidthHz);
      if (decoderBandwidthValue) decoderBandwidthValue.textContent = spectrumFormatBandwidthV265(spectrumWaterfallSettingsV265.decoderBandwidthHz || 8000);
      if (gainValue) gainValue.textContent = spectrumFormatGainV265(spectrumWaterfallSettingsV265.gainDb);
      if (squelchValue) squelchValue.textContent = spectrumFormatDbV265(spectrumWaterfallSettingsV265.squelchDb);
    }

    function redrawSpectrumWithCurrentControlsV265() {
      const canvas = document.getElementById("spectrumCanvasV250");
      if (canvas && spectrumWaterfallLastBinsV265.length) {
        try { drawSpectrumV250(canvas, spectrumWaterfallLastBinsV265); } catch (error) { console.warn("Spectrum redraw fallback", error); drawSpectrumFallbackV269(canvas, spectrumWaterfallLastBinsV265); }
      }
    }

    function bindSpectrumTuningControlsV265() {
      const bandwidthInput = document.getElementById("spectrumBandwidthSliderV265");
      const decoderBandwidthInput = document.getElementById("spectrumDecoderBandwidthSliderV266");
      const gainInput = document.getElementById("spectrumGainSliderV265");
      const squelchInput = document.getElementById("spectrumSquelchSliderV265");
      const autoButton = document.getElementById("spectrumAutoSquelchButtonV265");

      if (bandwidthInput) {
        bandwidthInput.addEventListener("input", () => {
          spectrumWaterfallSettingsV265.bandwidthHz = spectrumClampV265(Number(bandwidthInput.value || 200000), 200000, SPECTRUM_SAMPLE_RATE_HZ_V265);
          updateSpectrumTuningControlLabelsV265();
          redrawSpectrumWithCurrentControlsV265();
        });
      }
      if (decoderBandwidthInput) {
        decoderBandwidthInput.addEventListener("input", () => {
          spectrumWaterfallSettingsV265.decoderBandwidthHz = spectrumClampV265(Number(decoderBandwidthInput.value || 8000), 4000, 30000);
          updateSpectrumTuningControlLabelsV265();
          redrawSpectrumWithCurrentControlsV265();
        });
      }
      if (gainInput) {
        gainInput.addEventListener("input", () => {
          spectrumWaterfallSettingsV265.gainDb = spectrumClampV265(Number(gainInput.value || 40), 0, 70);
          updateSpectrumTuningControlLabelsV265();
        });
      }
      if (squelchInput) {
        squelchInput.addEventListener("input", () => {
          spectrumSquelchUserAdjustedV265 = true;
          spectrumWaterfallSettingsV265.squelchDb = spectrumClampV265(Number(squelchInput.value || -96), -120, -40);
          updateSpectrumTuningControlLabelsV265();
          redrawSpectrumWithCurrentControlsV265();
        });
      }
      if (autoButton) {
        autoButton.addEventListener("click", () => {
          spectrumSquelchUserAdjustedV265 = false;
          spectrumMaybeAutoSetSquelchV265(spectrumWaterfallLastBinsV265);
          redrawSpectrumWithCurrentControlsV265();
        });
      }

      updateSpectrumTuningControlLabelsV265();
    }

    function createSpectrumWaterfallModalV250() {
      if (document.getElementById("spectrumWaterfallModal")) return;

      const modal = document.createElement("div");
      modal.id = "spectrumWaterfallModal";
      modal.className = "spectrum-waterfall-backdrop";
      modal.hidden = true;
      modal.innerHTML = `
        <div class="spectrum-waterfall-modal" role="dialog" aria-modal="true" aria-labelledby="spectrumWaterfallTitle">
          <div class="spectrum-waterfall-header">
            <div>
              <h2 id="spectrumWaterfallTitle">Spectrum / Waterfall</h2>
              <div id="spectrumWaterfallSubtitle" class="spectrum-waterfall-subtitle">Select an active pass.</div>
            </div>
            <button id="spectrumWaterfallCloseButton" type="button" class="secondary">Close</button>
          </div>
          <div class="spectrum-waterfall-body">
            <!-- SPECTRUM_COMPACT_TUNING_CONTROLS_V2_6_7 -->
          <div class="spectrum-tuning-controls spectrum-tuning-controls-compact" id="spectrumTuningControlsV265" aria-label="Spectrum tuning controls">
            <div class="spectrum-tuning-row">
              <label for="spectrumBandwidthSliderV265">RF BW</label>
              <input id="spectrumBandwidthSliderV265" type="range" min="200000" max="2400000" step="25000" value="200000">
              <span id="spectrumBandwidthValueV265" class="spectrum-tuning-value">200 kHz</span>
            </div>
            <div class="spectrum-tuning-row">
              <label for="spectrumDecoderBandwidthSliderV266">Decoder</label>
              <input id="spectrumDecoderBandwidthSliderV266" type="range" min="4000" max="30000" step="1000" value="8000">
              <span id="spectrumDecoderBandwidthValueV266" class="spectrum-tuning-value">8 kHz</span>
            </div>
            <div class="spectrum-tuning-row">
              <label for="spectrumGainSliderV265">Gain</label>
              <input id="spectrumGainSliderV265" type="range" min="0" max="70" step="1" value="40">
              <span id="spectrumGainValueV265" class="spectrum-tuning-value">40 dB</span>
            </div>
            <div class="spectrum-tuning-row spectrum-tuning-row-squelch">
              <label for="spectrumSquelchSliderV265">Squelch</label>
              <input id="spectrumSquelchSliderV265" type="range" min="-120" max="-40" step="1" value="-96">
              <span id="spectrumSquelchValueV265" class="spectrum-tuning-value">-96 dB</span>
              <button id="spectrumAutoSquelchButtonV265" type="button" class="secondary small-button compact-auto-button" title="Set squelch to noise floor plus 6 dB">Auto +6</button>
            </div>
          </div>
            <div class="spectrum-waterfall-panel">
              <div class="spectrum-waterfall-panel-title">Spectrum</div>
              <canvas id="spectrumCanvasV250" width="960" height="260"></canvas>
            </div>
            <div class="spectrum-waterfall-panel">
              <div class="spectrum-waterfall-panel-title">Waterfall</div>
              <canvas id="waterfallCanvasV250" width="960" height="380"></canvas>
            </div>
            <div id="spectrumWaterfallStatus" class="spectrum-waterfall-status">
              Ready to request live Pluto spectrum snapshots. Live audio uses these controls when Listen starts.
            </div>
          </div>
        </div>
      `;

      modal.addEventListener("click", (event) => {
        if (event.target === modal) {
          closeSpectrumWaterfallModalV250();
        }
      });

      document.body.appendChild(modal);
      document.getElementById("spectrumWaterfallCloseButton")?.addEventListener("click", closeSpectrumWaterfallModalV250);
      bindSpectrumTuningControlsV265();
    }

    function closeSpectrumWaterfallModalV250() {
      const modal = document.getElementById("spectrumWaterfallModal");
      if (modal) {
        modal.hidden = true;
        modal.classList.remove("open");
      }
      if (spectrumWaterfallTimerV250) {
        window.clearInterval(spectrumWaterfallTimerV250);
        spectrumWaterfallTimerV250 = 0;
      }
      document.body.classList.remove("spectrum-waterfall-modal-open");
    }

    function spectrumWaterfallBinsV250(pass) {
      const bins = 160;
      const data = [];
      const point = liveLookPointForPass(
        pass,
        trackedPointForPass(pass, lastTrackState),
        focusedMapPoint(pass, lastTrackState)
      );
      const elevation = Number((point && point.elevation_deg) || pass.max_elevation_deg || 20);
      const peakCenter = 0.5 + 0.10 * Math.sin(spectrumWaterfallFrameV250 / 9);
      const secondaryCenter = 0.5 - 0.20 * Math.cos(spectrumWaterfallFrameV250 / 13);
      const baseNoise = -116 + Math.min(22, Math.max(0, elevation) * 0.22);

      for (let i = 0; i < bins; i += 1) {
        const x = bins > 1 ? i / (bins - 1) : 0;
        const main = 54 * Math.exp(-Math.pow((x - peakCenter) / 0.055, 2));
        const secondary = 18 * Math.exp(-Math.pow((x - secondaryCenter) / 0.035, 2));
        const ripple = 4 * Math.sin((i * 0.47) + (spectrumWaterfallFrameV250 * 0.31));
        const noise = 3 * Math.sin((i * 1.73) + (spectrumWaterfallFrameV250 * 0.19));
        data.push(Math.max(-125, Math.min(-42, baseNoise + main + secondary + ripple + noise)));
      }

      return data;
    }

    /* SPECTRUM_CENTER_FREQ_LABEL_V2_5_4 */
    function spectrumWaterfallFormatCenterMHzV254(freqHz) {
      const hz = Number(freqHz || 0);
      if (!Number.isFinite(hz) || hz <= 0) return "";
      return `${(hz / 1000000).toFixed(6)} MHz`;
    }

    function drawSpectrumTuningOverlayV265(ctx, padL, padT, plotW, plotH, dbMin, dbMax) {
      const settings = spectrumWaterfallSettingsV265 || {};
      const centerX = padL + plotW / 2;
      const rfBandwidthHz = spectrumClampV265(Number(settings.bandwidthHz || 200000), 200000, SPECTRUM_SAMPLE_RATE_HZ_V265);
      const rfPassbandW = spectrumClampV265((rfBandwidthHz / SPECTRUM_SAMPLE_RATE_HZ_V265) * plotW, 10, plotW);
      const rfLeftX = centerX - rfPassbandW / 2;
      const rfRightX = centerX + rfPassbandW / 2;

      const decoderBandwidthHz = spectrumClampV265(Number(settings.decoderBandwidthHz || 8000), 4000, 30000);
      const decoderPassbandW = spectrumClampV265((decoderBandwidthHz / SPECTRUM_SAMPLE_RATE_HZ_V265) * plotW, 6, plotW);
      const decoderLeftX = centerX - decoderPassbandW / 2;
      const decoderRightX = centerX + decoderPassbandW / 2;

      ctx.save();
      ctx.fillStyle = "rgba(56, 189, 248, 0.055)";
      ctx.fillRect(rfLeftX, padT, rfPassbandW, plotH);
      ctx.strokeStyle = "rgba(56, 189, 248, 0.34)";
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(rfLeftX, padT);
      ctx.lineTo(rfLeftX, padT + plotH);
      ctx.moveTo(rfRightX, padT);
      ctx.lineTo(rfRightX, padT + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.fillStyle = "rgba(34, 197, 94, 0.12)";
      ctx.fillRect(decoderLeftX, padT, decoderPassbandW, plotH);
      ctx.strokeStyle = "rgba(34, 197, 94, 0.92)";
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(decoderLeftX, padT);
      ctx.lineTo(decoderLeftX, padT + plotH);
      ctx.moveTo(decoderRightX, padT);
      ctx.lineTo(decoderRightX, padT + plotH);
      ctx.stroke();

      const squelchDb = spectrumClampV265(Number(settings.squelchDb || -96), dbMin, dbMax);
      const squelchY = padT + ((dbMax - squelchDb) / (dbMax - dbMin)) * plotH;
      ctx.strokeStyle = "rgba(250, 204, 21, 0.95)";
      ctx.lineWidth = 1.5;
      ctx.setLineDash([9, 5]);
      ctx.beginPath();
      ctx.moveTo(padL, squelchY);
      ctx.lineTo(padL + plotW, squelchY);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = "rgba(7, 17, 28, 0.86)";
      ctx.fillRect(padL + 7, squelchY - 15, 92, 18);
      ctx.strokeStyle = "rgba(250, 204, 21, 0.65)";
      ctx.strokeRect(padL + 7, squelchY - 15, 92, 18);
      ctx.fillStyle = "#fde68a";
      ctx.font = "11px system-ui, sans-serif";
      ctx.fillText(`Squelch ${squelchDb.toFixed(0)} dB`, padL + 12, squelchY - 2);
      ctx.restore();
    }

    /* SPECTRUM_RENDER_GUARD_V2_6_9
     * Safety fallback: draw a basic spectrum/waterfall even if optional tuning overlays throw.
     */
    function drawSpectrumFallbackV269(canvas, bins) {
      const ctx = canvas && canvas.getContext ? canvas.getContext("2d") : null;
      const values = (bins || []).map((db) => Number(db)).filter((db) => Number.isFinite(db));
      if (!ctx || !values.length) return;

      const rect = canvas.getBoundingClientRect ? canvas.getBoundingClientRect() : { width: canvas.width || 960, height: canvas.height || 260 };
      const scale = window.devicePixelRatio || 1;
      const w = Math.max(320, Math.floor((rect.width || canvas.width || 960) * scale));
      const h = Math.max(180, Math.floor((rect.height || canvas.height || 260) * scale));
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;

      const padL = 52 * scale;
      const padR = 18 * scale;
      const padT = 20 * scale;
      const padB = 30 * scale;
      const plotW = Math.max(1, w - padL - padR);
      const plotH = Math.max(1, h - padT - padB);
      const dbMin = -125;
      const dbMax = -40;

      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = "#07111c";
      ctx.fillRect(0, 0, w, h);
      ctx.strokeStyle = "rgba(148, 163, 184, 0.24)";
      ctx.lineWidth = 1 * scale;
      ctx.font = `${12 * scale}px system-ui, sans-serif`;
      ctx.fillStyle = "#9db1c3";

      for (let db = -120; db <= -40; db += 20) {
        const y = padT + ((dbMax - db) / (dbMax - dbMin)) * plotH;
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(w - padR, y);
        ctx.stroke();
        ctx.fillText(`${db} dB`, 8 * scale, y + 4 * scale);
      }

      const centerX = padL + plotW / 2;
      ctx.strokeStyle = "rgba(56, 189, 248, 0.55)";
      ctx.setLineDash([6 * scale, 5 * scale]);
      ctx.beginPath();
      ctx.moveTo(centerX, padT);
      ctx.lineTo(centerX, padT + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.strokeStyle = "#38bdf8";
      ctx.lineWidth = 2 * scale;
      ctx.beginPath();
      values.forEach((db, index) => {
        const x = padL + (index / Math.max(1, values.length - 1)) * plotW;
        const y = padT + ((dbMax - db) / (dbMax - dbMin)) * plotH;
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();

      ctx.strokeStyle = "rgba(226, 232, 240, 0.34)";
      ctx.strokeRect(padL, padT, plotW, plotH);
      ctx.fillStyle = "#9db1c3";
      ctx.fillText("Live Pluto snapshot", w - 176 * scale, 16 * scale);
    }

    function waterfallColorFallbackV269(db) {
      const t = Math.max(0, Math.min(1, (Number(db || -125) + 125) / 85));
      const r = Math.round(8 + t * 235);
      const g = Math.round(20 + Math.max(0, t - 0.15) * 210);
      const b = Math.round(45 + Math.max(0, 0.80 - t) * 175);
      return `rgb(${r},${g},${b})`;
    }

    function drawWaterfallFallbackV269(canvas, rows) {
      const ctx = canvas && canvas.getContext ? canvas.getContext("2d") : null;
      if (!ctx) return;
      const rect = canvas.getBoundingClientRect ? canvas.getBoundingClientRect() : { width: canvas.width || 960, height: canvas.height || 380 };
      const scale = window.devicePixelRatio || 1;
      const w = Math.max(320, Math.floor((rect.width || canvas.width || 960) * scale));
      const h = Math.max(180, Math.floor((rect.height || canvas.height || 380) * scale));
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;

      const padL = 46 * scale;
      const padR = 16 * scale;
      const padT = 16 * scale;
      const padB = 24 * scale;
      const plotW = Math.max(1, w - padL - padR);
      const plotH = Math.max(1, h - padT - padB);
      const validRows = (rows || []).filter((row) => Array.isArray(row) && row.length);

      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = "#06111f";
      ctx.fillRect(0, 0, w, h);
      if (!validRows.length) {
        ctx.fillStyle = "#94a3b8";
        ctx.font = `${13 * scale}px system-ui, sans-serif`;
        ctx.fillText("Waiting for spectrum rows...", padL, padT + 28 * scale);
      } else {
        const maxRows = Math.min(validRows.length, Math.floor(plotH));
        for (let y = 0; y < maxRows; y += 1) {
          const row = validRows[y];
          for (let x = 0; x < plotW; x += 1) {
            const idx = Math.min(row.length - 1, Math.floor((x / Math.max(1, plotW - 1)) * row.length));
            ctx.fillStyle = waterfallColorFallbackV269(row[idx]);
            ctx.fillRect(padL + x, padT + y, 1, 1);
          }
        }
      }
      const centerX = padL + plotW / 2;
      ctx.setLineDash([5 * scale, 5 * scale]);
      ctx.strokeStyle = "rgba(224, 242, 254, 0.78)";
      ctx.beginPath();
      ctx.moveTo(centerX, padT);
      ctx.lineTo(centerX, padT + plotH);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.strokeStyle = "rgba(226, 232, 240, 0.34)";
      ctx.strokeRect(padL, padT, plotW, plotH);
    }

    function drawSpectrumWaterfallSafeV269(spectrumCanvas, waterfallCanvas, bins, rows, status) {
      const notes = [];
      try {
        drawSpectrumV250(spectrumCanvas, bins);
      } catch (error) {
        console.warn("Spectrum overlay draw failed; using fallback renderer", error);
        drawSpectrumFallbackV269(spectrumCanvas, bins);
        notes.push("spectrum overlay fallback");
      }

      try {
        drawWaterfallV250(waterfallCanvas, rows);
      } catch (error) {
        console.warn("Waterfall draw failed; using fallback renderer", error);
        drawWaterfallFallbackV269(waterfallCanvas, rows);
        notes.push("waterfall fallback");
      }

      if (notes.length && status && status.textContent) {
        status.textContent = `${status.textContent} · ${notes.join(" · ")}`;
      }
    }

    function drawSpectrumV250(canvas, bins) {
      const ctx = canvas && canvas.getContext ? canvas.getContext("2d") : null;
      if (!ctx || !bins.length) return;

      const w = canvas.width;
      const h = canvas.height;
      const padL = 52;
      const padR = 18;
      const padT = 20;
      const padB = 30;
      const plotW = w - padL - padR;
      const plotH = h - padT - padB;
      const dbMin = -125;
      const dbMax = -40;

      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = "#07111c";
      ctx.fillRect(0, 0, w, h);

      ctx.strokeStyle = "rgba(148, 163, 184, 0.24)";
      ctx.lineWidth = 1;
      ctx.font = "12px system-ui, sans-serif";
      ctx.fillStyle = "#9db1c3";

      for (let db = -120; db <= -40; db += 20) {
        const y = padT + ((dbMax - db) / (dbMax - dbMin)) * plotH;
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(w - padR, y);
        ctx.stroke();
        ctx.fillText(`${db} dB`, 8, y + 4);
      }

      for (let tick = 0; tick <= 4; tick += 1) {
        const x = padL + (plotW * tick / 4);
        ctx.beginPath();
        ctx.moveTo(x, padT);
        ctx.lineTo(x, padT + plotH);
        ctx.stroke();
      }

      const centerX = padL + plotW / 2;
      ctx.strokeStyle = "rgba(56, 189, 248, 0.55)";
      ctx.setLineDash([6, 5]);
      ctx.beginPath();
      ctx.moveTo(centerX, padT);
      ctx.lineTo(centerX, padT + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      try { drawSpectrumTuningOverlayV265(ctx, padL, padT, plotW, plotH, dbMin, dbMax); } catch (error) { console.warn("Spectrum tuning overlay disabled", error); }

      const centerLabelV254 = spectrumWaterfallFormatCenterMHzV254(spectrumWaterfallCenterHzV254);
      if (centerLabelV254) {
        ctx.font = "12px system-ui, sans-serif";
        const labelWidth = ctx.measureText(centerLabelV254).width;
        const labelX = Math.max(padL + 6, Math.min(w - padR - labelWidth - 10, centerX - labelWidth / 2));
        const labelY = padT + 14;
        ctx.fillStyle = "rgba(7, 17, 28, 0.82)";
        ctx.fillRect(labelX - 5, labelY - 12, labelWidth + 10, 16);
        ctx.strokeStyle = "rgba(56, 189, 248, 0.45)";
        ctx.strokeRect(labelX - 5, labelY - 12, labelWidth + 10, 16);
        ctx.fillStyle = "#e0f2fe";
        ctx.fillText(centerLabelV254, labelX, labelY);
      }

      ctx.strokeStyle = "#38bdf8";
      ctx.lineWidth = 2;
      ctx.beginPath();
      bins.forEach((db, index) => {
        const x = padL + (index / (bins.length - 1)) * plotW;
        const y = padT + ((dbMax - db) / (dbMax - dbMin)) * plotH;
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();

      ctx.fillStyle = "#ecf4fb";
      ctx.fillText("Relative frequency", padL + plotW / 2 - 48, h - 8);
      ctx.fillStyle = "#9db1c3";
      ctx.fillText("Live Pluto snapshot", w - 176, 16);
    }

    function waterfallColorV250(db) {
      const normalized = Math.max(0, Math.min(1, (db + 125) / 85));
      const r = Math.round(20 + normalized * 235);
      const g = Math.round(45 + Math.max(0, normalized - 0.20) * 210);
      const b = Math.round(80 + Math.max(0, 0.75 - normalized) * 160);
      return `rgb(${r},${g},${b})`;
    }

        /* SMOOTH_WATERFALL_RENDER_V2_5_6 */
    function waterfallClampV256(value, min, max) {
      return Math.max(min, Math.min(max, value));
    }

    function waterfallLerpV256(a, b, t) {
      return a + (b - a) * t;
    }

    function waterfallPaletteV256(db) {
      const floorDb = -120;
      const ceilingDb = -35;
      const t = waterfallClampV256((Number(db || floorDb) - floorDb) / (ceilingDb - floorDb), 0, 1);
      const stops = [
        [0.00, 4, 8, 20],
        [0.18, 12, 32, 68],
        [0.38, 18, 93, 132],
        [0.58, 42, 172, 161],
        [0.76, 212, 194, 82],
        [0.91, 239, 122, 54],
        [1.00, 255, 245, 190]
      ];

      for (let i = 1; i < stops.length; i += 1) {
        if (t <= stops[i][0]) {
          const prev = stops[i - 1];
          const next = stops[i];
          const local = (t - prev[0]) / Math.max(0.0001, next[0] - prev[0]);
          return [
            Math.round(waterfallLerpV256(prev[1], next[1], local)),
            Math.round(waterfallLerpV256(prev[2], next[2], local)),
            Math.round(waterfallLerpV256(prev[3], next[3], local))
          ];
        }
      }
      return [255, 245, 190];
    }

    function waterfallSampleRowV256(row, x, width) {
      if (!Array.isArray(row) || !row.length) return -120;
      if (row.length === 1 || width <= 1) return Number(row[0] || -120);

      const pos = (x / Math.max(1, width - 1)) * (row.length - 1);
      const i0 = Math.floor(pos);
      const i1 = Math.min(row.length - 1, i0 + 1);
      const frac = pos - i0;
      const a = Number(row[i0]);
      const b = Number(row[i1]);
      const av = Number.isFinite(a) ? a : -120;
      const bv = Number.isFinite(b) ? b : av;
      return waterfallLerpV256(av, bv, frac);
    }

    /* PRO_WATERFALL_SCROLL_V2_6_1
     * Render the waterfall like a traditional SDR display:
     * - new snapshots are inserted as one fine row at the top,
     * - older rows move downward,
     * - unused history remains blank until enough snapshots accumulate,
     * - rows are not stretched to fill the whole waterfall.
     */
    function waterfallBackgroundColorV261(y, height) {
      const t = waterfallClampV256(y / Math.max(1, height - 1), 0, 1);
      return [
        Math.round(waterfallLerpV256(3, 6, t)),
        Math.round(waterfallLerpV256(8, 17, t)),
        Math.round(waterfallLerpV256(18, 31, t))
      ];
    }

    function waterfallPlotMetricsV261(canvas) {
      const rect = canvas.getBoundingClientRect();
      const scale = window.devicePixelRatio || 1;
      const w = Math.max(320, Math.floor(rect.width * scale));
      const h = Math.max(180, Math.floor(rect.height * scale));
      const padL = Math.round(46 * scale);
      const padR = Math.round(16 * scale);
      const padT = Math.round(16 * scale);
      const padB = Math.round(24 * scale);
      const plotW = Math.max(1, Math.floor(w - padL - padR));
      const plotH = Math.max(1, Math.floor(h - padT - padB));
      return { rect, scale, w, h, padL, padR, padT, padB, plotW, plotH };
    }

    function waterfallMaxHistoryRowsV261(canvas) {
      if (!canvas || !canvas.getBoundingClientRect) return 420;
      const metrics = waterfallPlotMetricsV261(canvas);
      return Math.max(180, Math.min(900, metrics.plotH));
    }

    function drawWaterfallV250(canvas, rows) {
      const ctx = canvas && canvas.getContext ? canvas.getContext("2d") : null;
      if (!ctx) return;

      const metrics = waterfallPlotMetricsV261(canvas);
      const { scale, w, h, padL, padR, padT, plotW, plotH } = metrics;
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
      }

      ctx.clearRect(0, 0, w, h);

      const bg = ctx.createLinearGradient(0, 0, 0, h);
      bg.addColorStop(0, "#06111f");
      bg.addColorStop(1, "#020617");
      ctx.fillStyle = bg;
      ctx.fillRect(0, 0, w, h);

      const validRows = (rows || []).filter((row) => Array.isArray(row) && row.length);
      const image = ctx.createImageData(plotW, plotH);
      const historyRows = validRows.slice(0, Math.min(validRows.length, plotH));

      for (let y = 0; y < plotH; y += 1) {
        const row = y < historyRows.length ? historyRows[y] : null;
        const bgColor = row ? null : waterfallBackgroundColorV261(y, plotH);
        for (let x = 0; x < plotW; x += 1) {
          const idx = (y * plotW + x) * 4;
          let r;
          let g;
          let b;
          if (row) {
            const db = waterfallSampleRowV256(row, x, plotW);
            [r, g, b] = waterfallPaletteV256(db);
          } else {
            [r, g, b] = bgColor;
          }
          image.data[idx] = r;
          image.data[idx + 1] = g;
          image.data[idx + 2] = b;
          image.data[idx + 3] = 255;
        }
      }

      ctx.putImageData(image, padL, padT);

      const topShine = ctx.createLinearGradient(0, padT, 0, padT + Math.min(plotH, 46 * scale));
      topShine.addColorStop(0.00, "rgba(255,255,255,0.08)");
      topShine.addColorStop(1.00, "rgba(255,255,255,0.00)");
      ctx.fillStyle = topShine;
      ctx.fillRect(padL, padT, plotW, Math.min(plotH, 46 * scale));

      ctx.strokeStyle = "rgba(148, 163, 184, 0.18)";
      ctx.lineWidth = 1 * scale;
      for (let i = 0; i <= 4; i += 1) {
        const x = padL + (plotW * i) / 4;
        ctx.beginPath();
        ctx.moveTo(x, padT);
        ctx.lineTo(x, padT + plotH);
        ctx.stroke();
      }

      const centerX = padL + plotW / 2;
      ctx.setLineDash([5 * scale, 5 * scale]);
      ctx.strokeStyle = "rgba(224, 242, 254, 0.78)";
      ctx.lineWidth = 1 * scale;
      ctx.beginPath();
      ctx.moveTo(centerX, padT);
      ctx.lineTo(centerX, padT + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      if (!validRows.length) {
        ctx.fillStyle = "#94a3b8";
        ctx.font = `${13 * scale}px system-ui, sans-serif`;
        ctx.fillText("Waterfall history will build downward from the top...", padL, padT + 28 * scale);
      } else {
        ctx.fillStyle = "rgba(203, 213, 225, 0.92)";
        ctx.font = `${11 * scale}px system-ui, sans-serif`;
        const rowText = `${historyRows.length} row${historyRows.length === 1 ? "" : "s"}`;
        ctx.fillText(rowText, padL + 8 * scale, padT + 15 * scale);
      }

      ctx.strokeStyle = "rgba(226, 232, 240, 0.34)";
      ctx.strokeRect(padL, padT, plotW, plotH);

      ctx.fillStyle = "#cbd5e1";
      ctx.font = `${11 * scale}px system-ui, sans-serif`;
      ctx.fillText("-1.2 MHz", padL, h - 7 * scale);
      const centerText = "Center";
      ctx.fillText(centerText, centerX - ctx.measureText(centerText).width / 2, h - 7 * scale);
      ctx.fillText("+1.2 MHz", w - padR - 56 * scale, h - 7 * scale);
    }

    /* SPECTRUM_SELECTED_FREQ_PARAM_V2_5_3 */
    function spectrumWaterfallFrequencyHzV253(pass) {
      if (!pass) return 0;

      const trackState = lastTrackState || {};
      const activePoint = trackedPointForPass(pass, trackState);
      const focusPoint = focusedMapPoint(pass, trackState);
      const livePoint = liveLookPointForPass(pass, activePoint, focusPoint);
      const radio = pass.radio || {};
      const downlinks = Array.isArray(pass.downlinks_hz) ? pass.downlinks_hz : [];
      const plan = pass.doppler_plan || {};
      const points = Array.isArray(plan.points) ? plan.points : [];
      let nearestPlanPoint = null;

      if (points.length) {
        const now = Date.now();
        let bestDelta = Infinity;
        points.forEach((point) => {
          const t = Date.parse(point.time_utc || "");
          if (!Number.isFinite(t)) return;
          const delta = Math.abs(t - now);
          if (delta < bestDelta) {
            bestDelta = delta;
            nearestPlanPoint = point;
          }
        });
      }

      const candidates = [
        trackState.rx_hz,
        trackState.downlink_hz,
        trackState.center_hz,
        livePoint && livePoint.rx_hz,
        livePoint && livePoint.downlink_hz,
        livePoint && livePoint.frequency_hz,
        activePoint && activePoint.rx_hz,
        activePoint && activePoint.downlink_hz,
        activePoint && activePoint.frequency_hz,
        focusPoint && focusPoint.rx_hz,
        focusPoint && focusPoint.downlink_hz,
        nearestPlanPoint && nearestPlanPoint.rx_hz,
        nearestPlanPoint && nearestPlanPoint.downlink_hz,
        nearestPlanPoint && nearestPlanPoint.frequency_hz,
        radio.downlink_hz,
        pass.downlink_hz,
        downlinks[0]
      ];

      for (const candidate of candidates) {
        const value = Number(candidate || 0);
        if (Number.isFinite(value) && value > 0) {
          return Math.round(value);
        }
      }

      return 0;
    }

    function spectrumWaterfallSnapshotUrlV253(pass) {
      const freqHz = spectrumWaterfallFrequencyHzV253(pass);
      const params = new URLSearchParams();
      params.set("bins", "192");
      params.set("request", String(Date.now()));
      const tuning = spectrumWaterfallSettingsV265 || {};
      if (freqHz > 0) {
        params.set("freq_hz", String(freqHz));
      }
      /* SPECTRUM_SNAPSHOT_NO_TUNING_PARAMS_V2_6_10A */
      ["bandwidth_hz", "decoder_bandwidth_hz", "decoder_bw_hz", "gain_db", "squelch_db"].forEach((key) => params.delete(key));

      return {
        url: `/api/radio/spectrum/snapshot?${params.toString()}`,
        freqHz
      };
    }



    /* SPECTRUM_PAUSE_DURING_AUDIO_V2_6_13
     * The Pluto cannot reliably run live audio DSP and repeated spectrum
     * iio_readdev snapshots at the same time.  When Listen is active, keep
     * the modal open but do not poll the spectrum endpoint, so audio is not
     * starved or killed by competing IIO access.
     */
    function spectrumWaterfallLiveAudioActiveV2613() {
      try {
        return Boolean(
          typeof analogAudioSession !== "undefined" &&
          analogAudioSession &&
          !analogAudioSession.stopped
        );
      } catch (_error) {
        return false;
      }
    }

    function drawSpectrumWaterfallPausedForAudioV2613(canvas, title, detail) {
      const ctx = canvas && canvas.getContext ? canvas.getContext("2d") : null;
      if (!ctx) return;

      const rect = canvas.getBoundingClientRect ? canvas.getBoundingClientRect() : { width: canvas.width || 640, height: canvas.height || 220 };
      const scale = window.devicePixelRatio || 1;
      const w = Math.max(320, Math.floor((rect.width || canvas.width || 640) * scale));
      const h = Math.max(170, Math.floor((rect.height || canvas.height || 220) * scale));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
      }

      ctx.clearRect(0, 0, w, h);
      const gradient = ctx.createLinearGradient(0, 0, 0, h);
      gradient.addColorStop(0, "#07111c");
      gradient.addColorStop(1, "#020617");
      ctx.fillStyle = gradient;
      ctx.fillRect(0, 0, w, h);

      const pad = 18 * scale;
      ctx.strokeStyle = "rgba(148, 163, 184, 0.22)";
      ctx.lineWidth = 1 * scale;
      ctx.strokeRect(pad, pad, w - 2 * pad, h - 2 * pad);

      ctx.fillStyle = "#e0f2fe";
      ctx.font = `${16 * scale}px system-ui, sans-serif`;
      const titleText = title || "Spectrum paused";
      ctx.fillText(titleText, pad + 16 * scale, pad + 34 * scale);

      ctx.fillStyle = "#94a3b8";
      ctx.font = `${12 * scale}px system-ui, sans-serif`;
      const detailText = detail || "Live audio is active; spectrum polling is paused to avoid Pluto/IIO contention.";
      ctx.fillText(detailText, pad + 16 * scale, pad + 58 * scale);

      ctx.fillStyle = "rgba(250, 204, 21, 0.90)";
      ctx.font = `${12 * scale}px system-ui, sans-serif`;
      ctx.fillText("Stop Listen to resume live spectrum and waterfall refresh.", pad + 16 * scale, pad + 80 * scale);
    }

    async function renderSpectrumWaterfallV250() {
      if (!spectrumWaterfallIsActivePassV250(currentSelectedPass)) {
        closeSpectrumWaterfallModalV250();
        spectrumWaterfallStatusV250("Spectrum/waterfall is available only while the selected pass is active.");
        return;
      }

      const title = currentSelectedPass.name || "Active pass";
      const point = liveLookPointForPass(
        currentSelectedPass,
        trackedPointForPass(currentSelectedPass, lastTrackState),
        focusedMapPoint(currentSelectedPass, lastTrackState)
      );
      const subtitle = document.getElementById("spectrumWaterfallSubtitle");
      const status = document.getElementById("spectrumWaterfallStatus");
      const spectrumCanvas = document.getElementById("spectrumCanvasV250");
      const waterfallCanvas = document.getElementById("waterfallCanvasV250");

      if (subtitle) {
        const az = point && Number.isFinite(Number(point.azimuth_deg)) ? Number(point.azimuth_deg).toFixed(1) : "--";
        const el = point && Number.isFinite(Number(point.elevation_deg)) ? Number(point.elevation_deg).toFixed(1) : "--";
        subtitle.textContent = `${title} · active · Az ${az}° / El ${el}°`;
      }

      if (status) {
        status.textContent = "Waiting for live Pluto spectrum snapshot...";
      }


      if (spectrumWaterfallLiveAudioActiveV2613()) {
        if (status) {
          status.textContent = "Spectrum/waterfall paused while live audio is playing. Stop Listen to resume live spectrum.";
        }
        drawSpectrumWaterfallPausedForAudioV2613(
          spectrumCanvas,
          "Spectrum paused for audio",
          "Live audio is using Pluto/IIO resources. Spectrum snapshots are paused to prevent audio dropouts."
        );
        drawSpectrumWaterfallPausedForAudioV2613(
          waterfallCanvas,
          "Waterfall paused for audio",
          "This avoids contention between pluto_fm_receiver and spectrum snapshots."
        );
        return;
      }

      spectrumWaterfallFrameV250 += 1;
      let bins = [];
      try {
        const request = spectrumWaterfallSnapshotUrlV253(currentSelectedPass);
        if (!request.freqHz) {
          throw new Error("Selected active pass does not have a usable downlink frequency.");
        }
        const snapshot = await getJson(request.url);
        bins = (snapshot.bins || []).map((bin) => Number(bin.db)).filter((value) => Number.isFinite(value));
        if (!bins.length) {
          throw new Error("Spectrum snapshot returned no bins.");
        }
        spectrumWaterfallCenterHzV254 = Number(snapshot.center_hz || request.freqHz || 0);
        spectrumWaterfallLastBinsV265 = bins.slice();
        spectrumMaybeAutoSetSquelchV265(bins);
        if (status) {
          status.textContent =
            `Live spectrum from Pluto · ${(Number(snapshot.center_hz || request.freqHz || 0) / 1000000).toFixed(3)} MHz · ` +
            `${snapshot.bin_count || bins.length} bins · ${snapshot.sample_count || 0} IQ samples · ` +
            `RF ${spectrumFormatBandwidthV265(spectrumWaterfallSettingsV265.bandwidthHz)} · ` +
            `Decoder ${spectrumFormatBandwidthV265(spectrumWaterfallSettingsV265.decoderBandwidthHz || 8000)} · ` +
            `Gain ${spectrumFormatGainV265(spectrumWaterfallSettingsV265.gainDb)} · ` +
            `Squelch ${spectrumFormatDbV265(spectrumWaterfallSettingsV265.squelchDb)}`;
        }
      } catch (error) {
        if (status) {
          status.textContent = `Spectrum backend snapshot failed: ${error.message || error}`;
        }
        return;
      }
      spectrumWaterfallRowsV250.unshift(bins);
      const maxRows = waterfallMaxHistoryRowsV261(waterfallCanvas);
      if (spectrumWaterfallRowsV250.length > maxRows) {
        spectrumWaterfallRowsV250.length = maxRows;
      }
      drawSpectrumWaterfallSafeV269(spectrumCanvas, waterfallCanvas, bins, spectrumWaterfallRowsV250, status);
    }

    function openSpectrumWaterfallModalV250() {
      if (!spectrumWaterfallIsActivePassV250(currentSelectedPass)) {
        spectrumWaterfallStatusV250("Spectrum/waterfall is available only while the selected pass is active.");
        return;
      }

      createSpectrumWaterfallModalV250();
      const modal = document.getElementById("spectrumWaterfallModal");
      if (!modal) return;

      spectrumWaterfallRowsV250 = [];
      spectrumWaterfallLastBinsV265 = [];
      spectrumSquelchUserAdjustedV265 = false;
      updateSpectrumTuningControlLabelsV265();
      modal.hidden = false;
      modal.classList.add("open");
      document.body.classList.add("spectrum-waterfall-modal-open");
      renderSpectrumWaterfallV250();

      if (spectrumWaterfallTimerV250) {
        window.clearInterval(spectrumWaterfallTimerV250);
      }
      spectrumWaterfallTimerV250 = window.setInterval(renderSpectrumWaterfallV250, 850);
    }

    function bindSpectrumWaterfallButtonV250() {
      const button = document.getElementById("openSpectrumWaterfallButton");
      if (!button) return;

      const active = spectrumWaterfallIsActivePassV250(currentSelectedPass);
      button.disabled = !active;
      button.title = active
        ? "Open spectrum and waterfall display for the active selected pass."
        : "Spectrum/waterfall is available only while the selected pass is active.";

      button.onclick = (event) => {
        event.preventDefault();
        openSpectrumWaterfallModalV250();
      };
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
              <div class="listen-panel pass-action-panel-v286">
                <button id="analogAudioToggleButton" class="pass-action-button-v286 pass-primary-action-button-v286" type="button" disabled>Listen</button>
                <button id="openRotatorFromListenButton" class="rotator-open-button" type="button">Rotator</button>
                <button id="openSpectrumWaterfallButton" class="spectrum-open-button" type="button" disabled>Spectrum</button>
                <span id="analogAudioStatus" class="listen-panel-status-hidden" hidden></span>
                <span id="receiveDecodeStatusV282" class="listen-panel-status-hidden" hidden></span>
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
      bindReceiveDecodePlaceholderV282(pass, node);
      bindRotatorModalOpenButtonsV248();
      bindSpectrumWaterfallButtonV250();
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

    /* LIVE_AUDIO_SPECTRUM_CONTROL_PARAMS_V2_6_8
     * Reuse the compact Spectrum modal tuning controls when starting live audio.
     * These fields are optional; if the controls are not present, backend defaults apply.
     */
    function spectrumLiveAudioParamsV268() {
      const params = {};
      const readNumber = (ids, fallback = null) => {
        for (const id of ids) {
          const node = document.getElementById(id);
          if (!node) continue;
          const value = Number(node.value);
          if (Number.isFinite(value)) return value;
        }
        return fallback;
      };

      const rfBwHz = readNumber([
        "spectrumRfBandwidthSliderV266",
        "spectrumRfBandwidthSliderV265",
        "spectrumBandwidthSliderV265"
      ]);
      const decoderBwHz = readNumber([
        "spectrumDecoderBandwidthSliderV266",
        "spectrumDecoderBandwidthSliderV265"
      ]);
      const gainDb = readNumber([
        "spectrumGainSliderV265",
        "spectrumReceiverGainSliderV265"
      ]);
      const squelchDb = readNumber([
        "spectrumSquelchSliderV265",
        "spectrumSquelchSliderV266"
      ]);

      if (Number.isFinite(rfBwHz)) params.rf_bw_hz = String(Math.round(rfBwHz));
      if (Number.isFinite(decoderBwHz)) params.decoder_bw_hz = String(Math.round(decoderBwHz));
      if (Number.isFinite(gainDb)) params.gain_db = Number(gainDb).toFixed(1);
      if (Number.isFinite(squelchDb)) params.squelch_db = Number(squelchDb).toFixed(1);
      return params;
    }


    /* LIVE_AUDIO_SQUELCH_APPLY_V2_6_14 */
    function spectrumLiveAudioControlParamsV2614() {
      const readNumber = (ids, fallback) => {
        for (const id of ids) {
          const node = document.getElementById(id);
          if (!node) continue;
          const value = Number(node.value);
          if (Number.isFinite(value)) return value;
        }
        return fallback;
      };
      const settings = (typeof spectrumWaterfallSettingsV265 !== "undefined" && spectrumWaterfallSettingsV265) ? spectrumWaterfallSettingsV265 : {};
      const rfBw = readNumber([
        "spectrumRfBandwidthSliderV266",
        "spectrumRfBwSliderV266",
        "spectrumBandwidthSliderV265",
        "spectrumRfBandwidthSliderV265"
      ], Number(settings.rfBandwidthHz || settings.bandwidthHz || 200000));
      const decoderBw = readNumber([
        "spectrumDecoderBandwidthSliderV266",
        "spectrumDecoderBwSliderV266",
        "spectrumDecoderBandwidthSliderV265",
        "spectrumDecoderSliderV266"
      ], Number(settings.decoderBandwidthHz || settings.decoderBwHz || 8000));
      const gain = readNumber([
        "spectrumGainSliderV266",
        "spectrumGainSliderV265"
      ], Number(settings.gainDb || 40));
      const squelch = readNumber([
        "spectrumSquelchSliderV266",
        "spectrumSquelchSliderV265"
      ], Number(settings.squelchDb || -120));
      return {
        rf_bw_hz: String(Math.round(Math.max(200000, Math.min(2400000, rfBw || 200000)))),
        decoder_bw_hz: String(Math.round(Math.max(4000, Math.min(30000, decoderBw || 8000)))),
        gain_db: String(Math.max(0, Math.min(70, gain || 40)).toFixed(1)),
        squelch_db: String(Math.max(-120, Math.min(-40, squelch || -120)).toFixed(1))
      };
    }

    /* LIVE_AUDIO_SQUELCH_CALIBRATION_V2_6_15
     * Persist and reuse the Spectrum tuning controls for live audio.
     * The Listen button may be outside the Spectrum modal, so relying only on DOM slider nodes
     * can silently fall back to defaults.  This keeps a canonical browser-side state.
     */
    const LIVE_AUDIO_CONTROL_STORAGE_KEY_V2615 = "plutoLiveAudioControlsV2615";

    function spectrumLiveAudioDefaultControlsV2615() {
      return { rf_bw_hz: 200000, decoder_bw_hz: 8000, gain_db: 40, squelch_db: -120 };
    }

    function spectrumClampNumberV2615(value, fallback, min, max) {
      const numeric = Number(value);
      const safe = Number.isFinite(numeric) ? numeric : fallback;
      return Math.max(min, Math.min(max, safe));
    }

    function spectrumReadStoredLiveAudioControlsV2615() {
      const defaults = spectrumLiveAudioDefaultControlsV2615();
      try {
        const raw = window.localStorage ? window.localStorage.getItem(LIVE_AUDIO_CONTROL_STORAGE_KEY_V2615) : "";
        if (!raw) return defaults;
        return { ...defaults, ...JSON.parse(raw) };
      } catch (_error) {
        return defaults;
      }
    }

    function spectrumWriteStoredLiveAudioControlsV2615(state) {
      try {
        if (window.localStorage) window.localStorage.setItem(LIVE_AUDIO_CONTROL_STORAGE_KEY_V2615, JSON.stringify(state));
      } catch (_error) {
      }
    }

    function spectrumLiveAudioControlsFromUiV2615() {
      const stored = spectrumReadStoredLiveAudioControlsV2615();
      const settings = (typeof spectrumWaterfallSettingsV265 !== "undefined" && spectrumWaterfallSettingsV265) ? spectrumWaterfallSettingsV265 : {};
      const nodeValue = (id, fallback) => {
        const node = document.getElementById(id);
        if (!node) return fallback;
        const value = Number(node.value);
        return Number.isFinite(value) ? value : fallback;
      };
      const state = {
        rf_bw_hz: Math.round(spectrumClampNumberV2615(
          nodeValue("spectrumBandwidthSliderV265", settings.bandwidthHz ?? stored.rf_bw_hz),
          stored.rf_bw_hz, 200000, 2400000)),
        decoder_bw_hz: Math.round(spectrumClampNumberV2615(
          nodeValue("spectrumDecoderBandwidthSliderV266", settings.decoderBandwidthHz ?? stored.decoder_bw_hz),
          stored.decoder_bw_hz, 4000, 30000)),
        gain_db: spectrumClampNumberV2615(
          nodeValue("spectrumGainSliderV265", settings.gainDb ?? stored.gain_db),
          stored.gain_db, 0, 70),
        squelch_db: spectrumClampNumberV2615(
          nodeValue("spectrumSquelchSliderV265", settings.squelchDb ?? stored.squelch_db),
          stored.squelch_db, -120, -40)
      };
      spectrumWriteStoredLiveAudioControlsV2615(state);
      return state;
    }

    function spectrumLiveAudioControlParamsV2615() {
      const state = spectrumLiveAudioControlsFromUiV2615();
      return {
        rf_bw_hz: String(state.rf_bw_hz),
        decoder_bw_hz: String(state.decoder_bw_hz),
        gain_db: String(Number(state.gain_db).toFixed(1)),
        squelch_db: String(Number(state.squelch_db).toFixed(1))
      };
    }

    function installSpectrumLiveAudioControlPersistenceV2615() {
      if (window.__plutoLiveAudioControlPersistenceV2615) return;
      window.__plutoLiveAudioControlPersistenceV2615 = true;
      const watched = new Set([
        "spectrumBandwidthSliderV265",
        "spectrumDecoderBandwidthSliderV266",
        "spectrumGainSliderV265",
        "spectrumSquelchSliderV265"
      ]);
      const capture = (event) => {
        if (!event || !event.target || !watched.has(event.target.id)) return;
        const state = spectrumLiveAudioControlsFromUiV2615();
        try {
          if (typeof spectrumWaterfallSettingsV265 !== "undefined" && spectrumWaterfallSettingsV265) {
            spectrumWaterfallSettingsV265.bandwidthHz = state.rf_bw_hz;
            spectrumWaterfallSettingsV265.decoderBandwidthHz = state.decoder_bw_hz;
            spectrumWaterfallSettingsV265.gainDb = state.gain_db;
            spectrumWaterfallSettingsV265.squelchDb = state.squelch_db;
          }
        } catch (_error) {
        }
      };
      document.addEventListener("input", capture, true);
      document.addEventListener("change", capture, true);
    }
    installSpectrumLiveAudioControlPersistenceV2615();
    function analogAudioUrl(pass) {
      const radio = pass && pass.radio ? pass.radio : {};
      const downlink = radio.downlink_hz || (pass && pass.downlinks_hz ? pass.downlinks_hz[0] : 0);
      if (!downlink) return "";
      const streamParams = new URLSearchParams({ downlink_hz: String(downlink) });
      const startParams = new URLSearchParams({ downlink_hz: String(downlink) });
      try {
        const controlParams = spectrumLiveAudioControlParamsV2615();
        Object.entries(controlParams).forEach(([key, value]) => {
          if (value !== undefined && value !== null && String(value) !== "") {
            startParams.set(key, String(value));
          }
        });
      } catch (_error) {
      }
      return {
        downlink,
        startUrl: `/api/radio/audio/live/start?${startParams.toString()}`,
        streamUrl: `/api/radio/audio/live.wav?stream=1&${streamParams.toString()}`,
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

      try {
        if (spectrumWaterfallModalOpenV250) {
          renderSpectrumWaterfallV250().catch(() => {});
        }
      } catch (_error) {
      }
    }

    async function startAnalogAudio(pass, button, statusNode) {
      /* BACKEND_AUDIO_START_VERIFY_V2_8_38
       * Deterministic and verified backend audio startup.
       * The audit proved the backend works when driven in this order:
       *   /live/stop cleanup, /live/start, then /live.wav.
       * This implementation does not open live.wav unless /live/start returns OK.
       * It also includes the mode in /live/start and surfaces the exact start/stream
       * response body in the operator status instead of entering a reset/retry loop.
       */
      const audioUrls = analogAudioUrl(pass);
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      const sampleRate = 24000;
      const targetChunkBytes = 4096 * 2;

      const setStatus = (message) => {
        const text = String(message || "");
        if (statusNode && statusNode.isConnected) statusNode.textContent = text;
        const statusBar = document.getElementById("status");
        if (statusBar) statusBar.textContent = text;
      };

      const displayName = (pass && pass.name) || "selected pass";

      if (!AudioCtx) {
        throw new Error("Web Audio is not available in this browser.");
      }
      if (!audioUrls) {
        throw new Error("No usable downlink is available for this pass.");
      }

      const compactBody = (text) => {
        const value = String(text || "").replace(/\s+/g, " ").trim();
        return value.length > 700 ? value.slice(0, 700) + " ..." : value;
      };

      const modeForStart = () => {
        const radio = (pass && pass.radio) || {};
        const values = [
          radio.mode,
          Array.isArray(pass && pass.modes) ? pass.modes[0] : "",
          radio.description
        ].filter(Boolean).join(" ").toUpperCase();
        if (/\b(NFM|FM|F3E|VOICE)\b/.test(values)) return "FM";
        if (/\b(AM|A3E)\b/.test(values)) return "AM";
        if (/\bUSB\b/.test(values)) return "USB";
        if (/\bLSB\b/.test(values)) return "LSB";
        return String(radio.mode || (Array.isArray(pass && pass.modes) ? pass.modes[0] : "") || "FM").trim() || "FM";
      };

      const urlWithMode = (url) => {
        const parsed = new URL(url, window.location.origin);
        if (!parsed.searchParams.get("mode")) parsed.searchParams.set("mode", modeForStart());
        return `${parsed.pathname}?${parsed.searchParams.toString()}`;
      };

      const fetchTextWithTimeout = async (url, options = {}, timeoutMs = 4500) => {
        const controller = new AbortController();
        const timer = window.setTimeout(() => controller.abort(), timeoutMs);
        try {
          const response = await fetch(url, Object.assign({ cache: "no-store" }, options, { signal: controller.signal }));
          const text = await response.text().catch(() => "");
          return { response, text };
        } catch (error) {
          if (error && error.name === "AbortError") {
            throw new Error(`${url}: timed out after ${timeoutMs} ms`);
          }
          throw error;
        } finally {
          window.clearTimeout(timer);
        }
      };

      const postJsonVerified = async (url, timeoutMs, label, requiredOk = true) => {
        const { response, text } = await fetchTextWithTimeout(url, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: "{}"
        }, timeoutMs);
        let data = null;
        try { data = text ? JSON.parse(text) : {}; } catch (_error) { data = null; }
        if (!response.ok) {
          throw new Error(`${label} failed: HTTP ${response.status}${text ? ` | ${compactBody(text)}` : ""}`);
        }
        if (requiredOk && data && data.ok === false) {
          throw new Error(`${label} rejected: ${compactBody(text)}`);
        }
        return { data: data || {}, text, status: response.status };
      };

      const getJsonBestEffort = async (url, timeoutMs, label) => {
        try {
          const { response, text } = await fetchTextWithTimeout(url, { method: "GET" }, timeoutMs);
          if (!response.ok) return null;
          return text ? JSON.parse(text) : null;
        } catch (error) {
          console.warn(`[Pluto] ${label || "GET"} best-effort failed`, error);
          return null;
        }
      };

      const clearBrowserSessionOnly = async () => {
        const session = analogAudioSession;
        analogAudioSession = null;
        if (!session) return;
        session.stopped = true;
        if (session.reconnectTimer) {
          window.clearTimeout(session.reconnectTimer);
          session.reconnectTimer = 0;
        }
        try { if (session.controller) session.controller.abort(); } catch (_error) {}
        try {
          for (const source of session.sources || []) {
            try { source.stop(); } catch (_error) {}
          }
        } catch (_error) {}
        try { if (session.context) await session.context.close(); } catch (_error) {}
        if (session.button && session.button.isConnected) session.button.textContent = "Listen";
      };

      setStatus(`Preparing backend audio test for ${displayName}...`);
      await clearBrowserSessionOnly();

      setStatus("Sending bounded backend audio cleanup...");
      try {
        await postJsonVerified(audioUrls.stopUrl, 1800, "live/stop cleanup", false);
      } catch (error) {
        setStatus(`Cleanup warning: ${error.message || error}. Continuing to live/start...`);
        await new Promise((resolve) => window.setTimeout(resolve, 150));
      }

      setStatus("Planning backend radio target...");
      try {
        await getJsonBestEffort(`/api/radio/plan?${new URLSearchParams({
          name: pass.name || "",
          norad: String(pass.norad_id || ""),
          aos: pass.aos_utc || "",
          downlink: String(audioUrls.downlink),
          mode: modeForStart(),
          description: (pass.radio && pass.radio.description) || ""
        }).toString()}`, 2200, "radio/plan");
      } catch (_error) {
      }

      const startUrl = urlWithMode(audioUrls.startUrl);
      setStatus(`Starting backend live audio: ${new URL(startUrl, window.location.origin).searchParams.toString()}`);
      const startResult = await postJsonVerified(startUrl, 6500, "live/start", true);
      setStatus(`live/start OK (${compactBody(startResult.text) || "HTTP 200"}). Opening WAV stream...`);

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
      if (button && button.isConnected) button.textContent = "Stop";

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
        setStatus(`Playing backend audio from Pluto (${bufferedSeconds.toFixed(1)}s buffered, ${session.totalBytes} bytes).`);
      };

      const controller = new AbortController();
      session.controller = controller;
      const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=0`;
      const response = await fetch(streamUrl, { cache: "no-store", signal: controller.signal });
      if (!response.ok) {
        const body = await response.text().catch(() => "");
        throw new Error(`${streamUrl}: ${response.status}${body ? ` | ${compactBody(body)}` : ""}`);
      }
      if (!response.body || !response.body.getReader) {
        throw new Error("Browser streaming fetch is not available.");
      }

      setStatus("WAV stream opened; receiving backend audio...");
      const reader = response.body.getReader();
      let pending = new Uint8Array();
      let headerSkipped = false;

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

      (async () => {
        try {
          while (!session.stopped && analogAudioSession === session) {
            const { value, done } = await reader.read();
            if (done) break;
            if (!value || !value.length) continue;
            session.totalBytes += value.length;
            pending = concatUint8Arrays(pending, value);
            processPending(false);
            if (session.totalBuffers === 0) setStatus(`Receiving backend audio stream (${session.totalBytes} bytes)...`);
          }
          processPending(true);
          if (!session.stopped && analogAudioSession === session) {
            setStatus(`Backend WAV stream ended after ${session.totalBytes} bytes. Press Listen to restart.`);
            await stopAnalogAudio(`Backend WAV stream ended after ${session.totalBytes} bytes.`);
          }
        } catch (error) {
          if (session.stopped || analogAudioSession !== session) return;
          const message = error && error.message ? error.message : "Backend audio stream failed.";
          setStatus(message);
          await stopAnalogAudio(message);
        }
      })();
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



    /* MODE_AWARE_RECEIVE_UI_PHASE1_V2_8_2
     * Phase 1 separates analog voice listening from future decoder receive work.
     * FM/AM voice modes keep using the proven Listen audio path. CW, packet,
     * APRS, AFSK, FSK, telemetry, and other data-like modes expose a Receive
     * button that explains the planned decoder path without starting unsupported
     * DSP yet.
     */
    function passModeTextV282(pass) {
      const values = [];
      const radio = pass && pass.radio ? pass.radio : {};
      if (radio.mode) values.push(radio.mode);
      if (radio.description) values.push(radio.description);
      for (const mode of (pass && pass.modes ? pass.modes : [])) {
        if (mode) values.push(mode);
      }
      return values.join(" ").toUpperCase();
    }

    function isDecodeReceiveModeV282(pass) {
      const text = passModeTextV282(pass);
      if (!text) return false;
      return /(CW|MORSE|APRS|PACKET|AFSK|FSK|GFSK|BPSK|QPSK|GMSK|G3RUH|AX\.25|AX25|RTTY|SSTV|PSK31|1200|9600|DATA|DIGITAL|TELEMETRY|BEACON)/.test(text);
    }

    function isVoiceListenModeV282(pass) {
      const text = passModeTextV282(pass);
      if (!text) return true;
      if (isDecodeReceiveModeV282(pass)) return false;
      return /(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E)/.test(text) || true;
    }

    function receiveDecoderLabelV282(pass) {
      const text = passModeTextV282(pass);
      if (/APRS/.test(text)) return "APRS / AX.25 packet decoder";
      if (/(PACKET|AX\.25|AX25)/.test(text)) return "packet / AX.25 decoder";
      if (/(CW|MORSE)/.test(text)) return "CW / Morse decoder";
      if (/(AFSK|1200)/.test(text)) return "1200 baud AFSK decoder";
      if (/(G3RUH|9600|GMSK|FSK|GFSK)/.test(text)) return "FSK/GMSK data decoder";
      if (/(TELEMETRY|DATA|DIGITAL|BEACON)/.test(text)) return "digital telemetry decoder";
      return "future decoder";
    }

    function receiveModeSummaryV282(pass) {
      const radio = pass && pass.radio ? pass.radio : {};
      const mode = radio.mode || (pass && pass.modes && pass.modes[0]) || "unknown";
      const downlink = radio.downlink_hz || (pass && pass.downlinks_hz ? pass.downlinks_hz[0] : 0);
      const parts = [];
      parts.push(`<div><strong>Satellite</strong>${escapeHtml((pass && pass.name) || "Selected pass")}</div>`);
      parts.push(`<div><strong>Mode</strong>${escapeHtml(mode)}</div>`);
      parts.push(`<div><strong>Downlink</strong>${escapeHtml(downlink ? formatHz(downlink) : "No downlink")}</div>`);
      parts.push(`<div><strong>Decoder</strong>${escapeHtml(receiveDecoderLabelV282(pass))}</div>`);
      parts.push(`<div><strong>Status</strong>Decoder DSP is not implemented yet. Phase 1 only routes non-voice modes to Receive instead of analog Listen.</div>`);
      return parts.join("");
    }

    function ensureReceivePlaceholderModalV282() {
      let modal = document.getElementById("receivePlaceholderModalV282");
      if (modal) return modal;
      modal = document.createElement("div");
      modal.id = "receivePlaceholderModalV282";
      modal.className = "receive-placeholder-modal-v282";
      modal.hidden = true;
      modal.innerHTML = `
        <div class="receive-placeholder-card-v282" role="dialog" aria-modal="true" aria-labelledby="receivePlaceholderTitleV282">
          <div class="receive-placeholder-header-v282">
            <h2 id="receivePlaceholderTitleV282">Receive Decoder</h2>
            <button id="receivePlaceholderCloseButtonV282" class="icon-button" type="button">Close</button>
          </div>
          <div id="receivePlaceholderBodyV282" class="receive-placeholder-body-v282"></div>
        </div>`;
      modal.addEventListener("click", (event) => {
        if (event.target === modal) modal.hidden = true;
      });
      document.body.appendChild(modal);
      document.getElementById("receivePlaceholderCloseButtonV282")?.addEventListener("click", () => {
        modal.hidden = true;
      });
      return modal;
    }

    function openReceivePlaceholderModalV282(pass) {
      const modal = ensureReceivePlaceholderModalV282();
      const body = document.getElementById("receivePlaceholderBodyV282");
      if (body) {
        body.innerHTML = `
          <p class="meta">This pass appears to use a non-voice satellite mode. Future releases will start the right decoder from this Receive control.</p>
          <div class="receive-placeholder-grid-v282">${receiveModeSummaryV282(pass)}</div>
          <div class="help-note">FM/AM voice satellites continue to use Listen. Data/CW/packet satellites now route here so decoder work can be added without disturbing the proven audio path.</div>`;
      }
      modal.hidden = false;
    }

    function bindReceiveDecodePlaceholderV282(pass, node) {
      const button = node.querySelector("#receiveDecodePlaceholderButtonV282");
      const status = node.querySelector("#receiveDecodeStatusV282");
      if (!button) return;
      const decodeMode = isDecodeReceiveModeV282(pass);
      button.hidden = !decodeMode;
      button.disabled = !decodeMode;
      button.textContent = "Receive";
      button.title = decodeMode ? `Open ${receiveDecoderLabelV282(pass)} placeholder` : "Receive is shown for CW/data/packet/APRS modes.";
      if (status) {
        status.hidden = true;
        status.textContent = decodeMode ? `${receiveDecoderLabelV282(pass)} placeholder ready.` : "";
      }
      if (!decodeMode) return;
      button.addEventListener("click", () => openReceivePlaceholderModalV282(pass));
    }



    /* RECEIVE_CAPABILITY_GUIDANCE_V2_8_3
     * Adds operator-facing decode guidance to the Receive placeholder without
     * touching backend DSP.  This makes the next decoder phases explicit while
     * preserving the proven Listen path for analog voice passes.
     */
    function receiveModeFamilyV283(pass) {
      const text = passModeTextV282(pass);
      if (/APRS/.test(text)) return { label: "APRS", detail: "APRS packet frames over AX.25." };
      if (/(PACKET|AX\.25|AX25)/.test(text)) return { label: "Packet", detail: "AX.25 packet receive/decode path." };
      if (/(CW|MORSE)/.test(text)) return { label: "CW", detail: "Morse tone detection and text decode path." };
      if (/(AFSK|1200)/.test(text)) return { label: "AFSK 1200", detail: "1200 baud audio FSK packet-style decode path." };
      if (/(G3RUH|9600)/.test(text)) return { label: "G3RUH 9600", detail: "9600 baud FSK/GMSK baseband decode path." };
      if (/(FSK|GFSK|GMSK)/.test(text)) return { label: "FSK/GMSK", detail: "Frequency-shift keyed telemetry decode path." };
      if (/(TELEMETRY|DATA|DIGITAL|BEACON)/.test(text)) return { label: "Telemetry", detail: "Generic digital telemetry capture/decode path." };
      return { label: "Digital", detail: "Future non-voice receive/decode path." };
    }

    function receiveCapabilityRowsV283(pass) {
      const radio = pass && pass.radio ? pass.radio : {};
      const mode = radio.mode || (pass && pass.modes && pass.modes[0]) || "unknown";
      const downlink = radio.downlink_hz || (pass && pass.downlinks_hz ? pass.downlinks_hz[0] : 0);
      const family = receiveModeFamilyV283(pass);
      const tunable = isPassTunable(pass) ? "Yes" : "No";
      return [
        ["Satellite", (pass && pass.name) || "Selected pass"],
        ["Detected family", family.label],
        ["Mode", mode],
        ["Downlink", downlink ? formatHz(downlink) : "No downlink"],
        ["Pluto tunable", tunable],
        ["Future decoder", receiveDecoderLabelV282(pass)]
      ];
    }

    function receiveStepListV283(pass) {
      const family = receiveModeFamilyV283(pass);
      const rows = [
        ["1", "Track", "Use the existing pass selection and Doppler planning so the receiver knows the satellite and downlink."],
        ["2", "Capture", "Future work will start the correct IQ/audio capture path for this mode family."],
        ["3", "Decode", family.detail],
        ["4", "Display", "Future work will show decoded frames/text/telemetry in this Receive dialog."]
      ];
      return rows.map(([num, title, detail]) => `
        <div class="receive-step-v283">
          <div class="receive-step-number-v283">${escapeHtml(num)}</div>
          <div><strong>${escapeHtml(title)}</strong><span>${escapeHtml(detail)}</span></div>
        </div>`).join("");
    }

    function receiveCapabilityGridV283(pass) {
      return receiveCapabilityRowsV283(pass).map(([label, value]) => `
        <div class="receive-capability-cell-v283">
          <strong>${escapeHtml(label)}</strong>
          <span>${escapeHtml(value)}</span>
        </div>`).join("");
    }

    function openReceivePlaceholderModalV282(pass) {
      const modal = ensureReceivePlaceholderModalV282();
      const body = document.getElementById("receivePlaceholderBodyV282");
      const family = receiveModeFamilyV283(pass);
      if (body) {
        body.innerHTML = `
          <div class="receive-guidance-hero-v283">
            <div>
              <div class="receive-badge-v283">${escapeHtml(family.label)}</div>
              <h3>Receive ${escapeHtml(receiveDecoderLabelV282(pass))}</h3>
              <p class="meta">This is a planning placeholder only. It does not start unsupported DSP, change the receiver, or interfere with the proven Listen audio path.</p>
            </div>
          </div>
          <div class="receive-capability-grid-v283">${receiveCapabilityGridV283(pass)}</div>
          <h3>Future receive flow</h3>
          <div class="receive-step-list-v283">${receiveStepListV283(pass)}</div>
          <div class="help-note">Voice-like FM/AM satellites continue to use Listen. Non-voice modes now land here so CW, APRS, packet, and telemetry decoder work can be added one mode at a time.</div>`;
      }
      modal.hidden = false;
    }

/* SPECTRUM_BUTTON_ROW_CLEANUP_V2_5_1 */
function bindAnalogAudio(pass, node) {
      const button = node.querySelector("#analogAudioToggleButton");
      const status = node.querySelector("#analogAudioStatus");
      const audioUrls = analogAudioUrl(pass);
      if (!button || !status) return;

      if (typeof isDecodeReceiveModeV282 === "function" && isDecodeReceiveModeV282(pass)) {
        button.disabled = true;
        button.title = "Use Receive for CW/data/packet/APRS modes.";
        status.textContent = "Use Receive for this non-voice mode.";
        return;
      }

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
      publishSelectedPassRotatorPlanV245(currentSelectedPass).catch(() => {});

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

/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6_BEGIN
 * Make pass-list capability badges into operator action buttons, use the same
 * light-blue action style under the sky/azimuth chart, and keep Listen/Receive
 * disabled until the selected pass is actively overhead.
 */
function passActionTimingStateV286(pass) {
  try {
    if (typeof passTimingState === "function") return passTimingState(pass);
  } catch (_error) {
  }
  return "unknown";
}

function passActionIsActiveV286(pass) {
  return passActionTimingStateV286(pass) === "active";
}

function passActionKindV286(pass) {
  try {
    if (typeof receiveKindForPassV2626D === "function") {
      const kind = receiveKindForPassV2626D(pass);
      if (kind === "cw" || kind === "digital") return "receive";
      return "listen";
    }
  } catch (_error) {
  }
  try {
    if (typeof isDecodeReceiveModeV282 === "function" && isDecodeReceiveModeV282(pass)) return "receive";
  } catch (_error) {
  }
  return "listen";
}

function passActionLabelV286(pass) {
  return passActionKindV286(pass) === "receive" ? "Receive" : "Listen";
}

function passActionDownlinkHzV286(pass) {
  const radio = (pass && pass.radio) || {};
  return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
}

function passActionTargetAvailableV286(pass) {
  if (!pass) return false;
  if (!passActionDownlinkHzV286(pass)) return false;
  try {
    if (typeof isPassTunable === "function") return !!isPassTunable(pass);
  } catch (_error) {
  }
  return true;
}

function passActionInactiveTextV286(pass) {
  const label = passActionLabelV286(pass);
  if (!pass) return `${label} is inactive until a pass is selected.`;
  if (!passActionTargetAvailableV286(pass)) return `${label} is unavailable because this pass has no tunable downlink.`;
  const state = passActionTimingStateV286(pass);
  if (state === "upcoming") return `${label} becomes active at AOS (${formatTime(pass.aos_utc)}).`;
  if (state === "stale") return `${label} is inactive because this pass has ended.`;
  return `${label} is inactive until the pass is active.`;
}

function applyPassActionButtonVisualsV286(button, pass) {
  if (!button) return false;
  const label = passActionLabelV286(pass);
  const active = passActionIsActiveV286(pass);
  const target = passActionTargetAvailableV286(pass);
  button.classList.add("pass-action-button-v286");
  button.classList.toggle("pass-action-receive-v286", label === "Receive");
  button.classList.toggle("pass-action-listen-v286", label === "Listen");
  button.dataset.passActionKindV286 = label.toLowerCase();
  button.dataset.passActiveV286 = active ? "1" : "0";
  if (!/^Stop/i.test(String(button.textContent || ""))) {
    button.textContent = label;
  }
  button.disabled = !(active && target);
  button.title = active && target ? `${label} active for this pass.` : passActionInactiveTextV286(pass);
  button.setAttribute("aria-disabled", button.disabled ? "true" : "false");
  return active && target;
}

function configurePassRowActionButtonV286(button, pass, onSelect) {
  if (!button) return;
  applyPassActionButtonVisualsV286(button, pass);
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    if (!passActionIsActiveV286(pass) || !passActionTargetAvailableV286(pass)) {
      button.title = passActionInactiveTextV286(pass);
      return;
    }
    if (typeof onSelect === "function") onSelect();
    window.setTimeout(() => {
      const primary = document.getElementById("analogAudioToggleButton");
      if (primary && !primary.disabled) primary.click();
    }, 80);
  });
}
/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6_END */

/* PASS_ACTION_ALWAYS_ENABLED_FOR_TEST_V2_8_22
 * Backend-test mode: keep pass Listen/Receive action buttons enabled whenever
 * the pass has a tunable downlink.  The button is no longer gated by AOS/LOS,
 * so operators can test the audio/decode backend outside a live pass.  Missing
 * or untunable downlinks still stay disabled because the backend has no valid
 * RF target to tune.
 */
function passActionManualTestAvailableV2822(pass) {
  if (!pass) return false;
  if (!passActionDownlinkHzV286(pass)) return false;
  try {
    if (typeof isPassTunable === "function") return !!isPassTunable(pass);
  } catch (_error) {
  }
  return true;
}

function passActionInactiveTextV286(pass) {
  const label = passActionLabelV286(pass);
  if (!pass) return `${label} is unavailable until a pass is selected.`;
  if (!passActionDownlinkHzV286(pass)) return `${label} is unavailable because this pass has no downlink.`;
  try {
    if (typeof isPassTunable === "function" && !isPassTunable(pass)) {
      return `${label} is unavailable because this downlink is outside the Pluto tuning range.`;
    }
  } catch (_error) {
  }
  return `${label} is enabled for backend testing outside pass time.`;
}

function applyPassActionButtonVisualsV286(button, pass) {
  if (!button) return false;
  const label = passActionLabelV286(pass);
  const active = passActionIsActiveV286(pass);
  const target = passActionManualTestAvailableV2822(pass);
  const state = passActionTimingStateV286(pass);
  button.classList.add("pass-action-button-v286");
  button.classList.toggle("pass-action-receive-v286", String(label).startsWith("Receive"));
  button.classList.toggle("pass-action-listen-v286", String(label).startsWith("Listen"));
  button.classList.toggle("pass-action-test-enabled-v2822", target && !active);
  button.dataset.passActionKindV286 = String(label).toLowerCase();
  button.dataset.passActiveV286 = active ? "1" : "0";
  button.dataset.passActionTestEnabledV2822 = target ? "1" : "0";
  if (!/^Stop/i.test(String(button.textContent || ""))) {
    button.textContent = label;
  }
  button.disabled = !target;
  button.title = target
    ? (active
      ? `${label} active for this pass.`
      : `${label} enabled for backend testing. Pass state: ${state}.`)
    : passActionInactiveTextV286(pass);
  button.setAttribute("aria-disabled", button.disabled ? "true" : "false");
  return target;
}

function configurePassRowActionButtonV286(button, pass, onSelect) {
  if (!button) return;
  applyPassActionButtonVisualsV286(button, pass);
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    if (!passActionManualTestAvailableV2822(pass)) {
      button.title = passActionInactiveTextV286(pass);
      return;
    }
    if (typeof onSelect === "function") onSelect();
    window.setTimeout(() => {
      const primary = document.getElementById("analogAudioToggleButton");
      if (primary && !primary.disabled) {
        primary.click();
      } else if (primary) {
        primary.disabled = false;
        primary.click();
      }
    }, 80);
  });
}
/* PASS_ACTION_ALWAYS_ENABLED_FOR_TEST_V2_8_22_END */


    

/* PASS_ROW_CLICK_ANYTIME_MAP_GUARD_V2_8_24
 * Backend-test mode final override.  Pass-row action buttons are clickable
 * whenever a pass has any usable downlink value, independent of AOS/LOS and
 * independent of broader pass readiness/tunability helpers.  This is meant for
 * manual backend audio/decode testing from the pass list.  No-downlink rows stay
 * disabled because there is no RF target.
 */
function passActionBackendTestDownlinkHzV2824(pass) {
  const radio = (pass && pass.radio) || {};
  const downlinks = (pass && Array.isArray(pass.downlinks_hz)) ? pass.downlinks_hz : [];
  const candidates = [
    radio.downlink_hz,
    pass && pass.downlink_hz,
    downlinks[0]
  ];
  for (const candidate of candidates) {
    const value = Number(candidate || 0);
    if (Number.isFinite(value) && value > 0) return Math.round(value);
  }
  return 0;
}

function passActionManualTestAvailableV2824(pass) {
  return passActionBackendTestDownlinkHzV2824(pass) > 0;
}

function passActionInactiveTextV286(pass) {
  const label = passActionLabelV286(pass);
  if (!pass) return `${label} is unavailable until a pass is selected.`;
  if (!passActionManualTestAvailableV2824(pass)) return `${label} is unavailable because this pass has no downlink.`;
  return `${label} is enabled for backend testing outside pass time.`;
}

function applyPassActionButtonVisualsV286(button, pass) {
  if (!button) return false;
  const label = passActionLabelV286(pass);
  const active = passActionIsActiveV286(pass);
  const target = passActionManualTestAvailableV2824(pass);
  const state = passActionTimingStateV286(pass);
  button.classList.add("pass-action-button-v286");
  button.classList.toggle("pass-action-receive-v286", String(label).startsWith("Receive"));
  button.classList.toggle("pass-action-listen-v286", String(label).startsWith("Listen"));
  button.classList.toggle("pass-action-test-enabled-v2824", target && !active);
  button.dataset.passActionKindV286 = String(label).toLowerCase();
  button.dataset.passActiveV286 = active ? "1" : "0";
  button.dataset.passActionTestEnabledV2824 = target ? "1" : "0";
  if (!/^Stop/i.test(String(button.textContent || ""))) {
    button.textContent = label;
  }
  button.disabled = !target;
  button.title = target
    ? (active
      ? `${label} active for this pass.`
      : `${label} enabled for backend testing. Pass state: ${state}.`)
    : passActionInactiveTextV286(pass);
  button.setAttribute("aria-disabled", button.disabled ? "true" : "false");
  return target;
}

function configurePassRowActionButtonV286(button, pass, onSelect) {
  if (!button) return;
  applyPassActionButtonVisualsV286(button, pass);
  button.addEventListener("click", (event) => {
    event.preventDefault();
    event.stopPropagation();
    if (!passActionManualTestAvailableV2824(pass)) {
      button.title = passActionInactiveTextV286(pass);
      return;
    }
    if (typeof onSelect === "function") onSelect();
    window.setTimeout(() => {
      const primary = document.getElementById("analogAudioToggleButton");
      if (primary) {
        primary.disabled = false;
        primary.removeAttribute("disabled");
        primary.setAttribute("aria-disabled", "false");
        primary.click();
        return;
      }
      if (typeof startAnalogAudio === "function") {
        const statusNode = document.getElementById("status") || { textContent: "" };
        startAnalogAudio(pass, button, statusNode).catch((error) => {
          const message = error && error.message ? error.message : String(error);
          if (statusNode) statusNode.textContent = message;
          button.title = message;
        });
      }
    }, 80);
  });
}
/* PASS_ROW_CLICK_ANYTIME_MAP_GUARD_V2_8_24_END */


/* PASS_ROW_DIRECT_ACTION_HANDLER_V2_8_25
 * Backend-test mode for pass-row action buttons.
 *
 * The previous enable patches still relied on the original button click path.
 * If that path decides the pass is not actionable, the button can look enabled
 * but still do nothing.  This override is intentionally direct:
 *   - renderPasses still builds each row normally
 *   - configurePassRowActionButtonV286 forces the row button interactive
 *   - clicking the row button selects the pass, then starts the existing
 *     backend-owned audio path through the primary analog button when present
 *   - if the old primary button is hidden/absent, it calls startAnalogAudio()
 *     directly with a small status node
 *
 * No backend C or radio API behavior is changed.
 */
function passRowActionStatusNodeV2825() {
  let node = document.getElementById("passRowDirectActionStatusV2825");
  if (!node) {
    node = document.createElement("div");
    node.id = "passRowDirectActionStatusV2825";
    node.className = "muted pass-row-direct-action-status-v2825";
    node.style.marginTop = "0.35rem";
    const passes = document.getElementById("passes");
    if (passes && passes.parentNode) {
      passes.parentNode.insertBefore(node, passes.nextSibling);
    } else if (document.body) {
      document.body.appendChild(node);
    }
  }
  return node;
}

function passRowDirectActionHasDownlinkV2825(pass) {
  try {
    return !!(passActionDownlinkHzV286(pass) || ((pass && pass.downlinks_hz) || [])[0]);
  } catch (_error) {
    return !!(((pass && pass.downlinks_hz) || [])[0]);
  }
}

function passRowForceInteractiveV2825(button, pass) {
  if (!button) return false;
  const hasDownlink = passRowDirectActionHasDownlinkV2825(pass);
  try {
    if (typeof applyPassActionButtonVisualsV286 === "function") {
      applyPassActionButtonVisualsV286(button, pass);
    }
  } catch (_error) {
  }
  button.classList.add("pass-action-button-v286", "pass-action-test-enabled-v2825");
  if (!/^Stop/i.test(String(button.textContent || ""))) {
    try {
      button.textContent = typeof passActionLabelV286 === "function" ? passActionLabelV286(pass) : "Listen";
    } catch (_error) {
      button.textContent = "Listen";
    }
  }
  button.disabled = !hasDownlink;
  if (hasDownlink) button.removeAttribute("disabled");
  button.setAttribute("aria-disabled", hasDownlink ? "false" : "true");
  button.style.pointerEvents = hasDownlink ? "auto" : "";
  button.style.opacity = hasDownlink ? "1" : "";
  button.title = hasDownlink
    ? "Enabled for backend testing. Pass does not need to be active."
    : "Unavailable because this pass has no downlink.";
  return hasDownlink;
}

function configurePassRowActionButtonV286(button, pass, onSelect) {
  if (!button) return;
  passRowForceInteractiveV2825(button, pass);
  button.addEventListener("click", async (event) => {
    event.preventDefault();
    event.stopPropagation();
    if (event.stopImmediatePropagation) event.stopImmediatePropagation();

    if (!passRowDirectActionHasDownlinkV2825(pass)) {
      button.title = "Unavailable because this pass has no downlink.";
      return;
    }

    passRowForceInteractiveV2825(button, pass);
    const statusNode = passRowActionStatusNodeV2825();
    statusNode.textContent = `Starting backend test action for ${pass && pass.name ? pass.name : "selected pass"}...`;

    try {
      if (typeof onSelect === "function") onSelect();
    } catch (_error) {
    }

    window.setTimeout(async () => {
      try {
        const primary = document.getElementById("analogAudioToggleButton");
        if (primary) {
          primary.disabled = false;
          primary.removeAttribute("disabled");
          primary.setAttribute("aria-disabled", "false");
          primary.style.pointerEvents = "auto";
          primary.click();
          return;
        }
        if (typeof startAnalogAudio === "function") {
          if (/^Stop/i.test(String(button.textContent || "")) && typeof stopAnalogAudio === "function") {
            await stopAnalogAudio("Pass-row backend test stopped.");
          } else {
            await startAnalogAudio(pass, button, statusNode);
          }
          return;
        }
        statusNode.textContent = "No audio action path is available in this UI build.";
      } catch (error) {
        const message = error && error.message ? error.message : String(error || "Pass-row action failed.");
        statusNode.textContent = message;
        const statusBar = document.getElementById("status");
        if (statusBar) statusBar.textContent = message;
        try { console.error("Pass-row direct action failed", error); } catch (_error) {}
      } finally {
        passRowForceInteractiveV2825(button, pass);
      }
    }, 100);
  }, true);
}

function repairPassRowButtonsV2825() {
  document.querySelectorAll(".pass-row-action-button-v286").forEach((button) => {
    if (button.disabled && button.textContent && button.closest(".pass-row")) {
      button.disabled = false;
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
      button.style.pointerEvents = "auto";
      button.style.opacity = "1";
      button.title = "Enabled for backend testing. Pass does not need to be active.";
    }
  });
}

window.plutoRepairPassRowButtonsV2825 = repairPassRowButtonsV2825;
window.setInterval(repairPassRowButtonsV2825, 1500);



/* PASS_ROW_SELECTED_BUTTON_UNLOCK_V2_8_26
 * Selected-pass backend-test unlock.
 *
 * v2.8.25 proved the pass-row action path can be enabled outside AOS/LOS,
 * but selecting a pass can still re-apply the legacy selected-pass disabled
 * state.  This patch does not change backend C or radio APIs.  It makes the
 * row action state authoritative after any pass selection/detail render:
 *   - pass-row action buttons are forced interactive after renderPassDetail()
 *   - a MutationObserver removes late disabled/aria-disabled changes
 *   - CSS pointer/opacity is overridden for selected rows
 *   - a console helper is exposed for manual repair/debugging
 */
function passRowButtonLooksUsableV2826(button) {
  return !!(button && button.closest && button.closest(".pass-row") && button.classList && button.classList.contains("pass-row-action-button-v286"));
}

function passRowUnlockButtonV2826(button) {
  if (!passRowButtonLooksUsableV2826(button)) return false;
  const row = button.closest(".pass-row");
  const rowText = row ? String(row.innerText || "") : "";
  const hasVisibleDownlink = /\b\d+(?:\.\d+)?\s*MHz\b/i.test(rowText);

  button.disabled = !hasVisibleDownlink;
  if (hasVisibleDownlink) {
    button.removeAttribute("disabled");
    button.setAttribute("aria-disabled", "false");
    button.style.pointerEvents = "auto";
    button.style.opacity = "1";
    button.style.cursor = "pointer";
    button.classList.add("pass-action-selected-unlocked-v2826");
    if (!button.title || /inactive|disabled|unavailable|AOS|ended/i.test(button.title)) {
      button.title = "Enabled for backend testing. Pass does not need to be active.";
    }
  } else {
    button.setAttribute("aria-disabled", "true");
    button.title = "Unavailable because this row does not show a downlink.";
  }
  return hasVisibleDownlink;
}

function unlockAllPassRowButtonsV2826() {
  let count = 0;
  document.querySelectorAll(".pass-row-action-button-v286").forEach((button) => {
    if (passRowUnlockButtonV2826(button)) count += 1;
  });
  return count;
}

function schedulePassRowUnlockV2826() {
  [0, 25, 100, 300, 800].forEach((delayMs) => {
    window.setTimeout(unlockAllPassRowButtonsV2826, delayMs);
  });
}

function installPassRowSelectedUnlockV2826() {
  if (window.__passRowSelectedUnlockV2826) return;
  window.__passRowSelectedUnlockV2826 = true;

  try {
    const style = document.createElement("style");
    style.id = "passRowSelectedUnlockStyleV2826";
    style.textContent = `
      .pass-row .pass-row-action-button-v286.pass-action-selected-unlocked-v2826,
      .pass-row.selected .pass-row-action-button-v286.pass-action-selected-unlocked-v2826 {
        pointer-events: auto !important;
        opacity: 1 !important;
        cursor: pointer !important;
        filter: none !important;
      }
    `;
    document.head.appendChild(style);
  } catch (_error) {
  }

  const originalRenderPassDetail = (typeof renderPassDetail === "function") ? renderPassDetail : null;
  if (originalRenderPassDetail && !originalRenderPassDetail.__selectedUnlockWrappedV2826) {
    const wrapped = function renderPassDetailSelectedUnlockV2826(...args) {
      const result = originalRenderPassDetail.apply(this, args);
      schedulePassRowUnlockV2826();
      return result;
    };
    wrapped.__selectedUnlockWrappedV2826 = true;
    renderPassDetail = wrapped;
  }

  document.addEventListener("click", (event) => {
    if (event && event.target && event.target.closest && event.target.closest(".pass-row")) {
      schedulePassRowUnlockV2826();
    }
  }, true);

  window.setTimeout(() => {
    const passes = document.getElementById("passes");
    if (!passes || !window.MutationObserver) return;
    try {
      const observer = new MutationObserver(() => schedulePassRowUnlockV2826());
      observer.observe(passes, {
        subtree: true,
        childList: true,
        attributes: true,
        attributeFilter: ["disabled", "aria-disabled", "class", "style"]
      });
      window.__passRowSelectedUnlockObserverV2826 = observer;
    } catch (_error) {
    }
  }, 0);

  window.plutoUnlockPassRowButtonsV2826 = unlockAllPassRowButtonsV2826;
  window.plutoSchedulePassRowUnlockV2826 = schedulePassRowUnlockV2826;
  window.setInterval(unlockAllPassRowButtonsV2826, 750);
  schedulePassRowUnlockV2826();
}
installPassRowSelectedUnlockV2826();
/* PASS_ROW_SELECTED_BUTTON_UNLOCK_V2_8_26_END */


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
      /* PASS_FILTER_BUTTONS_REMOVED_V2_8_6F
       * The pass list intentionally uses the default useful pass set without
       * the Upcoming/Ready/Active/All filter button row.
       */
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
          <div class="pass-row-actions-v286">
            <button class="pass-row-action-button-v286 pass-action-button-v286" type="button">Radio</button>
            <button class="pass-detail-button" type="button">Details</button>
          </div>
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

        const passActionButtonV286 = row.querySelector(".pass-row-action-button-v286");
        if (passActionButtonV286 && typeof configurePassRowActionButtonV286 === "function") {
          configurePassRowActionButtonV286(passActionButtonV286, pass, () => selectPass(pass, row));
        }

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

    installCollapsibleDrawerSectionsV248();

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
        closeRotatorModalV248();
        closeSpectrumWaterfallModalV250();
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

  let rotatorConfigDirtyV2410 = false;
  let rotatorFormLoadingV2410 = false;

  function rotatorTypeUsesHostV2410(type) {
    return String(type || "simulation") !== "simulation";
  }

  function rotatorSuggestedHostV2410() {
    // ROTATOR_LAN_HOST_ERROR_SOURCE_V2_4_11
    const host = window.location && window.location.hostname ? String(window.location.hostname) : "";
    if (host && host !== "localhost" && host !== "127.0.0.1" && host !== "0.0.0.0") {
      return host;
    }

    return "192.168.68.104";
  }

  function updateRotatorDirtyNoticeV2410() {
    // ROTATOR_APPLY_SAVE_HOST_UX_V2_4_10
    const notice = document.getElementById("rotatorUnsavedNotice");
    const saveButton = document.getElementById("rotatorSaveConfigBtn");
    const headerSaveButton = document.getElementById("rotatorHeaderSaveConfigBtn");

    if (notice) {
      notice.hidden = !rotatorConfigDirtyV2410;
    }

    [saveButton, headerSaveButton].forEach((button) => {
      if (!button) return;
      button.classList.toggle("rotator-save-needed", rotatorConfigDirtyV2410);
      button.textContent = rotatorConfigDirtyV2410 ? "Apply / Save Now" : "Apply / Save Config";
    });
  }

  function markRotatorConfigDirtyV2410(message) {
    if (rotatorFormLoadingV2410) return;

    rotatorConfigDirtyV2410 = true;
    updateRotatorDirtyNoticeV2410();

    if (message) {
      setRotatorStatus(message, {
        ok: true,
        unsaved: true,
        note: "Live rotator state still uses the last saved config until Apply / Save is clicked."
      }, false);
    }
  }

  function normalizeRotatorHostForTypeV2410() {
    const type = textValue("rotatorType", "simulation");
    const hostEl = document.getElementById("rotatorHost");
    if (!hostEl || !rotatorTypeUsesHostV2410(type)) return;

    const current = String(hostEl.value || "").trim();
    if (!current || current === "127.0.0.1" || current === "localhost") {
      hostEl.value = rotatorSuggestedHostV2410();
    }
  }

  function rotatorRequireSavedConfigV2410(actionName) {
    if (!rotatorConfigDirtyV2410) return true;

    setRotatorStatus(`${actionName} blocked until rotator settings are applied.`, {
      ok: false,
      unsaved: true,
      action: actionName,
      next_step: "Click Apply / Save Now, then retry the action."
    }, true);
    updateRotatorDirtyNoticeV2410();
    return false;
  }

  function bindRotatorConfigDirtyHandlersV2410() {
    const ids = [
      "rotatorEnabled",
      "rotatorType",
      "rotatorHost",
      "rotatorPort",
      "rotatorUpdateInterval",
      "rotatorMinMove",
      "rotatorAzOffset",
      "rotatorElOffset",
      "rotatorMinEl",
      "rotatorMaxEl",
      "rotatorParkOnLos",
      "rotatorParkAz",
      "rotatorParkEl"
    ];

    ids.forEach((id) => {
      const el = document.getElementById(id);
      if (!el) return;

      el.addEventListener("change", () => {
        if (id === "rotatorType") {
          normalizeRotatorHostForTypeV2410();
        }

        markRotatorConfigDirtyV2410("Rotator settings changed. Click Apply / Save Now before testing or tracking. Host is the LAN IP reachable from the Pluto.");
      });
    });

    updateRotatorDirtyNoticeV2410();
  }

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
    normalizeRotatorHostForTypeV2410();

    const type = textValue("rotatorType", "simulation");
    const defaultHost = rotatorSuggestedHostV2410();

    return {
      enabled: boolValue("rotatorEnabled"),
      type,
      host: textValue("rotatorHost", defaultHost),
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

    rotatorFormLoadingV2410 = true;
    try {
      const type = cfg.type || "simulation";
      const connection = cfg.connection || {};
      const defaultHost = rotatorSuggestedHostV2410();

      setValue("rotatorEnabled", !!cfg.enabled);
      setValue("rotatorType", type);
      setValue("rotatorHost", cfg.host || connection.host || defaultHost);
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
    } finally {
      rotatorFormLoadingV2410 = false;
    }
  }

  async function loadRotatorConfig() {
    const data = await rotatorApi("/api/rotator/config");
    if (data.http_ok) {
      fillRotatorForm(data);
      rotatorConfigDirtyV2410 = false;
      updateRotatorDirtyNoticeV2410();
      setRotatorStatus("Rotator config loaded. Form matches saved active config.", data, false);
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

    setRotatorStatus(data.http_ok ? "Rotator config applied and saved." : "Failed to apply/save rotator config.", data, !data.http_ok);
    if (data.http_ok) {
      rotatorConfigDirtyV2410 = false;
      updateRotatorDirtyNoticeV2410();
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

function rotatorTargetSourceLabelV246(data) {
  // ROTATOR_SOURCE_READOUT_V2_4_6
  // ROTATOR_UI_SUCCESS_STATE_V2_4_13
  const raw = [
    data && data.source,
    data && data.message,
    data && data.last_result,
    data && data.state,
    data && data.type
  ]
    .filter((value) => value !== undefined && value !== null)
    .map((value) => String(value).toLowerCase())
    .join(" ");

  const typeText = String((data && data.type) || "").toLowerCase();
  const stateText = String((data && data.state) || "").toLowerCase();
  const resultText = String((data && data.last_result) || "").toLowerCase();
  const success =
    resultText === "written" ||
    resultText === "simulated" ||
    stateText === "commanded" ||
    stateText === "simulated" ||
    stateText === "parked";

  if (success) {
    if (typeText === "easycomm2" || raw.includes("easycomm2")) return "easycomm2 / TCP";
    if (typeText === "hamlib_rotctld" || raw.includes("hamlib") || raw.includes("rotctld")) return "hamlib rotctld / TCP";
    if (typeText === "yaesu_gs232" || raw.includes("yaesu")) return "yaesu gs-232 / TCP";
    if (typeText === "simulation" || raw.includes("simulation target")) return "simulation";
    return "command accepted";
  }

  if (raw.includes("selected_plan_point") || raw.includes("radio_track.json:selected")) {
    return "selected pass";
  }
  if (raw.includes("radio_track_state.json")) {
    return "radio state";
  }
  if (raw.includes("nearest_pass_point")) {
    return "pass-list fallback";
  }
  if (raw.includes("simulation target") || raw.includes("test command") || raw.includes("rotator test")) {
    return "manual/test";
  }
  if (raw.includes("error")) {
    return "error - check Host/Port and protocol";
  }
  if (
    raw.includes("connection") ||
    raw.includes("connect") ||
    raw.includes("refused") ||
    raw.includes("timed out") ||
    raw.includes("timeout") ||
    raw.includes("network") ||
    raw.includes("unreachable")
  ) {
    return "network adapter error";
  }
  if (raw.includes("waiting") || raw.includes("idle") || raw.includes("stopped") || raw.includes("not_started")) {
    return "none";
  }
  if (data && data.source) {
    return String(data.source);
  }
  return "not reported";
}

function updateRotatorLiveTargetV242(data) {
  // ROTATOR_UI_LIVE_TARGET_WAITING_FIX_V2_4_2B
  const box = document.getElementById("rotatorLiveTarget");
  if (!box) {
    return;
  }

  const stateText = data && data.state ? String(data.state) : "unknown";
  const stateLower = stateText.toLowerCase();
  const sourceLabel = rotatorTargetSourceLabelV246(data);
  const sourceText = sourceLabel ? ` · Rotator source: ${sourceLabel}` : "";
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
    if (!rotatorRequireSavedConfigV2410("Test Move")) {
      return null;
    }

    const az = numberValue("rotatorTestAz", 180);
    const el = numberValue("rotatorTestEl", 45);
    return rotatorPost(`/api/rotator/test?az=${encodeURIComponent(az)}&el=${encodeURIComponent(el)}`,
      "Rotator test command sent.",
      "Rotator test failed.");
  }

  async function startRotatorTrackingV2410() {
    if (!rotatorRequireSavedConfigV2410("Start Rotator Tracking")) {
      return null;
    }

    return rotatorPost("/api/rotator/track/start", "Rotator tracking started.", "Rotator tracking start failed.");
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

  function openRotatorModalV248() {
    const modal = document.getElementById("rotatorControlModal");
    if (!modal) return;

    modal.hidden = false;
    modal.classList.add("open");
    document.body.classList.add("rotator-modal-open");
    loadRotatorState().catch(() => {});
  }

  function closeRotatorModalV248() {
    const modal = document.getElementById("rotatorControlModal");
    if (!modal) return;

    modal.classList.remove("open");
    modal.hidden = true;
    document.body.classList.remove("rotator-modal-open");
  }

  function bindRotatorModalOpenButtonsV248() {
    ["openRotatorMenuButton", "openRotatorFromListenButton"].forEach((id) => {
      const button = document.getElementById(id);
      if (!button) return;

      button.onclick = (event) => {
        event.preventDefault();
        openRotatorModalV248();
        if (id === "openRotatorMenuButton" && typeof closeDrawer === "function") {
          closeDrawer();
        }
      };
    });
  }

  function createRotatorPanel() {
    if (document.getElementById("rotatorControlPanel")) {
      return;
    }

    const typeOptions = ROTATOR_TYPES.map(([value, label]) => `<option value="${value}">${label}</option>`).join("");

    const modal = document.createElement("div");
    modal.id = "rotatorControlModal";
    modal.className = "rotator-modal-backdrop";
    modal.hidden = true;

    const dialog = document.createElement("div");
    dialog.className = "rotator-modal";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "rotatorControlTitle");

    const panel = document.createElement("section");
    panel.id = "rotatorControlPanel";
    panel.className = "rotator-card";
    panel.innerHTML = `
      <div class="rotator-card-header">
        <div>
          <h2 id="rotatorControlTitle">Rotator Control</h2>
          <div id="rotatorLiveTarget" class="rotator-live-target">Target Az -- / El -- · waiting</div>
          <p>Configure and test satellite antenna rotator control. Changes are not active until you click Apply / Save Config. For Hamlib/EasyComm/Yaesu, Host must be a LAN IP reachable from the Pluto.</p>
        </div>
        <div class="rotator-modal-header-actions">
          <button type="button" id="rotatorHeaderSaveConfigBtn" class="rotator-small-button rotator-apply-button">Apply / Save Config</button>
          <button type="button" id="rotatorRefreshStateBtn" class="rotator-small-button">Refresh</button>
          <button type="button" id="rotatorCloseDialogBtn" class="rotator-small-button">Close</button>
        </div>
      </div>

      <div class="rotator-grid">
        ${createField("Enabled", '<input id="rotatorEnabled" type="checkbox" />')}
        ${createField("Type", `<select id="rotatorType">${typeOptions}</select>`)}
        ${createField("Host", '<input id="rotatorHost" type="text" value="192.168.68.104" />')}
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

      <div id="rotatorUnsavedNotice" class="rotator-unsaved-notice" hidden>
        Unsaved rotator settings. Click <strong>Apply / Save Now</strong> before Preview/Test/Tracking. Live target still uses the last saved config.
      </div>

      <div class="rotator-actions">
        <button type="button" id="rotatorLoadConfigBtn">Load Saved Config</button>
        <button type="button" id="rotatorSaveConfigBtn" class="rotator-apply-button">Apply / Save Config</button>
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

    dialog.appendChild(panel);
    modal.appendChild(dialog);
    document.body.appendChild(modal);

    modal.addEventListener("click", (event) => {
      if (event.target === modal) {
        closeRotatorModalV248();
      }
    });

    document.getElementById("rotatorLoadConfigBtn")?.addEventListener("click", loadRotatorConfig);
    document.getElementById("rotatorSaveConfigBtn")?.addEventListener("click", saveRotatorConfig);
    document.getElementById("rotatorHeaderSaveConfigBtn")?.addEventListener("click", saveRotatorConfig);
    document.getElementById("rotatorRefreshStateBtn")?.addEventListener("click", loadRotatorState);
    document.getElementById("rotatorCloseDialogBtn")?.addEventListener("click", closeRotatorModalV248);
    document.getElementById("rotatorPreviewCommandBtn")?.addEventListener("click", previewRotatorProtocolCommandV244);
    document.getElementById("rotatorTestBtn")?.addEventListener("click", testRotator);
    document.getElementById("rotatorParkBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/park", "Rotator park command sent.", "Rotator park failed."));
    document.getElementById("rotatorStopBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/stop", "Rotator stop command sent.", "Rotator stop failed."));
    document.getElementById("rotatorTrackStartBtn")?.addEventListener("click", startRotatorTrackingV2410);
    document.getElementById("rotatorTrackStopBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/track/stop", "Rotator tracking stopped.", "Rotator tracking stop failed."));
    document.getElementById("rotatorTrackStepBtn")?.addEventListener("click", () => rotatorPost("/api/rotator/track/step", "Rotator tracking step complete.", "Rotator tracking step failed."));
    bindRotatorModalOpenButtonsV248();
    bindRotatorConfigDirtyHandlersV2410();
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
    open: openRotatorModalV248,
    close: closeRotatorModalV248,
    test: testRotator
  };
})();



/* UI_VERSION_BADGE_V2_6_18 */
(function () {
  const uiVersion = "v2.6.25";
  function setVersionBadge(text, className) {
    const badge = document.getElementById("appVersionBadge");
    if (!badge) return;
    badge.textContent = text;
    badge.classList.remove("version-backend-ok", "version-backend-warn");
    if (className) badge.classList.add(className);
  }
  async function refreshVersionBadgeV2618() {
    setVersionBadge(`UI ${uiVersion}`, "version-backend-warn");
    try {
      const response = await fetch("/api/status", { cache: "no-store" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const status = await response.json();
      const backendVersion = status && status.version ? String(status.version) : "unknown";
      setVersionBadge(`UI ${uiVersion} | Backend ${backendVersion}`, "version-backend-ok");
    } catch (error) {
      setVersionBadge(`UI ${uiVersion} | Backend offline`, "version-backend-warn");
    }
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", refreshVersionBadgeV2618, { once: true });
  } else {
    refreshVersionBadgeV2618();
  }
  window.refreshVersionBadgeV2618 = refreshVersionBadgeV2618;
})();


/* RECEIVE_MODE_UI_FOUNDATION_V2_6_21 */
(function receiveModeFoundationV261(){
  "use strict";
  const digitalPattern = /\b(CW|MORSE|AX\.?25|APRS|PACKET|FSK|GMSK|BPSK|QPSK|PSK|TELEMETRY|DIGITAL|DATA)\b/i;
  const voicePattern = /\b(FM|NFM|WFM|VOICE|AUDIO|PHONE|VHF|UHF)\b/i;
  const modalId = "receiveDecodeModalV261";
  let lastContext = null;

  function ensureModal(){
    let modal = document.getElementById(modalId);
    if (modal) return modal;
    modal = document.createElement("div");
    modal.id = modalId;
    modal.className = "receive-decode-modal-v261 hidden";
    modal.innerHTML = [
      '<div class="receive-decode-card-v261">',
      '  <div class="receive-decode-header-v261">',
      '    <div>',
      '      <div class="receive-decode-title-v261">Decoded Output</div>',
      '      <div class="receive-decode-subtitle-v261" id="receiveDecodeSubtitleV261">Decoder foundation</div>',
      '    </div>',
      '    <button type="button" class="receive-decode-close-v261" id="receiveDecodeCloseV261">×</button>',
      '  </div>',
      '  <pre class="receive-decode-console-v261" id="receiveDecodeConsoleV261"></pre>',
      '  <div class="receive-decode-footer-v261">FM voice still uses the existing Listen path. CW decode output now polls live while capture is active.</div>',
      '</div>'
    ].join("");
    document.body.appendChild(modal);
    const close = document.getElementById("receiveDecodeCloseV261");
    if (close) close.addEventListener("click", () => modal.classList.add("hidden"));
    modal.addEventListener("click", (ev) => { if (ev.target === modal) modal.classList.add("hidden"); });
    return modal;
  }

  function nearestContextText(el){
    const container = el && el.closest ? el.closest(".pass-card,.pass-row,.sat-card,.modal,.card,tr,li,section,article,div") : null;
    return (container && container.textContent ? container.textContent : (document.body ? document.body.textContent : "")).replace(/\s+/g, " ").slice(0, 3000);
  }

  function classify(text){
    if (digitalPattern.test(text || "")) {
      if (/\b(CW|MORSE)\b/i.test(text || "")) return "cw";
      return "digital";
    }
    if (voicePattern.test(text || "")) return "listen";
    return "listen";
  }

  function modeLabel(kind){
    if (kind === "cw") return "Decode CW";
    if (kind === "digital") return "Decode";
    return "Listen";
  }

  function extractField(text, label){
    const re = new RegExp(label + "\\s*[:=]\\s*([^|,;\\n]+)", "i");
    const m = String(text || "").match(re);
    return m ? m[1].trim().slice(0, 80) : "";
  }

  function buildContext(el){
    const text = nearestContextText(el);
    const kind = classify(text);
    const mode = extractField(text, "Mode") || (kind === "cw" ? "CW" : kind === "digital" ? "Digital" : "FM voice");
    const name = extractField(text, "Satellite") || extractField(text, "Name") || "selected satellite";
    const freqMatch = text.match(/(\d{3,4}\.\d{3,6})\s*MHz/i);
    const hzMatch = text.match(/(\d{8,10})\s*Hz/i);
    let downlink = "";
    if (hzMatch) downlink = hzMatch[1];
    else if (freqMatch) downlink = String(Math.round(parseFloat(freqMatch[1]) * 1000000));
    return {kind, mode, name, downlink, text};
  }

  async function showDecode(ctx){
    const modal = ensureModal();
    const subtitle = document.getElementById("receiveDecodeSubtitleV261");
    const consoleEl = document.getElementById("receiveDecodeConsoleV261");
    const startedAt = new Date();
    modal.classList.remove("hidden");
    if (subtitle) subtitle.textContent = `${ctx.name} | ${ctx.mode} | live decode`;
    if (window.receiveDecodePollTimerV2625) {
      clearInterval(window.receiveDecodePollTimerV2625);
      window.receiveDecodePollTimerV2625 = null;
    }

    const params = new URLSearchParams();
    params.set("mode", ctx.mode || ctx.kind);
    params.set("name", ctx.name || "selected satellite");
    if (ctx.downlink) params.set("downlink_hz", ctx.downlink);

    function appendLine(line){
      if (!consoleEl) return;
      consoleEl.textContent += line + "\n";
      consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    function renderObject(obj){
      const lines = [];
      const age = Math.round((Date.now() - startedAt.getTime()) / 1000);
      lines.push(`[${age}s] ${obj.state || "decode"} (${obj.decoder_state || "unknown"})`);
      if (obj.mode) lines.push(`Mode: ${obj.mode}`);
      if (obj.sample_count || obj.pcm_bytes) lines.push(`PCM: ${obj.sample_count || 0} samples, ${obj.pcm_bytes || 0} bytes`);
      if (typeof obj.rms !== "undefined" || typeof obj.peak !== "undefined") lines.push(`Level: RMS ${obj.rms ?? "?"}, peak ${obj.peak ?? "?"}`);
      if (typeof obj.estimated_tone_hz !== "undefined") lines.push(`Tone estimate: ${obj.estimated_tone_hz} Hz`);
      if (typeof obj.key_duty_percent !== "undefined") lines.push(`Key duty: ${obj.key_duty_percent}%`);
      if (obj.morse) lines.push(`Morse: ${obj.morse}`);
      if (obj.decoded_text) lines.push(`Decoded: ${obj.decoded_text}`);
      if (Array.isArray(obj.lines)) {
        for (const line of obj.lines) lines.push(`[decode] ${line}`);
      }
      return lines.join("\n");
    }

    async function pollOnce(){
      if (!consoleEl) return;
      if (modal.classList.contains("hidden")) {
        if (window.receiveDecodePollTimerV2625) clearInterval(window.receiveDecodePollTimerV2625);
        window.receiveDecodePollTimerV2625 = null;
        return;
      }
      try {
        const outResp = await fetch(`/api/radio/decode/output?${params.toString()}&request=${Date.now()}`, {cache:"no-store"});
        const outText = await outResp.text();
        let rendered = outText;
        try { rendered = renderObject(JSON.parse(outText)); } catch (_) {}
        consoleEl.textContent = rendered + "\n";
        consoleEl.scrollTop = consoleEl.scrollHeight;
      } catch (err) {
        appendLine(`Decoder poll error: ${err && err.message ? err.message : err}`);
      }
    }

    if (consoleEl) consoleEl.textContent = `Starting ${ctx.kind === "cw" ? "CW" : "digital"} receive/decode...\n`;
    try {
      const startResp = await fetch(`/api/radio/receive/start?${params.toString()}`, {method:"POST", cache:"no-store"});
      const startText = await startResp.text();
      try { appendLine(JSON.stringify(JSON.parse(startText), null, 2)); } catch (_) { appendLine(startText); }
      appendLine("\nPolling decoded output every 1.5 seconds. Close this window to stop polling.");
      setTimeout(pollOnce, 900);
      let pollCount = 0;
      window.receiveDecodePollTimerV2625 = setInterval(function(){
        pollCount += 1;
        if (pollCount > 40 || modal.classList.contains("hidden")) {
          clearInterval(window.receiveDecodePollTimerV2625);
          window.receiveDecodePollTimerV2625 = null;
          return;
        }
        pollOnce();
      }, 1500);
    } catch (err) {
      appendLine(`Decoder endpoint error: ${err && err.message ? err.message : err}`);
    }
  }

  function relabelReceiveButtons(){
    const buttons = Array.from(document.querySelectorAll("button,a[role='button'],.button"));
    for (const btn of buttons) {
      const text = (btn.textContent || "").trim();
      if (!/^(Listen|Receive|Decode|Decode CW)$/i.test(text)) continue;
      const ctx = buildContext(btn);
      btn.dataset.receiveModeKindV261 = ctx.kind;
      if (ctx.kind !== "listen") {
        btn.textContent = modeLabel(ctx.kind);
        btn.title = "Open decoded text output for CW/digital modes";
      } else if (/^(Decode|Decode CW|Receive)$/i.test(text)) {
        btn.textContent = "Listen";
      }
    }
  }

  document.addEventListener("click", function(ev){
    const btn = ev.target && ev.target.closest ? ev.target.closest("button,a[role='button'],.button") : null;
    if (!btn) return;
    const label = (btn.textContent || "").trim();
    if (!/^(Decode|Decode CW)$/i.test(label) && btn.dataset.receiveModeKindV261 !== "cw" && btn.dataset.receiveModeKindV261 !== "digital") return;
    const ctx = buildContext(btn);
    if (ctx.kind === "listen") return;
    ev.preventDefault();
    ev.stopPropagation();
    if (typeof ev.stopImmediatePropagation === "function") ev.stopImmediatePropagation();
    lastContext = ctx;
    showDecode(ctx);
  }, true);

  document.addEventListener("DOMContentLoaded", function(){
    ensureModal();
    relabelReceiveButtons();
    setInterval(relabelReceiveButtons, 1500);
  });
  if (document.readyState !== "loading") {
    ensureModal();
    relabelReceiveButtons();
    setInterval(relabelReceiveButtons, 1500);
  }
})();


/* DECODE_MODAL_LIVE_POLL_V2_6_25 */
window.decodeModalLivePollV2625 = true;


/* RECEIVE_BUTTON_DECODE_OVERRIDE_V2_6_26AA
 * Override the pass-detail Listen binding so CW/digital selected passes use the
 * receive/decode endpoints instead of starting speaker audio. FM/voice passes
 * intentionally keep the original analog audio path.
 */
let receiveDecodeSessionV2626 = null;
let receiveDecodePollTimerV2626 = 0;

function receivePassTextV2626(pass) {
  const radio = (pass && pass.radio) || {};
  const parts = [];
  const add = (value) => {
    if (value === undefined || value === null) return;
    if (Array.isArray(value)) value.forEach(add);
    else if (typeof value === 'object') Object.keys(value).forEach((key) => add(value[key]));
    else parts.push(String(value));
  };
  add(radio.mode);
  add(radio.type);
  add(radio.status);
  add(radio.description);
  add(pass && pass.mode);
  add(pass && pass.modes);
  return parts.join(' ').toLowerCase();
}

function receiveKindForPassV2626(pass) {
  const text = receivePassTextV2626(pass);
  if (/\b(cw|morse)\b/.test(text) || /\ba1a\b/.test(text)) return 'cw';
  if (/\b(ax\.?25|aprs|packet|fsk|gmsk|bpsk|bsk|psk|9600|1200\s*baud|telemetry|digital|data)\b/.test(text)) return 'digital';
  return 'voice';
}

function receiveModeForPassV2626(pass) {
  const radio = (pass && pass.radio) || {};
  return radio.mode || (pass && pass.modes && pass.modes[0]) || '';
}

function receiveDownlinkForPassV2626(pass) {
  const radio = (pass && pass.radio) || {};
  return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || '';
}

function ensureDecodeModalV2626() {
  let modal = document.getElementById('receiveDecodeModalV2626');
  if (modal) return modal;
  modal = document.createElement('div');
  modal.id = 'receiveDecodeModalV2626';
  modal.hidden = true;
  modal.style.cssText = 'position:fixed;inset:0;z-index:9999;background:rgba(2,6,23,.72);display:flex;align-items:center;justify-content:center;padding:18px;';
  modal.innerHTML = `
    <div style="width:min(920px,96vw);max-height:86vh;overflow:auto;background:#0f172a;color:#e5e7eb;border:1px solid #334155;border-radius:14px;box-shadow:0 20px 80px rgba(0,0,0,.45);">
      <div style="display:flex;align-items:flex-start;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid #334155;">
        <div>
          <h2 id="receiveDecodeTitleV2626" style="margin:0;font-size:20px;">Decode</h2>
          <div id="receiveDecodeSubtitleV2626" style="margin-top:4px;color:#94a3b8;font-size:13px;">Starting decoder...</div>
        </div>
        <button id="receiveDecodeCloseV2626" type="button" class="secondary">Close</button>
      </div>
      <pre id="receiveDecodeOutputV2626" style="margin:0;padding:16px 18px;white-space:pre-wrap;word-break:break-word;font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:#dbeafe;background:#020617;min-height:240px;"></pre>
    </div>`;
  document.body.appendChild(modal);
  modal.addEventListener('click', (event) => {
    if (event.target === modal) closeDecodeModalV2626();
  });
  document.getElementById('receiveDecodeCloseV2626')?.addEventListener('click', closeDecodeModalV2626);
  return modal;
}

function closeDecodeModalV2626() {
  const modal = document.getElementById('receiveDecodeModalV2626');
  if (modal) modal.hidden = true;
  if (receiveDecodePollTimerV2626) {
    window.clearInterval(receiveDecodePollTimerV2626);
    receiveDecodePollTimerV2626 = 0;
  }
}

function renderDecodeOutputV2626(payload) {
  const out = document.getElementById('receiveDecodeOutputV2626');
  if (!out) return;
  const lines = [];
  if (!payload || payload.ok === false) {
    lines.push((payload && payload.error) || 'Decode output unavailable.');
  } else {
    lines.push(`State: ${payload.state || '-'}`);
    lines.push(`Decoder: ${payload.decoder_state || '-'}`);
    if (payload.receive_kind) lines.push(`Kind: ${payload.receive_kind}`);
    if (payload.mode) lines.push(`Mode: ${payload.mode}`);
    if (payload.pcm_bytes !== undefined) lines.push(`PCM bytes: ${payload.pcm_bytes}`);
    if (payload.sample_count !== undefined) lines.push(`Samples: ${payload.sample_count}`);
    if (payload.rms !== undefined) lines.push(`RMS: ${payload.rms}`);
    if (payload.peak !== undefined) lines.push(`Peak: ${payload.peak}`);
    if (payload.estimated_tone_hz !== undefined) lines.push(`Estimated tone: ${payload.estimated_tone_hz} Hz`);
    if (payload.key_duty_percent !== undefined) lines.push(`Key duty: ${payload.key_duty_percent}%`);
    if (payload.morse) lines.push(`Morse: ${payload.morse}`);
    if (payload.decoded_text) lines.push(`Decoded: ${payload.decoded_text}`);
    if (Array.isArray(payload.lines) && payload.lines.length) {
      lines.push('');
      payload.lines.forEach((line) => lines.push(String(line)));
    }
  }
  out.textContent = lines.join('\n');
}

async function pollDecodeOutputV2626(kind, mode) {
  const params = new URLSearchParams({ mode: mode || (kind === 'cw' ? 'CW' : 'digital'), request: String(Date.now()) });
  const payload = await getJson(`/api/radio/decode/output?${params.toString()}`);
  renderDecodeOutputV2626(payload);
}

async function stopReceiveDecodeV2626(message) {
  if (receiveDecodePollTimerV2626) {
    window.clearInterval(receiveDecodePollTimerV2626);
    receiveDecodePollTimerV2626 = 0;
  }
  try { await postJson('/api/radio/receive/stop', {}); } catch (_) {}
  if (receiveDecodeSessionV2626 && receiveDecodeSessionV2626.statusNode?.isConnected) {
    receiveDecodeSessionV2626.statusNode.textContent = message || 'Decode stopped.';
  }
  if (receiveDecodeSessionV2626 && receiveDecodeSessionV2626.button?.isConnected) {
    receiveDecodeSessionV2626.button.textContent = receiveDecodeSessionV2626.kind === 'cw' ? 'Decode CW' : 'Decode';
    receiveDecodeSessionV2626.button.disabled = false;
  }
  receiveDecodeSessionV2626 = null;
}

async function startReceiveDecodeV2626(pass, button, status) {
  const kind = receiveKindForPassV2626(pass);
  const mode = receiveModeForPassV2626(pass) || (kind === 'cw' ? 'CW' : 'digital');
  const downlink = receiveDownlinkForPassV2626(pass);
  const params = new URLSearchParams({
    mode,
    name: (pass && pass.name) || '',
    downlink_hz: String(downlink || '')
  });

  await stopAnalogAudio();
  status.textContent = kind === 'cw' ? 'Starting CW decode capture...' : 'Starting digital decode placeholder...';
  button.disabled = true;

  const payload = await postJson(`/api/radio/receive/start?${params.toString()}`, {});
  const modal = ensureDecodeModalV2626();
  document.getElementById('receiveDecodeTitleV2626').textContent = kind === 'cw' ? 'Decode CW' : 'Decode Digital';
  document.getElementById('receiveDecodeSubtitleV2626').textContent = `${(pass && pass.name) || 'Selected pass'} | ${formatHz(downlink)} | ${mode}`;
  modal.hidden = false;
  renderDecodeOutputV2626(payload);

  receiveDecodeSessionV2626 = { passKey: passKey(pass), kind, mode, button, statusNode: status };
  button.disabled = false;
  button.textContent = 'Stop Decode';
  status.textContent = kind === 'cw' ? 'CW decode capture running; audio is not played to speaker.' : 'Digital decode placeholder running; audio is not played to speaker.';

  await pollDecodeOutputV2626(kind, mode).catch(() => {});
  if (receiveDecodePollTimerV2626) window.clearInterval(receiveDecodePollTimerV2626);
  receiveDecodePollTimerV2626 = window.setInterval(() => {
    pollDecodeOutputV2626(kind, mode).catch((error) => {
      const out = document.getElementById('receiveDecodeOutputV2626');
      if (out) out.textContent = error.message || 'Decode polling failed.';
    });
  }, 1500);
}

function bindAnalogAudio(pass, node) {
  const button = node.querySelector('#analogAudioToggleButton');
  const status = node.querySelector('#analogAudioStatus');
  const audioUrls = analogAudioUrl(pass);
  if (!button || !status) return;

  const kind = receiveKindForPassV2626(pass);
  const isDecode = kind === 'cw' || kind === 'digital';

  button.disabled = !audioUrls && !receiveDownlinkForPassV2626(pass);
  if (button.disabled) {
    status.textContent = 'No downlink is available for this pass.';
    return;
  }

  if (isDecode) {
    const idleLabel = kind === 'cw' ? 'Decode CW' : 'Decode';
    if (receiveDecodeSessionV2626 && receiveDecodeSessionV2626.passKey === passKey(pass)) {
      receiveDecodeSessionV2626.button = button;
      receiveDecodeSessionV2626.statusNode = status;
      button.textContent = 'Stop Decode';
      status.textContent = kind === 'cw' ? 'CW decode capture running; audio is not played to speaker.' : 'Digital decode placeholder running; audio is not played to speaker.';
    } else {
      button.textContent = idleLabel;
      status.textContent = kind === 'cw' ? 'Ready to decode CW for this pass.' : 'Ready to decode digital telemetry for this pass.';
    }

    button.addEventListener('click', async (event) => {
      event.preventDefault();
      event.stopPropagation();
      if (receiveDecodeSessionV2626 && receiveDecodeSessionV2626.passKey === passKey(pass)) {
        await stopReceiveDecodeV2626('Decode stopped.');
        return;
      }
      try {
        await startReceiveDecodeV2626(pass, button, status);
      } catch (error) {
        await stopReceiveDecodeV2626();
        button.textContent = idleLabel;
        status.textContent = error.message || 'Unable to start decode.';
        const statusBar = document.getElementById('status');
        if (statusBar) statusBar.textContent = status.textContent;
      }
    }, { capture: true });
    return;
  }

  button.disabled = !audioUrls;
  if (!audioUrls) {
    status.textContent = 'No downlink is available for this pass.';
    return;
  }

  const setIdle = (message) => {
    button.textContent = 'Listen';
    status.textContent = message;
  };

  if (analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
    analogAudioSession.button = button;
    analogAudioSession.statusNode = status;
    button.textContent = 'Stop';
    status.textContent = 'Streaming live analog FM audio from Pluto...';
  } else {
    setIdle('Ready to listen to this pass.');
  }

  button.addEventListener('click', async () => {
    if (analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
      await stopAnalogAudio('Analog monitor stopped.');
      return;
    }

    try {
      await stopReceiveDecodeV2626();
      status.textContent = 'Connecting to Pluto FM audio stream...';
      await startAnalogAudio(pass, button, status);
    } catch (error) {
      await stopAnalogAudio();
      setIdle(error.message || 'Unable to start analog audio.');
      document.getElementById('status').textContent = error.message || 'Unable to start analog audio.';
    }
  });
}

try {
  if (currentSelectedPass) {
    renderPassDetail(currentSelectedPass);
  }
} catch (_) {}

/* RECEIVE_BUTTON_FM_PRIORITY_V2_6_26D
 * Final receive button override: explicit FM/voice modes must stay on the
 * existing analog Listen path. CW/digital modes route to the decode modal only
 * when those keywords are explicit in the pass metadata.
 */
(function installReceiveButtonFmPriorityV2626D() {
  if (window.__plutoReceiveButtonFmPriorityV2626D) return;
  window.__plutoReceiveButtonFmPriorityV2626D = true;

  function textV2626D(value) {
    if (value === undefined || value === null) return "";
    if (Array.isArray(value)) return value.map(textV2626D).join(" ");
    if (typeof value === "object") {
      try { return JSON.stringify(value); } catch (_) { return ""; }
    }
    return String(value);
  }

  function radioTextV2626D(pass, primaryOnly) {
    const radio = (pass && pass.radio) || {};
    const primary = [radio.mode, (pass && pass.modes) || [], radio.description].map(textV2626D).join(" ");
    if (primaryOnly) return primary.toUpperCase();
    return [
      pass && pass.name,
      radio.mode,
      (pass && pass.modes) || [],
      radio.description,
      radio.type,
      radio.status,
      pass && pass.transmitters,
      pass && pass.downlinks,
      pass && pass.uplinks
    ].map(textV2626D).join(" ").toUpperCase();
  }

  function receiveKindForPassV2626D(pass) {
    const primary = radioTextV2626D(pass, true);
    const all = radioTextV2626D(pass, false);

    const hasCw = /(^|[^A-Z0-9])(CW|MORSE|A1A)([^A-Z0-9]|$)/.test(all);
    if (hasCw) return "cw";

    const hasVoice = /(^|[^A-Z0-9])(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E|J3E)([^A-Z0-9]|$)/.test(primary);
    if (hasVoice) return "voice";

    const hasDigital = /(^|[^A-Z0-9])(AX\.?25|APRS|PACKET|AFSK|GMSK|BPSK|QPSK|PSK|FSK|TELEMETRY|DIGITAL|DATA|9K6|9600)([^A-Z0-9]|$)/.test(all);
    if (hasDigital) return "digital";

    return "voice";
  }

  function downlinkHzForPassV2626D(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function modeForPassV2626D(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.mode || (((pass && pass.modes) || [])[0]) || "";
  }

  function ensureDecodeModalV2626D() {
    let modal = document.getElementById("receiveDecodeModalV2626D");
    if (modal) return modal;
    modal = document.createElement("div");
    modal.id = "receiveDecodeModalV2626D";
    modal.className = "receive-decode-backdrop";
    modal.hidden = true;
    modal.innerHTML = `
      <div class="receive-decode-modal" role="dialog" aria-modal="true" aria-labelledby="receiveDecodeTitleV2626D">
        <div class="receive-decode-header">
          <div>
            <h2 id="receiveDecodeTitleV2626D">Decode</h2>
            <div id="receiveDecodeSubtitleV2626D" class="receive-decode-subtitle">Ready.</div>
          </div>
          <button id="receiveDecodeCloseButtonV2626D" type="button" class="secondary">Close</button>
        </div>
        <pre id="receiveDecodeOutputV2626D" class="receive-decode-output">Waiting for decoder output...</pre>
      </div>`;
    document.body.appendChild(modal);
    modal.addEventListener("click", (event) => {
      if (event.target === modal) closeDecodeModalV2626D();
    });
    document.getElementById("receiveDecodeCloseButtonV2626D")?.addEventListener("click", closeDecodeModalV2626D);
    return modal;
  }

  function closeDecodeModalV2626D() {
    const session = window.__plutoDecodeSessionV2626D || {};
    if (session.timer) window.clearInterval(session.timer);
    window.__plutoDecodeSessionV2626D = { timer: 0 };
    const modal = document.getElementById("receiveDecodeModalV2626D");
    if (modal) {
      modal.hidden = true;
      modal.classList.remove("open");
    }
    fetch("/api/radio/receive/stop", { method: "POST", cache: "no-store" }).catch(() => {});
  }

  function formatDecodePayloadV2626D(payload) {
    if (!payload) return "No decoder payload.";
    const lines = [];
    lines.push(`State: ${payload.state || "unknown"}`);
    if (payload.decoder_state) lines.push(`Decoder: ${payload.decoder_state}`);
    if (payload.receive_kind) lines.push(`Kind: ${payload.receive_kind}`);
    if (payload.mode) lines.push(`Mode: ${payload.mode}`);
    if (payload.sample_count !== undefined) lines.push(`Samples: ${payload.sample_count}`);
    if (payload.pcm_bytes !== undefined) lines.push(`PCM bytes: ${payload.pcm_bytes}`);
    if (payload.rms !== undefined) lines.push(`RMS: ${payload.rms}`);
    if (payload.peak !== undefined) lines.push(`Peak: ${payload.peak}`);
    if (payload.estimated_tone_hz !== undefined) lines.push(`Tone estimate: ${payload.estimated_tone_hz} Hz`);
    if (payload.key_duty_percent !== undefined) lines.push(`Key duty: ${payload.key_duty_percent}%`);
    if (payload.morse) lines.push(`Morse: ${payload.morse}`);
    if (payload.decoded_text) lines.push(`Decoded: ${payload.decoded_text}`);
    if (Array.isArray(payload.lines)) {
      lines.push("");
      payload.lines.forEach((line) => lines.push(String(line)));
    }
    if (!lines.length) return JSON.stringify(payload, null, 2);
    return lines.join("\n");
  }

  async function pollDecodeOutputV2626D(kind, mode) {
    const output = document.getElementById("receiveDecodeOutputV2626D");
    const subtitle = document.getElementById("receiveDecodeSubtitleV2626D");
    const params = new URLSearchParams({ mode: mode || (kind === "cw" ? "CW" : "digital"), request: String(Date.now()) });
    try {
      const payload = await getJson(`/api/radio/decode/output?${params.toString()}`);
      if (output) output.textContent = formatDecodePayloadV2626D(payload);
      if (subtitle) subtitle.textContent = `${kind === "cw" ? "CW" : "Digital"} decode output updating live.`;
    } catch (error) {
      if (output) output.textContent = error.message || "Decode output failed.";
    }
  }

  async function startDecodeForPassV2626D(pass, kind, button, status) {
    const modal = ensureDecodeModalV2626D();
    const title = document.getElementById("receiveDecodeTitleV2626D");
    const subtitle = document.getElementById("receiveDecodeSubtitleV2626D");
    const output = document.getElementById("receiveDecodeOutputV2626D");
    const mode = modeForPassV2626D(pass) || (kind === "cw" ? "CW" : "digital");
    const downlink = downlinkHzForPassV2626D(pass);
    const params = new URLSearchParams({
      mode,
      name: (pass && pass.name) || "",
      downlink_hz: String(downlink || "")
    });

    button.disabled = true;
    button.textContent = kind === "cw" ? "Starting CW..." : "Starting decode...";
    if (status) status.textContent = kind === "cw" ? "Starting CW decode capture..." : "Starting digital decode screen...";

    try {
      const payload = await getJson(`/api/radio/receive/start?${params.toString()}`, { method: "POST" });
      modal.hidden = false;
      modal.classList.add("open");
      if (title) title.textContent = kind === "cw" ? "Decode CW" : "Decode Digital";
      if (subtitle) subtitle.textContent = `${(pass && pass.name) || "Selected pass"} | ${mode || kind} | ${downlink ? formatHz(downlink) : "no downlink"}`;
      if (output) output.textContent = formatDecodePayloadV2626D(payload);
      if (window.__plutoDecodeSessionV2626D && window.__plutoDecodeSessionV2626D.timer) {
        window.clearInterval(window.__plutoDecodeSessionV2626D.timer);
      }
      const timer = window.setInterval(() => pollDecodeOutputV2626D(kind, mode), 1500);
      window.__plutoDecodeSessionV2626D = { timer, kind, mode };
      pollDecodeOutputV2626D(kind, mode);
      button.textContent = kind === "cw" ? "Decode CW" : "Decode";
      if (status) status.textContent = kind === "cw" ? "CW decode screen open." : "Digital decode screen open.";
    } finally {
      button.disabled = false;
    }
  }

  bindAnalogAudio = function bindAnalogAudioV2626D(pass, node) {
    const button = node.querySelector("#analogAudioToggleButton");
    const status = node.querySelector("#analogAudioStatus");
    const audioUrls = analogAudioUrl(pass);
    const kind = receiveKindForPassV2626D(pass);
    if (!button || !status) return;

    button.disabled = !audioUrls;
    if (!audioUrls) {
      status.textContent = "No downlink is available for this pass.";
      return;
    }

    const idleLabel = kind === "cw" ? "Decode CW" : (kind === "digital" ? "Decode" : "Listen");
    const idleMessage = kind === "cw"
      ? "Ready to decode CW for this pass."
      : (kind === "digital" ? "Ready to decode this digital pass." : "Ready to listen to this pass.");

    const setIdle = (message) => {
      button.textContent = idleLabel;
      status.textContent = message || idleMessage;
    };

    if (kind === "voice" && analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
      analogAudioSession.button = button;
      analogAudioSession.statusNode = status;
      button.textContent = "Stop";
      status.textContent = "Streaming live analog FM audio from Pluto...";
    } else {
      setIdle(idleMessage);
    }

    button.addEventListener("click", async () => {
      if (kind === "voice") {
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
          const statusBar = document.getElementById("status");
          if (statusBar) statusBar.textContent = error.message || "Unable to start analog audio.";
        }
        return;
      }

      try {
        await startDecodeForPassV2626D(pass, kind, button, status);
      } catch (error) {
        setIdle(error.message || "Unable to start decoder.");
        const statusBar = document.getElementById("status");
        if (statusBar) statusBar.textContent = error.message || "Unable to start decoder.";
      }
    });
  };
})();


/* DECODE_SELFTEST_UI_V2_6_28 START */
(function installDecodeSelfTestUiV2628() {
  if (window.__decodeSelfTestUiV2628Installed) return;
  window.__decodeSelfTestUiV2628Installed = true;

  function prettyJsonV2628(payload) {
    try {
      return JSON.stringify(payload, null, 2);
    } catch (_) {
      return String(payload);
    }
  }

  async function getJsonNoStoreV2628(url) {
    const response = await fetch(url, { cache: "no-store" });
    const text = await response.text();
    if (!response.ok) {
      throw new Error(`${url}: HTTP ${response.status} ${text.slice(0, 160)}`);
    }
    try {
      return JSON.parse(text);
    } catch (error) {
      throw new Error(`${url}: invalid JSON ${error.message} ${text.slice(0, 160)}`);
    }
  }

  function ensurePanelV2628() {
    let panel = document.getElementById("decodeSelfTestPanelV2628");
    if (panel) return panel;

    panel = document.createElement("section");
    panel.id = "decodeSelfTestPanelV2628";
    panel.className = "decode-selftest-panel-v2628";
    panel.innerHTML = `
      <div class="decode-selftest-header-v2628">
        <strong>Decoder Self-Test</strong>
        <button id="decodeSelfTestMinimizeV2628" type="button" class="secondary">Hide</button>
      </div>
      <p class="decode-selftest-note-v2628">
        Self-tests use synthetic frames/timing. Live satellite decode only shows what Pluto is currently receiving.
      </p>
      <div class="decode-selftest-actions-v2628">
        <button id="decodeSelfTestCwV2628" type="button">CW SOS Test</button>
        <button id="decodeSelfTestAx25V2628" type="button">AX.25/APRS Test</button>
      </div>
      <pre id="decodeSelfTestOutputV2628" class="decode-selftest-output-v2628">Ready.</pre>
    `;
    document.body.appendChild(panel);

    const output = panel.querySelector("#decodeSelfTestOutputV2628");
    const run = async (label, url, expectedText) => {
      output.textContent = `${label}: running...`;
      try {
        const payload = await getJsonNoStoreV2628(url);
        const flat = prettyJsonV2628(payload);
        const ok =
          payload && payload.ok !== false &&
          (!expectedText || flat.toUpperCase().includes(expectedText.toUpperCase()));
        output.textContent = `${label}: ${ok ? "PASS" : "WARN"}\n\n${flat}`;
        const status = document.getElementById("status");
        if (status) status.textContent = `${label}: ${ok ? "PASS" : "WARN"}`;
      } catch (error) {
        output.textContent = `${label}: FAIL\n\n${error.message || error}`;
        const status = document.getElementById("status");
        if (status) status.textContent = `${label}: FAIL`;
      }
    };

    panel.querySelector("#decodeSelfTestCwV2628")?.addEventListener("click", () => {
      run("CW SOS self-test", "/api/radio/decode/cw/selftest", "SOS");
    });
    panel.querySelector("#decodeSelfTestAx25V2628")?.addEventListener("click", () => {
      run("AX.25/APRS self-test", "/api/radio/decode/ax25/selftest", "AX25 SELFTEST");
    });
    panel.querySelector("#decodeSelfTestMinimizeV2628")?.addEventListener("click", () => {
      panel.classList.toggle("collapsed");
      const collapsed = panel.classList.contains("collapsed");
      panel.querySelector("#decodeSelfTestMinimizeV2628").textContent = collapsed ? "Show" : "Hide";
    });

    return panel;
  }

  function ensureLauncherV2628() {
    if (document.getElementById("decodeSelfTestLauncherV2628")) return;
    const launcher = document.createElement("button");
    launcher.id = "decodeSelfTestLauncherV2628";
    launcher.type = "button";
    launcher.className = "decode-selftest-launcher-v2628";
    launcher.textContent = "Decoder Tests";
    launcher.title = "Run CW and AX.25/APRS decoder self-tests";
    launcher.addEventListener("click", () => {
      const panel = ensurePanelV2628();
      panel.classList.remove("collapsed");
      const hide = panel.querySelector("#decodeSelfTestMinimizeV2628");
      if (hide) hide.textContent = "Hide";
    });
    document.body.appendChild(launcher);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", ensureLauncherV2628);
  } else {
    ensureLauncherV2628();
  }
})();
/* DECODE_SELFTEST_UI_V2_6_28 END */

/* RECEIVE_BUTTON_BPSK_PRIORITY_V2_6_26E
 * Final receive button override: explicit current transmitter mode is the
 * strongest classifier. BPSK/PSK/FSK/GMSK/AX.25/APRS/packet modes are digital,
 * not CW, even if another metadata field mentions beacon/CW historically.
 */
(function installReceiveButtonBpskPriorityV2626E() {
  if (window.__plutoReceiveButtonBpskPriorityV2626E) return;
  window.__plutoReceiveButtonBpskPriorityV2626E = true;

  function textV2626E(value) {
    if (value === undefined || value === null) return "";
    if (Array.isArray(value)) return value.map(textV2626E).join(" ");
    if (typeof value === "object") {
      try { return JSON.stringify(value); } catch (_) { return ""; }
    }
    return String(value);
  }

  function rxFieldsV2626E(pass) {
    const radio = (pass && pass.radio) || {};
    return {
      mode: textV2626E(radio.mode || ((pass && pass.modes) || [])[0] || "").toUpperCase(),
      primary: [radio.mode, (pass && pass.modes) || [], radio.description].map(textV2626E).join(" ").toUpperCase(),
      all: [
        pass && pass.name,
        radio.mode,
        (pass && pass.modes) || [],
        radio.description,
        radio.type,
        radio.status,
        pass && pass.transmitters,
        pass && pass.downlinks,
        pass && pass.uplinks
      ].map(textV2626E).join(" ").toUpperCase()
    };
  }

  function hasTokenV2626E(text, pattern) {
    return pattern.test(String(text || ""));
  }

  const digitalModeReV2626E = /(^|[^A-Z0-9])(AX\.?25|APRS|PACKET|AFSK|GMSK|BPSK|QPSK|PSK|FSK|TELEMETRY|DIGITAL|DATA|9K6|9600)([^A-Z0-9]|$)/;
  const cwModeReV2626E = /(^|[^A-Z0-9])(CW|MORSE|A1A)([^A-Z0-9]|$)/;
  const voiceModeReV2626E = /(^|[^A-Z0-9])(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E|J3E)([^A-Z0-9]|$)/;

  function receiveKindForPassV2626E(pass) {
    const fields = rxFieldsV2626E(pass);

    /* The current transmitter Mode field wins first. */
    if (hasTokenV2626E(fields.mode, digitalModeReV2626E)) return "digital";
    if (hasTokenV2626E(fields.mode, cwModeReV2626E)) return "cw";
    if (hasTokenV2626E(fields.mode, voiceModeReV2626E)) return "voice";

    /* Description/modes are next: explicit FM/voice should stay Listen. */
    if (hasTokenV2626E(fields.primary, voiceModeReV2626E)) return "voice";
    if (hasTokenV2626E(fields.primary, digitalModeReV2626E)) return "digital";
    if (hasTokenV2626E(fields.primary, cwModeReV2626E)) return "cw";

    /* Full metadata last; digital beats CW here to avoid BPSK+beacon being CW. */
    if (hasTokenV2626E(fields.all, digitalModeReV2626E)) return "digital";
    if (hasTokenV2626E(fields.all, cwModeReV2626E)) return "cw";
    return "voice";
  }

  function downlinkHzForPassV2626E(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function modeForPassV2626E(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.mode || (((pass && pass.modes) || [])[0]) || "";
  }

  async function postJsonNoBodyV2626E(url) {
    const response = await fetch(url, { method: "POST", cache: "no-store" });
    if (typeof parseJsonResponse === "function") return parseJsonResponse(response, url);
    const text = await response.text();
    if (!response.ok) throw new Error(`${url}: ${response.status}`);
    return JSON.parse(text);
  }

  function ensureDecodeModalV2626E() {
    let modal = document.getElementById("receiveDecodeModalV2626E");
    if (modal) return modal;
    modal = document.createElement("div");
    modal.id = "receiveDecodeModalV2626E";
    modal.className = "receive-decode-backdrop";
    modal.hidden = true;
    modal.innerHTML = `
      <div class="receive-decode-modal" role="dialog" aria-modal="true" aria-labelledby="receiveDecodeTitleV2626E">
        <div class="receive-decode-header">
          <div>
            <h2 id="receiveDecodeTitleV2626E">Decode</h2>
            <div id="receiveDecodeSubtitleV2626E" class="receive-decode-subtitle">Ready.</div>
          </div>
          <button id="receiveDecodeCloseButtonV2626E" type="button" class="secondary">Close</button>
        </div>
        <pre id="receiveDecodeOutputV2626E" class="receive-decode-output">Waiting for decoder output...</pre>
      </div>`;
    document.body.appendChild(modal);
    modal.addEventListener("click", (event) => {
      if (event.target === modal) closeDecodeModalV2626E();
    });
    document.getElementById("receiveDecodeCloseButtonV2626E")?.addEventListener("click", closeDecodeModalV2626E);
    return modal;
  }

  function closeDecodeModalV2626E() {
    const session = window.__plutoDecodeSessionV2626E || {};
    if (session.timer) window.clearInterval(session.timer);
    window.__plutoDecodeSessionV2626E = { timer: 0 };
    const modal = document.getElementById("receiveDecodeModalV2626E");
    if (modal) {
      modal.hidden = true;
      modal.classList.remove("open");
    }
    fetch("/api/radio/receive/stop", { method: "POST", cache: "no-store" }).catch(() => {});
  }

  function formatDecodePayloadV2626E(payload) {
    if (!payload) return "No decoder payload.";
    const lines = [];
    lines.push(`State: ${payload.state || "unknown"}`);
    if (payload.decoder_state) lines.push(`Decoder: ${payload.decoder_state}`);
    if (payload.receive_kind) lines.push(`Kind: ${payload.receive_kind}`);
    if (payload.mode) lines.push(`Mode: ${payload.mode}`);
    if (payload.sample_count !== undefined) lines.push(`Samples: ${payload.sample_count}`);
    if (payload.pcm_bytes !== undefined) lines.push(`PCM bytes: ${payload.pcm_bytes}`);
    if (payload.rms !== undefined) lines.push(`RMS: ${payload.rms}`);
    if (payload.peak !== undefined) lines.push(`Peak: ${payload.peak}`);
    if (payload.estimated_tone_hz !== undefined) lines.push(`Tone estimate: ${payload.estimated_tone_hz} Hz`);
    if (payload.key_duty_percent !== undefined) lines.push(`Key duty: ${payload.key_duty_percent}%`);
    if (payload.morse) lines.push(`Morse: ${payload.morse}`);
    if (payload.decoded_text) lines.push(`Decoded: ${payload.decoded_text}`);
    if (payload.info) lines.push(`Info: ${payload.info}`);
    if (payload.error) lines.push(`Error: ${payload.error}`);
    if (Array.isArray(payload.lines)) {
      lines.push("");
      payload.lines.forEach((line) => lines.push(String(line)));
    }
    return lines.join("\n") || JSON.stringify(payload, null, 2);
  }

  async function pollDecodeOutputV2626E(kind, mode) {
    const output = document.getElementById("receiveDecodeOutputV2626E");
    const subtitle = document.getElementById("receiveDecodeSubtitleV2626E");
    const params = new URLSearchParams({ mode: mode || (kind === "cw" ? "CW" : "digital"), request: String(Date.now()) });
    try {
      const payload = await getJson(`/api/radio/decode/output?${params.toString()}`);
      if (output) output.textContent = formatDecodePayloadV2626E(payload);
      if (subtitle) subtitle.textContent = `${kind === "cw" ? "CW" : "Digital"} decode output updating live.`;
    } catch (error) {
      if (output) output.textContent = error.message || "Decode output failed.";
    }
  }

  async function startDecodeForPassV2626E(pass, kind, button, status) {
    const modal = ensureDecodeModalV2626E();
    const title = document.getElementById("receiveDecodeTitleV2626E");
    const subtitle = document.getElementById("receiveDecodeSubtitleV2626E");
    const output = document.getElementById("receiveDecodeOutputV2626E");
    const mode = modeForPassV2626E(pass) || (kind === "cw" ? "CW" : "digital");
    const downlink = downlinkHzForPassV2626E(pass);
    const params = new URLSearchParams({
      mode,
      name: (pass && pass.name) || "",
      downlink_hz: String(downlink || "")
    });

    button.disabled = true;
    button.textContent = kind === "cw" ? "Starting CW..." : "Starting decode...";
    if (status) status.textContent = kind === "cw" ? "Starting CW decode capture..." : "Starting digital decode screen...";

    try {
      const payload = await postJsonNoBodyV2626E(`/api/radio/receive/start?${params.toString()}`);
      modal.hidden = false;
      modal.classList.add("open");
      if (title) title.textContent = kind === "cw" ? "Decode CW" : "Decode Digital";
      if (subtitle) subtitle.textContent = `${(pass && pass.name) || "Selected pass"} | ${mode || kind} | ${downlink ? formatHz(downlink) : "no downlink"}`;
      if (output) output.textContent = formatDecodePayloadV2626E(payload);
      if (window.__plutoDecodeSessionV2626E && window.__plutoDecodeSessionV2626E.timer) {
        window.clearInterval(window.__plutoDecodeSessionV2626E.timer);
      }
      const timer = window.setInterval(() => pollDecodeOutputV2626E(kind, mode), 1500);
      window.__plutoDecodeSessionV2626E = { timer, kind, mode };
      pollDecodeOutputV2626E(kind, mode);
      button.textContent = kind === "cw" ? "Decode CW" : "Decode";
      if (status) status.textContent = kind === "cw" ? "CW decode screen open." : "Digital decode screen open.";
    } finally {
      button.disabled = false;
    }
  }

  bindAnalogAudio = function bindAnalogAudioV2626E(pass, node) {
    const button = node.querySelector("#analogAudioToggleButton");
    const status = node.querySelector("#analogAudioStatus");
    const audioUrls = analogAudioUrl(pass);
    const kind = receiveKindForPassV2626E(pass);
    if (!button || !status) return;

    button.disabled = !audioUrls;
    if (!audioUrls) {
      status.textContent = "No downlink is available for this pass.";
      return;
    }

    const idleLabel = kind === "cw" ? "Decode CW" : (kind === "digital" ? "Decode" : "Listen");
    const idleMessage = kind === "cw"
      ? "Ready to decode CW for this pass."
      : (kind === "digital" ? "Ready to decode this digital pass." : "Ready to listen to this pass.");

    const setIdle = (message) => {
      button.textContent = idleLabel;
      status.textContent = message || idleMessage;
    };

    if (kind === "voice" && analogAudioSession && analogAudioSession.passKey === passKey(pass)) {
      analogAudioSession.button = button;
      analogAudioSession.statusNode = status;
      button.textContent = "Stop";
      status.textContent = "Streaming live analog FM audio from Pluto...";
    } else {
      setIdle(idleMessage);
    }

    button.addEventListener("click", async () => {
      if (kind === "voice") {
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
          const statusBar = document.getElementById("status");
          if (statusBar) statusBar.textContent = error.message || "Unable to start analog audio.";
        }
        return;
      }

      try {
        await startDecodeForPassV2626E(pass, kind, button, status);
      } catch (error) {
        setIdle(error.message || "Unable to start decoder.");
        const statusBar = document.getElementById("status");
        if (statusBar) statusBar.textContent = error.message || "Unable to start decoder.";
      }
    });
  };
})();

/* PERIODIC_PASS_REFRESH_V2_6_29
 * Keep the near-term pass queue fresh while the browser UI is open.
 * This intentionally queues the existing backend pass refresh endpoint rather
 * than adding a separate scheduler process. It is guarded against overlapping
 * refreshes and backs off if the backend already reports a running refresh.
 */
(function installPeriodicPassRefreshV2629() {
  if (window.__plutoPeriodicPassRefreshV2629) return;
  window.__plutoPeriodicPassRefreshV2629 = true;

  const PERIODIC_PASS_REFRESH_INTERVAL_MS_V2629 = 15 * 60 * 1000;
  const PERIODIC_PASS_REFRESH_INITIAL_DELAY_MS_V2629 = 60 * 1000;
  let periodicPassRefreshTimerV2629 = 0;
  let periodicPassRefreshInFlightV2629 = false;
  let periodicPassRefreshLastRunMsV2629 = 0;

  function periodicPassRefreshStatusV2629(message) {
    const node = document.getElementById("status");
    if (node && message) node.textContent = message;
  }

  async function periodicPassRefreshBackendBusyV2629() {
    try {
      const status = await getJson("/api/refresh/status");
      const state = String((status && status.state) || "").toLowerCase();
      const target = String((status && status.target) || "").toLowerCase();
      return target === "passes" && !["", "idle", "ok", "failed", "complete", "completed"].includes(state);
    } catch (error) {
      return false;
    }
  }

  async function periodicPassRefreshOnceV2629(reason) {
    const now = Date.now();
    if (periodicPassRefreshInFlightV2629) return false;
    if (reason !== "initial" && periodicPassRefreshLastRunMsV2629 &&
        (now - periodicPassRefreshLastRunMsV2629) < (PERIODIC_PASS_REFRESH_INTERVAL_MS_V2629 - 30000)) {
      return false;
    }

    periodicPassRefreshInFlightV2629 = true;
    try {
      if (await periodicPassRefreshBackendBusyV2629()) {
        periodicPassRefreshStatusV2629("Pass refresh already running on Pluto.");
        return false;
      }
      await postJson("/api/refresh/passes", {});
      periodicPassRefreshLastRunMsV2629 = Date.now();
      lastAutoPassRefreshMs = periodicPassRefreshLastRunMsV2629;
      periodicPassRefreshStatusV2629("Queued periodic pass refresh on Pluto.");
      if (typeof startPassFileWatchV3 === "function") {
        startPassFileWatchV3("Refreshing pass queue...", "Periodic 15-minute refresh queued on Pluto.");
      }
      if (typeof refresh === "function") {
        window.setTimeout(() => refresh().catch(() => {}), 2500);
      }
      return true;
    } catch (error) {
      periodicPassRefreshStatusV2629(error && error.message ? error.message : "Periodic pass refresh failed.");
      return false;
    } finally {
      periodicPassRefreshInFlightV2629 = false;
    }
  }

  window.plutoPeriodicPassRefreshNowV2629 = () => periodicPassRefreshOnceV2629("manual");

  function startPeriodicPassRefreshV2629() {
    if (periodicPassRefreshTimerV2629) return;
    window.setTimeout(() => periodicPassRefreshOnceV2629("initial"), PERIODIC_PASS_REFRESH_INITIAL_DELAY_MS_V2629);
    periodicPassRefreshTimerV2629 = window.setInterval(() => {
      periodicPassRefreshOnceV2629("timer");
    }, PERIODIC_PASS_REFRESH_INTERVAL_MS_V2629);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startPeriodicPassRefreshV2629, { once: true });
  } else {
    startPeriodicPassRefreshV2629();
  }
})();

/* LINEAR_TRANSPONDER_CW_OVERRIDE_V2_6_32
 * USB/LSB/SSB linear transponders are analog listen targets by default.
 * They may carry user CW inside the passband, so add a secondary manual
 * Decode CW button without changing the primary Listen behavior.
 */
(function installLinearTransponderCwOverrideV2632() {
  if (window.__plutoLinearTransponderCwOverrideV2632) return;
  window.__plutoLinearTransponderCwOverrideV2632 = true;

  function textV2632(value) {
    if (value === undefined || value === null) return "";
    if (Array.isArray(value)) return value.map(textV2632).join(" ");
    if (typeof value === "object") {
      try { return JSON.stringify(value); } catch (_) { return ""; }
    }
    return String(value);
  }

  function radioV2632(pass) {
    return (pass && pass.radio) || {};
  }

  function isLinearSsbTransponderV2632(pass) {
    const radio = radioV2632(pass);
    const mode = textV2632(radio.mode || ((pass && pass.modes) || [])[0] || "").toUpperCase();
    const type = textV2632(radio.type).toUpperCase();
    const desc = textV2632(radio.description).toUpperCase();
    const all = [mode, type, desc].join(" ");
    const ssbMode = /(^|[^A-Z0-9])(USB|LSB|SSB|J3E)([^A-Z0-9]|$)/.test(mode) || /(^|[^A-Z0-9])(USB|LSB|SSB|J3E)([^A-Z0-9]|$)/.test(desc);
    const transponder = /TRANSPONDER|LINEAR/.test(type) || /TRANSPONDER|LINEAR/.test(desc);
    const explicitDigital = /(^|[^A-Z0-9])(AX\.?25|APRS|PACKET|AFSK|GMSK|BPSK|QPSK|PSK|FSK|TELEMETRY|DIGITAL|DATA|9K6|9600)([^A-Z0-9]|$)/.test(mode);
    const explicitCwMode = /(^|[^A-Z0-9])(CW|MORSE|A1A)([^A-Z0-9]|$)/.test(mode);
    return ssbMode && transponder && !explicitDigital && !explicitCwMode && all.length > 0;
  }

  function parseDownlinkHzV2632(value) {
    if (value === undefined || value === null || value === "") return "";
    if (typeof value === "number" && Number.isFinite(value)) {
      return String(Math.round(value < 1000000 ? value * 1000000 : value));
    }
    const raw = String(value).trim();
    const upper = raw.toUpperCase();
    const match = raw.match(/[-+]?\d+(?:\.\d+)?/);
    if (!match) return "";
    let n = Number(match[0]);
    if (!Number.isFinite(n)) return "";
    if (upper.includes("GHZ")) n *= 1000000000;
    else if (upper.includes("MHZ")) n *= 1000000;
    else if (upper.includes("KHZ")) n *= 1000;
    else if (n < 1000000) n *= 1000000;
    return String(Math.round(n));
  }

  function downlinkHzForPassV2632(pass) {
    const radio = radioV2632(pass);
    const candidates = [
      radio.downlink_hz,
      radio.downlinkHz,
      radio.downlink,
      pass && pass.downlink_hz,
      pass && pass.downlinkHz,
      ((pass && pass.downlinks_hz) || [])[0],
      ((pass && pass.downlinks) || [])[0]
    ];
    for (const candidate of candidates) {
      const hz = parseDownlinkHzV2632(candidate);
      if (hz) return hz;
    }
    return "";
  }

  function passNameV2632(pass) {
    return textV2632((pass && (pass.name || pass.satellite || pass.satellite_name || pass.object_name)) || "Selected pass");
  }

  async function postJsonV2632(url) {
    const response = await fetch(url, { method: "POST", cache: "no-store" });
    const body = await response.text();
    let data = null;
    try { data = body ? JSON.parse(body) : {}; } catch (_) { data = { ok: false, raw: body }; }
    data.http_ok = response.ok;
    if (!response.ok) throw new Error(data.message || data.error || `${url}: HTTP ${response.status}`);
    return data;
  }

  async function getJsonV2632(url) {
    if (typeof getJson === "function") return getJson(url);
    const response = await fetch(url, { cache: "no-store" });
    const text = await response.text();
    const data = text ? JSON.parse(text) : {};
    data.http_ok = response.ok;
    return data;
  }

  function ensureLinearCwModalV2632() {
    let modal = document.getElementById("linearCwDecodeModalV2632");
    if (modal) return modal;
    modal = document.createElement("div");
    modal.id = "linearCwDecodeModalV2632";
    modal.className = "receive-decode-backdrop linear-cw-decode-backdrop-v2632";
    modal.hidden = true;
    modal.innerHTML = `
      <div class="receive-decode-modal linear-cw-decode-modal-v2632" role="dialog" aria-modal="true" aria-labelledby="linearCwDecodeTitleV2632">
        <div class="receive-decode-header">
          <div>
            <h2 id="linearCwDecodeTitleV2632">Decode CW</h2>
            <div id="linearCwDecodeSubtitleV2632" class="receive-decode-subtitle">Ready.</div>
          </div>
          <button id="linearCwDecodeCloseButtonV2632" type="button" class="secondary">Close</button>
        </div>
        <pre id="linearCwDecodeOutputV2632" class="receive-decode-output">Waiting for CW decoder output...</pre>
      </div>`;
    document.body.appendChild(modal);
    modal.addEventListener("click", (event) => {
      if (event.target === modal) closeLinearCwModalV2632();
    });
    document.getElementById("linearCwDecodeCloseButtonV2632")?.addEventListener("click", closeLinearCwModalV2632);
    return modal;
  }

  function closeLinearCwModalV2632() {
    const session = window.__plutoLinearCwDecodeSessionV2632 || {};
    if (session.timer) window.clearInterval(session.timer);
    window.__plutoLinearCwDecodeSessionV2632 = { timer: 0 };
    const modal = document.getElementById("linearCwDecodeModalV2632");
    if (modal) {
      modal.hidden = true;
      modal.classList.remove("open");
    }
    fetch("/api/radio/receive/stop", { method: "POST", cache: "no-store" }).catch(() => {});
  }

  function formatPayloadV2632(payload) {
    if (!payload) return "No decoder payload.";
    const lines = [];
    lines.push(`State: ${payload.state || "unknown"}`);
    if (payload.version) lines.push(`Backend: ${payload.version}`);
    if (payload.decoder_state) lines.push(`Decoder: ${payload.decoder_state}`);
    if (payload.mode) lines.push(`Mode: ${payload.mode}`);
    if (payload.sample_count !== undefined) lines.push(`Samples: ${payload.sample_count}`);
    if (payload.pcm_bytes !== undefined) lines.push(`PCM bytes: ${payload.pcm_bytes}`);
    if (payload.rms !== undefined) lines.push(`RMS: ${payload.rms}`);
    if (payload.peak !== undefined) lines.push(`Peak: ${payload.peak}`);
    if (payload.estimated_tone_hz !== undefined) lines.push(`Tone estimate: ${payload.estimated_tone_hz} Hz`);
    if (payload.key_duty_percent !== undefined) lines.push(`Key duty: ${payload.key_duty_percent}%`);
    if (payload.morse) lines.push(`Morse: ${payload.morse}`);
    if (payload.decoded_text) lines.push(`Decoded: ${payload.decoded_text}`);
    if (payload.error) lines.push(`Error: ${payload.error}`);
    if (Array.isArray(payload.lines) && payload.lines.length) {
      lines.push("");
      payload.lines.forEach((line) => lines.push(String(line)));
    }
    return lines.join("\n") || JSON.stringify(payload, null, 2);
  }

  async function pollLinearCwOutputV2632() {
    const output = document.getElementById("linearCwDecodeOutputV2632");
    const subtitle = document.getElementById("linearCwDecodeSubtitleV2632");
    try {
      const data = await getJsonV2632(`/api/radio/decode/output?mode=CW&request=${Date.now()}`);
      if (output) output.textContent = formatPayloadV2632(data);
      if (subtitle) subtitle.textContent = "Manual CW decode output updating live.";
    } catch (error) {
      if (output) output.textContent = error.message || "CW decode output failed.";
    }
  }

  async function startManualLinearCwV2632(pass, button, status) {
    const hz = downlinkHzForPassV2632(pass);
    if (!hz) throw new Error("Unable to determine downlink frequency for CW decode.");
    const params = new URLSearchParams({
      mode: "CW",
      name: passNameV2632(pass),
      downlink_hz: hz
    });
    button.disabled = true;
    button.textContent = "Starting CW...";
    if (status) status.textContent = "Starting manual CW decode for this USB/LSB transponder pass...";
    try {
      const start = await postJsonV2632(`/api/radio/receive/start?${params.toString()}`);
      const modal = ensureLinearCwModalV2632();
      const title = document.getElementById("linearCwDecodeTitleV2632");
      const subtitle = document.getElementById("linearCwDecodeSubtitleV2632");
      const output = document.getElementById("linearCwDecodeOutputV2632");
      modal.hidden = false;
      modal.classList.add("open");
      if (title) title.textContent = "Decode CW";
      if (subtitle) subtitle.textContent = `${passNameV2632(pass)} | manual CW override | ${typeof formatHz === "function" ? formatHz(hz) : hz + " Hz"}`;
      if (output) output.textContent = formatPayloadV2632(start);
      const existing = window.__plutoLinearCwDecodeSessionV2632 || {};
      if (existing.timer) window.clearInterval(existing.timer);
      const timer = window.setInterval(pollLinearCwOutputV2632, 1500);
      window.__plutoLinearCwDecodeSessionV2632 = { timer };
      pollLinearCwOutputV2632();
      if (status) status.textContent = "Manual CW decode screen open. Close the decode window to stop capture.";
    } finally {
      button.disabled = false;
      button.textContent = "Decode CW";
    }
  }

  function addLinearCwButtonV2632(pass, node) {
    if (!node || !isLinearSsbTransponderV2632(pass)) return;
    const listenButton = node.querySelector("#analogAudioToggleButton");
    const status = node.querySelector("#analogAudioStatus");
    if (!listenButton || !listenButton.parentElement) return;
    if (node.querySelector("#linearTransponderDecodeCwButtonV2632")) return;
    const button = document.createElement("button");
    button.id = "linearTransponderDecodeCwButtonV2632";
    button.type = "button";
    button.className = "secondary linear-cw-override-button-v2632";
    button.textContent = "Decode CW";
    button.title = "Manual CW decode override for USB/LSB linear transponder passes.";
    button.addEventListener("click", async (event) => {
      event.preventDefault();
      try {
        await startManualLinearCwV2632(pass, button, status);
      } catch (error) {
        if (status) status.textContent = error.message || "Unable to start manual CW decode.";
        const statusBar = document.getElementById("status");
        if (statusBar) statusBar.textContent = error.message || "Unable to start manual CW decode.";
      }
    });
    listenButton.insertAdjacentElement("afterend", button);
    if (status && !String(status.textContent || "").includes("Decode CW")) {
      status.textContent = `${status.textContent || "Ready."} USB/LSB linear transponder: use Listen for audio, or Decode CW if CW is present in the passband.`;
    }
  }

  if (typeof bindAnalogAudio === "function") {
    const previousBindAnalogAudioV2632 = bindAnalogAudio;
    bindAnalogAudio = function bindAnalogAudioLinearCwOverrideV2632(pass, node) {
      previousBindAnalogAudioV2632(pass, node);
      try { addLinearCwButtonV2632(pass, node); } catch (_) {}
    };
  }
})();


// DECODE_MODAL_WAITING_COPY_V2_6_33
(function installDecodeModalWaitingCopyV2633() {
  if (window.__decodeModalWaitingCopyV2633Installed) return;
  window.__decodeModalWaitingCopyV2633Installed = true;

  const replacements = [
    {
      from: /\[decode\]\s*Start Decode CW and allow one or two seconds of live capture\./g,
      to: "Waiting for the first live audio samples from the active Decode CW session."
    },
    {
      from: /Start Decode CW and allow one or two seconds of live capture\./g,
      to: "Waiting for the first live audio samples from the active Decode CW session."
    },
    {
      from: /\[decode\]\s*Waiting for live PCM capture\./g,
      to: "Waiting for live audio samples from Pluto."
    },
    {
      from: /Waiting for live PCM capture\./g,
      to: "Waiting for live audio samples from Pluto."
    },
    {
      from: /\[decode\]\s*/g,
      to: ""
    }
  ];

  function normalizeTextV2633(text) {
    let next = String(text || "");
    for (const item of replacements) {
      next = next.replace(item.from, item.to);
    }
    return next;
  }

  function shouldInspectNodeV2633(node) {
    if (!node || !node.parentElement) return false;
    const parent = node.parentElement;
    const host = parent.closest(
      "#decodeModal, #decodeOutput, #decodeStatus, .decode-modal, .decode-output, [id*='decode'], [class*='decode']"
    );
    if (host) return true;
    const text = node.nodeValue || "";
    return text.includes("[decode]") || text.includes("Start Decode CW") || text.includes("live PCM capture");
  }

  function scrubDecodeCopyV2633(root) {
    const start = root || document.body;
    if (!start) return;
    const walker = document.createTreeWalker(start, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) {
      const node = walker.currentNode;
      if (shouldInspectNodeV2633(node)) {
        nodes.push(node);
      }
    }
    for (const node of nodes) {
      const before = node.nodeValue || "";
      const after = normalizeTextV2633(before);
      if (after !== before) {
        node.nodeValue = after;
      }
    }
  }

  function annotateDecodeButtonsV2633() {
    const buttons = Array.from(document.querySelectorAll("button"));
    for (const button of buttons) {
      const label = (button.textContent || "").trim().toLowerCase();
      if (label === "decode cw" && !button.title) {
        button.title = "Start live CW audio capture and open the decoder. No second start button is required.";
      }
      if (label === "decode" && !button.title) {
        button.title = "Start live digital audio capture and open the decoder. No second start button is required.";
      }
    }
  }

  function refreshDecodeCopyV2633() {
    scrubDecodeCopyV2633(document.body);
    annotateDecodeButtonsV2633();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", refreshDecodeCopyV2633, { once: true });
  } else {
    refreshDecodeCopyV2633();
  }

  const observer = new MutationObserver(() => refreshDecodeCopyV2633());
  observer.observe(document.documentElement || document.body, {
    childList: true,
    subtree: true,
    characterData: true
  });

  window.plutoScrubDecodeCopyV2633 = refreshDecodeCopyV2633;
})();

// DECODE_SELFTEST_UI_ALL_DIGITAL_V2_6_36
(function () {
  "use strict";

  const MARKER = "DECODE_SELFTEST_UI_ALL_DIGITAL_V2_6_36";
  const TESTS = [
    { id: "decoderSelfTestCwButtonV2636", label: "CW SOS", path: "/api/radio/decode/cw/selftest", group: "CW" },
    { id: "decoderSelfTestAx25ButtonV2636", label: "AX.25 Parser", path: "/api/radio/decode/ax25/selftest", group: "AX.25" },
    { id: "decoderSelfTestAfskButtonV2636", label: "AX.25 AFSK", path: "/api/radio/decode/ax25/afsk-selftest", group: "AX.25" },
    { id: "decoderSelfTestDigitalButtonV2636", label: "Digital Stack", path: "/api/radio/decode/digital/selftest", group: "Digital" },
    { id: "decoderSelfTestBpskButtonV2636", label: "BPSK", path: "/api/radio/decode/bpsk/selftest", group: "Digital" },
    { id: "decoderSelfTestFskButtonV2636", label: "FSK", path: "/api/radio/decode/fsk/selftest", group: "Digital" },
    { id: "decoderSelfTestGmskButtonV2636", label: "GMSK", path: "/api/radio/decode/gmsk/selftest", group: "Digital" }
  ];

  function ready(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn, { once: true });
    } else {
      fn();
    }
  }

  async function fetchJson(path, options) {
    const response = await fetch(path, Object.assign({ cache: "no-store" }, options || {}));
    const text = await response.text();
    let data;
    try {
      data = JSON.parse(text || "{}");
    } catch (error) {
      data = {
        ok: false,
        error: "invalid_json",
        raw: text.slice(0, 800)
      };
    }
    data.http_ok = response.ok;
    data.http_status = response.status;
    data.endpoint = path;
    return data;
  }

  function buttonStyle(button, variant) {
    button.style.border = "0";
    button.style.borderRadius = "8px";
    button.style.padding = "7px 10px";
    button.style.cursor = "pointer";
    button.style.fontWeight = variant === "primary" ? "700" : "600";
    button.style.background = variant === "primary" ? "#dbeafe" : "#e5e7eb";
    button.style.color = "#0f172a";
  }

  function ensurePanel() {
    let panel = document.getElementById("decoderSelfTestPanelV2628");
    if (panel) {
      return panel;
    }

    panel = document.createElement("section");
    panel.id = "decoderSelfTestPanelV2628";
    panel.className = "decoder-selftest-panel";
    panel.style.position = "fixed";
    panel.style.right = "18px";
    panel.style.bottom = "18px";
    panel.style.zIndex = "9999";
    panel.style.maxWidth = "520px";
    panel.style.padding = "12px";
    panel.style.border = "1px solid rgba(148, 163, 184, 0.45)";
    panel.style.borderRadius = "12px";
    panel.style.background = "rgba(15, 23, 42, 0.95)";
    panel.style.color = "#e5e7eb";
    panel.style.boxShadow = "0 12px 28px rgba(0,0,0,0.35)";
    panel.style.fontFamily = "system-ui, -apple-system, Segoe UI, sans-serif";
    panel.innerHTML = [
      '<div style="display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px;">',
      '<div><strong>Decoder Tests</strong><div style="font-size:12px;color:#cbd5e1;">Offline decoder self-tests; no satellite pass required.</div></div>',
      '<button type="button" id="decoderSelfTestCloseV2628" style="border:0;border-radius:8px;padding:4px 8px;cursor:pointer;">×</button>',
      '</div>',
      '<div id="decoderSelfTestButtonsV2628" style="display:flex;flex-wrap:wrap;gap:8px;margin-bottom:8px;"></div>',
      '<pre id="decoderSelfTestOutputV2628" style="max-height:310px;overflow:auto;white-space:pre-wrap;background:rgba(2,6,23,0.75);border-radius:8px;padding:8px;margin:0;font-size:12px;"></pre>'
    ].join("");
    document.body.appendChild(panel);

    const close = document.getElementById("decoderSelfTestCloseV2628");
    if (close) close.onclick = () => { panel.hidden = true; };

    return panel;
  }

  function outputNode() {
    const panel = ensurePanel();
    return panel.querySelector("#decoderSelfTestOutputV2628");
  }

  function setOutput(data) {
    const out = outputNode();
    if (out) out.textContent = JSON.stringify(data, null, 2);
  }

  async function runOne(test) {
    const result = await fetchJson(test.path);
    result.test = test.label;
    result.group = test.group;
    return result;
  }

  function makeTestButton(test) {
    const button = document.createElement("button");
    button.id = test.id;
    button.type = "button";
    button.textContent = test.label;
    button.title = test.path;
    buttonStyle(button);
    button.onclick = async () => {
      const oldText = button.textContent;
      button.disabled = true;
      button.textContent = `${test.label}...`;
      try {
        setOutput({ ok: true, state: "running", test: test.label, endpoint: test.path });
        setOutput(await runOne(test));
      } catch (error) {
        setOutput({ ok: false, test: test.label, error: String(error && error.message ? error.message : error) });
      } finally {
        button.disabled = false;
        button.textContent = oldText;
      }
    };
    return button;
  }

  function ensureAllButtons() {
    const panel = ensurePanel();
    const box = panel.querySelector("#decoderSelfTestButtonsV2628") || panel;

    if (!document.getElementById("decoderSelfTestRunAllButtonV2636")) {
      const runAll = document.createElement("button");
      runAll.id = "decoderSelfTestRunAllButtonV2636";
      runAll.type = "button";
      runAll.textContent = "Run All";
      buttonStyle(runAll, "primary");
      runAll.onclick = async () => {
        const oldText = runAll.textContent;
        runAll.disabled = true;
        runAll.textContent = "Running...";
        const results = [];
        setOutput({ ok: true, state: "running_all", tests: TESTS.map((test) => test.label) });
        for (const test of TESTS) {
          try {
            results.push(await runOne(test));
          } catch (error) {
            results.push({ ok: false, test: test.label, endpoint: test.path, error: String(error && error.message ? error.message : error) });
          }
          setOutput({ ok: true, state: "running_all", completed: results.length, total: TESTS.length, results });
        }
        const failed = results.filter((result) => !(result && result.ok !== false && result.http_ok !== false && result.pass !== false && result.decoder_state !== "fail"));
        setOutput({ ok: failed.length === 0, state: "complete", passed: results.length - failed.length, failed: failed.length, results });
        runAll.disabled = false;
        runAll.textContent = oldText;
      };
      box.appendChild(runAll);
    }

    for (const test of TESTS) {
      if (!document.getElementById(test.id)) {
        box.appendChild(makeTestButton(test));
      }
    }
  }

  function ensureLauncher() {
    let launcher = document.getElementById("decoderSelfTestLauncherV2628");
    if (!launcher) {
      launcher = document.createElement("button");
      launcher.id = "decoderSelfTestLauncherV2628";
      launcher.type = "button";
      launcher.textContent = "Decoder Tests";
      launcher.style.position = "fixed";
      launcher.style.right = "18px";
      launcher.style.bottom = "18px";
      launcher.style.zIndex = "9998";
      launcher.style.border = "0";
      launcher.style.borderRadius = "999px";
      launcher.style.padding = "9px 13px";
      launcher.style.cursor = "pointer";
      launcher.style.boxShadow = "0 8px 20px rgba(0,0,0,0.28)";
      document.body.appendChild(launcher);
    }
    launcher.onclick = () => {
      const panel = ensurePanel();
      ensureAllButtons();
      panel.hidden = false;
    };
  }

  ready(() => {
    ensureLauncher();
    const existingPanel = document.getElementById("decoderSelfTestPanelV2628");
    if (existingPanel) ensureAllButtons();
    window.plutoDecoderSelfTestsV2636 = {
      tests: TESTS.slice(),
      run: (labelOrPath) => {
        const key = String(labelOrPath || "").toLowerCase();
        const test = TESTS.find((item) => item.label.toLowerCase() === key || item.path.toLowerCase() === key || item.id.toLowerCase() === key);
        if (!test) return Promise.reject(new Error(`Unknown decoder self-test: ${labelOrPath}`));
        return runOne(test).then((data) => { setOutput(data); return data; });
      },
      runAll: async () => {
        const results = [];
        for (const test of TESTS) results.push(await runOne(test));
        setOutput({ ok: true, state: "complete", results });
        return results;
      }
    };
    console.log(`${MARKER}: installed`);
  });
})();

// DIGITAL_DECODE_MODAL_FRIENDLY_STATE_V2_6_38
// User-facing cleanup for digital decode modal states. Backend state names remain unchanged.
(function installDigitalDecodeModalFriendlyStateV2638() {
  "use strict";

  const marker = "DIGITAL_DECODE_MODAL_FRIENDLY_STATE_V2_6_38_INSTALLED";
  if (window[marker]) return;
  window[marker] = true;

  function friendlyDigitalDecodeTextV2638(text) {
    if (text === null || text === undefined) return text;
    let value = String(text);

    value = value.replace(/\[decode\]\s*/gi, "");

    const replacements = [
      [/\bdigital_bpsk_live_diagnostic\b/gi, "BPSK signal diagnostics"],
      [/\bdigital_bpsk_waiting\b/gi, "BPSK decoder waiting"],
      [/\bdigital_fsk_gmsk_live_diagnostic\b/gi, "FSK/GMSK signal diagnostics"],
      [/\bdigital_fsk_gmsk_waiting\b/gi, "FSK/GMSK decoder waiting"],
      [/\bax25_afsk_live_diagnostic\b/gi, "AX.25/APRS AFSK signal diagnostics"],
      [/\bax25_afsk_waiting\b/gi, "AX.25/APRS AFSK decoder waiting"],
      [/\bax25_live_diagnostic\b/gi, "AX.25/APRS signal diagnostics"],
      [/\bax25_waiting\b/gi, "AX.25/APRS decoder waiting"],
      [/\bcw_morse_experimental_waiting\b/gi, "CW decoder waiting"],
      [/\bcw_morse_experimental\b/gi, "CW decoder"],
      [/\(signal_diagnostic\)/gi, "(signal diagnostics)"],
      [/\bsignal_diagnostic\b/gi, "signal diagnostics"],
      [/\blive_diagnostic\b/gi, "live diagnostics"],
      [/\bnot_implemented\b/gi, "not implemented"],
      [/\bselftest\b/gi, "self-test"],
      [/\bdecoder_state\b/gi, "decoder state"]
    ];

    for (const [pattern, replacement] of replacements) {
      value = value.replace(pattern, replacement);
    }

    value = value.replace(
      /Waiting for live PCM from Decode on a digital\/packet pass\./gi,
      "Waiting for live audio samples from the active digital decode session."
    );
    value = value.replace(
      /Waiting for live PCM capture\./gi,
      "Waiting for live audio samples from Pluto."
    );
    value = value.replace(
      /Next stage after this diagnostic is Bell 202 bit\/HDLC recovery\./gi,
      "The decoder is measuring signal quality before attempting packet recovery."
    );
    value = value.replace(
      /Live BPSK pass decoding will reuse this signal path after carrier\/clock recovery is connected\./gi,
      "Live BPSK decoding is using this path for carrier and clock diagnostics."
    );
    value = value.replace(
      /GMSK\/9600 live decoding still needs mode-specific clock and slicing against real audio\./gi,
      "Live FSK/GMSK decoding is using this path for signal and clock diagnostics."
    );

    return value;
  }

  function cleanDigitalDecodeModalNodeV2638(node) {
    if (!node) return;

    if (node.nodeType === Node.TEXT_NODE) {
      const next = friendlyDigitalDecodeTextV2638(node.nodeValue);
      if (next !== node.nodeValue) node.nodeValue = next;
      return;
    }

    if (node.nodeType !== Node.ELEMENT_NODE) return;

    const id = String(node.id || "").toLowerCase();
    const cls = String(node.className || "").toLowerCase();
    const role = String(node.getAttribute && (node.getAttribute("role") || "")).toLowerCase();
    const likelyDecode =
      id.includes("decode") ||
      cls.includes("decode") ||
      role.includes("dialog") ||
      node.closest?.('[id*="decode" i], [class*="decode" i]');

    if (!likelyDecode) return;

    for (const child of Array.from(node.childNodes || [])) {
      cleanDigitalDecodeModalNodeV2638(child);
    }
  }

  function cleanAllDigitalDecodeModalTextV2638() {
    const roots = document.querySelectorAll(
      '[id*="decode" i], [class*="decode" i], [role="dialog"]'
    );
    roots.forEach(cleanDigitalDecodeModalNodeV2638);
  }

  const observer = new MutationObserver((mutations) => {
    let shouldSweep = false;
    for (const mutation of mutations) {
      if (mutation.type === "characterData") {
        cleanDigitalDecodeModalNodeV2638(mutation.target);
        shouldSweep = true;
      }
      for (const node of Array.from(mutation.addedNodes || [])) {
        cleanDigitalDecodeModalNodeV2638(node);
        shouldSweep = true;
      }
    }
    if (shouldSweep) cleanAllDigitalDecodeModalTextV2638();
  });

  function startV2638() {
    cleanAllDigitalDecodeModalTextV2638();
    observer.observe(document.body || document.documentElement, {
      childList: true,
      subtree: true,
      characterData: true
    });
    window.setInterval(cleanAllDigitalDecodeModalTextV2638, 1500);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startV2638, { once: true });
  } else {
    startV2638();
  }

  window.plutoFriendlyDigitalDecodeTextV2638 = friendlyDigitalDecodeTextV2638;
})();

// REMOVE_DECODER_TESTS_UI_V2_7_1 START
// Release cleanup: hide/remove offline decoder self-test launcher from the normal app UI.
(function removeDecoderTestsUiV271() {
  "use strict";

  if (window.__removeDecoderTestsUiV271Installed) return;
  window.__removeDecoderTestsUiV271Installed = true;

  const ids = [
    "decodeSelfTestLauncherV2628",
    "decodeSelfTestPanelV2628",
    "decoderSelfTestLauncherV2628",
    "decoderSelfTestPanelV2628"
  ];

  const selectors = [
    "#decodeSelfTestLauncherV2628",
    "#decodeSelfTestPanelV2628",
    "#decoderSelfTestLauncherV2628",
    "#decoderSelfTestPanelV2628",
    ".decode-selftest-launcher-v2628",
    ".decode-selftest-panel-v2628",
    ".decoder-selftest-panel"
  ];

  function removeDecodeTestNodes() {
    ids.forEach((id) => {
      const node = document.getElementById(id);
      if (node && node.parentNode) node.parentNode.removeChild(node);
    });
    document.querySelectorAll(selectors.join(",")).forEach((node) => {
      if (node && node.parentNode) node.parentNode.removeChild(node);
    });
  }

  function installRemovalObserver() {
    if (!document.body || window.__removeDecoderTestsUiV271Observer) return;
    window.__removeDecoderTestsUiV271Observer = new MutationObserver(() => removeDecodeTestNodes());
    window.__removeDecoderTestsUiV271Observer.observe(document.body, { childList: true, subtree: true });
  }

  try {
    window.plutoDecoderSelfTestsV2636 = undefined;
  } catch (_) {}

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      removeDecodeTestNodes();
      installRemovalObserver();
      window.setTimeout(removeDecodeTestNodes, 250);
      window.setTimeout(removeDecodeTestNodes, 1000);
    }, { once: true });
  } else {
    removeDecodeTestNodes();
    installRemovalObserver();
    window.setTimeout(removeDecodeTestNodes, 250);
    window.setTimeout(removeDecodeTestNodes, 1000);
  }
})();
// REMOVE_DECODER_TESTS_UI_V2_7_1 END


// PLUTO_UI_BADGE_PASS_REFRESH_RESCUE_V2_7_2_BEGIN
(function () {
  "use strict";
  const UI_VERSION = "v2.7.2";
  const BADGE_ID = "plutoVersionBadge";
  const PASS_RESCUE_KEY = "plutoPassRefreshRescueLastQueuedMsV272";
  const PASS_RESCUE_COOLDOWN_MS = 12 * 60 * 1000;
  const PASS_RESCUE_INTERVAL_MS = 60 * 1000;

  function ready(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn, { once: true });
    } else {
      fn();
    }
  }

  function ensureBadge() {
    let badge = document.getElementById(BADGE_ID);
    if (!badge) {
      badge = document.createElement("div");
      badge.id = BADGE_ID;
      badge.setAttribute("aria-label", "Pluto Satellite Tracker version");
      badge.style.position = "fixed";
      badge.style.right = "10px";
      badge.style.bottom = "8px";
      badge.style.zIndex = "9999";
      badge.style.padding = "4px 8px";
      badge.style.borderRadius = "8px";
      badge.style.fontFamily = "system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif";
      badge.style.fontSize = "11px";
      badge.style.lineHeight = "1.25";
      badge.style.color = "#d8ecff";
      badge.style.background = "rgba(2, 11, 22, 0.78)";
      badge.style.border = "1px solid rgba(125, 180, 255, 0.35)";
      badge.style.boxShadow = "0 2px 10px rgba(0,0,0,0.25)";
      badge.style.backdropFilter = "blur(4px)";
      document.body.appendChild(badge);
    }
    return badge;
  }

  function setBadge(text, warn) {
    const badge = ensureBadge();
    badge.textContent = text;
    badge.style.color = warn ? "#ffd6a0" : "#d8ecff";
    badge.title = "UI version and backend version. Added by v2.7.2 UI hotfix.";
  }

  async function refreshVersionBadge() {
    try {
      const resp = await fetch(`/api/status?version_badge=${Date.now()}`, { cache: "no-store" });
      if (!resp.ok) {
        setBadge(`UI ${UI_VERSION} | Backend unavailable`, true);
        return;
      }
      const data = await resp.json();
      const backend = data && data.version ? String(data.version) : "unknown";
      setBadge(`UI ${UI_VERSION} | Backend v${backend}`, false);
    } catch (err) {
      setBadge(`UI ${UI_VERSION} | Backend unavailable`, true);
    }
  }

  function pageLooksLikeWaitingForPassFile() {
    const text = (document.body && document.body.innerText ? document.body.innerText : "").toLowerCase();
    return text.includes("watching /api/passes for a new pass file") ||
           text.includes("loading quick pass preview");
  }

  function getLastRescueMs() {
    const raw = window.localStorage ? window.localStorage.getItem(PASS_RESCUE_KEY) : "";
    const value = Number(raw || 0);
    return Number.isFinite(value) ? value : 0;
  }

  function setLastRescueMs(value) {
    try {
      if (window.localStorage) {
        window.localStorage.setItem(PASS_RESCUE_KEY, String(value));
      }
    } catch (err) {
      // Ignore private-mode/localStorage errors.
    }
  }

  async function getRefreshState() {
    try {
      const resp = await fetch(`/api/refresh/status?pass_rescue=${Date.now()}`, { cache: "no-store" });
      if (!resp.ok) return "unknown";
      const data = await resp.json();
      return String((data && data.state) || "unknown").toLowerCase();
    } catch (err) {
      return "unknown";
    }
  }

  async function queuePassRefresh(reason) {
    const now = Date.now();
    const last = getLastRescueMs();
    if (now - last < PASS_RESCUE_COOLDOWN_MS) {
      return false;
    }
    const state = await getRefreshState();
    if (state === "running" || state === "queued" || state === "busy") {
      return false;
    }
    try {
      const resp = await fetch(`/api/refresh/passes?source=ui_rescue_v272&reason=${encodeURIComponent(reason)}`, {
        method: "POST",
        cache: "no-store"
      });
      if (resp.ok) {
        setLastRescueMs(now);
        console.info("[Pluto] Queued pass refresh rescue:", reason);
        refreshVersionBadge();
        return true;
      }
    } catch (err) {
      console.warn("[Pluto] Pass refresh rescue failed:", err);
    }
    return false;
  }

  async function passRefreshRescueTick() {
    if (pageLooksLikeWaitingForPassFile()) {
      await queuePassRefresh("quick_pass_preview_waiting");
    }
  }

  ready(function () {
    refreshVersionBadge();
    window.setInterval(refreshVersionBadge, 60 * 1000);

    // If the app opens to a missing/stale pass file while refresh is idle, kick a pass scan.
    window.setTimeout(passRefreshRescueTick, 2500);
    window.setInterval(passRefreshRescueTick, PASS_RESCUE_INTERVAL_MS);

    // Manual browser-console hook for field debugging.
    window.plutoPassRefreshRescueNowV272 = function () {
      return queuePassRefresh("manual_console");
    };
    window.plutoRefreshVersionBadgeV272 = refreshVersionBadge;
  });
})();
// PLUTO_UI_BADGE_PASS_REFRESH_RESCUE_V2_7_2_END

// PASS_REFRESH_IDLE_FORCE_V2_7_3 START
(function () {
  "use strict";

  const MARKER = "PASS_REFRESH_IDLE_FORCE_V2_7_3";
  const COOLDOWN_MS = 10 * 60 * 1000;
  const TICK_MS = 20000;
  let lastRefreshPostMs = 0;
  let inFlight = false;

  function pageText() {
    return (document.body && document.body.innerText) ? document.body.innerText : "";
  }

  function pageLooksStuckWaitingForPasses() {
    const text = pageText();
    return text.includes("Loading quick pass preview") &&
      text.includes("Watching /api/passes for a new pass file") &&
      text.includes("Refresh state: idle");
  }

  function ensureRefreshRescueBadge() {
    let badge = document.getElementById("plutoPassRefreshRescueBadgeV273");
    if (badge) return badge;
    badge = document.createElement("div");
    badge.id = "plutoPassRefreshRescueBadgeV273";
    badge.style.position = "fixed";
    badge.style.right = "12px";
    badge.style.bottom = "42px";
    badge.style.zIndex = "99999";
    badge.style.padding = "6px 9px";
    badge.style.borderRadius = "8px";
    badge.style.background = "rgba(15, 23, 42, 0.86)";
    badge.style.color = "#e5e7eb";
    badge.style.font = "12px system-ui, -apple-system, Segoe UI, sans-serif";
    badge.style.boxShadow = "0 6px 20px rgba(0,0,0,0.25)";
    badge.style.display = "none";
    document.body.appendChild(badge);
    return badge;
  }

  function showRefreshRescueMessage(message, ms) {
    const badge = ensureRefreshRescueBadge();
    badge.textContent = message;
    badge.style.display = "block";
    window.clearTimeout(showRefreshRescueMessage._timer);
    showRefreshRescueMessage._timer = window.setTimeout(() => {
      badge.style.display = "none";
    }, ms || 9000);
  }

  async function fetchJson(path, options) {
    const response = await fetch(path + (path.includes("?") ? "&" : "?") + "_=" + Date.now(), Object.assign({ cache: "no-store" }, options || {}));
    let data = null;
    try {
      data = await response.json();
    } catch (_) {
      data = { ok: false, parse_error: true };
    }
    data.http_ok = response.ok;
    data.http_status = response.status;
    return data;
  }

  function refreshStateIsBusy(data) {
    const state = String((data && data.state) || "").toLowerCase();
    return state === "running" || state === "queued" || state === "busy" || state === "refreshing";
  }

  async function queuePassRefreshV273(reason) {
    const now = Date.now();
    if (inFlight) return { ok: false, skipped: "in_flight" };
    if (now - lastRefreshPostMs < COOLDOWN_MS) return { ok: false, skipped: "cooldown" };

    inFlight = true;
    try {
      const status = await fetchJson("/api/refresh/status");
      if (refreshStateIsBusy(status)) {
        showRefreshRescueMessage("Pass refresh is already running.");
        return { ok: true, skipped: "already_running", status };
      }

      lastRefreshPostMs = now;
      const queued = await fetchJson("/api/refresh/passes", { method: "POST", body: "" });
      window.plutoLastPassRefreshRescueV273 = { marker: MARKER, reason, queued, at: new Date().toISOString() };
      if (queued && queued.http_ok && queued.ok !== false) {
        showRefreshRescueMessage("Queued pass refresh on Pluto.");
        if (typeof window.loadPasses === "function") {
          window.setTimeout(() => window.loadPasses().catch(() => {}), 5000);
        }
      } else {
        showRefreshRescueMessage("Pass refresh request failed; check backend status.", 12000);
      }
      return queued;
    } catch (error) {
      window.plutoLastPassRefreshRescueV273 = { marker: MARKER, reason, error: String(error), at: new Date().toISOString() };
      showRefreshRescueMessage("Pass refresh rescue error; check console.", 12000);
      return { ok: false, error: String(error) };
    } finally {
      inFlight = false;
    }
  }

  async function refreshRescueTickV273() {
    if (pageLooksStuckWaitingForPasses()) {
      await queuePassRefreshV273("quick pass preview waiting while refresh status is idle");
    }
  }

  window.plutoForcePassRefreshWhenIdleV273 = queuePassRefreshV273;
  window.plutoPassRefreshIdleRescueTickV273 = refreshRescueTickV273;

  function startRefreshRescueV273() {
    window.setTimeout(refreshRescueTickV273, 1500);
    window.setTimeout(refreshRescueTickV273, 8000);
    window.setInterval(refreshRescueTickV273, TICK_MS);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startRefreshRescueV273, { once: true });
  } else {
    startRefreshRescueV273();
  }
})();
// PASS_REFRESH_IDLE_FORCE_V2_7_3 END

// VERSION_BADGE_PASS_REPAIR_V2_7_4
(function () {
  const UI_VERSION = "2.7.4";
  const REFRESH_COOLDOWN_MS = 12 * 60 * 1000;
  let lastForcedRefreshMs = 0;

  function ensureVersionBadgeV274() {
    let style = document.getElementById("plutoVersionBadgeV274Style");
    if (!style) {
      style = document.createElement("style");
      style.id = "plutoVersionBadgeV274Style";
      style.textContent = "#plutoVersionBadgeV274{position:fixed;right:10px;bottom:8px;z-index:2147483000;padding:4px 8px;border-radius:8px;background:rgba(15,23,42,.84);color:#e5e7eb;font:11px/1.25 system-ui,-apple-system,Segoe UI,sans-serif;box-shadow:0 2px 10px rgba(0,0,0,.25);pointer-events:none;white-space:nowrap}#plutoVersionBadgeV274.warn{background:rgba(127,29,29,.88);color:#fee2e2}";
      (document.head || document.documentElement).appendChild(style);
    }
    let badge = document.getElementById("plutoVersionBadgeV274");
    if (!badge) {
      badge = document.createElement("div");
      badge.id = "plutoVersionBadgeV274";
      badge.textContent = "UI v" + UI_VERSION + " | Backend ...";
      (document.body || document.documentElement).appendChild(badge);
    }
    return badge;
  }

  function setVersionBadgeV274(text, warn) {
    const badge = ensureVersionBadgeV274();
    badge.textContent = text;
    badge.classList.toggle("warn", !!warn);
  }

  async function refreshVersionBadgeV274() {
    try {
      const res = await fetch("/api/status?version_badge=" + Date.now(), { cache: "no-store" });
      const data = await res.json();
      const backend = data && data.version ? data.version : "unknown";
      setVersionBadgeV274("UI v" + UI_VERSION + " | Backend v" + backend, !res.ok);
    } catch (err) {
      setVersionBadgeV274("UI v" + UI_VERSION + " | Backend unavailable", true);
    }
  }

  function pageLooksPassRefreshStuckV274() {
    const text = (document.body && document.body.innerText) ? document.body.innerText : "";
    return text.includes("Loading quick pass preview") &&
      text.includes("Watching /api/passes for a new pass file") &&
      text.toLowerCase().includes("refresh state: idle");
  }

  async function forcePassRefreshWhenIdleV274(reason) {
    const now = Date.now();
    if (now - lastForcedRefreshMs < REFRESH_COOLDOWN_MS) {
      return { ok: true, skipped: true, reason: "cooldown" };
    }
    try {
      const statusRes = await fetch("/api/refresh/status?pass_rescue=" + now, { cache: "no-store" });
      const status = await statusRes.json().catch(() => ({}));
      const state = String((status && status.state) || "").toLowerCase();
      if (state && state !== "idle" && state !== "stopped" && state !== "complete" && state !== "completed") {
        return { ok: true, skipped: true, state };
      }
      lastForcedRefreshMs = now;
      const res = await fetch("/api/refresh/passes?pass_rescue=" + encodeURIComponent(reason || "ui"), {
        method: "POST",
        cache: "no-store"
      });
      const data = await res.json().catch(() => ({}));
      console.log("Pass refresh rescue queued", reason, data);
      return data;
    } catch (err) {
      console.warn("Pass refresh rescue failed", err);
      return { ok: false, error: String(err) };
    }
  }

  function startPassRefreshWatcherV274() {
    setInterval(() => {
      if (pageLooksPassRefreshStuckV274()) {
        forcePassRefreshWhenIdleV274("quick-pass-preview-idle");
      }
    }, 5000);
    setTimeout(() => {
      if (pageLooksPassRefreshStuckV274()) {
        forcePassRefreshWhenIdleV274("quick-pass-preview-initial");
      }
    }, 1500);
  }

  function bootV274() {
    ensureVersionBadgeV274();
    refreshVersionBadgeV274();
    startPassRefreshWatcherV274();
    setInterval(refreshVersionBadgeV274, 15000);
  }

  window.plutoRefreshVersionBadgeV274 = refreshVersionBadgeV274;
  window.plutoForcePassRefreshWhenIdleV274 = forcePassRefreshWhenIdleV274;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootV274, { once: true });
  } else {
    bootV274();
  }
})();

/* RECEIVE_DIAGNOSTICS_PANEL_V2_8_4
 * UI-only selected-pass receive/decode readiness panel.  This does not start
 * hardware, touch backend C, or change the existing Listen audio path.
 */
function receiveDiagnosticsModeTextV284(pass) {
  const radio = pass && pass.radio ? pass.radio : {};
  const modes = Array.isArray(pass && pass.modes) ? pass.modes : [];
  return String(radio.mode || modes[0] || radio.description || "").trim();
}

function receiveDiagnosticsDownlinkHzV284(pass) {
  const radio = pass && pass.radio ? pass.radio : {};
  const downlinks = Array.isArray(pass && pass.downlinks_hz) ? pass.downlinks_hz : [];
  return Number(radio.downlink_hz || downlinks[0] || 0);
}

function receiveDiagnosticsFamilyV284(pass) {
  const radio = pass && pass.radio ? pass.radio : {};
  const text = [
    receiveDiagnosticsModeTextV284(pass),
    radio.description || "",
    Array.isArray(pass && pass.modes) ? pass.modes.join(" ") : "",
    pass && pass.name ? pass.name : ""
  ].join(" ").toUpperCase();

  if (/APRS/.test(text)) return { family: "APRS", action: "Receive", target: "AX.25/APRS packet decoder", kind: "digital" };
  if (/AX\.?25|PACKET/.test(text)) return { family: "Packet", action: "Receive", target: "AX.25 packet decoder", kind: "digital" };
  if (/AFSK/.test(text)) return { family: "AFSK", action: "Receive", target: "AFSK demodulator plus packet decoder", kind: "digital" };
  if (/FSK|GFSK|MSK|GMSK|BPSK|QPSK|PSK/.test(text)) return { family: "FSK/PSK", action: "Receive", target: "symbol demodulator and telemetry decoder", kind: "digital" };
  if (/CW|MORSE|BEACON/.test(text)) return { family: "CW", action: "Receive", target: "CW tone detector and Morse decoder", kind: "digital" };
  if (/TELEMETRY|DATA|DIGITAL|BPSK|9K6|1K2|1200|9600/.test(text)) return { family: "Telemetry", action: "Receive", target: "telemetry decoder matched to satellite mode", kind: "digital" };
  if (/FM|NFM|WFM|AM|VOICE|SSB|USB|LSB/.test(text)) return { family: "Voice", action: "Listen", target: "analog audio monitor", kind: "voice" };
  return { family: "Unknown", action: "Receive", target: "mode-specific decoder after capture support is added", kind: "unknown" };
}

function receiveDiagnosticsTimingV284(pass) {
  try {
    if (typeof passTimingState === "function") return passTimingState(pass);
  } catch (_error) {
  }
  return "unknown";
}

function receiveDiagnosticsTunableV284(pass) {
  try {
    return typeof isPassTunable === "function" && isPassTunable(pass);
  } catch (_error) {
    return false;
  }
}

function renderReceiveDiagnosticsPanelV284(pass) {
  const statusNode = document.getElementById("receiveDiagnosticsStatusV284");
  const noteNode = document.getElementById("receiveDiagnosticsNoteV284");
  if (!statusNode) return;

  const selected = pass || currentSelectedPass || null;
  if (!selected) {
    setDl("receiveDiagnosticsStatusV284", [
      ["Selected pass", "Select Details from Next Passes."],
      ["Receive state", "Idle"]
    ]);
    if (noteNode) noteNode.textContent = "Select a pass to inspect receive/decode readiness.";
    return;
  }

  const downlink = receiveDiagnosticsDownlinkHzV284(selected);
  const mode = receiveDiagnosticsModeTextV284(selected) || "Unknown";
  const family = receiveDiagnosticsFamilyV284(selected);
  const tunable = receiveDiagnosticsTunableV284(selected);
  const timing = receiveDiagnosticsTimingV284(selected);
  const action = family.action;
  const receiveReady = tunable && timing !== "stale";

  setDl("receiveDiagnosticsStatusV284", [
    ["Selected pass", selected.name || "Unnamed satellite"],
    ["Mode", mode],
    ["Receive family", family.family],
    ["Operator action", action],
    ["Downlink", downlink ? formatHz(downlink) : "No downlink"],
    ["Pluto tunable", tunable ? "Yes" : "No"],
    ["Pass timing", timing],
    ["Future decoder target", family.target],
    ["Phase", receiveReady ? "Ready for future Track → Capture → Decode workflow" : "Not ready for receive capture"]
  ]);

  if (noteNode) {
    if (family.kind === "voice") {
      noteNode.textContent = "Voice-style passes should continue to use Listen. Receive is reserved for decode/capture modes.";
    } else if (receiveReady) {
      noteNode.textContent = "Receive Phase 1 is UI guidance only. A later backend step will add capture/decode for this mode family.";
    } else {
      noteNode.textContent = "This pass is not currently ready for receive capture; check downlink, tunable status, and pass timing.";
    }
  }
}

function bindReceiveDiagnosticsPanelV284() {
  const button = document.getElementById("refreshReceiveDiagnosticsButtonV284");
  if (button && button.dataset.receiveDiagnosticsBoundV284 !== "1") {
    button.dataset.receiveDiagnosticsBoundV284 = "1";
    button.addEventListener("click", () => renderReceiveDiagnosticsPanelV284(currentSelectedPass));
  }
  renderReceiveDiagnosticsPanelV284(currentSelectedPass);
}

(function installReceiveDiagnosticsPanelV284() {
  const install = () => {
    bindReceiveDiagnosticsPanelV284();
    if (typeof renderPassDetail === "function" && renderPassDetail.receiveDiagnosticsWrappedV284 !== true) {
      const originalRenderPassDetailV284 = renderPassDetail;
      renderPassDetail = function receiveDiagnosticsRenderPassDetailWrapperV284(pass) {
        const result = originalRenderPassDetailV284.apply(this, arguments);
        try { renderReceiveDiagnosticsPanelV284(pass); } catch (_error) {}
        return result;
      };
      renderPassDetail.receiveDiagnosticsWrappedV284 = true;
    }
  };
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", install, { once: true });
  } else {
    install();
  }
})();



/* PASS_LIST_RECEIVE_BADGES_V2_8_5
 * Adds lightweight Listen/Receive capability badges to pass-list style rows.
 * This is UI-only: it does not tune, start audio, or call decoder endpoints.
 */
(function installPassListReceiveBadgesV285() {
  const badgeClass = "receive-capability-badge-v285";

  function modeTextFromElementV285(el) {
    const text = String(el ? el.textContent || "" : "").replace(/\s+/g, " ").trim().toUpperCase();
    return text;
  }

  function receiveFamilyFromTextV285(text) {
    if (/APRS/.test(text)) return { family: "APRS", kind: "receive", label: "Receive: APRS" };
    if (/(AX\.25|AX25|PACKET)/.test(text)) return { family: "Packet", kind: "receive", label: "Receive: Packet" };
    if (/(CW|MORSE|A1A)/.test(text)) return { family: "CW", kind: "receive", label: "Receive: CW" };
    if (/(AFSK|1200\s*BAUD)/.test(text)) return { family: "AFSK", kind: "receive", label: "Receive: AFSK" };
    if (/(G3RUH|9600|GMSK|GFSK|FSK|BPSK|PSK|TELEMETRY|DIGITAL|DATA|BEACON)/.test(text)) {
      return { family: "Digital", kind: "receive", label: "Receive: Digital" };
    }
    if (/(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E)/.test(text)) {
      return { family: "Voice", kind: "listen", label: "Listen" };
    }
    return { family: "Unknown", kind: "unknown", label: "Radio" };
  }

  function likelyPassRowV285(el) {
    if (!el || el.nodeType !== 1) return false;
    if (el.closest && el.closest("#appDrawer, #receivePlaceholderModalV282, #receiveDecodeModalV2626, #spectrumWaterfallModal, #rotatorControlModal")) return false;
    const text = modeTextFromElementV285(el);
    if (text.length < 20) return false;
    const hasRadioHint = /(MHz|DOWNLINK|MODE|AOS|LOS|MAX EL|SATELLITE|NORAD|DETAILS|LISTEN|RECEIVE|DECODE)/.test(text);
    const hasAction = !!el.querySelector("button, a[role='button']");
    return hasRadioHint && hasAction;
  }

  function badgeHostV285(row) {
    return row.querySelector(".pass-meta, .pass-summary, .pass-card-header, .pass-list-meta, .detail-subtitle") || row.firstElementChild || row;
  }

  function decorateOneRowV285(row) {
    if (!likelyPassRowV285(row)) return;
    const existing = row.querySelector(`.${badgeClass}`);
    const classification = receiveFamilyFromTextV285(modeTextFromElementV285(row));
    if (classification.kind === "unknown") {
      if (existing) existing.remove();
      return;
    }

    const badge = existing || document.createElement("span");
    badge.className = `${badgeClass} ${badgeClass}-${classification.kind}`;
    badge.textContent = classification.label;
    badge.title = classification.kind === "receive"
      ? `${classification.family} pass: use Receive/decode workflow, not analog Listen.`
      : "Voice-style pass: use the proven Listen audio path.";
    badge.setAttribute("aria-label", badge.title);

    if (!existing) {
      const host = badgeHostV285(row);
      if (host && host.prepend) host.prepend(badge);
    }
  }

  function decoratePassRowsV285() {
    const rows = Array.from(document.querySelectorAll("tr, li, article, .pass-card, .pass-row, .pass-list-item, .pass-item, .next-pass, .pass-detail-compact"));
    rows.forEach(decorateOneRowV285);
  }

  let scheduled = 0;
  function scheduleDecoratePassRowsV285() {
    if (scheduled) return;
    scheduled = window.setTimeout(() => {
      scheduled = 0;
      decoratePassRowsV285();
    }, 100);
  }

  document.addEventListener("DOMContentLoaded", () => {
    decoratePassRowsV285();
    const observer = new MutationObserver(scheduleDecoratePassRowsV285);
    observer.observe(document.body, { childList: true, subtree: true, characterData: true });
    window.setInterval(decoratePassRowsV285, 2500);
  });
  if (document.readyState !== "loading") {
    decoratePassRowsV285();
    window.setInterval(decoratePassRowsV285, 2500);
  }
})();


/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6_BEGIN */
(function installActivePassActionButtonPolicyV286() {
  if (window.__plutoActivePassActionButtonPolicyV286) return;
  window.__plutoActivePassActionButtonPolicyV286 = true;

  if (typeof bindReceiveDecodePlaceholderV282 === "function") {
    bindReceiveDecodePlaceholderV282 = function bindReceiveDecodePlaceholderHiddenV286(_pass, node) {
      const receiveButton = node && node.querySelector ? node.querySelector("#receiveDecodePlaceholderButtonV282") : null;
      const receiveStatus = node && node.querySelector ? node.querySelector("#receiveDecodeStatusV282") : null;
      if (receiveButton) {
        receiveButton.hidden = true;
        receiveButton.disabled = true;
        receiveButton.classList.add("pass-action-button-v286");
      }
      if (receiveStatus) {
        receiveStatus.hidden = true;
        receiveStatus.textContent = "";
      }
    };
  }

  if (typeof bindAnalogAudio === "function" && !bindAnalogAudio.activePassActionWrappedV286) {
    const previousBindAnalogAudioV286 = bindAnalogAudio;
    bindAnalogAudio = function bindAnalogAudioActivePassActionV286(pass, node) {
      const result = previousBindAnalogAudioV286.apply(this, arguments);
      const button = node && node.querySelector ? node.querySelector("#analogAudioToggleButton") : null;
      const status = node && node.querySelector ? node.querySelector("#analogAudioStatus") : null;
      if (!button) return result;
      button.classList.add("pass-action-button-v286", "pass-primary-action-button-v286");
      const allowed = applyPassActionButtonVisualsV286(button, pass);
      if (status && !allowed) status.textContent = passActionInactiveTextV286(pass);
      button.addEventListener("click", (event) => {
        if (!passActionIsActiveV286(pass) || !passActionTargetAvailableV286(pass)) {
          event.preventDefault();
          event.stopPropagation();
          if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
          if (status) status.textContent = passActionInactiveTextV286(pass);
        }
      }, true);
      return result;
    };
    bindAnalogAudio.activePassActionWrappedV286 = true;
  }

  function cleanupOldPassiveBadgesV286() {
    document.querySelectorAll(".pass-row .receive-capability-badge-v285").forEach((badge) => {
      if (badge.closest(".pass-row-actions-v286")) return;
      badge.remove();
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", cleanupOldPassiveBadgesV286, { once: true });
  } else {
    cleanupOldPassiveBadgesV286();
  }
  window.setInterval(cleanupOldPassiveBadgesV286, 2500);
})();
/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6_END */



/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6D_PROTOCOL_LABEL_FIX
 * Correct active-pass action labels so the compact buttons use the same
 * protocol-aware language as the pass-list badges.  Also remove the redundant
 * secondary Decode CW button under the sky plot; the primary action button now
 * carries the correct protocol label and remains inactive until AOS.
 */
function passActionTextV286D(value) {
  if (value === undefined || value === null) return "";
  if (Array.isArray(value)) return value.map(passActionTextV286D).join(" ");
  if (typeof value === "object") {
    try { return JSON.stringify(value); } catch (_error) { return ""; }
  }
  return String(value);
}

function passActionClassificationV286(pass) {
  const radio = (pass && pass.radio) || {};
  const text = [
    radio.mode,
    radio.description,
    radio.type,
    radio.status,
    (pass && pass.modes) || [],
    pass && pass.name,
    pass && pass.transmitters,
    pass && pass.downlinks,
    pass && pass.uplinks
  ].map(passActionTextV286D).join(" ").replace(/\s+/g, " ").toUpperCase();

  if (/APRS/.test(text)) return { family: "APRS", kind: "receive", label: "Receive: APRS" };
  if (/(AX\.?25|PACKET)/.test(text)) return { family: "Packet", kind: "receive", label: "Receive: Packet" };
  if (/(CW|MORSE|A1A)/.test(text)) return { family: "CW", kind: "receive", label: "Receive: CW" };
  if (/(AFSK|1200\s*BAUD|1K2)/.test(text)) return { family: "AFSK", kind: "receive", label: "Receive: AFSK" };
  if (/(G3RUH|9600|9K6|GMSK|GFSK|FSK|BPSK|QPSK|PSK|TELEMETRY|DIGITAL|DATA|BEACON)/.test(text)) {
    return { family: "Digital", kind: "receive", label: "Receive: Digital" };
  }
  if (/(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E|J3E)/.test(text)) {
    return { family: "Voice", kind: "listen", label: "Listen" };
  }
  return { family: "Unknown", kind: "listen", label: "Radio" };
}

function passActionKindV286(pass) {
  const classification = passActionClassificationV286(pass);
  return classification.kind === "receive" ? "receive" : "listen";
}

function passActionLabelV286(pass) {
  return passActionClassificationV286(pass).label;
}

function passActionInactiveTextV286(pass) {
  const label = passActionLabelV286(pass);
  if (!pass) return `${label} is inactive until a pass is selected.`;
  if (!passActionTargetAvailableV286(pass)) return `${label} unavailable: no tunable downlink.`;
  const state = passActionTimingStateV286(pass);
  if (state === "upcoming") return `${label} active at AOS (${formatTime(pass.aos_utc)}).`;
  if (state === "stale") return `${label} inactive; pass ended.`;
  return `${label} inactive until the pass is active.`;
}

(function installActivePassActionButtonCleanupV286D() {
  if (window.__plutoActivePassActionButtonCleanupV286D) return;
  window.__plutoActivePassActionButtonCleanupV286D = true;

  function cleanupV286D() {
    document.querySelectorAll("#linearTransponderDecodeCwButtonV2632, .linear-cw-override-button-v2632").forEach((button) => button.remove());
    document.querySelectorAll(".listen-panel #analogAudioStatus, .listen-panel #receiveDecodeStatusV282").forEach((node) => {
      node.hidden = true;
      node.textContent = "";
    });
    document.querySelectorAll(".pass-row-action-button-v286, #analogAudioToggleButton.pass-action-button-v286").forEach((button) => {
      if (/^Receive$/.test(String(button.textContent || "").trim())) {
        button.textContent = button.dataset.passActionKindV286 === "receive" ? "Receive: Digital" : button.textContent;
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", cleanupV286D, { once: true });
  } else {
    cleanupV286D();
  }
  window.setInterval(cleanupV286D, 1200);
})();
/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6D_PROTOCOL_LABEL_FIX_END */

/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6E_BEGIN
 * Polish v2.8.6 pass action controls: protocol-aware compact blue buttons,
 * Details button shape alignment, and no redundant Listen/Receive control under
 * the sky/azimuth plot.  UI-only; backend C remains unchanged.
 */
(function installActivePassActionButtonPolishV286E() {
  if (window.__plutoActivePassActionButtonPolishV286E) return;
  window.__plutoActivePassActionButtonPolishV286E = true;

  function textV286E(value) {
    if (value === undefined || value === null) return "";
    if (Array.isArray(value)) return value.map(textV286E).join(" ");
    if (typeof value === "object") {
      try { return JSON.stringify(value); } catch (_error) { return ""; }
    }
    return String(value);
  }

  function passProtocolTextV286E(pass) {
    const radio = (pass && pass.radio) || {};
    return [
      radio.mode,
      radio.type,
      radio.status,
      radio.description,
      pass && pass.mode,
      pass && pass.modes,
      pass && pass.name,
      pass && pass.transmitters,
      pass && pass.downlinks,
      pass && pass.uplinks
    ].map(textV286E).join(" ").toUpperCase();
  }

  function passActionClassificationV286E(pass) {
    const text = passProtocolTextV286E(pass);
    if (/APRS/.test(text)) return { kind: "receive", family: "APRS", label: "Receive: APRS" };
    if (/(AX\.?25|AX25|PACKET)/.test(text)) return { kind: "receive", family: "Packet", label: "Receive: Packet" };
    if (/(CW|MORSE|A1A)/.test(text)) return { kind: "receive", family: "CW", label: "Receive: CW" };
    if (/(AFSK|1200\s*BAUD|1K2)/.test(text)) return { kind: "receive", family: "AFSK", label: "Receive: AFSK" };
    if (/(G3RUH|9K6|9600|GMSK|GFSK|FSK|BPSK|QPSK|PSK|TELEMETRY|DIGITAL|DATA)/.test(text)) {
      return { kind: "receive", family: "Digital", label: "Receive: Digital" };
    }
    if (/(FM|NFM|WFM|AM|SSB|USB|LSB|VOICE|F3E|A3E|J3E)/.test(text)) {
      return { kind: "listen", family: "Voice", label: "Listen" };
    }
    return { kind: "listen", family: "Voice", label: "Listen" };
  }

  function actionStateV286E(pass) {
    try {
      if (typeof passTimingState === "function") return passTimingState(pass);
    } catch (_error) {}
    return "unknown";
  }

  function actionIsActiveV286E(pass) {
    return actionStateV286E(pass) === "active";
  }

  function actionDownlinkV286E(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function actionTargetAvailableV286E(pass) {
    if (!pass || !actionDownlinkV286E(pass)) return false;
    try {
      if (typeof isPassTunable === "function") return !!isPassTunable(pass);
    } catch (_error) {}
    return true;
  }

  function inactiveTextV286E(pass) {
    const label = passActionClassificationV286E(pass).label;
    if (!pass) return `${label} is inactive until a pass is selected.`;
    if (!actionTargetAvailableV286E(pass)) return `${label} is unavailable because this pass has no tunable downlink.`;
    const state = actionStateV286E(pass);
    if (state === "upcoming") return `${label} becomes active at AOS (${typeof formatTime === "function" ? formatTime(pass.aos_utc) : pass.aos_utc || "scheduled AOS"}).`;
    if (state === "stale") return `${label} is inactive because this pass has ended.`;
    return `${label} is inactive until the pass is active.`;
  }

  window.passActionClassificationV286E = passActionClassificationV286E;

  try {
    passActionLabelV286 = function passActionProtocolLabelV286E(pass) {
      return passActionClassificationV286E(pass).label;
    };
  } catch (_error) {}

  try {
    passActionKindV286 = function passActionProtocolKindV286E(pass) {
      return passActionClassificationV286E(pass).kind;
    };
  } catch (_error) {}

  try {
    passActionInactiveTextV286 = inactiveTextV286E;
  } catch (_error) {}

  try {
    applyPassActionButtonVisualsV286 = function applyPassActionButtonVisualsPolishedV286E(button, pass) {
      if (!button) return false;
      const classification = passActionClassificationV286E(pass);
      const active = actionIsActiveV286E(pass);
      const target = actionTargetAvailableV286E(pass);
      button.classList.add("pass-action-button-v286", "pass-action-button-v286e");
      button.classList.toggle("pass-action-receive-v286", classification.kind === "receive");
      button.classList.toggle("pass-action-listen-v286", classification.kind === "listen");
      button.dataset.passActionKindV286 = classification.kind;
      button.dataset.passActionFamilyV286e = classification.family;
      button.dataset.passActiveV286 = active ? "1" : "0";
      if (!/^Stop/i.test(String(button.textContent || ""))) {
        button.textContent = classification.label;
      }
      button.disabled = !(active && target);
      button.title = active && target ? `${classification.label} active for this pass.` : inactiveTextV286E(pass);
      button.setAttribute("aria-disabled", button.disabled ? "true" : "false");
      return active && target;
    };
  } catch (_error) {}

  function hideSkyPrimaryActionsV286E(root) {
    const scope = root || document;
    scope.querySelectorAll(
      ".listen-panel #analogAudioToggleButton, " +
      ".listen-panel #receiveDecodePlaceholderButtonV282, " +
      ".listen-panel #linearTransponderDecodeCwButtonV2632"
    ).forEach((button) => {
      button.classList.add("sky-primary-action-hidden-v286e");
      button.setAttribute("aria-hidden", "true");
      button.tabIndex = -1;
    });

    scope.querySelectorAll(".listen-panel #analogAudioStatus, .listen-panel #receiveDecodeStatusV282").forEach((status) => {
      status.hidden = true;
      status.classList.add("sky-primary-action-hidden-v286e");
    });
  }

  function polishPassRowsV286E(root) {
    const scope = root || document;
    scope.querySelectorAll(".pass-row").forEach((row) => {
      const actionButton = row.querySelector(".pass-row-action-button-v286");
      const detailsButton = row.querySelector(".pass-detail-button");
      const readiness = row.querySelector(".radio-ok, .radio-warn");
      if (actionButton) {
        actionButton.classList.add("pass-action-button-v286e", "pass-row-action-button-v286e");
        if (typeof currentSelectedPass !== "undefined") {
          const strong = row.querySelector("strong");
          const selectedName = currentSelectedPass && currentSelectedPass.name;
          if (selectedName && strong && strong.textContent === selectedName) {
            try { applyPassActionButtonVisualsV286(actionButton, currentSelectedPass); } catch (_error) {}
          }
        }
      }
      if (detailsButton) {
        detailsButton.classList.add("pass-detail-button-v286e", "pass-action-button-v286e");
      }
      if (readiness) {
        readiness.classList.add("pass-radio-readiness-hidden-v286e");
      }
    });
  }

  function polishAllV286E() {
    hideSkyPrimaryActionsV286E(document);
    polishPassRowsV286E(document);
  }

  if (typeof renderMapPanel === "function" && renderMapPanel.activePassActionPolishWrappedV286E !== true) {
    const previousRenderMapPanelV286E = renderMapPanel;
    renderMapPanel = function renderMapPanelActivePassActionPolishV286E() {
      const result = previousRenderMapPanelV286E.apply(this, arguments);
      try { hideSkyPrimaryActionsV286E(document); } catch (_error) {}
      return result;
    };
    renderMapPanel.activePassActionPolishWrappedV286E = true;
  }

  if (typeof renderPasses === "function" && renderPasses.activePassActionPolishWrappedV286E !== true) {
    const previousRenderPassesV286E = renderPasses;
    renderPasses = function renderPassesActivePassActionPolishV286E() {
      const result = previousRenderPassesV286E.apply(this, arguments);
      try { polishPassRowsV286E(document); } catch (_error) {}
      return result;
    };
    renderPasses.activePassActionPolishWrappedV286E = true;
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", polishAllV286E, { once: true });
  } else {
    polishAllV286E();
  }

  const observer = new MutationObserver(() => {
    window.clearTimeout(window.__plutoActionButtonPolishTimerV286E || 0);
    window.__plutoActionButtonPolishTimerV286E = window.setTimeout(polishAllV286E, 60);
  });
  if (document.body) observer.observe(document.body, { childList: true, subtree: true });
  window.setInterval(polishAllV286E, 2000);
})();
/* ACTIVE_PASS_ACTION_BUTTONS_V2_8_6E_END */

/* GMSK_SIGNAL_DIAGNOSTICS_V2_8_7
 * Phase 1 GMSK/9k6 receive diagnostics display.
 * This does not claim bit/HDLC/text decode. It replaces generic AFSK tone
 * copy in the live decode modal with GMSK-specific clock/slicer readiness
 * information derived from the existing signal diagnostic output.
 */
(function installGmskSignalDiagnosticsV287() {
  const MARKER = "GMSK_SIGNAL_DIAGNOSTICS_V2_8_7";
  if (window.__plutoGmskSignalDiagnosticsV287) return;
  window.__plutoGmskSignalDiagnosticsV287 = true;

  function numberFromTextV287(text, patterns) {
    for (const pattern of patterns) {
      const match = String(text || "").match(pattern);
      if (match && match[1] !== undefined) {
        const value = Number(match[1]);
        if (Number.isFinite(value)) return value;
      }
    }
    return null;
  }

  function hasGmskDiagnosticV287(text) {
    const upper = String(text || "").toUpperCase();
    return /\b(GMSK|G3RUH|9K6|9600)\b/.test(upper) || upper.includes("FSK/GMSK");
  }

  function stripPreviousGmskBlockV287(text) {
    return String(text || "")
      .replace(/\n?--- GMSK Phase 1 diagnostics \(GMSK_SIGNAL_DIAGNOSTICS_V2_8_7\)[\s\S]*$/m, "")
      .replace(/\n?\[GMSK_SIGNAL_DIAGNOSTICS_V2_8_7[\s\S]*$/m, "")
      .trimEnd();
  }

  function removeGenericAfskLinesForGmskV287(text) {
    return String(text || "")
      .split(/\r?\n/)
      .filter((line) => {
        const normalized = line.trim();
        if (/^(1200|2200)\s*Hz energy/i.test(normalized)) return false;
        if (/^Dominant tone\b/i.test(normalized)) return false;
        if (/^mark balance\b/i.test(normalized)) return false;
        if (/Live bit\/HDLC\/text recovery is not claimed/i.test(normalized)) return false;
        return true;
      })
      .join("\n")
      .trimEnd();
  }

  function levelSummaryV287(rms, peak) {
    if (rms === null && peak === null) return "unknown - no level metrics reported";
    if (peak !== null && peak >= 30000) return "too hot / clipping risk - reduce gain or input level";
    if (rms !== null && rms < 200) return "weak - signal may be below reliable slicer threshold";
    if (rms !== null && rms < 700) return "usable but light - antenna/aim/gain may still matter";
    if (rms !== null && rms <= 6000 && (peak === null || peak < 22000)) return "healthy diagnostic level";
    return "strong - verify it is not clipping during peak Doppler/audio changes";
  }

  function lockSummaryV287(sampleRate, baud, zc, dominant) {
    const sps = sampleRate / baud;
    let clock = "diagnostic only - symbol clock recovery not connected";
    let slicer = "diagnostic only - slicer threshold not locked";
    let transition = "not estimated from bit transitions yet";

    if (zc !== null) {
      const normalized = zc / baud;
      if (normalized > 0.35 && normalized < 1.35) {
        transition = `candidate transition activity present (${normalized.toFixed(2)} x symbol rate)`;
      } else {
        transition = `transition estimate not near symbol rate (${normalized.toFixed(2)} x symbol rate)`;
      }
    }

    if (dominant !== null) {
      if (dominant > 2500 && dominant < 5500) {
        slicer = "diagnostic candidate - discriminator energy present; slicer still not locked";
      } else {
        slicer = "diagnostic only - dominant component does not prove GMSK slicer lock";
      }
    }

    if (sps < 2.2) {
      clock = `very tight at ${sps.toFixed(2)} samples/symbol - prefer higher-rate discriminator path`;
    } else if (sps < 3.2) {
      clock = `tight but workable for diagnostics at ${sps.toFixed(2)} samples/symbol`;
    } else {
      clock = `adequate diagnostic sampling at ${sps.toFixed(2)} samples/symbol`;
    }

    return { samplesPerSymbol: sps, clock, slicer, transition };
  }

  function buildGmskBlockV287(text) {
    if (!hasGmskDiagnosticV287(text)) return "";

    const sampleRate = numberFromTextV287(text, [
      /at\s+(\d+(?:\.\d+)?)\s*Hz/i,
      /sample[_ ]rate[^0-9]*(\d+(?:\.\d+)?)/i
    ]) || 24000;
    const samples = numberFromTextV287(text, [
      /Samples:\s*(\d+(?:\.\d+)?)/i,
      /PCM samples analyzed:\s*(\d+(?:\.\d+)?)/i
    ]);
    const rms = numberFromTextV287(text, [/RMS:\s*([0-9]+(?:\.[0-9]+)?)/i, /RMS\s+([0-9]+(?:\.[0-9]+)?)/i]);
    const peak = numberFromTextV287(text, [/Peak:\s*([0-9]+(?:\.[0-9]+)?)/i, /peak\s+([0-9]+(?:\.[0-9]+)?)/i]);
    const zc = numberFromTextV287(text, [/zero-crossing estimate\s+([0-9]+(?:\.[0-9]+)?)/i]);
    const dominant = numberFromTextV287(text, [/Dominant tone\s+([0-9]+(?:\.[0-9]+)?)/i]);
    const baud = /\b(9K6|9600|G3RUH)\b/i.test(text) ? 9600 : 9600;
    const lock = lockSummaryV287(sampleRate, baud, zc, dominant);
    const duration = samples !== null && sampleRate > 0 ? samples / sampleRate : null;

    const lines = [];
    lines.push(`--- GMSK Phase 1 diagnostics (${MARKER}) ---`);
    lines.push(`Target modem: GMSK/FSK diagnostic path, assuming ${baud} symbols/sec until satellite-specific metadata says otherwise.`);
    lines.push(`Sample window: ${samples !== null ? String(Math.round(samples)) : "unknown"} samples${duration !== null ? ` (${duration.toFixed(2)} sec)` : ""} at ${Math.round(sampleRate)} Hz.`);
    lines.push(`Samples per symbol: ${lock.samplesPerSymbol.toFixed(2)}.`);
    lines.push(`Level check: ${levelSummaryV287(rms, peak)}${rms !== null || peak !== null ? ` (RMS ${rms ?? "?"}, peak ${peak ?? "?"}).` : "."}`);
    lines.push(`Clock readiness: ${lock.clock}.`);
    lines.push(`Slicer readiness: ${lock.slicer}.`);
    lines.push(`Transition activity: ${lock.transition}.`);
    lines.push("Frame recovery: HDLC/AX.25/text decode is not claimed in this phase.");
    lines.push("Next backend step: feed a higher-rate discriminator/baseband stream into clock recovery, then add slicer and HDLC flag/frame counting.");
    return lines.join("\n");
  }

  function enhanceDecodeTextV287(text) {
    const base = stripPreviousGmskBlockV287(text);
    const block = buildGmskBlockV287(base);
    if (!block) return text;
    const cleaned = removeGenericAfskLinesForGmskV287(base);
    return `${cleaned}\n\n${block}`;
  }

  function shouldEnhanceNodeV287(node) {
    if (!node || node.nodeType !== Node.ELEMENT_NODE) return false;
    const id = String(node.id || "");
    const cls = String(node.className || "");
    return /decode.*output/i.test(id) || /receive.*decode.*output/i.test(id) || /decode.*output/i.test(cls);
  }

  function enhanceNodeV287(node) {
    if (!shouldEnhanceNodeV287(node)) return;
    const before = node.textContent || "";
    if (!hasGmskDiagnosticV287(before)) return;
    const after = enhanceDecodeTextV287(before);
    if (after !== before) node.textContent = after;
  }

  function scanV287(root) {
    const scope = root && root.querySelectorAll ? root : document;
    if (shouldEnhanceNodeV287(scope)) enhanceNodeV287(scope);
    scope.querySelectorAll?.("pre, code, textarea, #receiveDecodeOutputV2626D, #receiveDecodeOutputV2626, #linearCwDecodeOutputV2632").forEach(enhanceNodeV287);
  }

  let scheduled = 0;
  function scheduleScanV287() {
    if (scheduled) return;
    scheduled = window.setTimeout(() => {
      scheduled = 0;
      scanV287(document);
    }, 80);
  }

  function installV287() {
    scanV287(document);
    const observer = new MutationObserver((records) => {
      for (const record of records) {
        if (record.type === "characterData") {
          const parent = record.target && record.target.parentElement;
          if (parent && shouldEnhanceNodeV287(parent)) {
            scheduleScanV287();
            return;
          }
        }
        for (const node of record.addedNodes || []) {
          if (node.nodeType === Node.ELEMENT_NODE && (shouldEnhanceNodeV287(node) || node.querySelector?.("pre, code, textarea"))) {
            scheduleScanV287();
            return;
          }
        }
      }
    });
    observer.observe(document.body || document.documentElement, { childList: true, subtree: true, characterData: true });
    window.setInterval(() => scanV287(document), 1500);
    window.plutoEnhanceGmskDiagnosticsV287 = enhanceDecodeTextV287;
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", installV287, { once: true });
  } else {
    installV287();
  }
})();


/* PASS_ROW_BACKEND_TEST_MODAL_V2_8_27
 * Row action test modal. Opens an explicit operator dialog for pass-row
 * Listen/Receive backend tests. This avoids relying on the removed azimuth-map
 * action controls and gives a visible Stop/Close/status surface even outside
 * AOS/LOS. Web UI only; backend C is unchanged.
 */
(function installPassRowBackendTestModalV2827() {
  if (window.__plutoPassRowBackendTestModalV2827) return;
  window.__plutoPassRowBackendTestModalV2827 = true;
  const MARKER = "PASS_ROW_BACKEND_TEST_MODAL_V2_8_27";

  function modeTextV2827(pass) {
    const radio = (pass && pass.radio) || {};
    return String(radio.mode || ((pass && pass.modes) || [])[0] || "").trim();
  }

  function downlinkHzV2827(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function formatHzV2827(hz) {
    try {
      if (typeof formatHz === "function") return formatHz(hz);
    } catch (_error) {}
    const value = Number(hz || 0);
    return Number.isFinite(value) && value > 0 ? `${(value / 1000000).toFixed(3)} MHz` : "No downlink";
  }

  function passLabelV2827(pass) {
    const mode = modeTextV2827(pass).toUpperCase();
    if (/\b(FM|NFM|WFM|AM|USB|LSB|SSB)\b/.test(mode)) return "Listen";
    return mode ? `Receive: ${modeTextV2827(pass)}` : "Radio";
  }

  function isReceiveModeV2827(pass) {
    try {
      if (typeof isDecodeReceiveModeV282 === "function" && isDecodeReceiveModeV282(pass)) return true;
    } catch (_error) {}
    const mode = modeTextV2827(pass).toUpperCase();
    if (!mode) return false;
    return !/\b(FM|NFM|WFM|AM|USB|LSB|SSB)\b/.test(mode);
  }

  function visiblePassesV2827() {
    const passes = Array.isArray(window.__plutoLastRenderedPassesV2827) ? window.__plutoLastRenderedPassesV2827 : [];
    try {
      return passes.filter((pass) => {
        const readiness = typeof passReadiness === "function" ? passReadiness(pass) : { state: "unknown", actionable: true };
        if (typeof passFilter !== "undefined" && passFilter === "all") return true;
        if (typeof passFilter !== "undefined" && passFilter === "ready") return !!readiness.actionable;
        if (typeof passFilter !== "undefined" && passFilter === "active") return readiness.state === "active";
        return readiness.state !== "stale";
      }).slice(0, 12);
    } catch (_error) {
      return passes.slice(0, 12);
    }
  }

  function passForRowV2827(row) {
    const rows = Array.from(document.querySelectorAll("#passes .pass-row, .pass-list .pass-row"));
    const index = rows.indexOf(row);
    const visible = visiblePassesV2827();
    if (index >= 0 && visible[index]) return visible[index];
    try {
      if (typeof currentSelectedPass !== "undefined" && currentSelectedPass) return currentSelectedPass;
    } catch (_error) {}
    return null;
  }

  function ensureStyleV2827() {
    if (document.getElementById("passRowBackendTestModalStyleV2827")) return;
    const style = document.createElement("style");
    style.id = "passRowBackendTestModalStyleV2827";
    style.textContent = `
      #passRowBackendTestModalV2827[hidden]{display:none!important}
      #passRowBackendTestModalV2827{position:fixed;inset:0;z-index:2147483400;display:flex;align-items:center;justify-content:center;background:rgba(15,23,42,.62);backdrop-filter:blur(2px)}
      .pass-row-backend-test-card-v2827{width:min(760px,calc(100vw - 28px));max-height:calc(100vh - 34px);overflow:auto;border-radius:16px;background:#0f172a;color:#e5e7eb;border:1px solid rgba(148,163,184,.35);box-shadow:0 22px 70px rgba(0,0,0,.46);font-family:system-ui,-apple-system,Segoe UI,sans-serif}
      .pass-row-backend-test-header-v2827{display:flex;align-items:flex-start;justify-content:space-between;gap:14px;padding:16px 18px;border-bottom:1px solid rgba(148,163,184,.24)}
      .pass-row-backend-test-header-v2827 h2{margin:0;font-size:18px;line-height:1.25;color:#f8fafc}
      .pass-row-backend-test-header-v2827 .meta{margin:4px 0 0;color:#bfdbfe;font-size:13px}
      .pass-row-backend-test-body-v2827{padding:16px 18px;display:grid;gap:12px}
      .pass-row-backend-test-grid-v2827{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px}
      .pass-row-backend-test-grid-v2827 div{border:1px solid rgba(148,163,184,.22);border-radius:10px;padding:9px;background:rgba(30,41,59,.55)}
      .pass-row-backend-test-grid-v2827 strong{display:block;color:#93c5fd;font-size:11px;text-transform:uppercase;letter-spacing:.05em;margin-bottom:3px}
      #passRowBackendTestStatusV2827{white-space:pre-wrap;min-height:96px;max-height:300px;overflow:auto;background:#020617;color:#dbeafe;border:1px solid rgba(96,165,250,.28);border-radius:10px;padding:11px;font:12px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace}
      .pass-row-backend-test-actions-v2827{display:flex;flex-wrap:wrap;gap:8px;justify-content:flex-end;padding:14px 18px;border-top:1px solid rgba(148,163,184,.24)}
      .pass-row-backend-test-actions-v2827 button{border:1px solid rgba(96,165,250,.55);background:#1d4ed8;color:white;border-radius:999px;padding:8px 13px;font-weight:650;cursor:pointer}
      .pass-row-backend-test-actions-v2827 button.secondary{background:#334155;border-color:rgba(148,163,184,.5)}
      .pass-row-backend-test-actions-v2827 button.danger{background:#991b1b;border-color:#fca5a5}
    `;
    (document.head || document.documentElement).appendChild(style);
  }

  function ensureModalV2827() {
    ensureStyleV2827();
    let modal = document.getElementById("passRowBackendTestModalV2827");
    if (modal) return modal;
    modal = document.createElement("div");
    modal.id = "passRowBackendTestModalV2827";
    modal.hidden = true;
    modal.innerHTML = `
      <div class="pass-row-backend-test-card-v2827" role="dialog" aria-modal="true" aria-labelledby="passRowBackendTestTitleV2827">
        <div class="pass-row-backend-test-header-v2827">
          <div>
            <h2 id="passRowBackendTestTitleV2827">Backend test action</h2>
            <div id="passRowBackendTestSubtitleV2827" class="meta"></div>
          </div>
          <button id="passRowBackendTestCloseTopV2827" class="secondary" type="button">Close</button>
        </div>
        <div class="pass-row-backend-test-body-v2827">
          <div id="passRowBackendTestGridV2827" class="pass-row-backend-test-grid-v2827"></div>
          <pre id="passRowBackendTestStatusV2827"></pre>
        </div>
        <div class="pass-row-backend-test-actions-v2827">
          <button id="passRowBackendTestOpenReceiveV2827" type="button">Open Receive dialog</button>
          <button id="passRowBackendTestStopV2827" class="danger" type="button">Stop audio</button>
          <button id="passRowBackendTestCopyV2827" class="secondary" type="button">Copy status</button>
          <button id="passRowBackendTestCloseV2827" class="secondary" type="button">Close</button>
        </div>
      </div>`;
    document.body.appendChild(modal);

    const close = () => { modal.hidden = true; };
    modal.addEventListener("click", (event) => { if (event.target === modal) close(); });
    document.getElementById("passRowBackendTestCloseTopV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestCloseV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestStopV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      try {
        if (typeof stopAnalogAudio === "function") await stopAnalogAudio("Backend test audio stopped.");
        if (status) status.textContent += "\nStopped audio session.";
      } catch (error) {
        if (status) status.textContent += `\nStop failed: ${error && error.message ? error.message : String(error)}`;
      }
    });
    document.getElementById("passRowBackendTestCopyV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      const text = status ? status.textContent : "";
      try { await navigator.clipboard.writeText(text); } catch (_error) {}
    });
    document.getElementById("passRowBackendTestOpenReceiveV2827")?.addEventListener("click", () => {
      const pass = window.__plutoBackendTestPassV2827;
      try {
        if (pass && typeof openReceivePlaceholderModalV282 === "function") openReceivePlaceholderModalV282(pass);
      } catch (error) {
        const status = document.getElementById("passRowBackendTestStatusV2827");
        if (status) status.textContent += `\nOpen Receive dialog failed: ${error && error.message ? error.message : String(error)}`;
      }
    });
    return modal;
  }

  function setModalPassV2827(pass, statusText) {
    const modal = ensureModalV2827();
    window.__plutoBackendTestPassV2827 = pass || null;
    const name = (pass && pass.name) || "Selected pass";
    const mode = modeTextV2827(pass) || "unknown";
    const downlink = downlinkHzV2827(pass);
    const label = passLabelV2827(pass);
    const title = document.getElementById("passRowBackendTestTitleV2827");
    const subtitle = document.getElementById("passRowBackendTestSubtitleV2827");
    const grid = document.getElementById("passRowBackendTestGridV2827");
    const status = document.getElementById("passRowBackendTestStatusV2827");
    const receiveButton = document.getElementById("passRowBackendTestOpenReceiveV2827");
    if (title) title.textContent = `${label} backend test`;
    if (subtitle) subtitle.textContent = name;
    if (grid) {
      const rows = [
        ["Satellite", name],
        ["Action", label],
        ["Mode", mode],
        ["Downlink", downlink ? formatHzV2827(downlink) : "No downlink"],
        ["AOS", pass && pass.aos_utc ? (typeof formatTime === "function" ? formatTime(pass.aos_utc) : pass.aos_utc) : "-"],
        ["LOS", pass && pass.los_utc ? (typeof formatTime === "function" ? formatTime(pass.los_utc) : pass.los_utc) : "-"]
      ];
      grid.innerHTML = rows.map(([k, v]) => `<div><strong>${String(k).replace(/[&<>]/g, "")}</strong><span>${String(v ?? "").replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;")}</span></div>`).join("");
    }
    if (status) status.textContent = statusText || `Ready. ${label} can be tested outside the pass window.`;
    if (receiveButton) receiveButton.hidden = !isReceiveModeV2827(pass);
    modal.hidden = false;
    return { modal, status };
  }

  function noteStatusV2827(message) {
    const status = document.getElementById("passRowBackendTestStatusV2827");
    if (status) status.textContent = message;
    const pill = document.getElementById("status");
    if (pill) pill.textContent = message.split("\n")[0] || message;
  }

  async function runBackendTestV2827(pass, sourceButton) {
    if (!pass) {
      setModalPassV2827(null, "No pass object was available for this row.");
      return;
    }
    const downlink = downlinkHzV2827(pass);
    const label = passLabelV2827(pass);
    const name = pass.name || "selected pass";
    const { status } = setModalPassV2827(pass, `Starting backend test action for ${name}...`);

    if (!downlink) {
      noteStatusV2827(`Cannot start ${label}: no downlink is available for ${name}.`);
      return;
    }

    try {
      if (typeof renderPassDetail === "function") renderPassDetail(pass);
    } catch (_error) {}

    if (isReceiveModeV2827(pass)) {
      try {
        if (typeof openReceivePlaceholderModalV282 === "function") {
          openReceivePlaceholderModalV282(pass);
          noteStatusV2827(`Opened Receive dialog for ${name}.\nMode: ${modeTextV2827(pass) || "unknown"}\nDownlink: ${formatHzV2827(downlink)}\n\nThis dialog is separate from analog audio. Use it to verify the selected receive/decode target while backend decoder work is developed.`);
          return;
        }
      } catch (error) {
        noteStatusV2827(`Receive dialog failed: ${error && error.message ? error.message : String(error)}`);
        return;
      }
      noteStatusV2827(`Receive target selected for ${name}.\nMode: ${modeTextV2827(pass) || "unknown"}\nDownlink: ${formatHzV2827(downlink)}\n\nNo Receive dialog function is installed in this UI build.`);
      return;
    }

    try {
      const testButton = document.getElementById("passRowBackendTestStopV2827") || sourceButton;
      if (testButton) {
        testButton.disabled = false;
        testButton.textContent = "Stop audio";
      }
      if (typeof startAnalogAudio === "function") {
        await startAnalogAudio(pass, testButton || sourceButton, status || document.getElementById("status"));
        return;
      }
      noteStatusV2827(`Cannot start Listen for ${name}: startAnalogAudio() is not available in this UI build.`);
    } catch (error) {
      try { if (typeof stopAnalogAudio === "function") await stopAnalogAudio(); } catch (_stopError) {}
      noteStatusV2827(`Listen backend test failed for ${name}: ${error && error.message ? error.message : String(error)}`);
    }
  }

  function annotateRowsV2827() {
    const rows = Array.from(document.querySelectorAll("#passes .pass-row, .pass-list .pass-row"));
    rows.forEach((row, index) => {
      row.dataset.passRowIndexV2827 = String(index);
      const button = row.querySelector(".pass-row-action-button-v286");
      if (!button) return;
      button.disabled = false;
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
      button.style.pointerEvents = "auto";
      button.style.opacity = "1";
      const pass = visiblePassesV2827()[index];
      if (pass) button.textContent = passLabelV2827(pass);
      button.title = "Open backend test dialog for this pass row.";
    });
  }

  if (typeof renderPasses === "function" && renderPasses.backendTestModalWrappedV2827 !== true) {
    const previousRenderPassesV2827 = renderPasses;
    renderPasses = function renderPassesBackendTestModalV2827(payload) {
      try { window.__plutoLastRenderedPassesV2827 = (payload && payload.passes) || []; } catch (_error) {}
      const result = previousRenderPassesV2827.apply(this, arguments);
      try { annotateRowsV2827(); } catch (_error) {}
      window.setTimeout(() => { try { annotateRowsV2827(); } catch (_error) {} }, 80);
      window.setTimeout(() => { try { annotateRowsV2827(); } catch (_error) {} }, 300);
      return result;
    };
    renderPasses.backendTestModalWrappedV2827 = true;
  }

  document.addEventListener("click", function passRowBackendTestClickV2827(event) {
    const button = event.target && event.target.closest ? event.target.closest(".pass-row-action-button-v286") : null;
    if (!button) return;
    const row = button.closest(".pass-row");
    if (!row) return;
    event.preventDefault();
    event.stopPropagation();
    if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
    const pass = passForRowV2827(row);
    runBackendTestV2827(pass, button);
  }, true);

  window.plutoOpenPassRowBackendTestModalV2827 = runBackendTestV2827;
  window.plutoAnnotatePassRowsV2827 = annotateRowsV2827;
  window.setInterval(() => { try { annotateRowsV2827(); } catch (_error) {} }, 2000);
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => { try { annotateRowsV2827(); } catch (_error) {} }, { once: true });
  } else {
    try { annotateRowsV2827(); } catch (_error) {}
  }
  console.info(`${MARKER} installed`);
})();
/* PASS_ROW_BACKEND_TEST_MODAL_V2_8_27_END */

/* PASS_ROW_SINGLE_MODAL_FIX_V2_8_28
 * Keep pass-row backend testing to a single operator modal. v2.8.27 opened
 * the new backend-test modal and the older Receive placeholder at the same
 * time for data rows.  This patch suppresses the legacy Receive placeholder
 * during pass-row backend-test clicks and styles it safely if opened elsewhere.
 * UI-only; backend C and radio endpoints are unchanged.
 */
(function installSinglePassRowActionModalFixV2828() {
  const MARKER = "PASS_ROW_SINGLE_MODAL_FIX_V2_8_28";
  if (window.__plutoSinglePassRowActionModalFixV2828) return;
  window.__plutoSinglePassRowActionModalFixV2828 = true;

  function injectStyleV2828() {
    if (document.getElementById("passRowSingleModalFixStyleV2828")) return;
    const style = document.createElement("style");
    style.id = "passRowSingleModalFixStyleV2828";
    style.textContent = `
      #receivePlaceholderModalV282.receive-placeholder-modal-v282,
      #receivePlaceholderModalV282 {
        background: rgba(2, 6, 23, 0.72) !important;
        color: #e5e7eb !important;
      }
      #receivePlaceholderModalV282 .receive-placeholder-card-v282,
      #receivePlaceholderModalV282 [role="dialog"] {
        background: #0f172a !important;
        color: #e5e7eb !important;
        border: 1px solid rgba(148, 163, 184, 0.35) !important;
        box-shadow: 0 20px 60px rgba(0, 0, 0, 0.55) !important;
      }
      #receivePlaceholderModalV282 h1,
      #receivePlaceholderModalV282 h2,
      #receivePlaceholderModalV282 h3,
      #receivePlaceholderModalV282 strong,
      #receivePlaceholderModalV282 dt {
        color: #f8fafc !important;
      }
      #receivePlaceholderModalV282 p,
      #receivePlaceholderModalV282 div,
      #receivePlaceholderModalV282 span,
      #receivePlaceholderModalV282 dd,
      #receivePlaceholderModalV282 .meta,
      #receivePlaceholderModalV282 .help-note {
        color: #dbeafe !important;
      }
      #receivePlaceholderModalV282 button {
        color: #e0f2fe !important;
        background: #1d4ed8 !important;
        border: 1px solid rgba(147, 197, 253, 0.65) !important;
      }
      .pass-row-backend-test-modal-v2827 ~ #receivePlaceholderModalV282:not([hidden]),
      .backend-test-modal-v2827 ~ #receivePlaceholderModalV282:not([hidden]) {
        display: none !important;
      }
    `;
    (document.head || document.documentElement).appendChild(style);
  }

  function nowV2828() {
    return Date.now ? Date.now() : new Date().getTime();
  }

  function suppressLegacyReceivePlaceholderV2828(ms) {
    window.__plutoSuppressReceivePlaceholderUntilV2828 = nowV2828() + (ms || 2500);
  }

  function receivePlaceholderSuppressedV2828() {
    return nowV2828() < Number(window.__plutoSuppressReceivePlaceholderUntilV2828 || 0);
  }

  function hideLegacyReceivePlaceholderV2828() {
    const modal = document.getElementById("receivePlaceholderModalV282");
    if (!modal) return false;
    if (!receivePlaceholderSuppressedV2828()) return false;
    modal.hidden = true;
    modal.setAttribute("aria-hidden", "true");
    modal.style.display = "none";
    return true;
  }

  function restoreLegacyReceivePlaceholderDisplayV2828() {
    const modal = document.getElementById("receivePlaceholderModalV282");
    if (!modal) return;
    if (receivePlaceholderSuppressedV2828()) return;
    if (modal.hidden) modal.style.display = "";
  }

  function isPassRowActionButtonV2828(target) {
    return !!(target && target.closest && target.closest(".pass-row-action-button-v286"));
  }

  function installClickSuppressionV2828() {
    document.addEventListener("click", (event) => {
      if (!isPassRowActionButtonV2828(event.target)) return;
      suppressLegacyReceivePlaceholderV2828(3500);
      window.setTimeout(hideLegacyReceivePlaceholderV2828, 0);
      window.setTimeout(hideLegacyReceivePlaceholderV2828, 50);
      window.setTimeout(hideLegacyReceivePlaceholderV2828, 150);
      window.setTimeout(hideLegacyReceivePlaceholderV2828, 400);
      window.setTimeout(restoreLegacyReceivePlaceholderDisplayV2828, 3600);
    }, true);
  }

  function wrapReceivePlaceholderOpenV2828() {
    try {
      if (typeof openReceivePlaceholderModalV282 !== "function") return;
      if (openReceivePlaceholderModalV282.singleModalWrappedV2828) return;
      const previousOpen = openReceivePlaceholderModalV282;
      openReceivePlaceholderModalV282 = function openReceivePlaceholderModalSingleModalGuardV2828() {
        if (receivePlaceholderSuppressedV2828()) {
          window.setTimeout(hideLegacyReceivePlaceholderV2828, 0);
          return null;
        }
        const result = previousOpen.apply(this, arguments);
        injectStyleV2828();
        return result;
      };
      openReceivePlaceholderModalV282.singleModalWrappedV2828 = true;
    } catch (_error) {
    }
  }

  function installReceivePlaceholderObserverV2828() {
    const root = document.body || document.documentElement;
    if (!root || window.__plutoReceivePlaceholderObserverV2828) return;
    window.__plutoReceivePlaceholderObserverV2828 = new MutationObserver(() => {
      injectStyleV2828();
      hideLegacyReceivePlaceholderV2828();
    });
    window.__plutoReceivePlaceholderObserverV2828.observe(root, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["hidden", "style", "class"]
    });
  }

  function initV2828() {
    injectStyleV2828();
    wrapReceivePlaceholderOpenV2828();
    installClickSuppressionV2828();
    installReceivePlaceholderObserverV2828();
    window.setInterval(() => {
      injectStyleV2828();
      wrapReceivePlaceholderOpenV2828();
      hideLegacyReceivePlaceholderV2828();
    }, 1000);
    window.plutoHideLegacyReceivePlaceholderV2828 = hideLegacyReceivePlaceholderV2828;
    window.plutoSuppressLegacyReceivePlaceholderV2828 = suppressLegacyReceivePlaceholderV2828;
    console.info(`[Pluto] ${MARKER} installed`);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initV2828, { once: true });
  } else {
    initV2828();
  }
})();
/* PASS_ROW_SINGLE_MODAL_FIX_V2_8_28_END */


/* RETIRE_LEGACY_RECEIVE_PLACEHOLDER_MODAL_V2_8_29
 * The pass-row backend-test modal is now the only dialog opened by pass-row
 * Listen/Receive actions.  The older Receive Decode placeholder modal can be
 * opened by legacy event listeners and causes duplicate white-on-white dialogs.
 * Retire that legacy modal globally, keep it hidden if an older listener opens
 * it, and suppress its old button binding.  UI-only; backend C untouched.
 */
(function retireLegacyReceivePlaceholderModalV2829() {
  const MARKER = "RETIRE_LEGACY_RECEIVE_PLACEHOLDER_MODAL_V2_8_29";
  if (window.__plutoRetireLegacyReceivePlaceholderV2829) return;
  window.__plutoRetireLegacyReceivePlaceholderV2829 = true;

  function installStyleV2829() {
    let style = document.getElementById("retireLegacyReceivePlaceholderStyleV2829");
    if (style) return style;
    style = document.createElement("style");
    style.id = "retireLegacyReceivePlaceholderStyleV2829";
    style.textContent = `
      #receivePlaceholderModalV282 {
        display: none !important;
        visibility: hidden !important;
        pointer-events: none !important;
        opacity: 0 !important;
      }
      #receiveDecodePlaceholderButtonV282 {
        display: none !important;
        visibility: hidden !important;
        pointer-events: none !important;
      }
    `;
    (document.head || document.documentElement).appendChild(style);
    return style;
  }

  function hideLegacyReceivePlaceholderV2829() {
    const modal = document.getElementById("receivePlaceholderModalV282");
    if (modal) {
      modal.hidden = true;
      modal.setAttribute("aria-hidden", "true");
      modal.style.display = "none";
      modal.style.visibility = "hidden";
      modal.style.pointerEvents = "none";
      modal.style.opacity = "0";
    }
    document.querySelectorAll("#receiveDecodePlaceholderButtonV282").forEach((button) => {
      button.hidden = true;
      button.disabled = true;
      button.setAttribute("aria-hidden", "true");
      button.style.display = "none";
      button.style.pointerEvents = "none";
    });
  }

  function legacyReceiveNoopV2829(pass) {
    hideLegacyReceivePlaceholderV2829();
    const status = document.getElementById("status");
    if (status) {
      const name = pass && pass.name ? String(pass.name) : "selected pass";
      status.textContent = `Legacy Receive Decode placeholder suppressed for ${name}; use the Backend Test modal.`;
    }
    return null;
  }

  function installOverridesV2829() {
    installStyleV2829();
    hideLegacyReceivePlaceholderV2829();

    try {
      openReceivePlaceholderModalV282 = legacyReceiveNoopV2829;
      window.openReceivePlaceholderModalV282 = legacyReceiveNoopV2829;
    } catch (_error) {
      window.openReceivePlaceholderModalV282 = legacyReceiveNoopV2829;
    }

    try {
      if (typeof bindReceiveDecodePlaceholderV282 === "function" && !bindReceiveDecodePlaceholderV282.retiredByV2829) {
        bindReceiveDecodePlaceholderV282 = function bindReceiveDecodePlaceholderRetiredV2829(_pass, node) {
          const button = node && node.querySelector ? node.querySelector("#receiveDecodePlaceholderButtonV282") : null;
          const status = node && node.querySelector ? node.querySelector("#receiveDecodeStatusV282") : null;
          if (button) {
            button.hidden = true;
            button.disabled = true;
            button.setAttribute("aria-hidden", "true");
            button.style.display = "none";
            button.style.pointerEvents = "none";
          }
          if (status) {
            status.hidden = true;
            status.textContent = "";
          }
          hideLegacyReceivePlaceholderV2829();
        };
        bindReceiveDecodePlaceholderV282.retiredByV2829 = true;
      }
    } catch (_error) {}
  }

  document.addEventListener("click", (event) => {
    const target = event.target && event.target.closest
      ? event.target.closest("#receivePlaceholderModalV282, #receiveDecodePlaceholderButtonV282")
      : null;
    if (!target) return;
    event.preventDefault();
    event.stopPropagation();
    if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
    hideLegacyReceivePlaceholderV2829();
  }, true);

  function startV2829() {
    installOverridesV2829();
    const observer = new MutationObserver(() => hideLegacyReceivePlaceholderV2829());
    if (document.body) observer.observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["hidden", "style", "class", "aria-hidden"]
    });
    window.setInterval(hideLegacyReceivePlaceholderV2829, 500);
  }

  window.plutoHideLegacyReceivePlaceholderV2829 = hideLegacyReceivePlaceholderV2829;
  window.plutoRetireLegacyReceivePlaceholderV2829 = installOverridesV2829;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startV2829, { once: true });
  } else {
    startV2829();
  }
})();
/* RETIRE_LEGACY_RECEIVE_PLACEHOLDER_MODAL_V2_8_29_END */

/* SINGLE_BACKEND_TEST_MODAL_V2_8_30
 * Keep pass-row Listen/Receive backend tests on one dialog.  v2.8.27 added a
 * modal but still exposed an Open Receive dialog button, and v2.8.29 retired
 * that legacy Receive placeholder.  This patch intercepts pass-row action
 * clicks before older handlers, opens/updates only the Backend Test modal, and
 * hides the obsolete Open Receive dialog button.  UI-only; backend C untouched.
 */
(function installSingleBackendTestModalV2830() {
  const MARKER = "SINGLE_BACKEND_TEST_MODAL_V2_8_30";
  if (window.__plutoSingleBackendTestModalV2830) return;
  window.__plutoSingleBackendTestModalV2830 = true;

  function cleanTextV2830(value) {
    return String(value === undefined || value === null ? "" : value).trim();
  }

  function modeTextV2830(pass) {
    const radio = (pass && pass.radio) || {};
    return cleanTextV2830(radio.mode || ((pass && pass.modes) || [])[0] || "");
  }

  function downlinkHzV2830(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function formatHzV2830(hz) {
    try {
      if (typeof formatHz === "function") return formatHz(hz);
    } catch (_error) {}
    const value = Number(hz || 0);
    return Number.isFinite(value) && value > 0 ? `${(value / 1000000).toFixed(3)} MHz` : "No downlink";
  }

  function htmlV2830(value) {
    return String(value === undefined || value === null ? "" : value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/\"/g, "&quot;");
  }

  function analogModeV2830(pass) {
    const mode = modeTextV2830(pass).toUpperCase();
    return /\b(FM|NFM|WFM|AM|USB|LSB|SSB)\b/.test(mode);
  }

  function actionLabelV2830(pass) {
    const mode = modeTextV2830(pass);
    if (analogModeV2830(pass)) return "Listen";
    return mode ? `Receive: ${mode}` : "Receive";
  }

  function visiblePassesV2830() {
    const source = Array.isArray(window.__plutoLastRenderedPassesV2827)
      ? window.__plutoLastRenderedPassesV2827
      : [];
    if (!source.length) return [];
    try {
      return source.filter((pass) => {
        const readiness = typeof passReadiness === "function" ? passReadiness(pass) : { state: "unknown", actionable: true };
        if (typeof passFilter !== "undefined" && passFilter === "all") return true;
        if (typeof passFilter !== "undefined" && passFilter === "ready") return !!readiness.actionable;
        if (typeof passFilter !== "undefined" && passFilter === "active") return readiness.state === "active";
        return readiness.state !== "stale";
      }).slice(0, 12);
    } catch (_error) {
      return source.slice(0, 12);
    }
  }

  function passForButtonV2830(button) {
    const row = button && button.closest ? button.closest(".pass-row") : null;
    if (!row) return null;
    const rows = Array.from(document.querySelectorAll("#passes .pass-row, .pass-list .pass-row"));
    const index = rows.indexOf(row);
    const visible = visiblePassesV2830();
    if (index >= 0 && visible[index]) return visible[index];
    try {
      if (typeof currentSelectedPass !== "undefined" && currentSelectedPass) return currentSelectedPass;
    } catch (_error) {}
    return null;
  }

  function hideLegacyActionButtonV2830() {
    document.querySelectorAll("#passRowBackendTestOpenReceiveV2827").forEach((button) => {
      button.hidden = true;
      button.disabled = true;
      button.setAttribute("aria-hidden", "true");
      button.style.display = "none";
      button.style.pointerEvents = "none";
    });
  }

  function ensureStyleV2830() {
    let style = document.getElementById("singleBackendTestModalStyleV2830");
    if (style) return style;
    style = document.createElement("style");
    style.id = "singleBackendTestModalStyleV2830";
    style.textContent = `
      #passRowBackendTestOpenReceiveV2827{display:none!important;visibility:hidden!important;pointer-events:none!important}
      #passRowBackendTestModalV2827 .pass-row-backend-test-card-v2827{background:#0f172a!important;color:#e5e7eb!important}
      #passRowBackendTestModalV2827 #passRowBackendTestStatusV2827{background:#020617!important;color:#dbeafe!important}
      .pass-row-action-button-v286{pointer-events:auto!important}
    `;
    (document.head || document.documentElement).appendChild(style);
    return style;
  }

  function ensureModalV2830() {
    ensureStyleV2830();
    let modal = document.getElementById("passRowBackendTestModalV2827");
    if (!modal) {
      modal = document.createElement("div");
      modal.id = "passRowBackendTestModalV2827";
      modal.hidden = true;
      modal.innerHTML = `
        <div class="pass-row-backend-test-card-v2827" role="dialog" aria-modal="true" aria-labelledby="passRowBackendTestTitleV2827">
          <div class="pass-row-backend-test-header-v2827">
            <div><h2 id="passRowBackendTestTitleV2827">Backend test</h2><div id="passRowBackendTestSubtitleV2827" class="meta"></div></div>
            <button id="passRowBackendTestCloseTopV2827" class="secondary" type="button">Close</button>
          </div>
          <div class="pass-row-backend-test-body-v2827">
            <div id="passRowBackendTestGridV2827" class="pass-row-backend-test-grid-v2827"></div>
            <pre id="passRowBackendTestStatusV2827"></pre>
          </div>
          <div class="pass-row-backend-test-actions-v2827">
            <button id="passRowBackendTestStopV2827" class="danger" type="button">Stop audio</button>
            <button id="passRowBackendTestCopyV2827" class="secondary" type="button">Copy status</button>
            <button id="passRowBackendTestCloseV2827" class="secondary" type="button">Close</button>
          </div>
        </div>`;
      document.body.appendChild(modal);
    }

    const close = () => { modal.hidden = true; };
    document.getElementById("passRowBackendTestCloseTopV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestCloseV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestCopyV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      try { await navigator.clipboard.writeText(status ? status.textContent : ""); } catch (_error) {}
    });
    document.getElementById("passRowBackendTestStopV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      try {
        if (typeof stopAnalogAudio === "function") await stopAnalogAudio("Backend test audio stopped.");
        if (status) status.textContent += "\nStopped audio session.";
      } catch (error) {
        if (status) status.textContent += `\nStop failed: ${error && error.message ? error.message : String(error)}`;
      }
    });

    hideLegacyActionButtonV2830();
    return modal;
  }

  function setModalV2830(pass, message) {
    const modal = ensureModalV2830();
    const label = actionLabelV2830(pass);
    const name = (pass && pass.name) || "Selected pass";
    const mode = modeTextV2830(pass) || "unknown";
    const downlink = downlinkHzV2830(pass);
    const title = document.getElementById("passRowBackendTestTitleV2827");
    const subtitle = document.getElementById("passRowBackendTestSubtitleV2827");
    const grid = document.getElementById("passRowBackendTestGridV2827");
    const status = document.getElementById("passRowBackendTestStatusV2827");
    const stop = document.getElementById("passRowBackendTestStopV2827");

    if (title) title.textContent = `${label} backend test`;
    if (subtitle) subtitle.textContent = name;
    if (grid) {
      const rows = [
        ["Satellite", name],
        ["Action", label],
        ["Mode", mode],
        ["Downlink", downlink ? formatHzV2830(downlink) : "No downlink"],
        ["AOS", pass && pass.aos_utc ? (typeof formatTime === "function" ? formatTime(pass.aos_utc) : pass.aos_utc) : "-"],
        ["LOS", pass && pass.los_utc ? (typeof formatTime === "function" ? formatTime(pass.los_utc) : pass.los_utc) : "-"]
      ];
      grid.innerHTML = rows.map(([key, value]) => `<div><strong>${htmlV2830(key)}</strong><span>${htmlV2830(value)}</span></div>`).join("");
    }
    if (stop) stop.hidden = !analogModeV2830(pass);
    if (status) status.textContent = message || `Ready. ${label} can be tested outside the pass window.`;
    modal.hidden = false;
    hideLegacyActionButtonV2830();
    return status;
  }

  function setPageStatusV2830(message) {
    const pill = document.getElementById("status");
    if (pill) pill.textContent = String(message || "").split("\n")[0] || "Backend test ready.";
  }

  async function runSingleBackendTestV2830(pass, sourceButton) {
    if (!pass) {
      setModalV2830(null, "No pass object was available for this row.");
      return;
    }

    const name = pass.name || "selected pass";
    const label = actionLabelV2830(pass);
    const downlink = downlinkHzV2830(pass);
    const status = setModalV2830(pass, `Starting ${label} backend test for ${name}...`);
    setPageStatusV2830(`Starting ${label} backend test for ${name}...`);

    if (!downlink) {
      const msg = `Cannot start ${label}: no downlink is available for ${name}.`;
      if (status) status.textContent = msg;
      setPageStatusV2830(msg);
      return;
    }

    if (!analogModeV2830(pass)) {
      const msg = [
        `Receive backend target selected for ${name}.`,
        `Mode: ${modeTextV2830(pass) || "unknown"}`,
        `Downlink: ${formatHzV2830(downlink)}`,
        "",
        "The legacy Receive Decode placeholder has been retired.",
        "This modal is now the single pass-row action surface.",
        "Next decoder work should attach the proper backend receive/capture/decode action here."
      ].join("\n");
      if (status) status.textContent = msg;
      setPageStatusV2830(`Receive backend target selected for ${name}.`);
      return;
    }

    try {
      const stopButton = document.getElementById("passRowBackendTestStopV2827") || sourceButton;
      if (stopButton) {
        stopButton.hidden = false;
        stopButton.disabled = false;
        stopButton.textContent = "Stop audio";
      }
      if (typeof startAnalogAudio === "function") {
        await startAnalogAudio(pass, stopButton || sourceButton, status || document.getElementById("status"));
        setPageStatusV2830(`Listen backend test running for ${name}.`);
      } else {
        const msg = `Cannot start Listen for ${name}: startAnalogAudio() is not available in this UI build.`;
        if (status) status.textContent = msg;
        setPageStatusV2830(msg);
      }
    } catch (error) {
      try { if (typeof stopAnalogAudio === "function") await stopAnalogAudio(); } catch (_stopError) {}
      const msg = `Listen backend test failed for ${name}: ${error && error.message ? error.message : String(error)}`;
      if (status) status.textContent = msg;
      setPageStatusV2830(msg);
    }
  }

  function interceptPassRowClickV2830(event) {
    const button = event.target && event.target.closest ? event.target.closest(".pass-row-action-button-v286") : null;
    if (!button) return;
    const row = button.closest(".pass-row");
    if (!row) return;
    event.preventDefault();
    event.stopPropagation();
    if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
    button.disabled = false;
    button.removeAttribute("disabled");
    button.setAttribute("aria-disabled", "false");
    runSingleBackendTestV2830(passForButtonV2830(button), button);
  }

  function annotateButtonsV2830() {
    document.querySelectorAll(".pass-row-action-button-v286").forEach((button) => {
      button.disabled = false;
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
      button.style.pointerEvents = "auto";
      button.style.opacity = "1";
      button.title = "Open the single Backend Test modal for this pass row.";
    });
    hideLegacyActionButtonV2830();
  }

  window.addEventListener("click", interceptPassRowClickV2830, true);
  window.plutoRunSingleBackendTestV2830 = runSingleBackendTestV2830;
  window.plutoAnnotatePassRowsV2830 = annotateButtonsV2830;
  window.plutoHideBackendTestLegacyReceiveButtonV2830 = hideLegacyActionButtonV2830;

  function startV2830() {
    ensureStyleV2830();
    annotateButtonsV2830();
    const observer = new MutationObserver(() => window.setTimeout(annotateButtonsV2830, 20));
    if (document.body) observer.observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ["hidden", "disabled", "style", "class"] });
    window.setInterval(annotateButtonsV2830, 1000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startV2830, { once: true });
  } else {
    startV2830();
  }
  console.info(`${MARKER} installed`);
})();
/* SINGLE_BACKEND_TEST_MODAL_V2_8_30_END */

/* BACKEND_TEST_MODAL_FORCE_OPEN_V2_8_31
 * Field repair for the pass-row Backend Test modal.  v2.8.30 started the audio
 * path but the operator modal could fail to appear, leaving only a console/URL
 * 409.  This patch force-creates and force-opens the modal before any backend
 * audio start, captures start/stream failures in the modal, resets stale audio
 * sessions before testing, and intercepts pass-row clicks at capture phase.
 * UI-only; backend C untouched.
 */
(function installBackendTestModalForceOpenV2831() {
  const MARKER = "BACKEND_TEST_MODAL_FORCE_OPEN_V2_8_31";
  if (window.__plutoBackendTestModalForceOpenV2831) return;
  window.__plutoBackendTestModalForceOpenV2831 = true;

  function textV2831(value) {
    return String(value === undefined || value === null ? "" : value).trim();
  }

  function escapeV2831(value) {
    return String(value === undefined || value === null ? "" : value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/\"/g, "&quot;");
  }

  function modeTextV2831(pass) {
    const radio = (pass && pass.radio) || {};
    return textV2831(radio.mode || ((pass && pass.modes) || [])[0] || "");
  }

  function downlinkHzV2831(pass) {
    const radio = (pass && pass.radio) || {};
    return radio.downlink_hz || ((pass && pass.downlinks_hz) || [])[0] || "";
  }

  function fmtHzV2831(hz) {
    try {
      if (typeof formatHz === "function") return formatHz(hz);
    } catch (_error) {}
    const value = Number(hz || 0);
    return Number.isFinite(value) && value > 0 ? `${(value / 1000000).toFixed(3)} MHz` : "No downlink";
  }

  function fmtTimeV2831(iso) {
    try {
      if (typeof formatTime === "function") return formatTime(iso);
    } catch (_error) {}
    return iso || "-";
  }

  function actionLabelV2831(pass, button) {
    const buttonText = textV2831(button && button.textContent);
    if (buttonText && !/^Radio$/i.test(buttonText)) return buttonText;
    try {
      if (typeof passActionLabelV286 === "function") return passActionLabelV286(pass);
    } catch (_error) {}
    const mode = modeTextV2831(pass);
    if (/\b(FM|NFM|WFM|AM|USB|LSB|SSB)\b/i.test(mode)) return "Listen";
    return mode ? `Receive: ${mode}` : "Receive";
  }

  function isListenActionV2831(pass, button) {
    return /^Listen\b/i.test(actionLabelV2831(pass, button));
  }

  function ensureStyleV2831() {
    let style = document.getElementById("backendTestModalForceOpenStyleV2831");
    if (style) return style;
    style = document.createElement("style");
    style.id = "backendTestModalForceOpenStyleV2831";
    style.textContent = `
      #passRowBackendTestModalV2827[hidden]{display:none!important}
      #passRowBackendTestModalV2827:not([hidden]){
        position:fixed!important;inset:0!important;z-index:2147483600!important;
        display:flex!important;align-items:center!important;justify-content:center!important;
        padding:18px!important;background:rgba(2,6,23,.72)!important;color:#e5e7eb!important;
      }
      #passRowBackendTestModalV2827 .pass-row-backend-test-card-v2827{
        width:min(760px,96vw)!important;max-height:88vh!important;overflow:auto!important;
        background:#0f172a!important;color:#e5e7eb!important;border:1px solid rgba(147,197,253,.45)!important;
        border-radius:14px!important;box-shadow:0 24px 80px rgba(0,0,0,.55)!important;padding:16px!important;
        font:14px/1.4 system-ui,-apple-system,Segoe UI,sans-serif!important;
      }
      #passRowBackendTestModalV2827 .pass-row-backend-test-header-v2827{display:flex!important;align-items:flex-start!important;justify-content:space-between!important;gap:12px!important;margin-bottom:12px!important}
      #passRowBackendTestModalV2827 h2{margin:0!important;color:#f8fafc!important;font-size:20px!important}
      #passRowBackendTestModalV2827 .meta{color:#bfdbfe!important;margin-top:3px!important}
      #passRowBackendTestModalV2827 .pass-row-backend-test-grid-v2827{display:grid!important;grid-template-columns:repeat(auto-fit,minmax(170px,1fr))!important;gap:8px!important;margin:10px 0 12px!important}
      #passRowBackendTestModalV2827 .pass-row-backend-test-grid-v2827>div{background:#111827!important;border:1px solid rgba(148,163,184,.3)!important;border-radius:10px!important;padding:8px!important}
      #passRowBackendTestModalV2827 .pass-row-backend-test-grid-v2827 strong{display:block!important;color:#93c5fd!important;font-size:12px!important;margin-bottom:2px!important}
      #passRowBackendTestModalV2827 #passRowBackendTestStatusV2827{white-space:pre-wrap!important;min-height:130px!important;background:#020617!important;color:#dbeafe!important;border:1px solid rgba(148,163,184,.35)!important;border-radius:10px!important;padding:10px!important;overflow:auto!important}
      #passRowBackendTestModalV2827 .pass-row-backend-test-actions-v2827{display:flex!important;gap:8px!important;flex-wrap:wrap!important;justify-content:flex-end!important;margin-top:12px!important}
      #passRowBackendTestModalV2827 button{cursor:pointer!important;border-radius:10px!important;border:1px solid rgba(147,197,253,.4)!important;background:#1d4ed8!important;color:#eff6ff!important;padding:7px 11px!important;font-weight:600!important}
      #passRowBackendTestModalV2827 button.secondary{background:#334155!important;color:#e2e8f0!important}
      #passRowBackendTestModalV2827 button.danger{background:#991b1b!important;color:#fee2e2!important}
      #passRowBackendTestOpenReceiveV2827{display:none!important;visibility:hidden!important;pointer-events:none!important}
      .pass-row-action-button-v286{pointer-events:auto!important;opacity:1!important}
    `;
    (document.head || document.documentElement).appendChild(style);
    return style;
  }

  function ensureModalV2831() {
    ensureStyleV2831();
    let modal = document.getElementById("passRowBackendTestModalV2827");
    if (!modal) {
      modal = document.createElement("div");
      modal.id = "passRowBackendTestModalV2827";
      modal.hidden = true;
      modal.innerHTML = `
        <div class="pass-row-backend-test-card-v2827" role="dialog" aria-modal="true" aria-labelledby="passRowBackendTestTitleV2827">
          <div class="pass-row-backend-test-header-v2827">
            <div><h2 id="passRowBackendTestTitleV2827">Backend test</h2><div id="passRowBackendTestSubtitleV2827" class="meta"></div></div>
            <button id="passRowBackendTestCloseTopV2827" class="secondary" type="button">Close</button>
          </div>
          <div class="pass-row-backend-test-body-v2827">
            <div id="passRowBackendTestGridV2827" class="pass-row-backend-test-grid-v2827"></div>
            <pre id="passRowBackendTestStatusV2827"></pre>
          </div>
          <div class="pass-row-backend-test-actions-v2827">
            <button id="passRowBackendTestStopV2827" class="danger" type="button">Stop audio</button>
            <button id="passRowBackendTestCopyV2827" class="secondary" type="button">Copy status</button>
            <button id="passRowBackendTestCloseV2827" class="secondary" type="button">Close</button>
          </div>
        </div>`;
      (document.body || document.documentElement).appendChild(modal);
    }

    const close = () => {
      modal.hidden = true;
      modal.setAttribute("hidden", "");
      modal.style.display = "none";
    };
    document.getElementById("passRowBackendTestCloseTopV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestCloseV2827")?.addEventListener("click", close);
    document.getElementById("passRowBackendTestCopyV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      try { await navigator.clipboard.writeText(status ? status.textContent : ""); } catch (_error) {}
    });
    document.getElementById("passRowBackendTestStopV2827")?.addEventListener("click", async () => {
      const status = document.getElementById("passRowBackendTestStatusV2827");
      try {
        if (typeof stopAnalogAudio === "function") await stopAnalogAudio("Backend test audio stopped.");
        try { if (typeof postJson === "function") await postJson("/api/radio/audio/live/stop", {}); } catch (_error) {}
        if (status) status.textContent += "\nStopped backend audio session.";
      } catch (error) {
        if (status) status.textContent += `\nStop failed: ${error && error.message ? error.message : String(error)}`;
      }
    });

    document.querySelectorAll("#passRowBackendTestOpenReceiveV2827").forEach((button) => {
      button.hidden = true;
      button.disabled = true;
      button.style.display = "none";
    });
    return modal;
  }

  function showModalV2831(pass, button, message) {
    const modal = ensureModalV2831();
    const label = actionLabelV2831(pass, button);
    const name = (pass && pass.name) || "Selected pass";
    const mode = modeTextV2831(pass) || "unknown";
    const downlink = downlinkHzV2831(pass);
    const title = document.getElementById("passRowBackendTestTitleV2827");
    const subtitle = document.getElementById("passRowBackendTestSubtitleV2827");
    const grid = document.getElementById("passRowBackendTestGridV2827");
    const status = document.getElementById("passRowBackendTestStatusV2827");
    const stop = document.getElementById("passRowBackendTestStopV2827");
    if (title) title.textContent = `${label} backend test`;
    if (subtitle) subtitle.textContent = name;
    if (grid) {
      const rows = [
        ["Satellite", name],
        ["Action", label],
        ["Mode", mode],
        ["Downlink", downlink ? fmtHzV2831(downlink) : "No downlink"],
        ["AOS", fmtTimeV2831(pass && pass.aos_utc)],
        ["LOS", fmtTimeV2831(pass && pass.los_utc)]
      ];
      grid.innerHTML = rows.map(([key, value]) => `<div><strong>${escapeV2831(key)}</strong><span>${escapeV2831(value)}</span></div>`).join("");
    }
    if (stop) stop.hidden = !isListenActionV2831(pass, button);
    if (status) status.textContent = message || `Ready. ${label} can be tested outside the pass window.`;
    modal.hidden = false;
    modal.removeAttribute("hidden");
    modal.style.display = "flex";
    modal.style.visibility = "visible";
    modal.style.opacity = "1";
    window.__plutoBackendTestModalLastOpenV2831 = { marker: MARKER, at: new Date().toISOString(), label, name, mode, downlink };
    return status;
  }

  function setPageStatusV2831(message) {
    const node = document.getElementById("status");
    if (node) node.textContent = String(message || "").split("\n")[0] || "Backend test ready.";
  }

  function visiblePassesV2831() {
    const candidates = [
      window.__plutoLastRenderedPassesV2827,
      window.__plutoBackendTestPassesV2831,
      window.__plutoLastRenderedPassesV2825
    ];
    for (const item of candidates) {
      if (Array.isArray(item) && item.length) return item.slice(0, 12);
    }
    return [];
  }

  function passForButtonV2831(button) {
    const row = button && button.closest ? button.closest(".pass-row") : null;
    const rows = Array.from(document.querySelectorAll("#passes .pass-row, .pass-list .pass-row"));
    const index = row ? rows.indexOf(row) : -1;
    const passes = visiblePassesV2831();
    if (index >= 0 && passes[index]) return passes[index];
    try {
      if (typeof currentSelectedPass !== "undefined" && currentSelectedPass) return currentSelectedPass;
    } catch (_error) {}
    return null;
  }

  async function resetAudioV2831(status) {
    if (status) status.textContent += "\nResetting any stale backend audio stream...";
    try { if (typeof stopAnalogAudio === "function") await stopAnalogAudio("Resetting backend test audio."); } catch (_error) {}
    try { if (typeof postJson === "function") await postJson("/api/radio/audio/live/stop", {}); } catch (_error) {}
    try { if (typeof getJson === "function") await getJson("/api/radio/track/stop"); } catch (_error) {}
  }

  async function runBackendTestV2831(pass, button) {
    const label = actionLabelV2831(pass, button);
    const name = (pass && pass.name) || "selected pass";
    const downlink = downlinkHzV2831(pass);
    const status = showModalV2831(pass, button, `Opening ${label} backend test for ${name}...`);
    setPageStatusV2831(`Opening ${label} backend test for ${name}...`);

    if (!pass) {
      if (status) status.textContent = "No pass object was available for this row. Refresh passes and try again.";
      return;
    }
    if (!downlink) {
      const msg = `Cannot start ${label}: no downlink is available for ${name}.`;
      if (status) status.textContent = msg;
      setPageStatusV2831(msg);
      return;
    }

    if (!isListenActionV2831(pass, button)) {
      const msg = [
        `${label} backend target selected for ${name}.`,
        `Mode: ${modeTextV2831(pass) || "unknown"}`,
        `Downlink: ${fmtHzV2831(downlink)}`,
        "",
        "This modal is the single pass-row receive test surface.",
        "Decoder-specific capture/decode wiring should attach here next."
      ].join("\n");
      if (status) status.textContent = msg;
      setPageStatusV2831(`${label} backend target selected for ${name}.`);
      return;
    }

    await resetAudioV2831(status);
    try {
      const stopButton = document.getElementById("passRowBackendTestStopV2827") || button;
      if (stopButton) {
        stopButton.hidden = false;
        stopButton.disabled = false;
        stopButton.textContent = "Stop audio";
      }
      if (status) status.textContent += `\nStarting existing backend audio path for ${fmtHzV2831(downlink)}...`;
      if (typeof startAnalogAudio !== "function") throw new Error("startAnalogAudio() is not available in this UI build.");
      await startAnalogAudio(pass, stopButton || button, status || document.getElementById("status"));
      if (status) status.textContent += "\nBackend audio stream opened.";
      setPageStatusV2831(`Listen backend test running for ${name}.`);
      showModalV2831(pass, button, status ? status.textContent : `Listen backend test running for ${name}.`);
    } catch (error) {
      try { if (typeof stopAnalogAudio === "function") await stopAnalogAudio(); } catch (_stopError) {}
      const msg = [
        `Listen backend test failed for ${name}.`,
        `Downlink: ${fmtHzV2831(downlink)}`,
        `Error: ${error && error.message ? error.message : String(error)}`,
        "",
        "If this is a 409, the backend rejected the live.wav stream as busy/not-ready after live/start.",
        "The modal is now open so the full error can be copied."
      ].join("\n");
      if (status) status.textContent = msg;
      setPageStatusV2831(`Listen backend test failed for ${name}.`);
      showModalV2831(pass, button, msg);
    }
  }

  function interceptClickV2831(event) {
    const button = event.target && event.target.closest ? event.target.closest(".pass-row-action-button-v286") : null;
    if (!button) return;
    const row = button.closest(".pass-row");
    if (!row) return;
    event.preventDefault();
    event.stopPropagation();
    if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
    button.disabled = false;
    button.removeAttribute("disabled");
    button.setAttribute("aria-disabled", "false");
    const pass = passForButtonV2831(button);
    showModalV2831(pass, button, `Opening ${actionLabelV2831(pass, button)} backend test...`);
    window.setTimeout(() => runBackendTestV2831(pass, button), 0);
  }

  function annotateV2831() {
    ensureStyleV2831();
    document.querySelectorAll(".pass-row-action-button-v286").forEach((button) => {
      button.disabled = false;
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
      button.style.pointerEvents = "auto";
      button.style.opacity = "1";
      button.title = "Open Backend Test modal for this pass row.";
    });
  }

  try {
    if (typeof renderPasses === "function" && renderPasses.backendTestPassCacheWrappedV2831 !== true) {
      const previousRenderPassesV2831 = renderPasses;
      renderPasses = function renderPassesBackendTestPassCacheV2831(payload) {
        try { window.__plutoBackendTestPassesV2831 = (payload && payload.passes) || []; } catch (_error) {}
        const result = previousRenderPassesV2831.apply(this, arguments);
        try { annotateV2831(); } catch (_error) {}
        return result;
      };
      renderPasses.backendTestPassCacheWrappedV2831 = true;
    }
  } catch (_error) {}

  try {
    configurePassRowActionButtonV286 = function configurePassRowActionButtonForceModalV2831(button, pass, onSelect) {
      if (!button) return;
      button.disabled = false;
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
      button.style.pointerEvents = "auto";
      button.style.opacity = "1";
      button.addEventListener("click", (event) => {
        event.preventDefault();
        event.stopPropagation();
        if (typeof event.stopImmediatePropagation === "function") event.stopImmediatePropagation();
        try { if (typeof onSelect === "function") onSelect(); } catch (_error) {}
        showModalV2831(pass, button, `Opening ${actionLabelV2831(pass, button)} backend test...`);
        window.setTimeout(() => runBackendTestV2831(pass, button), 0);
      }, true);
    };
  } catch (_error) {}

  document.addEventListener("click", interceptClickV2831, true);
  window.addEventListener("click", interceptClickV2831, true);
  window.plutoOpenBackendTestModalV2831 = showModalV2831;
  window.plutoRunBackendTestV2831 = runBackendTestV2831;
  window.plutoAnnotateBackendTestRowsV2831 = annotateV2831;

  function startV2831() {
    ensureModalV2831();
    annotateV2831();
    const observer = new MutationObserver(() => window.setTimeout(annotateV2831, 20));
    if (document.body) observer.observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ["disabled", "hidden", "class", "style"] });
    window.setInterval(annotateV2831, 1000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startV2831, { once: true });
  } else {
    startV2831();
  }
  console.info(`${MARKER} installed`);
})();
/* BACKEND_TEST_MODAL_FORCE_OPEN_V2_8_31_END */


/* BACKEND_AUDIO_DETERMINISTIC_SEQUENCE_V2_8_35
 * Replace the experimental browser-side 409 reset/retry wrappers with one
 * deterministic backend audio sequence proven by the v2.8.34b audit:
 *   stop stale backend audio -> plan/track best effort -> live/start -> live.wav
 * The stream is not reset/retried automatically after HTTP 409.  If live.wav
 * returns 409, the backend response body is shown to the operator so the root
 * state-machine problem is visible instead of hidden by browser retries.
 * UI-only; backend C is unchanged.
 */
(function installBackendAudioDeterministicSequenceV2835() {
  if (window.__plutoBackendAudioDeterministicSequenceV2835) return;
  window.__plutoBackendAudioDeterministicSequenceV2835 = true;

  const MARKER = "BACKEND_AUDIO_DETERMINISTIC_SEQUENCE_V2_8_35";
  const AUDIO_SAMPLE_RATE = 24000;
  const TARGET_CHUNK_BYTES = 4096 * 2;

  function delayV2835(ms) {
    return new Promise((resolve) => window.setTimeout(resolve, ms));
  }

  function statusTargetsV2835(statusNode) {
    const nodes = [];
    if (statusNode && statusNode.isConnected) nodes.push(statusNode);
    [
      "passRowBackendTestStatusV2827",
      "backendTestModalStatusV2827",
      "backendTestStatusV2831",
      "status"
    ].forEach((id) => {
      const node = document.getElementById(id);
      if (node && !nodes.includes(node)) nodes.push(node);
    });
    document.querySelectorAll(".pass-row-backend-test-status-v2827, .backend-test-status-v2827").forEach((node) => {
      if (node && !nodes.includes(node)) nodes.push(node);
    });
    return nodes;
  }

  function setAudioStatusV2835(statusNode, message, warn) {
    const text = String(message || "");
    statusTargetsV2835(statusNode).forEach((node) => {
      node.textContent = text;
      node.classList.toggle("warn", !!warn);
      node.classList.toggle("error", !!warn);
    });
    try {
      window.plutoLastBackendAudioStatusV2835 = {
        marker: MARKER,
        message: text,
        warn: !!warn,
        at: new Date().toISOString()
      };
    } catch (_error) {}
  }

  function buttonSetV2835(button, text, disabled) {
    if (!button || !button.isConnected) return;
    if (text) button.textContent = text;
    button.disabled = !!disabled;
    if (disabled) {
      button.setAttribute("disabled", "");
      button.setAttribute("aria-disabled", "true");
    } else {
      button.removeAttribute("disabled");
      button.setAttribute("aria-disabled", "false");
    }
  }

  async function fetchTextWithTimeoutV2835(url, options, timeoutMs) {
    const controller = new AbortController();
    const timer = window.setTimeout(() => controller.abort(`timeout_${timeoutMs}ms`), timeoutMs || 8000);
    try {
      const response = await fetch(url, Object.assign({ cache: "no-store", signal: controller.signal }, options || {}));
      let text = "";
      try { text = await response.text(); } catch (_error) {}
      if (!response.ok) {
        const snippet = String(text || "").replace(/\s+/g, " ").trim().slice(0, 900);
        throw new Error(`${url}: ${response.status}${snippet ? ` | ${snippet}` : ""}`);
      }
      return text;
    } finally {
      window.clearTimeout(timer);
    }
  }

  async function getJsonBestEffortV2835(url, timeoutMs) {
    try {
      return await fetchTextWithTimeoutV2835(url, { method: "GET" }, timeoutMs || 5000);
    } catch (_error) {
      return null;
    }
  }

  async function postJsonRequiredV2835(url, timeoutMs) {
    return fetchTextWithTimeoutV2835(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    }, timeoutMs || 8000);
  }

  async function postJsonBestEffortV2835(url, timeoutMs) {
    try {
      return await postJsonRequiredV2835(url, timeoutMs || 5000);
    } catch (_error) {
      return null;
    }
  }

  function stopBrowserAudioOnlyV2835(reason) {
    const session = typeof analogAudioSession !== "undefined" ? analogAudioSession : null;
    analogAudioSession = null;
    if (!session) return;
    session.stopped = true;
    if (session.reconnectTimer) {
      window.clearTimeout(session.reconnectTimer);
      session.reconnectTimer = 0;
    }
    try { if (session.controller) session.controller.abort("manual_stop"); } catch (_error) {}
    try {
      for (const source of session.sources || []) {
        try { source.stop(); } catch (_error) {}
      }
    } catch (_error) {}
    try { if (session.context) session.context.close(); } catch (_error) {}
    buttonSetV2835(session.button, "Listen", false);
    if (session.statusNode && session.statusNode.isConnected && reason) {
      session.statusNode.textContent = reason;
    }
  }

  async function stopAnalogAudioDeterministicV2835(reason = "Analog monitor stopped.") {
    const session = typeof analogAudioSession !== "undefined" ? analogAudioSession : null;
    stopBrowserAudioOnlyV2835(reason);
    const stopUrl = (session && session.stopUrl) || "/api/radio/audio/live/stop";
    await postJsonBestEffortV2835(stopUrl, 4500);
    try {
      if (typeof spectrumWaterfallModalOpenV250 !== "undefined" && spectrumWaterfallModalOpenV250 && typeof renderSpectrumWaterfallV250 === "function") {
        renderSpectrumWaterfallV250().catch(() => {});
      }
    } catch (_error) {}
  }

  function liveAudioModeV2835(pass) {
    const radio = (pass && pass.radio) || {};
    const mode = String(radio.mode || (pass && pass.modes && pass.modes[0]) || "FM").trim().toUpperCase();
    if (/USB|LSB|SSB/.test(mode)) return mode;
    if (/AM/.test(mode)) return "AM";
    if (/CW/.test(mode)) return "CW";
    return mode || "FM";
  }

  function liveStreamErrorV2835(streamUrl, response, text) {
    const snippet = String(text || "").replace(/\s+/g, " ").trim().slice(0, 1200);
    if (response.status === 409) {
      return new Error(`${streamUrl}: 409 | ${snippet || "backend reports live audio is not running or is busy"}`);
    }
    return new Error(`${streamUrl}: ${response.status}${snippet ? ` | ${snippet}` : ""}`);
  }

  async function startAnalogAudioDeterministicV2835(pass, button, statusNode) {
    const audioUrls = typeof analogAudioUrl === "function" ? analogAudioUrl(pass) : null;
    const AudioCtx = window.AudioContext || window.webkitAudioContext;
    if (!AudioCtx) throw new Error("Web Audio is not available in this browser.");
    if (!audioUrls) throw new Error("No usable downlink is available for this pass.");

    buttonSetV2835(button, "Starting", true);
    setAudioStatusV2835(statusNode, "Stopping any previous browser/backend audio session...", false);
    stopBrowserAudioOnlyV2835("Resetting audio before backend test.");
    await postJsonBestEffortV2835(audioUrls.stopUrl || "/api/radio/audio/live/stop", 4500);
    await delayV2835(250);

    setAudioStatusV2835(statusNode, "Planning backend tuning and Doppler tracking...", false);
    try {
      await getJsonBestEffortV2835(`/api/radio/plan?${new URLSearchParams({
        name: pass && pass.name ? pass.name : "",
        norad: String(pass && pass.norad_id ? pass.norad_id : ""),
        aos: pass && pass.aos_utc ? pass.aos_utc : "",
        downlink: String(audioUrls.downlink),
        mode: liveAudioModeV2835(pass),
        description: (pass && pass.radio && pass.radio.description) || ""
      }).toString()}`, 5000);
    } catch (_error) {}

    if (pass && pass.doppler_plan && (pass.doppler_plan.points || []).length) {
      try {
        if (typeof dopplerTrackPayload === "function") {
          await postJsonRequiredV2835("/api/radio/track/plan", 5000).catch(() => null);
          if (typeof postJson === "function") await postJson("/api/radio/track/plan", dopplerTrackPayload(pass));
          await getJsonBestEffortV2835("/api/radio/track/auto/start", 5000);
        }
      } catch (_error) {
        // Continue with fixed-frequency audio if tracking cannot start.
      }
    }

    setAudioStatusV2835(statusNode, "Starting backend audio DSP...", false);
    await postJsonRequiredV2835(audioUrls.startUrl, 9000);

    const context = new AudioCtx({ sampleRate: AUDIO_SAMPLE_RATE });
    await context.resume();

    const session = {
      button,
      context,
      controller: null,
      nextTime: context.currentTime + 0.35,
      passKey: typeof passKey === "function" ? passKey(pass) : `${audioUrls.downlink}`,
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
    buttonSetV2835(button, "Stop", false);
    setAudioStatusV2835(statusNode, "Opening backend decoded audio stream...", false);

    const schedulePcmBytes = (pcmBytes) => {
      if (session.stopped || !pcmBytes || !pcmBytes.length) return;
      const evenLength = pcmBytes.length - (pcmBytes.length % 2);
      if (evenLength <= 0) return;
      const audioBuffer = pcm16ToAudioBuffer(context, pcmBytes.slice(0, evenLength), AUDIO_SAMPLE_RATE);
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
      setAudioStatusV2835(statusNode, `Playing backend decoded audio from Pluto (${bufferedSeconds.toFixed(1)}s buffered, reconnects ${session.reconnectCount}).`, false);
    };

    const readOneStreamSegment = async () => {
      const controller = new AbortController();
      session.controller = controller;
      const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=${session.reconnectCount}`;
      const response = await fetch(streamUrl, { cache: "no-store", signal: controller.signal });
      if (session.controller === controller) session.controller = null;
      if (!response.ok) {
        let text = "";
        try { text = await response.clone().text(); } catch (_error) {}
        throw liveStreamErrorV2835(streamUrl, response, text);
      }
      if (!response.body || !response.body.getReader) throw new Error("Browser streaming fetch is not available.");

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
        while (pending.length >= TARGET_CHUNK_BYTES || (force && pending.length >= 2)) {
          const take = force ? pending.length - (pending.length % 2) : TARGET_CHUNK_BYTES;
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
        if (session.totalBuffers === 0) {
          setAudioStatusV2835(statusNode, `Receiving backend audio stream (${session.totalBytes} bytes)...`, false);
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
          setAudioStatusV2835(statusNode, `Backend stream segment ended after ${segmentBytes} bytes; reconnecting (${bufferedSeconds.toFixed(1)}s buffered).`, false);
          const delayMs = bufferedSeconds > 0.8 ? 200 : 20;
          await new Promise((resolve) => {
            session.reconnectTimer = window.setTimeout(resolve, delayMs);
          });
          session.reconnectTimer = 0;
        } catch (error) {
          if (session.stopped || analogAudioSession !== session) return;
          const message = error && error.message ? error.message : String(error || "Backend audio stream failed.");
          setAudioStatusV2835(statusNode, message, true);
          await stopAnalogAudioDeterministicV2835(message);
          buttonSetV2835(button, "Listen", false);
          return;
        }
      }
    };

    reconnectLoop();
  }

  stopAnalogAudio = stopAnalogAudioDeterministicV2835;
  startAnalogAudio = startAnalogAudioDeterministicV2835;

  window.plutoBackendAudioDeterministicV2835 = {
    marker: MARKER,
    stop: () => stopAnalogAudioDeterministicV2835("Backend audio stopped."),
    lastStatus: () => window.plutoLastBackendAudioStatusV2835 || null
  };
})();
/* BACKEND_AUDIO_DETERMINISTIC_SEQUENCE_V2_8_35_END */

/* BACKEND_AUDIO_BOUNDED_STOP_V2_8_36
 * Keep live-audio cleanup from blocking a new backend audio test.
 * The backend audit proved the reliable sequence is stop -> start -> live.wav.
 * This UI-only guard keeps stop idempotent and bounded: try to clear stale
 * backend audio, but never let a hung stop request prevent live/start.
 */
(function installBackendAudioBoundedStopV2836() {
  const MARKER = "BACKEND_AUDIO_BOUNDED_STOP_V2_8_36";
  if (window.__plutoBackendAudioBoundedStopV2836) return;
  window.__plutoBackendAudioBoundedStopV2836 = true;

  const STOP_TIMEOUT_MS = 1800;
  const previousPostJsonV2836 = (typeof postJson === "function") ? postJson : null;

  function isLiveStopUrlV2836(url) {
    return String(url || "").indexOf("/api/radio/audio/live/stop") >= 0;
  }

  function statusTextV2836(message) {
    try {
      const modalStatus = document.getElementById("backendTestStatusV2827") ||
        document.getElementById("passRowBackendTestStatusV2827") ||
        document.querySelector(".backend-test-status-v2827, .pass-row-backend-test-status-v2827");
      if (modalStatus) modalStatus.textContent = message;
      const statusBar = document.getElementById("status");
      if (statusBar) statusBar.textContent = message;
    } catch (_error) {}
  }

  async function boundedLiveStopV2836(url, payload, timeoutMs) {
    const targetUrl = String(url || "/api/radio/audio/live/stop");
    const ms = Number.isFinite(Number(timeoutMs)) ? Math.max(250, Number(timeoutMs)) : STOP_TIMEOUT_MS;
    const controller = new AbortController();
    const timer = window.setTimeout(() => {
      try { controller.abort(); } catch (_error) {}
    }, ms);

    try {
      const response = await fetch(targetUrl, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload || {}),
        cache: "no-store",
        signal: controller.signal
      });
      const body = await response.text().catch(() => "");
      let data = null;
      try { data = body ? JSON.parse(body) : {}; } catch (_error) { data = { raw_body: body.slice(0, 1000) }; }
      const result = Object.assign({}, data || {}, {
        ok: response.ok && (!data || data.ok !== false),
        http_ok: response.ok,
        http_status: response.status,
        marker: MARKER,
        bounded_stop: true
      });
      if (!response.ok) {
        statusTextV2836(`Backend audio stop returned HTTP ${response.status}; continuing with start.`);
      }
      return result;
    } catch (error) {
      const timedOut = error && (error.name === "AbortError" || /abort/i.test(String(error.message || error)));
      const message = timedOut
        ? `Backend audio stop timed out after ${ms} ms; continuing with start.`
        : `Backend audio stop failed (${error && error.message ? error.message : String(error)}); continuing with start.`;
      statusTextV2836(message);
      return {
        ok: false,
        marker: MARKER,
        bounded_stop: true,
        timeout: !!timedOut,
        error: error && error.message ? error.message : String(error),
        message
      };
    } finally {
      window.clearTimeout(timer);
    }
  }

  if (previousPostJsonV2836) {
    postJson = async function postJsonWithBoundedLiveStopV2836(url, payload) {
      if (isLiveStopUrlV2836(url)) {
        return boundedLiveStopV2836(url, payload, STOP_TIMEOUT_MS);
      }
      return previousPostJsonV2836.apply(this, arguments);
    };
  }

  window.plutoBoundedLiveAudioStopV2836 = boundedLiveStopV2836;
  window.plutoBackendAudioBoundedStopMarkerV2836 = MARKER;
})();


/* BACKEND_AUDIO_DIRECT_START_NO_LEGACY_STOP_V2_8_37_BEGIN
 * Clean backend-test Listen startup sequence after v2.8.34b proved the backend
 * works when called as stop -> start -> live.wav.  This intentionally bypasses
 * the legacy stopAnalogAudio() await path, because that path can block forever
 * before /live/start is reached.  Browser cleanup is local and non-blocking;
 * backend /live/stop is bounded; then /live/start is called once and the stream
 * is opened once with no browser-side 409 reset/retry loop.
 */
(function installBackendAudioDirectStartV2837() {
  const MARKER = "BACKEND_AUDIO_DIRECT_START_NO_LEGACY_STOP_V2_8_37";
  if (window.__plutoBackendAudioDirectStartV2837) return;
  window.__plutoBackendAudioDirectStartV2837 = true;

  function setAudioStatusV2837(statusNode, message) {
    if (statusNode && statusNode.isConnected) statusNode.textContent = message;
    const statusBar = document.getElementById("status");
    if (statusBar) statusBar.textContent = message;
  }

  function concatV2837(a, b) {
    if (typeof concatUint8Arrays === "function") return concatUint8Arrays(a, b);
    if (!a || !a.length) return b || new Uint8Array();
    if (!b || !b.length) return a;
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
  }

  async function fetchTextTimeoutV2837(url, options, timeoutMs) {
    const controller = new AbortController();
    const timer = window.setTimeout(() => controller.abort(), timeoutMs || 2000);
    try {
      const response = await fetch(url, Object.assign({ cache: "no-store" }, options || {}, { signal: controller.signal }));
      const text = await response.text().catch(() => "");
      let data = null;
      try { data = text ? JSON.parse(text) : null; } catch (_error) { data = null; }
      return { ok: response.ok, status: response.status, statusText: response.statusText, text, data, url };
    } catch (error) {
      return { ok: false, status: 0, statusText: "fetch_error", text: String(error && error.message ? error.message : error), error: String(error && error.message ? error.message : error), url };
    } finally {
      window.clearTimeout(timer);
    }
  }

  async function postJsonTimeoutV2837(url, timeoutMs) {
    const result = await fetchTextTimeoutV2837(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    }, timeoutMs || 2500);
    if (!result.ok) {
      const detail = result.text ? ` | ${result.text.slice(0, 300)}` : "";
      throw new Error(`${url}: ${result.status || "fetch_error"}${detail}`);
    }
    return result.data || { ok: true, http_status: result.status };
  }

  async function getJsonTimeoutV2837(url, timeoutMs) {
    const result = await fetchTextTimeoutV2837(url, { method: "GET" }, timeoutMs || 1500);
    if (!result.ok) {
      const detail = result.text ? ` | ${result.text.slice(0, 300)}` : "";
      throw new Error(`${url}: ${result.status || "fetch_error"}${detail}`);
    }
    return result.data || { ok: true, http_status: result.status };
  }

  function clearBrowserAudioSessionLocalV2837(reason) {
    const session = typeof analogAudioSession !== "undefined" ? analogAudioSession : null;
    try { analogAudioSession = null; } catch (_error) {}
    if (!session) return { hadSession: false };
    try { session.stopped = true; } catch (_error) {}
    try {
      if (session.reconnectTimer) {
        window.clearTimeout(session.reconnectTimer);
        session.reconnectTimer = 0;
      }
    } catch (_error) {}
    try { if (session.controller) session.controller.abort(); } catch (_error) {}
    try {
      for (const source of session.sources || []) {
        try { source.stop(); } catch (_error) {}
      }
    } catch (_error) {}
    try { if (session.context) session.context.close().catch(() => {}); } catch (_error) {}
    try { if (session.button && session.button.isConnected) session.button.textContent = "Listen"; } catch (_error) {}
    try { if (session.statusNode && session.statusNode.isConnected) session.statusNode.textContent = reason || "Previous browser audio session cleared."; } catch (_error) {}
    return { hadSession: true };
  }

  async function boundedBackendStopV2837(statusNode) {
    setAudioStatusV2837(statusNode, "Clearing backend audio session with bounded /live/stop...");
    const result = await fetchTextTimeoutV2837("/api/radio/audio/live/stop", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    }, 1200);
    if (result.ok) {
      setAudioStatusV2837(statusNode, "Backend stop cleanup completed; starting new audio DSP...");
      return result;
    }
    setAudioStatusV2837(statusNode, `Backend stop cleanup did not complete (${result.status || result.statusText}); continuing to start.`);
    return result;
  }

  async function startAnalogAudioDirectV2837(pass, button, statusNode) {
    const audioUrls = typeof analogAudioUrl === "function" ? analogAudioUrl(pass) : null;
    const AudioCtx = window.AudioContext || window.webkitAudioContext;
    const sampleRate = 24000;
    const targetChunkBytes = 4096 * 2;

    if (!AudioCtx) throw new Error("Web Audio is not available in this browser.");
    if (!audioUrls) throw new Error("No usable downlink is available for this pass.");

    setAudioStatusV2837(statusNode, "Clearing previous browser audio session locally...");
    clearBrowserAudioSessionLocalV2837("Previous browser audio session cleared.");
    await boundedBackendStopV2837(statusNode);

    try {
      setAudioStatusV2837(statusNode, "Planning radio target for backend audio test...");
      await getJsonTimeoutV2837(`/api/radio/plan?${new URLSearchParams({
        name: pass && pass.name ? pass.name : "",
        norad: String(pass && pass.norad_id ? pass.norad_id : ""),
        aos: pass && pass.aos_utc ? pass.aos_utc : "",
        downlink: String(audioUrls.downlink || ""),
        mode: (pass && pass.radio && pass.radio.mode) || ((pass && pass.modes) || [])[0] || "",
        description: (pass && pass.radio && pass.radio.description) || ""
      }).toString()}`, 1400);
    } catch (error) {
      setAudioStatusV2837(statusNode, `Radio plan skipped: ${error.message || error}`);
    }

    // For backend-test row clicks, do not let Doppler auto-start block audio.
    // The audit-proven path only needs live/start to tune by downlink_hz.
    setAudioStatusV2837(statusNode, `Starting backend audio DSP: ${new URL(audioUrls.startUrl, window.location.origin).searchParams.toString()}`);
    await postJsonTimeoutV2837(audioUrls.startUrl, 3500);

    const context = new AudioCtx({ sampleRate });
    await context.resume();

    const session = {
      button,
      context,
      controller: null,
      nextTime: context.currentTime + 0.35,
      passKey: typeof passKey === "function" ? passKey(pass) : `pass:${Date.now()}`,
      reconnectCount: 0,
      reconnectTimer: 0,
      sources: [],
      statusNode,
      stopUrl: audioUrls.stopUrl,
      stopped: false,
      totalBytes: 0,
      totalBuffers: 0
    };
    try { analogAudioSession = session; } catch (_error) {}

    if (button && button.isConnected) button.textContent = "Stop";
    setAudioStatusV2837(statusNode, "Opening backend audio stream after successful /live/start...");

    const schedulePcmBytes = (pcmBytes) => {
      if (session.stopped || !pcmBytes || !pcmBytes.length) return;
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
      setAudioStatusV2837(statusNode, `Playing backend audio from Pluto (${bufferedSeconds.toFixed(1)}s buffered, ${session.totalBytes} bytes).`);
    };

    const readOneStreamSegment = async () => {
      const controller = new AbortController();
      session.controller = controller;
      const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=${session.reconnectCount}`;
      const response = await fetch(streamUrl, { cache: "no-store", signal: controller.signal });
      if (!response.ok) {
        const body = await response.clone().text().catch(() => "");
        throw new Error(`${streamUrl}: ${response.status}${body ? ` | ${body.slice(0, 500)}` : ""}`);
      }
      if (!response.body || !response.body.getReader) throw new Error("Browser streaming fetch is not available.");

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
        pending = concatV2837(pending, value);
        processPending(false);
        if (session.totalBuffers === 0) setAudioStatusV2837(statusNode, `Receiving backend audio stream (${session.totalBytes} bytes)...`);
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
          setAudioStatusV2837(statusNode, `Backend stream segment ended after ${segmentBytes} bytes; reconnecting (${bufferedSeconds.toFixed(1)}s buffered).`);
          const delayMs = bufferedSeconds > 0.8 ? 200 : 20;
          await new Promise((resolve) => { session.reconnectTimer = window.setTimeout(resolve, delayMs); });
          session.reconnectTimer = 0;
        } catch (error) {
          if (session.stopped || analogAudioSession !== session) return;
          const message = error && error.message ? error.message : "Backend audio stream failed.";
          setAudioStatusV2837(statusNode, message);
          try { await stopAnalogAudio(message); } catch (_error) {}
          return;
        }
      }
    };

    reconnectLoop();
  }

  try {
    startAnalogAudio = startAnalogAudioDirectV2837;
    startAnalogAudio.backendAudioDirectStartV2837 = true;
  } catch (_error) {
  }
  window.plutoStartAnalogAudioDirectV2837 = startAnalogAudioDirectV2837;
  window.plutoClearBrowserAudioSessionLocalV2837 = clearBrowserAudioSessionLocalV2837;
})();
/* BACKEND_AUDIO_DIRECT_START_NO_LEGACY_STOP_V2_8_37_END */

/* LIVE_WAV_VERIFIED_START_GATE_V2_8_39
 * Central guard for every browser fetch of /api/radio/audio/live.wav.
 * The backend audit proved the correct order is start -> stream.  This guard
 * prevents old or duplicate UI handlers from opening live.wav before a verified
 * /api/radio/audio/live/start call succeeds.  It does not add a blind reset loop.
 */
(function installLiveWavVerifiedStartGateV2839() {
  if (window.__plutoLiveWavVerifiedStartGateV2839) return;
  window.__plutoLiveWavVerifiedStartGateV2839 = true;

  const marker = "LIVE_WAV_VERIFIED_START_GATE_V2_8_39";
  const originalFetch = window.fetch.bind(window);
  const activeStarts = new Map();
  let lastEvent = { marker, state: "installed", at: new Date().toISOString() };

  function asUrlV2839(input) {
    const raw = typeof input === "string" ? input : ((input && input.url) || String(input || ""));
    try { return new URL(raw, window.location.origin); } catch (_error) { return null; }
  }

  function nowIsoV2839() {
    return new Date().toISOString();
  }

  function noteV2839(state, detail) {
    lastEvent = { marker, state, at: nowIsoV2839(), ...(detail || {}) };
    try { console.log("Pluto live WAV gate:", lastEvent); } catch (_error) {}
    try {
      const modalStatus = document.getElementById("passRowBackendTestStatusV2827") ||
        document.getElementById("passRowBackendTestBodyV2827") ||
        document.getElementById("analogAudioStatus") ||
        document.getElementById("status");
      if (modalStatus && detail && detail.message) modalStatus.textContent = detail.message;
    } catch (_error) {}
  }

  function modeForStartV2839(url) {
    const explicit = String(url.searchParams.get("mode") || "").trim();
    if (explicit) return explicit;
    try {
      const pass = (typeof currentSelectedPass !== "undefined" && currentSelectedPass) ? currentSelectedPass : null;
      const raw = String((pass && pass.radio && pass.radio.mode) || (pass && pass.modes && pass.modes[0]) || "").toUpperCase();
      if (/^(N?FM|WFM|AM|USB|LSB|SSB)$/.test(raw)) return raw;
    } catch (_error) {}
    return "FM";
  }

  function startUrlForLiveWavV2839(liveUrl) {
    const downlink = liveUrl.searchParams.get("downlink_hz") || liveUrl.searchParams.get("downlink") || "";
    if (!downlink) throw new Error("Cannot start live audio: live.wav URL has no downlink_hz.");
    const params = new URLSearchParams({ downlink_hz: String(downlink), mode: modeForStartV2839(liveUrl) });
    try {
      if (typeof spectrumLiveAudioControlParamsV2615 === "function") {
        const controls = spectrumLiveAudioControlParamsV2615();
        Object.entries(controls || {}).forEach(([key, value]) => {
          if (value !== undefined && value !== null && String(value) !== "") params.set(key, String(value));
        });
      }
    } catch (_error) {}
    return { downlink, url: `/api/radio/audio/live/start?${params.toString()}` };
  }

  async function responseTextV2839(response) {
    try { return await response.clone().text(); } catch (_error) { return ""; }
  }

  async function verifiedStartForLiveWavV2839(liveUrl, reason) {
    const start = startUrlForLiveWavV2839(liveUrl);
    const existing = activeStarts.get(start.downlink);
    if (existing && existing.ok === true) {
      return existing;
    }

    noteV2839("starting", {
      downlink_hz: start.downlink,
      start_url: start.url,
      message: `Starting backend live audio before stream (${start.downlink} Hz)...`
    });

    const response = await originalFetch(start.url, {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    });
    const bodyText = await responseTextV2839(response);
    let body = null;
    try { body = bodyText ? JSON.parse(bodyText) : null; } catch (_error) {}

    if (!response.ok || (body && body.ok === false)) {
      activeStarts.delete(start.downlink);
      const message = `Backend live/start failed (${response.status}): ${bodyText || response.statusText}`;
      noteV2839("start_failed", { downlink_hz: start.downlink, status: response.status, body: bodyText, message });
      throw new Error(message);
    }

    const record = { ok: true, downlink_hz: start.downlink, start_url: start.url, status: response.status, body, reason, at: Date.now() };
    activeStarts.set(start.downlink, record);
    noteV2839("start_ok", {
      downlink_hz: start.downlink,
      status: response.status,
      message: `Backend live audio started; opening stream (${start.downlink} Hz)...`
    });
    return record;
  }

  window.fetch = async function plutoLiveWavVerifiedStartFetchV2839(input, init) {
    const url = asUrlV2839(input);
    if (url && url.pathname.includes("/api/radio/audio/live/stop")) {
      activeStarts.clear();
      noteV2839("stop_seen", { message: "Backend live audio stop requested." });
      return originalFetch(input, init);
    }

    if (!url || !url.pathname.includes("/api/radio/audio/live.wav")) {
      return originalFetch(input, init);
    }

    await verifiedStartForLiveWavV2839(url, "pre_stream_gate");
    let response = await originalFetch(input, init);
    if (response.ok || response.status !== 409) return response;

    const bodyText = await responseTextV2839(response);
    const downlink = url.searchParams.get("downlink_hz") || url.searchParams.get("downlink") || "";
    noteV2839("stream_409", {
      downlink_hz: downlink,
      status: response.status,
      body: bodyText,
      message: `live.wav returned 409 after verified start: ${bodyText || response.statusText}`
    });

    if (/live analog audio is not running/i.test(bodyText || "")) {
      activeStarts.delete(String(downlink));
      await verifiedStartForLiveWavV2839(url, "post_409_not_running");
      response = await originalFetch(input, init);
    }
    return response;
  };

  window.plutoLiveWavStartGateV2839 = {
    marker,
    status: () => ({ lastEvent, activeStarts: Array.from(activeStarts.entries()) }),
    clear: () => { activeStarts.clear(); noteV2839("cleared", { message: "Live WAV start gate state cleared." }); },
    startForUrl: (url) => verifiedStartForLiveWavV2839(new URL(url, window.location.origin), "manual")
  };

  noteV2839("installed", { message: "Live WAV verified-start gate installed." });
})();
/* LIVE_WAV_VERIFIED_START_GATE_V2_8_39_END */

