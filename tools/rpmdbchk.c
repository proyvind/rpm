#include "system.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <popt.h>

#include <rpmio.h>
#include <rpmlog.h>
#include <rpmmacro.h>
#include <argv.h>
#include <rpmfileutil.h>

#define _RPMTAG_INTERNAL
#define _RPMDB_INTERNAL
#define WITH_DB
#include <rpmdb.h>

#include <db.h>
#include "lib/rpmchroot.h"
#include "lib/rpmdb_internal.h"
#include "lib/fprint.h"
#include "lib/header_internal.h"	/* XXX for headerSetInstance() */
#include "lib/backend/dbiset.h"
#include "lib/backend/dbi.h"

#include <rpmts.h>
#include <rpmcli.h>

static char *rootPath = NULL;
static char *rpmdbPath = NULL;
static int checkOnly = 0;

struct node {
    uint32_t state;
    uint32_t keysize;
    void *keydata;
    struct node *next;
};


struct dbiCursor_s {
    dbiIndex dbi;
    const void *key;
    unsigned int keylen;
    int flags;
    DBC *cursor;
};

static int
rpmdb_check(uint32_t **state, uint32_t *nkeys, struct node **broken)
{
    rpmts ts = NULL;
    DBC *dbcp = NULL;
    dbiIndex dbi = NULL;
    dbiCursor dbc = NULL;
    DBT key;
    DBT data;
    DB_TXN *txnid = NULL;
    DB *bdb;
    void *dbi_stats;
    int rc = 0;


    uint32_t hdrNum = 0;
    uint32_t damaged = 0;
    float pct = 0;
    uint8_t tmp;

    int xx;

    ts = rpmtsCreate();

    rpmtsSetRootDir(ts, rootPath && rootPath[0] ? rootPath : NULL);
    if(rpmtsOpenDB(ts, O_RDONLY))
	goto exit;

    rc = dbiOpen(rpmtsGetRdb(ts), RPMDBI_PACKAGES, &dbi, 0);

    dbc = dbiCursorInit(dbi, DBC_READ);


    *nkeys = 0;

    memset(&key, 0, sizeof(key));
    memset(&data, 0, sizeof(data));
    bdb = dbi->dbi_db;

    /* Acquire a cursor for the database. */
    dbcp = dbc->cursor;

    xx = bdb->stat(bdb, 0, &dbi_stats, 0);
    if (xx)
	goto exit;

    switch (bdb->type) {
    case DB_BTREE:
    case DB_RECNO:{
	DB_BTREE_STAT *db_stat = (typeof(db_stat))&dbi_stats;
	*nkeys = db_stat->bt_nkeys;
    }   break;
    case DB_HASH:{
	DB_HASH_STAT *db_stat = (typeof(db_stat))&dbi_stats;
	*nkeys = db_stat->hash_nkeys;
    }   break;
    case DB_QUEUE:{
	DB_QUEUE_STAT *db_stat = (typeof(db_stat))&dbi_stats;
	*nkeys = db_stat->qs_nkeys;
    }   break;
    case DB_UNKNOWN:
    default:
	xx = -1;
	goto exit;
	break;
    }
    uint32_t *status = calloc(*nkeys+10, sizeof(uint32_t));
    struct node *curr;

    hdrNum = 0;
    pct = 0;

    while ((xx = dbcp->c_get(dbcp, &key, &data, DB_NEXT)) == 0) {
	tmp = pct;
	pct = (100 * (float) ++hdrNum / *nkeys) + 0.5;
	/* TODO: callbacks for status output? */
	if (tmp < (int) (pct + 0.5)) {
	    fprintf(stderr, "\rchecking %s%s/Packages: %u/%u %d%%",
			rootPath && rootPath[0] ? rootPath : "",
			rpmdbPath, hdrNum, *nkeys, (int) pct);
	}
	char *msg = NULL;
	int lvl = headerCheck(ts, data.data, data.size, &msg);
	//rpmtsCleanDig(ts);
	if (lvl == RPMRC_FAIL) {
	    status[hdrNum-1] = htonl(*(uint32_t*)(dbcp->rkey->data));
	    damaged++;
	    fprintf(stderr, "\n%d (%d): %s\n", hdrNum-1, status[hdrNum-1], msg);
	} else if (key.size != sizeof(hdrNum)) {
	    curr = malloc(sizeof(struct node));
	    curr->state = htonl(*(uint32_t*)(dbcp->rkey->data));
	    curr->keysize = key.size;
	    curr->keydata = malloc(key.size);
	    memcpy(curr->keydata, key.data, key.size);
	    curr->next = *broken;
	    *broken = curr;
	    status[hdrNum-1] = -1;
	    damaged++;
	    fprintf(stderr, "\n%d: %s (key.size(%d) != %d)\n", hdrNum-1, msg, key.size, sizeof(hdrNum));
	} else
	    status[hdrNum-1] = -1;
	fflush(stderr);
    }

    fprintf(stderr, "\n");


    *state = status;
    xx = dbiClose(dbi,  0);
    
exit:
    xx = rpmtsCloseDB(ts);
    ts = rpmtsFree(ts);

    return damaged;
}

