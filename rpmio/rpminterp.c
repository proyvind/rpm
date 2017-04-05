#include "system.h"

#include <dlfcn.h>
#include <rpm/rpmlog.h>

#include <rpm/rpmio.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpminterp.h>

#include "debug.h"

int _rpminterp_debug = 0;

static void rpminterpFree(int argc, void *p) {
    rpminterp interp = p;
    if (interp->free != NULL)
	interp->free();
    if (interp->h && dlclose(interp->h))
	rpmlog(RPMLOG_WARNING, "Error closing rpminterp \"%s\": %s", interp->name, dlerror());
}

rpminterp rpminterpLoad(const char* name, const char *modpath) {
    char buf[64];
    struct rpminterp_s *interp = NULL;
    void *h = NULL;
    Dl_info info;
    ARGV_t files = NULL;
    rpmRC rc = RPMRC_FAIL;
    rpminterpFlag flags = RPMINTERP_DEFAULT;

    snprintf(buf, sizeof(buf), "%%_rpminterp_%s_flags", name);

    flags = rpmExpandNumeric(buf);

    snprintf(buf, sizeof(buf), "rpminterp_%s", name);

    if (!(interp = dlsym(RTLD_DEFAULT, buf))) {

	if (_rpminterp_debug)
	    rpmlog(RPMLOG_DEBUG, " %8s (modpath) %s\n", __func__, modpath);

	if (rpmGlob(modpath, NULL, &files) != 0) {
	    rpmlog(RPMLOG_WARNING, "\"%s\" does not exist, "
		    "embedded %s will not be available\n",
		    modpath, name);
	} else if (!(h = dlopen((modpath = *files), RTLD_LAZY|RTLD_GLOBAL))) {
	    rpmlog(RPMLOG_WARNING, "Unable to open \"%s\" (%s), "
		    "embedded %s will not be available\n",
		    modpath, dlerror(), name);
	} else if (!(interp = dlsym(h, buf))) {
	    rpmlog(RPMLOG_WARNING, "Opened library \"%s\" is incompatible (%s), "
		    "embedded %s will not be available\n",
		    modpath, dlerror(), name);
	} else if (dladdr(interp->init, &info) && strcmp(modpath, info.dli_fname)) {
	    rpmlog(RPMLOG_WARNING, "\"%s\" lacks %s interpreter support, "
		    "embedding of will not be available\n",
		    modpath, name);
	} else
	    interp->h = h;
    }

    if (interp != NULL) {
	if (interp->init != NULL) {
	    if ((rc = interp->init(NULL, flags)) != RPMRC_OK) {
		rpmlog(RPMLOG_WARNING, "%s->init() != RPMRC_OK: %d\n",
			buf, rc);
		rpminterpFree(0, interp);
	    }
	    else
		on_exit(rpminterpFree, interp);
	} else
	    rc = RPMRC_OK;
    } else if (h && dlclose(h)) {
	    rpmlog(RPMLOG_WARNING, "Error closing library \"%s\": %s", modpath,
		    dlerror());
    }

    if (_rpminterp_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (_rpminterp_%s_modpath, %d) %s\n", __func__, name, rc, modpath);

    if (rc != RPMRC_OK)
	interp = NULL;

    argvFree(files);

    return interp;
}
