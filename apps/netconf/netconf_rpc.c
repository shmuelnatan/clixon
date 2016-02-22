/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLICON.

  CLICON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLICON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLICON; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>.

 *
 *  Code for handling netconf rpc messages
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <assert.h>
#include <grp.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clicon/clicon.h>

#include "clicon_netconf.h"
#include "netconf_lib.h"
#include "netconf_filter.h"
#include "netconf_plugin.h"
#include "netconf_rpc.h"

/*
 * <rpc [attributes]> 
    <!- - tag elements in a request from a client application - -> 
    </rpc> 
 */

/* get-config help function
 * xfilter is a filter expression starting with <filter>
 * only <filter type="xpath"/> supported
 * needs refactoring: move the lower part (after xfilter) up to get-config or 
 * sub-function., focus on xfilter part here.
 * @param[in]  h       Clicon handle
 * @param[in]  xfilter Parsed xpath expression
 * @param[out] cb      Output xml stream. For reply
 * @param[out] cb_err  Error xml stream. For error reply
 * @param[in]  xorig   Original request as xml tree.
 * @param[in]  target  Database name
 * @see netconf_get_config
 * Variants of the functions where x-axis is the variants of the <filter> clause
 * and y-axis is whether a <filter><configuration> or <filter select=""> is present.
 *                  | no filter | filter subnet | filter xpath |
 * -----------------+-----------+---------------+--------------+
 * no config        |           |               |              |
 * -----------------+-----------+---------------+--------------+
 * config/select    |     -     |               |              |
 * -----------------+-----------+---------------+--------------+
 * Example requests of each:
 * no filter + no config
     <rpc><get-config><source><candidate/></source></get-config></rpc>]]>]]>
 * filter subnet + no config:
     <rpc><get-config><source><candidate/></source><filter/></get-config></rpc>]]>]]>
 * filter xpath + no select:
     <rpc><get-config><source><candidate/></source><filter type="xpath"/></get-config></rpc>]]>]]>
 * filter subnet + config:
     <rpc><get-config><source><candidate/></source><filter type="subtree"><configuration><interfaces><interface><ipv4/></interface></interfaces></configuration></filter></get-config></rpc>]]>]]>
 * filter xpath + select:
     <rpc><get-config><source><candidate/></source><filter type="xpath" select="/interfaces/interface/ipv4"/></get-config></rpc>]]>]]>
 */

/*! Variant for xmldb */
static int
netconf_filter_xmldb(clicon_handle      h, 
		     cxobj             *xfilter, 
		     enum filter_option foption,
		     cbuf              *cb, 
		     cbuf              *cb_err, 
		     cxobj             *xorig, 
		     char              *source)
{
    cxobj      *xdb = NULL; 
    cxobj      *xc; 
    cxobj      *xfilterconf = NULL; 
    int         retval = -1;
    yang_spec  *ys = clicon_dbspec_yang(h);
    char       *selector;

    /* Default subtree filter */
    switch (foption){
    case FILTER_SUBTREE:
	/* Get the whole database as xml */
	if (xmldb_get(source, NULL, ys, &xdb) < 0){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "application", 
				     "error", 
				     NULL,
				     "read-registry");
	    goto done;
	}

	/* Add <configuration> under <top> as reply */
	if ((xc = xml_insert(xdb, "configuration")) == NULL){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "application", 
				     "error", 
				     NULL,
				     "filtering");
	    goto done;
	}
	if (xfilter)
	    xfilterconf = xpath_first(xfilter, "//configuration");
	/* xml_filter removes parts of xml tree not matching */
	if (xfilterconf != NULL){ 
	    if (xml_filter(xfilterconf, xc) < 0){
		netconf_create_rpc_error(cb_err, xorig, 
					 "operation-failed", 
					 "application", 
					 "error", 
					 NULL,
					 "filtering");
		goto done;
	    }
	}
	if (xml_child_nr(xc))
	    clicon_xml2cbuf(cb, xc, 0, 1);
	break;
    case FILTER_XPATH:
	selector = xml_find_value(xfilter, "select");
	if (xmldb_get(source, selector, ys, &xdb) < 0){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "application", 
				     "error", 
				     NULL,
				     "read-registry");
	    goto done;
	}
	if ((xc = xml_insert(xdb, "configuration")) == NULL){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "application", 
				     "error", 
				     NULL,
				     "filtering");
	    goto done;
	}
	if (xml_child_nr(xc))
	    clicon_xml2cbuf(cb, xc, 0, 1);
	break;
    default:
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "application", 
				 "error", 
				 "filter type not supported",
				 "type");
	goto done;
    }
    retval = 0;
  done:
    if (xdb)
	xml_free(xdb);
    return retval; 
}


