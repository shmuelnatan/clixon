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
 */

#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

#include "clicon_err.h"
#include "clicon_queue.h"
#include "clicon_hash.h"
#include "clicon_handle.h"
#include "clicon_plugin.h"


static find_plugin_t *
clicon_find_plugin(clicon_handle h)
{
    void *p;
    find_plugin_t *fp = NULL;
    clicon_hash_t *data = clicon_data(h);
    
    if ((p = hash_value(data, "CLICON_FIND_PLUGIN", NULL)) != NULL)
	memcpy(&fp, p, sizeof(fp));

    return fp;
}


/*! Return a function pointer based on name of plugin and function.
 * If plugin is specified, ask daemon registered function to return 
 * the dlsym handle of the plugin.
 */
void *
clicon_find_func(clicon_handle h, char *plugin, char *func)
{
    find_plugin_t *plgget;
    void          *dlhandle = NULL;

    if (plugin) {
	/* find clicon_plugin_get() in global namespace */
	if ((plgget = clicon_find_plugin(h)) == NULL) {
	    clicon_err(OE_UNIX, errno, "Specified plugin not supported");
	    return NULL;
	}
	dlhandle = plgget(h, plugin);
    }
    
    return dlsym(dlhandle, func);
}
