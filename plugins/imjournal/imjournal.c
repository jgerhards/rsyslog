/* The systemd journal import module
 *
 * To test under Linux:
 * emmit log message into systemd journal
 *
 * Copyright (C) 2008-2014 Adiscon GmbH
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <systemd/sd-journal.h>

#include "dirty.h"
#include "cfsysline.h"
#include "obj.h"
#include "msg.h"
#include "module-template.h"
#include "datetime.h"
#include "net.h"
#include "glbl.h"
#include "parser.h"
#include "prop.h"
#include "errmsg.h"
#include "srUtils.h"
#include "unicode-helper.h"
#include "ratelimit.h"

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imjournal")

/* Module static data */
DEF_IMOD_STATIC_DATA
DEFobjCurrIf(datetime)
DEFobjCurrIf(glbl)
DEFobjCurrIf(parser)
DEFobjCurrIf(prop)
DEFobjCurrIf(net)
DEFobjCurrIf(errmsg)

struct modConfData_s {
};

static struct configSettings_s {
	char *stateFile;
	int iPersistStateInterval;
	int ratelimitInterval;
	int ratelimitBurst;
	int bIgnorePrevious;
	int iDfltSeverity;
	int iDfltFacility;
} cs;

static rsRetVal facilityHdlr(uchar **pp, void *pVal);

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {
	{ "statefile", eCmdHdlrGetWord, 0 },
	{ "ratelimit.interval", eCmdHdlrInt, 0 },
	{ "ratelimit.burst", eCmdHdlrInt, 0 },
	{ "persiststateinterval", eCmdHdlrInt, 0 },
	{ "ignorepreviousmessages", eCmdHdlrBinary, 0 },
	{ "defaultseverity", eCmdHdlrSeverity, 0 },
	{ "defaultfacility", eCmdHdlrString, 0 }
};
static struct cnfparamblk modpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
	  modpdescr
	};

#define DFLT_persiststateinterval 10
#define DFLT_SEVERITY pri2fac(LOG_NOTICE)
#define DFLT_FACILITY pri2sev(LOG_USER)

static int bLegacyCnfModGlobalsPermitted = 1;/* are legacy module-global config parameters permitted? */

static prop_t *pInputName = NULL;	/* there is only one global inputName for all messages generated by this module */
static prop_t *pLocalHostIP = NULL;	/* a pseudo-constant propterty for 127.0.0.1 */

static ratelimit_t *ratelimiter = NULL;
static sd_journal *j;


/* ugly workaround to handle facility numbers; values
 * derived from names need to be eight times smaller,
 * i.e.: 0..23
 */
static rsRetVal facilityHdlr(uchar **pp, void *pVal)
{
	DEFiRet;
	char *p;

	skipWhiteSpace(pp);
	p = (char *) *pp;

	if (isdigit((int) *p)) {
		*((int *) pVal) = (int) strtol(p, (char **) pp, 10);
	} else {
		int len;
		syslogName_t *c;

		for (len = 0; p[len] && !isspace((int) p[len]); len++)
			/* noop */;
		for (c = syslogFacNames; c->c_name; c++) {
			if (!strncasecmp(p, (char *) c->c_name, len)) {
				*((int *) pVal) = pri2fac(c->c_val);
				break;
			}
		}
		*pp += len;
	}

	RETiRet;
}


/* Currently just replaces '\0' with ' '. Not doing so would cause
 * the value to be truncated. New space is allocated for the resulting
 * string.
 */
static rsRetVal
sanitizeValue(const char *in, size_t len, char **out)
{
	char *buf, *p;
	DEFiRet;

	CHKmalloc(p = buf = malloc(len + 1));
	memcpy(buf, in, len);
	buf[len] = '\0';

	while ((p = memchr(p, '\0', len + buf - p)) != NULL) {
		*p++ = ' ';
	}

	*out = buf;

finalize_it:
	RETiRet;
}


