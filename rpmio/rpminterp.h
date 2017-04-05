#ifndef _H_INTERP_
#define _H_INTERP_

extern int _rpminterp_debug;

/** \ingroup rpminterp
 * Initialization flags for init().
 */
typedef	enum rpminterpFlag_e {
    RPMINTERP_NO_INIT    	= 1<<30,
    RPMINTERP_NO_IO_REDIR	= 1<<29,
    RPMINTERP_DEFAULT 		= 0
} rpminterpFlag;

struct rpminterp_s {
	const char	*name;
	rpmRC 		(*init) (ARGV_t *av, rpminterpFlag flags);
	void 		(*free) (void);
	rpmRC		(*run) (const char * str, char ** resultp);
	void		*h;
};

typedef const struct rpminterp_s * rpminterp;

#define rpminterpInit(name, init, free, run) \
	struct rpminterp_s rpminterp_ ## name = { #name, init, free, run, NULL}

#ifdef __cplusplus
extern "C" {
#endif

rpminterp rpminterpLoad(const char* name, const char *modpath);

#ifdef __cplusplus
}
#endif

#endif /* _H_INTERP_ */
