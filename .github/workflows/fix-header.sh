#!/bin/sh
set -e

BASEDIR=$(dirname "$0")

tmpfile=$(mktemp)
cat "${BASEDIR}/header" >$tmpfile
tail -n +2 "$1" >>$tmpfile
cat $tmpfile >"$1"
rm $tmpfile
