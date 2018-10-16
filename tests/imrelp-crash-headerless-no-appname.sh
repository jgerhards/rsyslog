#!/bin/bash
# addd 2016-05-13 by RGerhards, released under ASL 2.0
. $srcdir/diag.sh init
generate_conf
add_conf '
module(load="../plugins/imrelp/.libs/imrelp")
input(type="imrelp" port="'$TCPFLOOD_PORT'" ruleset="input")

ruleset(name="input") {
    set $!tag = $app-name;
    action(type="omfile" file="/dummy.log")
}

template(name="outfmt" type="string" string="%msg:F,58:2%\n")
:msg, contains, "msgnum:" action(type="omfile" template="outfmt"
			         file=`echo $RSYSLOG_OUT_LOG`)
'
startup
tcpflood -p $TCPFLOOD_PORT -T relp-plain -M "[2018-10-16 09:59:13] ping pong"
shutdown_when_empty # shut down rsyslogd when done processing messages
wait_shutdown
exit_test
