#!/usr/bin/env bash
# Parse "all" IETF yangmodels from https://github.com/YangModels/yang/standard/ieee and experimental/ieee
# Notes:
# - Only a simple smoketest (CLI check) is made, essentially YANG parsing. A full system may not work
# - Env variable YANGMODELS should point to checkout place. (define it in site.sh for example)
# - Some FEATURES are set to make it work
# - Some DIFFs are necessary in yangmodels
#     - standard/ietf/RFC/ietf-mud@2019-01-28.yang
#           -      + "/acl:l4/acl:tcp/acl:tcp" {
#           +      + "/acl:l4/acl:tcp" {
#     - standard/ietf/RFC/ietf-acldns@2019-01-28.yang
#                augment "/acl:acls/acl:acl/acl:aces/acl:ace/acl:matches"
#            -        + "/acl:l3/acl:ipv4/acl:ipv4" {
#            +        + "/acl:l3/acl:ipv4" {
#                 description
#                   "Adding domain names to matching.";
#            +    if-feature acl:match-on-ipv4;
#                 uses dns-matches;
#               }
#               augment "/acl:acls/acl:acl/acl:aces/acl:ace/acl:matches"
#            -        + "/acl:l3/acl:ipv6/acl:ipv6" {
#            +        + "/acl:l3/acl:ipv6" {
#                 description
#                   "Adding domain names to matching.";
#            +    if-feature acl:match-on-ipv6;


# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

# Yang specifics: multi-keys and empty type
APPNAME=example

cfg=$dir/conf_yang.xml

if [ ! -d "$YANGMODELS" ]; then
#    err "Hmm Yangmodels dir does not seem to exist, try git clone https://github.com/YangModels/yang?"
    echo "...skipped: YANGMODELS not set"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-alarms:alarm-shelving</CLICON_FEATURE>
  <CLICON_FEATURE>ietf-subscribed-notifications:configured</CLICON_FEATURE>
  <CLICON_FEATURE>ietf-subscribed-notifications:replay</CLICON_FEATURE>
  <CLICON_FEATURE>ietf-access-control-list:match-on-tcp</CLICON_FEATURE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$YANGMODELS/standard/ieee/published/802.1</CLICON_YANG_DIR> 
  <CLICON_YANG_DIR>$YANGMODELS/standard/ietf/RFC</CLICON_YANG_DIR>	
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
</clixon-config>
EOF

# Standard IETF
files=$(find $YANGMODELS/standard/ietf/RFC -name "*.yang")
for f in $files; do
    if [ -n "$(head -1 $f|grep '^module')" ]; then
	# Mask old revision
	if [ $f != $YANGMODELS/standard/ietf/RFC/ietf-yang-types@2010-09-24.yang ]; then
	    new "$clixon_cli -D $DBG -1f $cfg -y $f show version"
	    expectpart "$($clixon_cli -D $DBG -1f $cfg -y $f show version)" 0 "${CLIXON_VERSION}"
	fi
    fi
done

rm -rf $dir

new "endtest"
endtest