/* enqueue the the journal message into the message queue.
 * The provided msg string is not freed - thus must be done
 * by the caller.
 */
static rsRetVal
enqMsg(uchar *msg, uchar *pszTag, int iFacility, int iSeverity, struct timeval *tp, struct json_object *json)
{
	struct syslogTime st;
	msg_t *pMsg;
	DEFiRet;

	assert(msg != NULL);
	assert(pszTag != NULL);

	if(tp == NULL) {
		CHKiRet(msgConstruct(&pMsg));
	} else {
		datetime.timeval2syslogTime(tp, &st);
		CHKiRet(msgConstructWithTime(&pMsg, &st, tp->tv_sec));
	}
	MsgSetFlowControlType(pMsg, eFLOWCTL_LIGHT_DELAY);
	MsgSetInputName(pMsg, pInputName);
	MsgSetRawMsgWOSize(pMsg, (char*)msg);
	parser.SanitizeMsg(pMsg);
	MsgSetMSGoffs(pMsg, 0);	/* we do not have a header... */
	MsgSetRcvFrom(pMsg, glbl.GetLocalHostNameProp());
	MsgSetRcvFromIP(pMsg, pLocalHostIP);
	MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
	MsgSetTAG(pMsg, pszTag, ustrlen(pszTag));
	pMsg->iFacility = iFacility;
	pMsg->iSeverity = iSeverity;

	if(json != NULL) {
		msgAddJSON(pMsg, (uchar*)"!", json);
	}

	CHKiRet(ratelimitAddMsg(ratelimiter, NULL, pMsg));

finalize_it:
	RETiRet;
}


/* Read journal log while data are available, each read() reads one
 * record of printk buffer.
 */