static int
rpmdb_dump_delete(DB *dbp, const char *db, const char *lost, DBT *key, uint32_t n) {
    int gotrec;
    int ret = 0;
    DBT data;

    memset(&data, 0, sizeof(data));

    if ((ret = dbp->get(dbp, NULL, key, &data, 0)) == 0) {
	char copy[1024];
	snprintf(copy, sizeof(copy), "%s/header.%d", lost, n);
	FILE *fp = fopen(copy, "w");
	fwrite(data.data, data.size, 1, fp);
	fclose(fp);
	gotrec = 0;
	memcpy(&gotrec, key->data, sizeof(gotrec));
	printf("db: get key: %p[%d] = 0x%x, data at %p[%d].\n",
		(char *)key->data, key->size, gotrec,
		(char *)data.data, data.size);
	printf("Dumping broken header to disk: %s\n", copy);
    } else {
	dbp->err(dbp, ret, "DB->get");
	if (ret == DB_NOTFOUND)
	    return 0;
	return ret;
    }

    if ((ret = dbp->del(dbp, NULL, key, 0)) == 0) {
	gotrec = 0;
	memcpy(&gotrec, key->data, sizeof(gotrec));
	printf("db: del key: %p[%d] = 0x%x, data at %p[%d].\n",
		(char *)key->data, key->size, gotrec,
		(char *)data.data, data.size);
    } else {
	dbp->err(dbp, ret, "DB->del");
	return ret;
    }
    return 0;
}

static int
rpmdb_fix(uint32_t *state, uint32_t nkeys, struct node *broken)
{
    DB * dbp;
    DBT key;
    struct stat sb;
    char * db = rpmGetPath(rootPath && rootPath[0] ? rootPath : "", rpmdbPath, "/Packages", NULL);
    char * lost = rpmGetPath(rootPath && rootPath[0] ? rootPath : "", rpmdbPath, "/broken", NULL);
    int ret, t_ret;
    uint32_t i;


    if ((ret = db_create(&dbp, NULL, 0)) != 0) {
	fprintf(stderr, "db_create: %s\n", db_strerror(ret));
	exit (1);
    }

    if (stat(lost, &sb))
	mkdir(lost, 0700);

    if ((ret = dbp->open(dbp, NULL, db, NULL, DB_BTREE, DB_CREATE, 0664)) != 0) {
	dbp->err(dbp, ret, "%s", db);
	goto err;
    }

    for (i = 0; i < nkeys; i++) {
	if (state[i] == -1) continue;
	int badrec, badrec2;
	memset(&key, 0, sizeof(key));
	badrec2 = state[i];
	badrec = htonl(badrec2);
	printf("fix record[%d] at #%d/#%d --\n", i, badrec2, badrec);
	key.data = &badrec;
	key.size = sizeof(badrec);

	ret = rpmdb_dump_delete(dbp, db, lost, &key, state[i]);
    }

    while (broken) {
	memset(&key, 0, sizeof(key));
	key.size = broken->keysize;
	key.data = broken->keydata;
	ret = rpmdb_dump_delete(dbp, db, lost, &key, broken->state);
	free(broken->keydata);
	free(broken);
	broken = broken->next;
    }


err:
    if ((t_ret = dbp->close(dbp, 0)) != 0 && ret == 0)
	ret = t_ret; 
    _free(db);
    _free(lost);

    return 0;
}

static struct poptOption optionsTable[] = {
  { "root", '\0', POPT_ARG_STRING, &rootPath, 0,
	"rpm root path", "path"},
  { "dbpath", '\0', POPT_ARG_STRING, &rpmdbPath, 0,
	"rpmdb path", "path"},
  { "checkonly", '\0', POPT_ARG_VAL, &checkOnly, 1,
	"only check, don't fix anything", NULL},  

    POPT_AUTOALIAS
    POPT_AUTOHELP
    POPT_TABLEEND
};

int main(int argc, char *argv[])
{
    poptContext optCon = rpmcliInit(argc, argv, optionsTable);
    ARGV_t av = (ARGV_t)poptGetArgs(optCon);
    int ac = argvCount(av);
    int rc = 2;		/* assume failure */
    uint32_t nkeys = 0;
    uint32_t *state = NULL;
    struct node *broken = NULL;

    if (ac) {
	poptPrintUsage(optCon, stderr, 0);
	goto exit;
    }

    rc = rpmReadConfigFiles(NULL, NULL);

    if(!rpmdbPath)
	rpmdbPath = rpmExpand("%{?_dbpath}%{?!_dbpath:/var/lib/rpm}", NULL);
    else
	addMacro(NULL, "_dbpath", NULL, rpmdbPath, -1);


    rc = rpmdb_check(&state, &nkeys, &broken);
    printf("%d/%d (%f%%) headers damaged", rc, nkeys, (float)rc/nkeys);
    printf("\n");
    if (!checkOnly && rc) {
	printf("fixing...\n");
	rc = rpmdb_fix(state, nkeys, broken);
    }

exit:
    _free(state);
    optCon = rpmcliFini(optCon);
    return rc;
}
