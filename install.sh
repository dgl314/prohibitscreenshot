#!/bin/bash

WHITELIST_CONF_PATH=/etc/screenshot_whitelist.conf
PRELOAD_LIB_PATH=/usr/local/lib/libprohibitscreenshot.so
PRELOAD_SHELL_PATH=/etc/profile.d/preload_prohibit_screenshot.sh

if [ ! -f "$PRELOAD_LIB_PATH" ]; then
    sudo cp ./libprohibitscreenshot.so /usr/local/lib/
fi

if [ ! -f "$PRELOAD_SHELL_PATH" ]; then
    sudo cp ./preload_prohibit_screenshot.sh /etc/profile.d/
fi

if [ ! -f "$WHITELIST_CONF_PATH" ]; then
    sudo cp ./screenshot_whitelist.conf /etc/
fi