/*! Get configuration
 * @param[in]  h       Clicon handle
 * @param[in]  xorig      Sub-tree (under xorig) at <get-config>...</> level.
 * @param[out] cb      Output xml stream. For reply
 * @param[out] cb_err  Error xml stream. For error reply
 * @param[in]  xorig      Original request as xml tree.
 * @note Only filter type="xpath" is supported

    <get-config> 
        <source> 
            <( candidate | running )/> 
        </source> 
    </get-config> 
    
    <get-config> 
        <source> 
            <( candidate | running )/> 
        </source> 
        <filter type="subtree"> 
            <configuration> 
                <!- - tag elements for each configuration element to return - -> 
            </configuration> 
        </filter> 
    </get-config> 
 Example:
   <rpc><get-config><source><running /></source>
     <filter type="xpath" select="//SenderTwampIpv4"/>
   </get-config></rpc>]]>]]>
 * Call graph, client to backend and formats
 * netconf_input_cb                      # client
 *   read
 *   process_incoming_packet             # 
 *     clicon_xml_parse_string           # 
 *     netconf_rpc_dispatch
 *       netconf_get_config              # 
 *         xpath_first
 *         netconf_filter                #
 *           db2xml_key                  # "^.*$"
 *             clicon_dbitems            # (db)
 *               db_regexp               # (lvec)
 *               lvec2cvec               # (cvec)
 *             cvec2xml                  # (xml)
 *           clicon_xml2cbuf             # (xml->char*)
 * wanted:
 *     netconf_get_config
 *       xpath(db, filter)
 */
