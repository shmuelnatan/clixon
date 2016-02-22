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
 * handling netconf plugins
 */

#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clicon/clicon.h>

/* clicon netconf*/
#include "clicon_netconf.h"
#include "netconf_lib.h"
#include "netconf_plugin.h"

/* 
 * Unload a plugin
 */
static int
plugin_unload(clicon_handle h, void *handle)
{
    int retval = 0;
    char *error;
    plgexit_t *exitfn;

    /* Call exit function is it exists */
    exitfn = dlsym(handle, PLUGIN_EXIT);
    if (dlerror() == NULL)
	exitfn(h);

    dlerror();    /* Clear any existing error */
    if (dlclose(handle) != 0) {
	error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlclose: %s\n", error ? error : "Unknown error");
	/* Just report */
    }
    return retval;
}



/*
 * Load a dynamic plugin object and call it's init-function
 * Note 'file' may be destructively modified
 */
static plghndl_t 
plugin_load (clicon_handle h, char *file, int dlflags, const char *cnklbl)
{
    char      *error;
    void      *handle = NULL;
    plginit_t *initfn;

    dlerror();    /* Clear any existing error */
    if ((handle = dlopen (file, dlflags)) == NULL) {
        error = (char*)dlerror();
	clicon_err(OE_PLUGIN, errno, "dlopen: %s\n", error ? error : "Unknown error");
	goto quit;
    }
    /* call plugin_init() if defined */
    if ((initfn = dlsym(handle, PLUGIN_INIT)) != NULL) {
	if (initfn(h) != 0) {
	    clicon_err(OE_PLUGIN, errno, "Failed to initiate %s\n", strrchr(file,'/')?strchr(file, '/'):file);
	    goto quit;
	}
    }
quit:

    return handle;
}

static int nplugins = 0;
static plghndl_t *plugins = NULL;
static netconf_reg_t *deps = NULL;

/*
 * netconf_plugin_load
 * Load allplugins you can find in CLICON_NETCONF_DIR
 */
int 
netconf_plugin_load(clicon_handle h)
{
    int            retval = -1;
    char          *dir;
    int            ndp;
    struct dirent *dp;
    int            i;
    char          *filename;
    plghndl_t     *handle;

    if ((dir = clicon_netconf_dir(h)) == NULL){
	clicon_err(OE_PLUGIN, 0, "clicon_netconf_dir not defined");
	goto quit;
    }

    /* Get plugin objects names from plugin directory */
    if((ndp = clicon_file_dirent(dir, &dp, "(.so)$", S_IFREG, __FUNCTION__))<0)
	goto quit;

    /* Load all plugins */
    for (i = 0; i < ndp; i++) {
	filename = chunk_sprintf(__FUNCTION__, "%s/%s", dir, dp[i].d_name);
	clicon_debug(1, "DEBUG: Loading plugin '%.*s' ...", 
		     (int)strlen(filename), filename);
	if (filename == NULL) {
	    clicon_err(OE_UNIX, errno, "chunk");
	    goto quit;
	}
	if ((handle = plugin_load (h, filename, RTLD_NOW, __FUNCTION__)) == NULL)
	    goto quit;
	if ((plugins = rechunk(plugins, (nplugins+1) * sizeof (*plugins), NULL)) == NULL) {
	    clicon_err(OE_UNIX, errno, "chunk");
	    goto quit;
	}
	plugins[nplugins++] = handle;
	unchunk (filename);
    }
    retval = 0;
quit:
    unchunk_group(__FUNCTION__);
    return retval;
}

int
netconf_plugin_unload(clicon_handle h)
{
    int i;
    netconf_reg_t *nr;

    while((nr = deps) != NULL) {
	DELQ(nr, deps, netconf_reg_t *);
	if (nr->nr_tag)
	    free(nr->nr_tag);
	free(nr);
    }
    for (i = 0; i < nplugins; i++) 
	plugin_unload(h, plugins[i]);
    if (plugins)
	unchunk(plugins);
    nplugins = 0;
    return 0;
}

/*
 * Call plugin_start in all plugins
 */
int
netconf_plugin_start(clicon_handle h, int argc, char **argv)
{
    int i;
    plgstart_t *startfn;

    for (i = 0; i < nplugins; i++) {
	/* Call exit function is it exists */
	if ((startfn = dlsym(plugins[i], PLUGIN_START)) == NULL)
	    break;
	optind = 0;
	if (startfn(h, argc, argv) < 0) {
	    clicon_debug(1, "plugin_start() failed\n");
	    return -1;
	}
    }
    return 0;
}


/*
 * netconf_register_callback
 * Called from plugin to register a callback for a specific netconf XML tag.
 */
int
netconf_register_callback(clicon_handle h,
			  netconf_cb_t cb,      /* Callback called */
			  void *arg,        /* Arg to send to callback */
			  char *tag)        /* Xml tag when callback is made */
{
    netconf_reg_t *nr;

    if ((nr = malloc(sizeof(netconf_reg_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto catch;
    }
    memset (nr, 0, sizeof (*nr));
    nr->nr_callback = cb;
    nr->nr_arg  = arg;
    nr->nr_tag  = strdup(tag); /* strdup */
    INSQ(nr, deps);
    return 0;
catch:
    if (nr){
	if (nr->nr_tag)
	    free(nr->nr_tag);
	free(nr);
    }
    return -1;
}
    
/*! See if there is any callback registered for this tag
 *
 * @param  xn      Sub-tree (under xorig) at child of rpc: <rpc><xn></rpc>.
 * @param  xf      Output xml stream. For reply
 * @param  xf_err  Error xml stream. For error reply
 * @param  xorig   Original request.
 *
 * @retval -1   Error
 * @retval  0   OK, not found handler.
 * @retval  1   OK, handler called
 */
int
netconf_plugin_callbacks(clicon_handle h,
			 cxobj *xn, 
			 cbuf *xf, 
			 cbuf *xf_err, 
			 cxobj *xorig)
{
    netconf_reg_t *nr;
    int            retval;

    if (deps == NULL)
	return 0;
    nr = deps;
    do {
	if (strcmp(nr->nr_tag, xml_name(xn)) == 0){
	    if ((retval = nr->nr_callback(h, 
					  xorig, 
					  xn, 
					  xf,
					  xf_err,
					  nr->nr_arg)) < 0)
		return -1;
	    else
		return 1; /* handled */
	}
	nr = NEXTQ(netconf_reg_t *, nr);
    } while (nr != deps);
    return 0;
}
    
