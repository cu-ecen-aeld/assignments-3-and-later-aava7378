#!/bin/sh

cd /home
echo "Running finder test"
./finder-test.sh
rc=$?

if [ $rc -eq 0 ]; then
    echo "Completed with success!!"
else
    echo "Completed with failure, rc=$rc"
fi

echo "Shutting down QEMU"
poweroff -f || halt -f

