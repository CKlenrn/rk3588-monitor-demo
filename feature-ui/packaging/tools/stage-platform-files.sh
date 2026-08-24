#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root on the development board" >&2
    exit 77
fi

script_dir="$(CDPATH= cd "$(dirname "$0")" && pwd)"
payload="$script_dir/../board"

mkdir -p /etc/monitor_demo/staged-init \
         /usr/libexec/monitor-demo-validation /usr/sbin
cp "$payload/etc/monitor_demo/weston.ini" /etc/monitor_demo/weston.ini
cp "$payload/etc/monitor_demo/staged-init/S49monitor_weston" \
   /etc/monitor_demo/staged-init/S49monitor_weston
cp "$payload/etc/monitor_demo/staged-init/S50monitor_demo" \
   /etc/monitor_demo/staged-init/S50monitor_demo
chmod 0644 /etc/monitor_demo/weston.ini
chmod 0755 /etc/monitor_demo/staged-init/S49monitor_weston \
           /etc/monitor_demo/staged-init/S50monitor_demo

for name in monitor-demo-prepare-usb monitor-demo-supervisor; do
    cp "$payload/usr/libexec/$name" "/usr/libexec/$name"
    chmod 0755 "/usr/libexec/$name"
done

for name in test-current-weston.sh test-isolated-weston.sh; do
    cp "$script_dir/../validation/$name" \
       "/usr/libexec/monitor-demo-validation/$name"
    chmod 0755 "/usr/libexec/monitor-demo-validation/$name"
done

for name in \
    monitor-demo-enable-boot \
    monitor-demo-install-release \
    monitor-demo-mark-validation \
    monitor-demo-preflight \
    monitor-demo-restore-ui \
    monitor-demo-rollback-release; do
    cp "$payload/usr/sbin/$name" "/usr/sbin/$name"
    chmod 0755 "/usr/sbin/$name"
done

echo "platform files staged; original startup scripts were not changed"