int
netconf_get_config(clicon_handle h, 
		   cxobj        *xn, 
		   cbuf         *cb, 
		   cbuf         *cb_err, 
		   cxobj        *xorig)
{
    cxobj *xfilter; /* filter */
    int    retval = -1;
    char  *source;
    enum filter_option foption = FILTER_SUBTREE;
    char       *ftype = NULL;

    if ((source = netconf_get_target(h, xn, "source")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>source</bad-element>");
	goto done;
    }
    /* ie <filter>...</filter> */
    if ((xfilter = xpath_first(xn, "filter")) != NULL) {
	if ((ftype = xml_find_value(xfilter, "type")) != NULL)
	    if (strcmp(ftype, "xpath")==0)
		foption = FILTER_XPATH;
    }
    if (netconf_filter_xmldb(h, xfilter, foption, cb, cb_err, xorig, source) < 0)
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Get options from netconf edit-config
 *  <edit-config>
 *     <config>XXX</config>
 *     <default-operation>(merge | none | replace)</default-operation> 
 *     <error-option>(stop-on-error | continue-on-error )</error-option> 
 *     <test-option>(set | test-then-set | test-only)</test-option> 
 *  </edit-config>
 * 
 *
 */
static int
get_edit_opts(cxobj               *xn,
	      enum operation_type *op, 
	      enum test_option    *testopt,
	      enum error_option   *erropt,
	      cbuf                *cb_err, 
	      cxobj               *xorig) 
{
    int    retval = -1;
    cxobj *x;
    char  *optstr;

    if ((x = xpath_first(xn, "default-operation")) != NULL){
	if ((optstr = xml_body(x)) != NULL){
	    if (strcmp(optstr, "replace") == 0)
		*op = OP_REPLACE;
	    else
		if (strcmp(optstr, "merge") == 0)
		    *op = OP_MERGE;
		else
		    if (strcmp(optstr, "none") == 0)
			*op = OP_NONE;
		    else{
			netconf_create_rpc_error(cb_err, xorig, 
						 "invalid-value", 
						 "protocol", 
						 "error", 
						 NULL,
						 NULL);
			goto done;
		    }
	}
    }
    if ((x = xpath_first(xn, "test-option")) != NULL){
	if ((optstr = xml_body(x)) != NULL){
	    if (strcmp(optstr, "test-then-set") == 0)
		*testopt = TEST_THEN_SET;
	    else
	    if (strcmp(optstr, "set") == 0)
		*testopt = SET;
	    else
	    if (strcmp(optstr, "test-only") == 0)
		*testopt = TEST_ONLY;
	    else{
		netconf_create_rpc_error(cb_err, xorig, 
					 "invalid-value", 
					 "protocol", 
					 "error", 
					 NULL,
					 NULL);
		goto done;
	    }
	}
    }
    if ((x = xpath_first(xn, "error-option")) != NULL){
	if ((optstr = xml_body(x)) != NULL){
	    if (strcmp(optstr, "stop-on-error") == 0)
		*erropt = STOP_ON_ERROR;
	    else
	    if (strcmp(optstr, "continue-on-error") == 0)
		*erropt = CONTINUE_ON_ERROR;
	    else{
		netconf_create_rpc_error(cb_err, xorig, 
					 "invalid-value", 
					 "protocol", 
					 "error", 
					 NULL,
					 NULL);
		goto done;
	    }
	}
    }
    retval = 0;
  done:
   return retval;
}


/*! Netconf edit configuration
  Write the change on a tmp file, then load that into candidate configuration.
    <edit-config> 
        <target> 
            <candidate/> 
        </target> 
    
    <!- - EITHER - -> 

        <config> 
            <configuration> 
                <!- - tag elements representing the data to incorporate - -> 
            </configuration> 
        </config> 

    <!- - OR - -> 
    
        <config-text> 
            <configuration-text> 
                <!- - tag elements inline configuration data in text format - -> 
            </configuration-text> 
        </config-text> 

    <!- - OR - -> 

        <url> 
            <!- - location specifier for file containing data - -> 
        </url> 
    
        <default-operation>(merge | none | replace)</default-operation> 
        <error-option>(stop-on-error | continue-on-error )</error-option> 
        <test-option>(set | test-then-set | test-only)</test-option> 
    <edit-config> 
 *
 * only 'config' supported
 * error-option: only stop-on-error supported
 * test-option:  not supported
 *
 * @note delete/remove not implemented
 *
 * Call graph, client to backend and formats
 * netconf_input_cb # client
 *   read
 *   process_incoming_packet             # (char*)
 *     clicon_xml_parse_string           # (xml)
 *     netconf_rpc_dispatch
 *       netconf_edit_config             # client
 *         clicon_xml2file               # (file)
 *         clicon_rpc_load
 *           from_client                 # backend
 *             from_client_load
 *               db_init                 # replace?
 *               load_xml_to_db
 *                 clicon_xml_parse_file #(xml)
 *                 xml2db
 *                   xml2cvec_key        #(cvec)
 *                   clicon_dbput
 *                     cvec2lvec         #(lvec)
 *                     db_set
 *                       dpopen
 *                       dpput           #(db)
 *                       dpclose
 * Which means that for a single edit, the following format conversions are made:
 * char*->xml->file->xml->cvec->lvec->db
 *         
 */
int
netconf_edit_config(clicon_handle h,
		    cxobj        *xn, 
		    cbuf         *cb, 
		    cbuf         *cb_err, 
		    cxobj        *xorig)
{
    int                 retval = -1;
    int                 ret;
    enum operation_type operation = OP_MERGE;
    enum test_option    testopt = TEST_THEN_SET;
    enum error_option   erropt = STOP_ON_ERROR;
    cxobj              *xc;       /* config */
    cxobj              *xcc;      /* child of config */
    char               *target;  /* db */
    char               *candidate_db;
    cbuf               *cbxml = NULL;
    char               *xmlstr;

    if ((candidate_db = clicon_candidate_db(h)) == NULL){
	   netconf_create_rpc_error(cb_err, xorig, 
				    "operation-failed", 
				    "protocol", "error", 
				    NULL, "Internal error"); 
	goto done;
    }
    /* must have target, and it should be candidate */
    if ((target = netconf_get_target(h, xn, "target")) == NULL ||
	strcmp(target, candidate_db)){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>target</bad-element>");
	goto done;
    }
    if (get_edit_opts(xn, &operation, &testopt, &erropt, cb_err, xorig) < 0)
	goto done;
    /* operation is OP_REPLACE, OP_MERGE, or OP_NONE pass all to backend */
    if ((xc  = xpath_first(xn, "config")) != NULL){
	/* application-specific code registers 'config' */
	if ((ret = netconf_plugin_callbacks(h, xc, cb, cb_err, xorig)) < 0){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "protocol", "error", 
				     NULL, "Validation"); 
	    goto done;
	}
	if ((cbxml = cbuf_new()) == NULL)
	    goto done;
	if ((xcc = xml_child_i(xc, 0)) != NULL)
	    if (clicon_xml2cbuf(cbxml, xcc, 0, 0) < 0)
		goto done;
	xmlstr = cbuf_get(cbxml);
	if (clicon_rpc_xmlput(h, target, 
			      operation,
			      xmlstr) < 0){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "access-denied", 
				     "protocol", 
				     "error", 
				     NULL,
				     "edit_config");
	    goto done;
	}
    }
    netconf_ok_set(1);
    retval = 0;
  done:
    if (cbxml)
	cbuf_free(cbxml);
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
    <copy-config> 
        <target> 
            <candidate/> 
        </target> 
        <source> 
            <url> 
                <!- - location specifier for file containing the new configuration - -> 
            </url> 
        </source> 
    <copy-config> 
 */
int
netconf_copy_config(clicon_handle h,
		    cxobj        *xn, 
		    cbuf         *cb, 
		    cbuf         *cb_err, 
		    cxobj        *xorig)
{
    char              *source, *target; /* filenames */
    int                retval = -1;

    if ((source = netconf_get_target(h, xn, "source")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>source</bad-element>");
	goto done;
    }
    if ((target = netconf_get_target(h, xn, "target")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>source</bad-element>");
	goto done;
    }

#ifdef notyet
    /* If target is locked and not by this client, then return an error */
    if (target_locked(&client) > 0 && client != ce_nr){
	netconf_create_rpc_error(cb_err, xorig, 
				 "access-denied", 
				 "protocol", 
				 "error", 
				 NULL,
				 "lock");
	goto done;
    }
#endif
    if (clicon_rpc_copy(h, source, target) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error"); 
	goto done;
    }
    netconf_ok_set(1);
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
  <delete-config> 
        <target> 
            <candidate/> 
        </target> 
    </delete-config> 
    Delete a configuration datastore.  The <running>
    configuration datastore cannot be deleted.
 */
int
netconf_delete_config(clicon_handle h,
		      cxobj        *xn, 
		      cbuf         *cb, 
		      cbuf         *cb_err, 
		      cxobj        *xorig)
{
    char              *target; /* filenames */
    int                retval = -1;
    char              *candidate_db;

    if ((candidate_db = clicon_candidate_db(h)) == NULL){
	   netconf_create_rpc_error(cb_err, xorig, 
				    "operation-failed", 
				    "protocol", "error", 
				    NULL, "Internal error"); 
	goto done;
    }

    if ((target = netconf_get_target(h, xn, "target")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>target</bad-element>");
	goto done;
    }
    if (strcmp(target, candidate_db)){
	netconf_create_rpc_error(cb_err, xorig, 
				 "bad-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>target</bad-element>");
	goto done;
    }
    if (clicon_rpc_rm(h, target) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error"); 
	goto done;
    }
    if (clicon_rpc_initdb(h, target) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error"); 
	goto done;
    }
    netconf_ok_set(1);
    retval = 0;
  done:
    return retval;
}

/*
    <close-session/> 
*/
int
netconf_close_session(cxobj *xn, 
		      cbuf  *cb, 
		      cbuf  *cb_err, 
		      cxobj *xorig)
{
    cc_closed++;
    netconf_ok_set(1);
    return 0;
}

/*
    <lock> 
        <target> 
            <candidate/> 
        </target> 
    </lock> 
 */
int
netconf_lock(clicon_handle h,
	     cxobj        *xn, 
	     cbuf         *cb, 
	     cbuf         *cb_err, 
	     cxobj        *xorig)
{
    char *target;
    int retval = -1;

    if ((target = netconf_get_target(h, xn, "target")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>source</bad-element>");
	goto done;
    }
#ifdef notyet
    if (target_locked(&client) > 0){
	snprintf(info, 64, "<session-id>%d</session-id>", client);
	netconf_create_rpc_error(cb_err, xorig, 
				 "lock-denied", 
				 "protocol", 
				 "error", 
				 "Lock failed, lock is already held",
				 info);
	return -1;
    }

    if (lock_target(target) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "lock-denied", 
				 "protocol", 
				 "error", 
				 "Lock failed, lock is already held",
				 NULL);
	return -1;
    }
#endif
    netconf_ok_set(1);
    retval = 0;
  done:
    return retval;
}

/*
   <unlock> 
        <target> 
            <candidate/> 
        </target> 
    </unlock> 
 */
int
netconf_unlock(clicon_handle h, 
	       cxobj *xn, 
	       cbuf *cb, cbuf *cb_err, 
	       cxobj *xorig)
{
    char *target;
    int retval = -1;

    if ((target = netconf_get_target(h, xn, "target")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>target</bad-element>");
	goto done;
    }
#ifdef notyet
    if (target_locked(&client) == 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "lock-denied", 
				 "protocol", 
				 "error", 
				 "Unlock failed, lock is not held",
				 NULL);
	return -1;
    }

    if (client != ce_nr){
	snprintf(info, 64, "<session-id>%d</session-id>", client);
	netconf_create_rpc_error(cb_err, xorig, 
				 "lock-denied", 
				 "protocol", 
				 "error", 
				 "Unlock failed, lock is held by other entity",
				 info);
	return -1;
    }
    if (unlock_target(target) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "lock-denied", 
				 "protocol", 
				 "error", 
				 "Unlock failed, unkown reason",
				 NULL);
	return -1;
    }
#endif /* notyet */
    netconf_ok_set(1);
    retval = 0;
  done:
    return retval;
}

/*
  <kill-session> 
        <session-id>PID</session-id> 
  </kill-session> 
 */
int
netconf_kill_session(cxobj *xn, cbuf *cb, cbuf *cb_err, cxobj *xorig)
{
#ifdef notyet
    cxobj *xsessionid;
    char *str, *ep;
    long  i;

    if ((xsessionid = xpath_first(xn, "//session-id")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>session-id</bad-element>");
	return -1;
    }
    if (xml_find_value(xsessionid, "body") == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>session-id</bad-element>");
	return -1;
    }
    if ((str = xml_find_value(xsessionid, "body")) != NULL){
	i = strtol(str, &ep, 0);
	if ((i == LONG_MIN || i == LONG_MAX) && errno)
	    return -1;
	if ((ce = find_ce_bynr(i)) == NULL){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "protocol", 
				     "error", 
				     NULL,
				     "No such client");
	    return -1;
	}
    }
//    ce_change_state(ce, CS_CLOSED, "Received close-session msg");
    netconf_ok_set(1);
#endif
    return 0;
}

/*
    <commit/> 
    :candidate
 */
int
netconf_commit(clicon_handle h,
	       cxobj        *xn, 
	       cbuf         *cb, 
	       cbuf         *cb_err, 
	       cxobj        *xorig)
{
    int                retval = -1;
    char              *candidate_db;
    char              *running_db;

    if ((candidate_db = clicon_candidate_db(h)) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error: candidate not set"); 
	goto done;
    }
    if ((running_db = clicon_running_db(h)) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error: running not set"); 
	goto done;
    }
    if (clicon_rpc_commit(h, candidate_db,
			  running_db, 
			  1, 1) < 0){
	   netconf_create_rpc_error(cb_err, xorig, 
				    "operation-failed", 
				    "protocol", "error", 
				    NULL, "Internal error"); 
	goto done;
    }
    netconf_ok_set(1);
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
    <discard-changes/> 
    :candidate
 */
int
netconf_discard_changes(clicon_handle h,
			cxobj        *xn, 
			cbuf         *cb, 
			cbuf         *cb_err, 
			cxobj        *xorig)
{
    int                retval = -1;
    char              *running_db;
    char              *candidate_db;

    if ((running_db = clicon_running_db(h)) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error: running not set"); 
	goto done;
    }
    if ((candidate_db = clicon_candidate_db(h)) == NULL){
	   netconf_create_rpc_error(cb_err, xorig, 
				    "operation-failed", 
				    "protocol", "error", 
				    NULL, "Internal error"); 
	goto done;
    }
    if (clicon_rpc_copy(h, running_db, candidate_db) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "protocol", "error", 
				 NULL, "Internal error"); 
	goto done;
    }
    netconf_ok_set(1);
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
    <validate/> 
    :validate
 */
int
netconf_validate(clicon_handle h, 
		 cxobj        *xn, 
		 cbuf         *cb, 
		 cbuf         *cb_err, 
		 cxobj        *xorig)
{
    char *target;
    int retval = -1;

    if ((target = netconf_get_target(h, xn, "source")) == NULL){
	netconf_create_rpc_error(cb_err, xorig, 
				 "missing-element", 
				 "protocol", 
				 "error", 
				 NULL,
				 "<bad-element>target</bad-element>");
	goto done;
    }
    /* Do validation */
    netconf_ok_set(1);
    retval = 0;
  done:
    return retval;
}

/*
 * netconf_notification_cb
 * Called when a notification has happened on backend
 * and this session has registered for that event.
 * Filter it and forward it.
   <notification>
      <eventTime>2007-07-08T00:01:00Z</eventTime>
      <event xmlns="http://example.com/event/1.0">
         <eventClass>fault</eventClass>
         <reportingEntity>
             <card>Ethernet0</card>
         </reportingEntity>
         <severity>major</severity>
      </event>
   </notification>
 */
static int
netconf_notification_cb(int   s, 
			void *arg)
{
    cxobj             *xfilter = (cxobj *)arg; 
    char              *selector;
    struct clicon_msg *reply;
    int                eof;
    char              *event = NULL;
    int                level;
    int                retval = -1;
    cbuf              *cb;
    cxobj             *xe = NULL; /* event xml */
    enum clicon_msg_type type;

    if (0){
	fprintf(stderr, "%s\n", __FUNCTION__); /* debug */
	clicon_xml2file(stderr, xfilter, 0, 1); /* debug */
    }
    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof, __FUNCTION__) < 0)
	goto done;
    /* handle close from remote end: this will exit the client */
    if (eof){
	clicon_err(OE_PROTO, ESHUTDOWN, "%s: Socket unexpected close", __FUNCTION__);
	close(s);
	errno = ESHUTDOWN;
	event_unreg_fd(s, netconf_notification_cb);
	if (xfilter)
	    xml_free(xfilter);
	goto done;
    }
    /* multiplex on message type: we only expect notify */
    type = ntohs(reply->op_type);
    switch (type){
    case CLICON_MSG_NOTIFY:
	if (clicon_msg_notify_decode(reply, &level, &event, __FUNCTION__) < 0) 
	    goto done;
	/* parse event */
	if (0){ /* XXX CLICON events are not xml */
	    if (clicon_xml_parse_string(&event, &xe) < 0)
		goto done;
	    /* find and apply filter */
	    if ((selector = xml_find_value(xfilter, "select")) == NULL)
		goto done;
	    if (xpath_first(xe, selector) == NULL) {
		fprintf(stderr, "%s no match\n", __FUNCTION__); /* debug */
		break;
	    }
	}
	/* create netconf message */
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_PLUGIN, errno, "%s: cbuf_new", __FUNCTION__);
	    goto done;
	}
	add_preamble(cb); /* Make it well-formed netconf xml */
	cprintf(cb, "<notification>"); 
	cprintf(cb, "<event>"); 
	cprintf(cb, "%s", event); 
	cprintf(cb, "</event>"); 
	cprintf(cb, "</notification>"); 
	add_postamble(cb);
	/* Send it to listening client on stdout */
	if (netconf_output(1, cb, "notification") < 0){
	    cbuf_free(cb);
	    goto done;
	}
	cbuf_free(cb);
	break;
    default:
	clicon_err(OE_PROTO, 0, "%s: unexpected reply: %d", 
		__FUNCTION__, type);
	goto done;
	break;
    }
    retval = 0;
  done:
    if (xe != NULL)
	xml_free(xe);
//    if (event)
//	free(event);
    unchunk_group(__FUNCTION__);
    return retval;
}

/*

    <create-subscription> 
       <stream>RESULT</stream> # If not present, events in the default NETCONF stream will be sent.
       <filter>XPATH-EXPR<(filter>
       <startTime/> # only for replay (NYI)
       <stopTime/>  # only for replay (NYI)
    </create-subscription> 
    Dont support replay
 */
static int
netconf_create_subscription(clicon_handle h, 
			    cxobj        *xn, 
			    cbuf         *cb, 
			    cbuf         *cb_err, 
			    cxobj        *xorig)
{
    cxobj           *xstream;
    cxobj           *xfilter; 
    cxobj           *xfilter2 = NULL; 
    char            *stream = NULL;
    int              s;
    char            *ftype;
    int              retval = -1;

    if ((xstream = xpath_first(xn, "//stream")) != NULL)
	stream = xml_find_value(xstream, "body");
    if (stream == NULL)
	stream = "NETCONF";
    if ((xfilter = xpath_first(xn, "//filter")) != NULL){
	if ((ftype = xml_find_value(xfilter, "type")) != NULL){
	    if (strcmp(ftype, "xpath") != 0){
		netconf_create_rpc_error(cb_err, xorig, 
					 "operation-failed", 
					 "application", 
					 "error", 
					 "only xpath filter type supported",
					 "type");
		goto done;
	    }
	}
	/* xfilter2 is applied to netconf_notification_cb below 
	   and is only freed on exit since no unreg is made.
	*/
	if ((xfilter2 = xml_dup(xfilter)) == NULL){
	    netconf_create_rpc_error(cb_err, xorig, 
				     "operation-failed", 
				     "application", 
				     "error", 
				     NULL,
				     "Internal server error");
	    goto done;
	}

    }

    if (clicon_rpc_subscription(h, 1, stream, MSG_NOTIFY_XML, "", &s) < 0){
	netconf_create_rpc_error(cb_err, xorig, 
				 "operation-failed", 
				 "application", 
				 "error", 
				 NULL,
				 "Internal protocol error");
	goto done;
    }
    if (event_reg_fd(s, netconf_notification_cb, xfilter2, "notification socket") < 0)
	goto done;
    netconf_ok_set(1);
    retval = 0;
  done:
    return retval;
}


/*! The central netconf rpc dispatcher. Look at first tag and dispach to sub-functions.
 * Call plugin handler if tag not found. If not handled by any handler, return
 * error.
 * @param[in]  h       clicon handle
 * @param[in]  xorig   Original request as xml tree.
 * @param[in]  xn      Sub-tree (under xorig) at <rpc>...</rpc> level.
 * @param[out] cb      Output xml stream. For reply
 * @param[out] cb_err  Error xml stream. For error reply
 */
int
netconf_rpc_dispatch(clicon_handle h,
		     cxobj        *xorig, 
		     cxobj        *xn, 
		     cbuf         *cb, 
		     cbuf         *cb_err)
{
    cxobj      *xe;
    int         ret = 0;
    
    xe = NULL;
    while ((xe = xml_child_each(xn, xe, CX_ELMNT)) != NULL) {
       if (strcmp(xml_name(xe), "close-session") == 0)
	   return netconf_close_session(xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "get-config") == 0)
	   return netconf_get_config(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "edit-config") == 0)
	   return netconf_edit_config(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "copy-config") == 0)
	   return netconf_copy_config(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "delete-config") == 0)
	   return netconf_delete_config(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "kill-session") == 0) /* TBD */
	   return netconf_kill_session(xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "lock") == 0) /* TBD */
	   return netconf_lock(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "unlock") == 0) /* TBD */
	   return netconf_unlock(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "commit") == 0)
	   return netconf_commit(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "discard-changes") == 0)
	   return netconf_discard_changes(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "validate") == 0)
	   return netconf_validate(h, xe, cb, cb_err, xorig);
       else
	   if (strcmp(xml_name(xe), "create-subscription") == 0)
	   return netconf_create_subscription(h, 
					      xe, cb, cb_err, xorig);
       else{
	   if ((ret = netconf_plugin_callbacks(h, 
					       xe, cb, cb_err, xorig)) < 0)
	       return -1;
	   if (ret == 0){ /* not handled by callback */
	       netconf_create_rpc_error(cb_err, xorig, 
					"operation-failed", 
					"rpc", "error", 
					xml_name(xe), "Not recognized"); 
	       ret = -1;
	   }
	   
       }
   }
    return ret;
}

