#!/bin/bash
# add 2016-11-22 by Jan Gerhards, released under ASL 2.0
. $srcdir/diag.sh init
. $srcdir/diag.sh generate-conf
. $srcdir/diag.sh add-conf '
template(name="outfmt" type="string" string="%msg%\n")

module(load="../plugins/mmanon/.libs/mmanon")
module(load="../plugins/imptcp/.libs/imptcp")
input(type="imptcp" port="13514" ruleset="testing")

ruleset(name="testing") {
	action(type="mmanon" mode="simple" ipv4.bits="24")
	action(type="omfile" file="./rsyslog.out.log" template="outfmt")
}'

. $srcdir/diag.sh startup
. $srcdir/diag.sh tcpflood -m1 -M "\"<129>Mar 10 01:00:00 172.20.245.8 tag: 255.255.255.255\""

. $srcdir/diag.sh shutdown-when-empty
. $srcdir/diag.sh wait-shutdown
echo ' 255.xxx.xxx.xxx' | cmp rsyslog.out.log
if [ ! $? -eq 0 ]; then
  echo "invalid response generated, rsyslog.out.log is:"
  cat rsyslog.out.log
  . $srcdir/diag.sh error-exit  1
fi;

. $srcdir/diag.sh exit
