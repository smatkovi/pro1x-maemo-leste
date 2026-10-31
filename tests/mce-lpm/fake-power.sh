#!/bin/sh
# bound over poweroff/reboot/halt/shutdown inside the test's namespace
echo "fake $0 $*" >> /tmp/mcetest/power-requests.log
exit 0