static rsRetVal
readjournal() {
	DEFiRet;

	struct timeval tv;
	uint64_t timestamp;

	struct json_object *json = NULL;
	int r;

	/* Information from messages */
	char *message = NULL;
	char *sys_iden;
	char *sys_iden_help = NULL;

	const void *get;
	const void *pidget;
	char *parse;
	size_t length;
	size_t pidlength;

	const void *equal_sign;
	struct json_object *jval;
	size_t l;

	long prefixlen = 0;

	int severity = cs.iDfltSeverity;
	int facility = cs.iDfltFacility;

	/* Get message text */
	if (sd_journal_get_data(j, "MESSAGE", &get, &length) < 0) {
		message = strdup("");
	} else {
		CHKiRet(sanitizeValue(((const char *)get) + 8, length - 8, &message));
	}

	/* Get message severity ("priority" in journald's terminology) */
	if (sd_journal_get_data(j, "PRIORITY", &get, &length) >= 0) {
		if (length == 10) {
			severity = ((char *)get)[9] - '0';
			if (severity < 0 || 7 < severity) {
				dbgprintf("The value of the 'PRIORITY' field is "
					"out of bounds: %d, resetting\n", severity);
				severity = cs.iDfltSeverity;
			}
		} else {
			dbgprintf("The value of the 'PRIORITY' field has an "
				"unexpected length: %d\n", length);
		}
	}

	/* Get syslog facility */
	if (sd_journal_get_data(j, "SYSLOG_FACILITY", &get, &length) >= 0) {
		if (length == 17 || length == 18) {
			facility = ((char *)get)[16] - '0';
			if (length == 18) {
				facility *= 10;
				facility += ((char *)get)[17] - '0';
			}
			if (facility < 0 || 23 < facility) {
				dbgprintf("The value of the 'FACILITY' field is "
					"out of bounds: %d, resetting\n", facility);
				facility = cs.iDfltFacility;
			}
		} else {
			dbgprintf("The value of the 'FACILITY' field has an "
				"unexpected length: %d\n", length);
		}
	}

	/* Get message identifier, client pid and add ':' */
	if (sd_journal_get_data(j, "SYSLOG_IDENTIFIER", &get, &length) >= 0) {
		CHKiRet(sanitizeValue(((const char *)get) + 18, length - 18, &sys_iden));
	} else {
		CHKmalloc(sys_iden = strdup("journal"));
	}

	if (sd_journal_get_data(j, "SYSLOG_PID", &pidget, &pidlength) >= 0) {
		char *sys_pid;

		CHKiRet_Hdlr(sanitizeValue(((const char *)pidget) + 11, pidlength - 11, &sys_pid)) {
			free (sys_iden);
			FINALIZE;
		}
		r = asprintf(&sys_iden_help, "%s[%s]:", sys_iden, sys_pid);
		free (sys_pid);
	} else {
		r = asprintf(&sys_iden_help, "%s:", sys_iden);
	}

	free (sys_iden);

	if (-1 == r) {
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
	}

	json = json_object_new_object();

	SD_JOURNAL_FOREACH_DATA(j, get, l) {
		char *data;
		char *name;

		/* locate equal sign, this is always present */
		equal_sign = memchr(get, '=', l);

		/* ... but we know better than to trust the specs */
		if (equal_sign == NULL) {
			errmsg.LogError(0, RS_RET_ERR, "SD_JOURNAL_FOREACH_DATA()"
				"returned a malformed field (has no '='): '%s'", (char*)get);
			continue; /* skip the entry */
		}

		/* get length of journal data prefix */
		prefixlen = ((char *)equal_sign - (char *)get);

		/* translate name fields to lumberjack names */
		parse = (char *)get;

		switch (*parse)
		{
		case '_':
			++parse;
			if (*parse == 'P') {
				if (!strncmp(parse+1, "ID=", 4)) {
					name = strdup("pid");
				} else {
					name = strndup(get, prefixlen);
				}
			} else if (*parse == 'G') {
				if (!strncmp(parse+1, "ID=", 4)) {
					name = strdup("gid");
				} else {
					name = strndup(get, prefixlen);
				}
			} else if (*parse == 'U') {
				if (!strncmp(parse+1, "ID=", 4)) {
					name = strdup("uid");
				} else {
					name = strndup(get, prefixlen);
				}
			} else if (*parse == 'E') {
				if (!strncmp(parse+1, "XE=", 4)) {
					name = strdup("exe");
				} else {
					name = strndup(get, prefixlen);
				}
			} else if (*parse == 'C') {
				parse++;
				if (*parse == 'O') {
					if (!strncmp(parse+1, "MM=", 4)) {
						name = strdup("appname");
					} else {
						name = strndup(get, prefixlen);
					}
				} else if (*parse == 'M') {
					if (!strncmp(parse+1, "DLINE=", 7)) {
						name = strdup("cmd");
					} else {
						name = strndup(get, prefixlen);
					}
				} else {
					name = strndup(get, prefixlen);
				}
			} else {
				name = strndup(get, prefixlen);
			}
			break;

		default:
			name = strndup(get, prefixlen);
			break;
		}

		CHKmalloc(name);

		prefixlen++; /* remove '=' */

		CHKiRet_Hdlr(sanitizeValue(((const char *)get) + prefixlen, l - prefixlen, &data)) {
			free (name);
			FINALIZE;
		}

		/* and save them to json object */
		jval = json_object_new_string((char *)data);
		json_object_object_add(json, name, jval);
		free (data);
		free (name);
	}

	/* calculate timestamp */
	if (sd_journal_get_realtime_usec(j, &timestamp) >= 0) {
		tv.tv_sec = timestamp / 1000000;
		tv.tv_usec = timestamp % 1000000;
	}

	/* submit message */
	enqMsg((uchar *)message, (uchar *) sys_iden_help, facility, severity, &tv, json);

finalize_it:
	if (sys_iden_help != NULL)
		free(sys_iden_help);
	if (message != NULL)
		free(message);
	RETiRet;
}


/* This function gets journal cursor and saves it into state file
 */
