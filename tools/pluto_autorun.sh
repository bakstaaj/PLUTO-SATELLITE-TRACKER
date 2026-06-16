#!/bin/sh
APP=/mnt/jffs2/pluto_sat_tracker
mkdir -p /tmp/pluto_sat_tracker/logs "$APP/data" "$APP/config" "$APP/web"
export PLUTO_SAT_DATA_DIR="$APP/data"
export PLUTO_SAT_WEB_DIR="$APP/web"
export PLUTO_SAT_CONFIG_DIR="$APP/config"
exec "$APP/bin/pluto_sat_tracker" --bind 0.0.0.0 --port 8080 --data-dir "$APP/data" --web-dir "$APP/web" --config-dir "$APP/config"
