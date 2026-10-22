#!/bin/sh
# Example install hook. Runs after the payload has been unpacked to
# ~/.local/share/soil/opt/<id> and with these variables set:
#   SOIL_APP_ID, SOIL_VERSION, SOIL_APPDIR, SOIL_DATA
echo "demo-installer: installing $SOIL_APP_ID $SOIL_VERSION"
echo "demo-installer: app files in $SOIL_APPDIR"
echo "demo-installer: data dir   $SOIL_DATA"
mkdir -p "$SOIL_DATA"
{
  echo "Installed by the Soil SDK demo installer."
  echo "app:     $SOIL_APP_ID"
  echo "version: $SOIL_VERSION"
  echo "files:   $SOIL_APPDIR"
  echo "data:    $SOIL_DATA"
  date
} > "$SOIL_DATA/demo-installed.txt"
echo "demo-installer: done (see $SOIL_DATA/demo-installed.txt)"