static rsRetVal
persistJournalState () {
	DEFiRet;
	FILE *sf; /* state file */
	char *cursor;
	int ret = 0;

	/* On success, sd_journal_get_cursor()  returns 1 in systemd
	   197 or older and 0 in systemd 198 or newer */
	if ((ret = sd_journal_get_cursor(j, &cursor)) >= 0) {
		if ((sf = fopen(cs.stateFile, "wb")) != NULL) {
			if (fprintf(sf, "%s", cursor) < 0) {
				iRet = RS_RET_IO_ERROR;
			}
			fclose(sf);
			free(cursor);
		} else {
			char errStr[256];
			rs_strerror_r(errno, errStr, sizeof(errStr));
			errmsg.LogError(0, RS_RET_FOPEN_FAILURE, "fopen() failed: "
				"'%s', path: '%s'\n", errStr, cs.stateFile);
			iRet = RS_RET_FOPEN_FAILURE;
		}
	} else {
		char errStr[256];
		rs_strerror_r(-(ret), errStr, sizeof(errStr));
		errmsg.LogError(0, RS_RET_ERR, "sd_journal_get_cursor() failed: '%s'\n", errStr);
		iRet = RS_RET_ERR;
	}
	RETiRet;
}


/* Polls the journal for new messages. Similar to sd_journal_wait()
 * except for the special handling of EINTR.
 */
static rsRetVal
pollJournal()
{
	DEFiRet;
	struct pollfd pollfd;
	int r;

	pollfd.fd = sd_journal_get_fd(j);
	pollfd.events = sd_journal_get_events(j);
	r = poll(&pollfd, 1, -1);
	if (r == -1) {
		if (errno == EINTR) {
			/* EINTR is also received during termination
			 * so return now to check the term state.
			 */
			ABORT_FINALIZE(RS_RET_OK);
		} else {
			char errStr[256];

			rs_strerror_r(errno, errStr, sizeof(errStr));
			errmsg.LogError(0, RS_RET_ERR,
				"poll() failed: '%s'", errStr);
			ABORT_FINALIZE(RS_RET_ERR);
		}
	}

	assert(r == 1);

	r = sd_journal_process(j);
	if (r < 0) {
		char errStr[256];

		rs_strerror_r(errno, errStr, sizeof(errStr));
		errmsg.LogError(0, RS_RET_ERR,
			"sd_journal_process() failed: '%s'", errStr);
		ABORT_FINALIZE(RS_RET_ERR);
	}

finalize_it:
	RETiRet;
}


static rsRetVal
skipOldMessages() {
	DEFiRet;

	if (sd_journal_seek_tail(j) < 0) {
		char errStr[256];

		rs_strerror_r(errno, errStr, sizeof(errStr));
		errmsg.LogError(0, RS_RET_ERR,
			"sd_journal_seek_tail() failed: '%s'", errStr);
		ABORT_FINALIZE(RS_RET_ERR);
	}
	if (sd_journal_previous(j) < 0) {
		char errStr[256];

		rs_strerror_r(errno, errStr, sizeof(errStr));
		errmsg.LogError(0, RS_RET_ERR,
			"sd_journal_previous() failed: '%s'", errStr);
		ABORT_FINALIZE(RS_RET_ERR);
	}

finalize_it:
	RETiRet;
}


/* This function loads a journal cursor from the state file.
 */
