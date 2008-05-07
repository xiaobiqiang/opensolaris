#!/bin/sh
#
# hal-system-power-shutdown-sunos.sh
#
# Licensed under the Academic Free License version 2.1
#

unsupported() {
	echo "org.freedesktop.Hal.Device.SystemPowerManagement.NotSupported" >&2
	echo "No shutdown command found" >&2
	exit 1
}

if [ -x "/sbin/init" ] ; then
	/sbin/init 5
	exit $?
else
	unsupported
fi
