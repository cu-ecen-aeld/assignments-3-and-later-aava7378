#!/bin/bash

if [ $# -ne 2 ]; then
	echo "Error: missing arg"
	exit 1
fi

writefile=$1
writestr=$2

mkdir -p "$(dirname "$writefile")" || exit 1
echo "$writestr" > "$writefile" || exit 1