/*
 * netconf_create_rpc_reply
 * Create an rpc reply msg in cb
 * Then send it using send_msg_to_client()
 */
int 
netconf_create_rpc_reply(cbuf *cb,            /* msg buffer */
			 cxobj *xr, /* orig request */
			 char *body,
			 int ok
    )
{
    cxobj *xn, *xa;

    add_preamble(cb);
    cprintf(cb, "<rpc-reply"); /* attributes from rpc */
    if (xr && (xn=xpath_first(xr, "//rpc")) != NULL){
	xa = NULL;
	while ((xa = xml_child_each(xn, xa, CX_ATTR)) != NULL) 
	    cprintf(cb, " %s=\"%s\"", xml_name(xa), xml_value(xa));
    }
    cprintf(cb, ">");
    if (ok) /* Just _maybe_ we should send data instead of ok if (if there is any)
	       even if ok is set? */
	cprintf(cb, "<ok/>");
    else{
	cprintf(cb, "<data>");
	cprintf(cb, "%s", body);
	cprintf(cb, "</data>");
    }
    cprintf(cb, "</rpc-reply>");
    add_postamble(cb);
    return 0;
}


/*
 * netconf_create_rpc_error
 * Create an rpc error msg in cb
 * (Then it should be sent at a later stage using send_msg_to_client() )
 * Arguments:
 *  cb   msg buffer to construct netconf message in
 *  xr   original request including an "rpc" tag
 *  ...  arguments to rpc-error reply message
 */
int 
netconf_create_rpc_error(cbuf *cb,            /* msg buffer */
			 cxobj *xr, /* orig request */
			 char *tag, 
			 char *type,
			 char *severity, 
			 char *message, 
			 char *info)
{
    add_error_preamble(cb, tag);
    cprintf(cb, "<rpc-reply>");
    cprintf(cb, "<rpc-error>");
    if (tag)
	cprintf(cb, "<error-tag>%s</error-tag>", tag);
    cprintf(cb, "<error-type>%s</error-type>", type);
    cprintf(cb, "<error-severity>%s</error-severity>", severity);
    if (message)
	cprintf(cb, "<error-message>%s</error-message>", message);
    if (info)
	cprintf(cb, "<error-info>%s</error-info>", info);
    cprintf(cb, "</rpc-error>");
    cprintf(cb, "</rpc-reply>");
    add_error_postamble(cb);
    return 0;
}

