#!/usr/bin/env bash
# Run a system around openconfig interface, ie: openconfig-if-ethernet

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

cfg=$dir/conf_yang.xml
fyang=$dir/clixon-example.yang

new "openconfig"
if [ ! -d "$OPENCONFIG" ]; then
#    err "Hmm Openconfig dir does not seem to exist, try git clone https://github.com/openconfig/public?"
    echo "...skipped: OPENCONFIG not set"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

OCDIR=$OPENCONFIG/release/models

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_YANG_DIR>$OPENCONFIG/third_party/ietf/</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR/interfaces</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR/types</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$OCDIR/wifi</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_FILE>$fyang</CLICON_YANG_MAIN_FILE>	
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_AUTOCLI_EXCLUDE>clixon-restconf ietf-interfaces</CLICON_CLI_AUTOCLI_EXCLUDE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_MODULE_LIBRARY_RFC7895>true</CLICON_MODULE_LIBRARY_RFC7895>
</clixon-config>
EOF

# Example yang
cat <<EOF > $fyang
module clixon-example{
  yang-version 1.1;
  namespace "urn:example:example";
  prefix ex;

  import ietf-interfaces { 
    prefix ietf-if; 
  }
  import openconfig-if-ethernet {
    prefix oc-eth;
  }
  identity eth { /* Need to create an interface-type identity for leafrefs */
    base ietf-if:interface-type;
  }
}
EOF

# Example system
cat <<EOF > $dir/startup_db
<config>
  <interfaces xmlns="http://openconfig.net/yang/interfaces">
    <interface>
      <name>e</name>
      <config>
         <name>e</name>
         <type>ex:eth</type>
         <loopback-mode>false</loopback-mode>
         <enabled>true</enabled>
      </config>
      <hold-time>
         <config>
            <up>0</up>
            <down>0</down>
         </config>
      </hold-time>
    </interface>
  </interfaces>
</config>
EOF

if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    sudo pkill -f clixon_backend # to be sure
    
    new "start backend -s startup -f $cfg"
    start_backend -s startup -f $cfg

    new "wait backend"
    wait_backend
fi

new "$clixon_cli -D $DBG -1f $cfg -y $f show version"
expectpart "$($clixon_cli -D $DBG -1f $cfg show version)" 0 "${CLIXON_VERSION}"

new "$clixon_netconf -qf $cfg"
expecteof "$clixon_netconf -qf $cfg" 0 "$DEFAULTHELLO<rpc $DEFAULTNS><get-config><source><candidate/></source></get-config></rpc>]]>]]>" "^<rpc-reply $DEFAULTNS><data><interfaces xmlns=\"http://openconfig.net/yang/interfaces\"><interface><name>e</name><config><name>e</name><type>ex:eth</type><loopback-mode>false</loopback-mode><enabled>true</enabled></config><hold-time><config><up>0</up><down>0</down></config></hold-time><oc-eth:ethernet xmlns:oc-eth=\"http://openconfig.net/yang/interfaces/ethernet\"><oc-eth:config><oc-eth:auto-negotiate>true</oc-eth:auto-negotiate><oc-eth:enable-flow-control>false</oc-eth:enable-flow-control></oc-eth:config></oc-eth:ethernet></interface></interfaces></data></rpc-reply>]]>]]>"

new "cli show configuration"
expectpart "$($clixon_cli -1 -f $cfg show conf xml)" 0 "^<interfaces xmlns=\"http://openconfig.net/yang/interfaces\">" "<oc-eth:ethernet xmlns:oc-eth=\"http://openconfig.net/yang/interfaces/ethernet\">"

new "cli set interfaces interface <tab> complete: e"
expectpart "$(echo "set interfaces interface 	" | $clixon_cli -f $cfg)" 0 "interface e"

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
	err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg
fi

rm -rf $dir

new "endtest"
endtest