static rsRetVal
loadJournalState()
{
	DEFiRet;

	if (cs.stateFile[0] != '/') {
		char *new_stateFile;

		if (-1 == asprintf(&new_stateFile, "%s/%s", (char *)glbl.GetWorkDir(), cs.stateFile)) {
			errmsg.LogError(0, RS_RET_OUT_OF_MEMORY, "imjournal: asprintf failed\n");
			ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
		}
		free (cs.stateFile);
		cs.stateFile = new_stateFile;
	}

	/* if state file exists, set cursor to appropriate position */
	if (access(cs.stateFile, F_OK|R_OK) != -1) {
		FILE *r_sf;

		if ((r_sf = fopen(cs.stateFile, "rb")) != NULL) {
			char readCursor[128 + 1];

			if (fscanf(r_sf, "%128s\n", readCursor) != EOF) {
				if (sd_journal_seek_cursor(j, readCursor) != 0) {
					errmsg.LogError(0, RS_RET_ERR, "imjournal: "
						"couldn't seek to cursor `%s'\n", readCursor);
					iRet = RS_RET_ERR;
					goto finalize_it;
				}
				sd_journal_next(j);
			} else {
				errmsg.LogError(0, RS_RET_IO_ERROR, "imjournal: "
					"fscanf on state file `%s' failed\n", cs.stateFile);
				iRet = RS_RET_IO_ERROR;
				goto finalize_it;
			}
			fclose(r_sf);
		} else {
			errmsg.LogError(0, RS_RET_FOPEN_FAILURE, "imjournal: "
					"open on state file `%s' failed\n", cs.stateFile);
		}
	} else if (cs.bIgnorePrevious) {
		/* Seek to the very end of the journal and ignore all
		 * older messages. */
		skipOldMessages();
	} 

finalize_it:
	RETiRet;
}

BEGINrunInput
CODESTARTrunInput
	CHKiRet(ratelimitNew(&ratelimiter, "imjournal", NULL));
	dbgprintf("imjournal: ratelimiting burst %d, interval %d\n", cs.ratelimitBurst,
		  cs.ratelimitInterval);
	ratelimitSetLinuxLike(ratelimiter, cs.ratelimitInterval, cs.ratelimitBurst);
	ratelimitSetNoTimeCache(ratelimiter);

	if (cs.stateFile) {
		/* Load our position in the journal from the state file. */
		CHKiRet(loadJournalState());
	} else if (cs.bIgnorePrevious) {
		/* Seek to the very end of the journal and ignore all
		 * older messages. */
		skipOldMessages();
	}

	/* this is an endless loop - it is terminated when the thread is
	 * signalled to do so. This, however, is handled by the framework.
	 */
	while (glbl.GetGlobalInputTermState() == 0) {
		int count = 0, r;

		r = sd_journal_next(j);
		if (r < 0) {
			char errStr[256];

			rs_strerror_r(errno, errStr, sizeof(errStr));
			errmsg.LogError(0, RS_RET_ERR,
				"sd_journal_next() failed: '%s'", errStr);
			ABORT_FINALIZE(RS_RET_ERR);
		}

		if (r == 0) {
			/* No new messages, wait for activity. */
			CHKiRet(pollJournal());
			continue;
		}

		CHKiRet(readjournal());
		if (cs.stateFile) { /* can't persist without a state file */
			/* TODO: This could use some finer metric. */
			count++;
			if (count == cs.iPersistStateInterval) {
				count = 0;
				persistJournalState();
			}
		}
	}

finalize_it:
ENDrunInput


BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	bLegacyCnfModGlobalsPermitted = 1;

	cs.iPersistStateInterval = DFLT_persiststateinterval;
	cs.stateFile = NULL;
	cs.ratelimitBurst = 20000;
	cs.ratelimitInterval = 600;
	cs.iDfltSeverity = DFLT_SEVERITY;
	cs.iDfltFacility = DFLT_FACILITY;
ENDbeginCnfLoad


BEGINendCnfLoad
CODESTARTendCnfLoad
ENDendCnfLoad


BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf


BEGINactivateCnf
CODESTARTactivateCnf
ENDactivateCnf


BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf

/* open journal */
BEGINwillRun
CODESTARTwillRun
	int ret;
	ret = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
	if (ret < 0) {
		iRet = RS_RET_IO_ERROR;
	}
ENDwillRun

/* close journal */
BEGINafterRun
CODESTARTafterRun
	if (cs.stateFile) { /* can't persist without a state file */
		persistJournalState();
	}
	sd_journal_close(j);
	ratelimitDestruct(ratelimiter);
ENDafterRun


