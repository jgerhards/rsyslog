#!/bin/bash
# add 2018-11-15 by Jan Gerhards, released under ASL 2.0

pgrep uxsockrcvr
. ${srcdir:=.}/diag.sh init

./uxsockrcvr &> $RSYSLOG_DYNNAME.output
grep -q -- "too few arguments" $RSYSLOG_DYNNAME.output 

if [ ! $? -eq 0 ]; then
  echo "invalid response generated"
  error_exit  1
fi;

exit_test
