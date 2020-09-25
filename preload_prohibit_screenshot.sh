result=$(echo $LD_PRELOAD | grep "/usr/local/lib/libprohibitscreenshot.so")
if [ ! -n "$result" ]; then
	export "LD_PRELOAD=/usr/local/lib/libprohibitscreenshot.so:$LD_PRELOAD"
fi