BEGINmodExit
CODESTARTmodExit
	if(pInputName != NULL)
		prop.Destruct(&pInputName);
	if(pLocalHostIP != NULL)
		prop.Destruct(&pLocalHostIP);

	/* release objects we used */
	objRelease(glbl, CORE_COMPONENT);
	objRelease(net, CORE_COMPONENT);
	objRelease(datetime, CORE_COMPONENT);
	objRelease(parser, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit


BEGINsetModCnf
	struct cnfparamvals *pvals = NULL;
	int i;
CODESTARTsetModCnf
	pvals = nvlstGetParams(lst, &modpblk, NULL);
	if (pvals == NULL) {
		errmsg.LogError(0, RS_RET_MISSING_CNFPARAMS, "error processing module "
				"config parameters [module(...)]");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	if (Debug) {
		dbgprintf("module (global) param blk for imjournal:\n");
		cnfparamsPrint(&modpblk, pvals);
	}

	for (i = 0 ; i < modpblk.nParams ; ++i) {
		if (!pvals[i].bUsed)
			continue;
		if (!strcmp(modpblk.descr[i].name, "persiststateinterval")) {
			cs.iPersistStateInterval = (int) pvals[i].val.d.n;
		} else if (!strcmp(modpblk.descr[i].name, "statefile")) {
			cs.stateFile = (char *)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(modpblk.descr[i].name, "ratelimit.burst")) {
			cs.ratelimitBurst = (int) pvals[i].val.d.n;
		} else if(!strcmp(modpblk.descr[i].name, "ratelimit.interval")) {
			cs.ratelimitInterval = (int) pvals[i].val.d.n;
		} else if (!strcmp(modpblk.descr[i].name, "ignorepreviousmessages")) {
			cs.bIgnorePrevious = (int) pvals[i].val.d.n; 
		} else if (!strcmp(modpblk.descr[i].name, "defaultseverity")) {
			cs.iDfltSeverity = (int) pvals[i].val.d.n;
		} else if (!strcmp(modpblk.descr[i].name, "defaultfacility")) {
			/* ugly workaround to handle facility numbers; values
			   derived from names need to be eight times smaller */

			char *fac, *p;

			fac = p = es_str2cstr(pvals[i].val.d.estr, NULL);
			facilityHdlr((uchar **) &p, (void *) &cs.iDfltFacility);
			free(fac);
		} else {
			dbgprintf("imjournal: program error, non-handled "
				"param '%s' in beginCnfLoad\n", modpblk.descr[i].name);
		}
	}


finalize_it:
	if (pvals != NULL)
		cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(parser, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));
	CHKiRet(objUse(net, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));

	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.CreateStringProp(&pInputName, UCHAR_CONSTANT("imjournal"), sizeof("imjournal") - 1));
	CHKiRet(prop.CreateStringProp(&pLocalHostIP, UCHAR_CONSTANT("127.0.0.1"), sizeof("127.0.0.1") - 1));

	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournalpersiststateinterval", 0, eCmdHdlrInt,
		NULL, &cs.iPersistStateInterval, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournalratelimitinterval", 0, eCmdHdlrInt,
		NULL, &cs.ratelimitInterval, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournalratelimitburst", 0, eCmdHdlrInt,
		NULL, &cs.ratelimitBurst, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournalstatefile", 0, eCmdHdlrGetWord,
		NULL, &cs.stateFile, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournalignorepreviousmessages", 0, eCmdHdlrBinary,
		NULL, &cs.bIgnorePrevious, STD_LOADABLE_MODULE_ID)); 
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournaldefaultseverity", 0, eCmdHdlrSeverity,
		NULL, &cs.iDfltSeverity, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"imjournaldefaultfacility", 0, eCmdHdlrCustomHandler,
		facilityHdlr, &cs.iDfltFacility, STD_LOADABLE_MODULE_ID));


ENDmodInit
/* vim:set ai:
 */
