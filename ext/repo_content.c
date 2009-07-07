/*
 * repo_content.c
 *
 * Parses 'content' file into .solv
 * See http://en.opensuse.org/Standards/YaST2_Repository_Metadata/content for a description
 * of the syntax
 *
 *
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "repo_content.h"
#define DISABLE_SPLIT
#include "tools_util.h"

/* split off a word, return null terminated pointer to it.
 * return NULL if there is no word left. */
static char *
splitword(char **lp)
{
  char *w, *l = *lp;

  while (*l == ' ' || *l == '\t')
    l++;
  w = *l ? l : 0;
  while (*l && *l != ' ' && *l != '\t')
    l++;
  if (*l)
    *l++ = 0;		/* terminate word */
  while (*l == ' ' || *l == '\t')
    l++; 		/* convenience: advance to next word */
  *lp = l;
  return w;
}

struct parsedata {
  Repo *repo;
  char *tmp;
  int tmpl;

  const char *tmpvers;
  const char *tmprel;
};

/*
 * dependency relations
 */

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};


/*
 * join up to three strings into one
 */

static char *
join(struct parsedata *pd, const char *s1, const char *s2, const char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > pd->tmpl)
    {
      pd->tmpl = l + 256;
      pd->tmp = sat_realloc(pd->tmp, pd->tmpl);
    }
  p = pd->tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  *p = 0;
  return pd->tmp;
}


/*
 * add dependency to pool
 * OBSOLETES product:SUSE_LINUX product:openSUSE < 11.0 package:openSUSE < 11.0
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, Id marker)
{
  char *name;
  Id id;

  while ((name = splitword(&line)) != 0)
    {
      /* Hack, as the content file adds 'package:' for package
         dependencies sometimes.  */
      if (!strncmp (name, "package:", 8))
        name += 8;
      id = str2id(pool, name, 1);
      if (*line == '<' || *line == '>' || *line == '=')	/* rel follows */
	{
	  char *rel = splitword(&line);
          char *evr = splitword(&line);
	  int flags;

	  if (!rel || !evr)
	    {
	      pool_debug(pool, SAT_FATAL, "repo_content: bad relation '%s %s'\n", name, rel);
	      exit(1);
	    }
	  for (flags = 0; flags < 6; flags++)
	    if (!strcmp(rel, flagtab[flags]))
	      break;
	  if (flags == 6)
	    {
	      pool_debug(pool, SAT_FATAL, "repo_content: unknown relation '%s'\n", rel);
	      exit(1);
	    }
	  id = rel2id(pool, id, str2id(pool, evr, 1), flags + 1, 1);
	}
      olddeps = repo_addid_dep(pd->repo, olddeps, id, marker);
    }
  return olddeps;
}


/*
 * split value and add to pool
 */

static void
add_multiple_strings(Repodata *data, Id handle, Id name, char *value)
{
  char *str;

  while ((str = splitword(&value)) != 0)
    repodata_add_poolstr_array(data, handle, name, str);
}

/*
 * split value and add to pool
 */

static void
add_multiple_urls(Repodata *data, Id handle, char *value, Id type)
{
  char *url;

  while ((url = splitword(&value)) != 0)
    {
      repodata_add_poolstr_array(data, handle, PRODUCT_URL, url);
      repodata_add_idarray(data, handle, PRODUCT_URL_TYPE, type);
    }
}



/*
 * add 'content' to repo
 *
 */

void
repo_add_content(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s;
  struct parsedata pd;
  Repodata *data;
  Id handle = 0;
  int contentstyle = 0;
  char *descrdir = 0;
  char *datadir = 0;
  char *defvendor = 0;

  int i = 0;

  /* architectures
     we use the first architecture in BASEARCHS or noarch
     for the product. At the end we create (clone) the product
     for each one of the remaining architectures
     we allow max 4 archs
  */
  unsigned int numotherarchs = 0;
  Id *otherarchs = 0;

  if (!(flags & REPO_REUSE_REPODATA))
    data = repo_add_repodata(repo, 0);
  else
    data = repo_last_repodata(repo);

  memset(&pd, 0, sizeof(pd));
  line = sat_malloc(1024);
  aline = 1024;

  if (repo->nrepodata)
    /* use last repodata */
    data = repo->repodata + repo->nrepodata - 1;
  else
    data = repo_add_repodata(repo, 0);

  pd.repo = repo;
  linep = line;
  s = 0;

  for (;;)
    {
      char *key, *value;

      /* read line into big-enough buffer */
      if (linep - line + 16 > aline)
	{
	  aline = linep - line;
	  line = sat_realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp))
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
        continue;
      while ( --linep > line && ( linep[-1] == ' ' ||  linep[-1] == '\t' ) )
        ; /* skip trailing ws */
      *linep = 0;
      linep = line;

      /* expect "key value" lines */
      value = line;
      key = splitword(&value);

      if (key)
        {
#if 0
	  fprintf (stderr, "key %s, value %s\n", key, value);
#endif

#define istag(x) (!strcmp (key, x))
#define code10 (contentstyle == 10)
#define code11 (contentstyle == 11)


	  if (istag ("CONTENTSTYLE"))
	    {
	      if (contentstyle)
	        pool_debug(pool, SAT_ERROR, "repo_content: 'CONTENTSTYLE' must be first line of 'content'\n");
	      contentstyle = atoi(value);
	      continue;
	    }
	  if (!contentstyle)
	    contentstyle = 10;

	  /* repository tags */
          /* we also replicate some of them into the product solvables
           * to be backward compatible */

	  if (istag ("DESCRDIR"))
	    {
	      if (descrdir)
		free(descrdir);
	      else
	        repo_set_str(repo, SOLVID_META, SUSETAGS_DESCRDIR, value);
	      if (s)
	        repo_set_str(repo, s - pool->solvables, SUSETAGS_DESCRDIR, value);
	      descrdir = strdup(value);
	      continue;
	    }
	  if (istag ("DATADIR"))
	    {
	      if (datadir)
		free(datadir);
	      else
	        repo_set_str(repo, SOLVID_META, SUSETAGS_DATADIR, value);
	      if (s)
	        repo_set_str(repo, s - pool->solvables, SUSETAGS_DATADIR, value);
	      datadir = strdup(value);
	      continue;
	    }
	  if (istag ("VENDOR"))
	    {
	      if (defvendor)
		free(defvendor);
	      else
	        repo_set_poolstr(repo, SOLVID_META, SUSETAGS_DEFAULTVENDOR, value);
	      if (s)
		s->vendor = str2id(pool, value, 1);
	      defvendor = strdup(value);
	      continue;
	    }

	  if (istag ("META") || istag ("HASH") || istag ("KEY"))
	    {
	      char *checksumtype, *checksum;
	      Id fh, type;
	      int l;

	      if ((checksumtype = splitword(&value)) == 0)
		continue;
	      if ((checksum = splitword(&value)) == 0)
		continue;
	      if (!*value)
		continue;
	      if (!strcasecmp(checksumtype, "sha") || !strcasecmp(checksumtype, "sha1"))
	        l = SIZEOF_SHA1 * 2, type = REPOKEY_TYPE_SHA1;
	      else if (!strcasecmp(checksumtype, "sha256"))
	        l = SIZEOF_SHA256 * 2, type = REPOKEY_TYPE_SHA256;
	      else if (!strcasecmp(checksumtype, "md5"))
	        l = SIZEOF_MD5 * 2, type = REPOKEY_TYPE_MD5;
	      else
	        {
		  fprintf(stderr, "Unknown checksum type: %s: %s\n", value, checksumtype);
		  exit(1);
	        }
	      if (strlen(checksum) != l)
	        {
		  fprintf(stderr, "Invalid checksum length: %s: for %s\n", value, checksum);
		  exit(1);
	        }
	      fh = repodata_new_handle(data);
	      repodata_set_poolstr(data, fh, SUSETAGS_FILE_TYPE, key);
	      repodata_set_str(data, fh, SUSETAGS_FILE_NAME, value);
	      repodata_set_checksum(data, fh, SUSETAGS_FILE_CHECKSUM, type, checksum);
	      repodata_add_flexarray(data, SOLVID_META, SUSETAGS_FILE, fh);
	      continue;
	    }

	  /* product tags */

	  if ((code10 && istag ("PRODUCT"))
	      || (code11 && istag ("NAME")))
	    {
	      if (s && !s->name)
		{
		  /* this solvable was created without seeing a
		     PRODUCT entry, just set the name and continue */
		  s->name = str2id(pool, join(&pd, "product", ":", value), 1);
		  continue;
		}
	      if (s)
		{
		  /* finish old solvable */
		  if (!s->arch)
		    s->arch = ARCH_NOARCH;
		  if (!s->evr)
		    s->evr = ID_EMPTY;
		  if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
		  if (code10)
		    s->supplements = repo_fix_supplements(repo, s->provides, s->supplements, 0);
		}
	      /* create new solvable */
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      repodata_extend(data, s - pool->solvables);
	      handle = s - pool->solvables;
	      s->name = str2id(pool, join(&pd, "product", ":", value), 1);
	      if (datadir)
	        repo_set_str(repo, s - pool->solvables, SUSETAGS_DATADIR, datadir);
	      if (descrdir)
	        repo_set_str(repo, s - pool->solvables, SUSETAGS_DESCRDIR, descrdir);
	      if (defvendor)
		s->vendor = str2id(pool, defvendor, 1);
	      continue;
	    }

	  /* Sometimes PRODUCT/NAME is not the first entry, but we need a solvable
	     from here on.  */
	  if (!s)
	    {
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      repodata_extend(data, s - pool->solvables);
	      handle = s - pool->solvables;
	    }

	  if (istag ("VERSION"))
            pd.tmpvers = strdup(value);
          else if (istag ("RELEASE"))
            pd.tmprel = strdup(value);
	  else if (code11 && istag ("DISTRIBUTION"))
	    repo_set_str(repo, s - pool->solvables, SOLVABLE_DISTRIBUTION, value);
	  else if (istag ("UPDATEURLS"))
	    add_multiple_urls(data, handle, value, str2id(pool, "update", 1));
	  else if (istag ("EXTRAURLS"))
	    add_multiple_urls(data, handle, value, str2id(pool, "extra", 1));
	  else if (istag ("OPTIONALURLS"))
	    add_multiple_urls(data, handle, value, str2id(pool, "optional", 1));
	  else if (istag ("RELNOTESURL"))
	    add_multiple_urls(data, handle, value, str2id(pool, "releasenotes", 1));
	  else if (istag ("SHORTLABEL"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_SHORTLABEL, value);
	  else if (istag ("LABEL")) /* LABEL is the products SUMMARY. */
	    repo_set_str(repo, s - pool->solvables, SOLVABLE_SUMMARY, value);
	  else if (!strncmp (key, "LABEL.", 6))
	    repo_set_str(repo, s - pool->solvables, pool_id2langid(pool, SOLVABLE_SUMMARY, key + 6, 1), value);
	  else if (istag ("FLAGS"))
	    add_multiple_strings(data, handle, PRODUCT_FLAGS, value);
	  else if (istag ("VENDOR"))	/* actually already handled above */
	    s->vendor = str2id(pool, value, 1);
          else if (istag ("BASEARCHS"))
            {
              char *arch;

	      if ((arch = splitword(&value)) != 0)
		{
		  s->arch = str2id(pool, arch, 1);
		  while ((arch = splitword(&value)) != 0)
		    {
		       otherarchs = sat_extend(otherarchs, numotherarchs, 1, sizeof(Id), 7);
		       otherarchs[numotherarchs++] = str2id(pool, arch, 1);
		    }
		}
            }

	  /*
	   * Every tag below is Code10 only
	   *
	   */

	  if (code10 && istag ("DISTPRODUCT"))
	    /* DISTPRODUCT is for registration and Yast, not for the solver. */
	    repo_set_str(repo, s - pool->solvables, PRODUCT_DISTPRODUCT, value);
	  else if (code10 && istag ("DISTVERSION"))
	    /* DISTVERSION is for registration and Yast, not for the solver. */
	    repo_set_str(repo, s - pool->solvables, PRODUCT_DISTVERSION, value);
	  else if (code10 && istag ("ARCH"))
	    /* Theoretically we want to have the best arch of the given
	       modifiers which still is compatible with the system
	       arch.  We don't know the latter here, though.  */
	    s->arch = ARCH_NOARCH;
	  else if (code10 && istag ("PREREQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, SOLVABLE_PREREQMARKER);
	  else if (code10 && istag ("REQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, -SOLVABLE_PREREQMARKER);
	  else if (code10 && istag ("PROVIDES"))
	    s->provides = adddep(pool, &pd, s->provides, value, 0);
	  else if (code10 && istag ("CONFLICTS"))
	    s->conflicts = adddep(pool, &pd, s->conflicts, value, 0);
	  else if (code10 && istag ("OBSOLETES"))
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, value, 0);
	  else if (code10 && istag ("RECOMMENDS"))
	    s->recommends = adddep(pool, &pd, s->recommends, value, 0);
	  else if (code10 && istag ("SUGGESTS"))
	    s->suggests = adddep(pool, &pd, s->suggests, value, 0);
	  else if (code10 && istag ("SUPPLEMENTS"))
	    s->supplements = adddep(pool, &pd, s->supplements, value, 0);
	  else if (code10 && istag ("ENHANCES"))
	    s->enhances = adddep(pool, &pd, s->enhances, value, 0);
	  /* FRESHENS doesn't seem to exist.  */
	  else if (code10 && istag ("TYPE"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_TYPE, value);

	  /* XXX do something about LINGUAS and ARCH?
          * <ma>: Don't think so. zypp does not use or propagate them.
          */
#undef istag
	}
      else
	pool_debug(pool, SAT_ERROR, "repo_content: malformed line: %s\n", line);
    }

  if (datadir)
    free(datadir);
  if (descrdir)
    free(descrdir);
  if (defvendor)
    free(defvendor);

  if (s)
    {
      if (!s->name)
	{
	  pool_debug(pool, SAT_FATAL, "repo_content: 'content' incomplete, no product solvable created!\n");
	  exit(1);
	}

      if (pd.tmprel)
	s->evr = makeevr(pool, join(&pd, pd.tmpvers, "-", pd.tmprel));
      else
	s->evr = makeevr(pool, pd.tmpvers);
      pd.tmpvers = sat_free((void *)pd.tmpvers);
      pd.tmprel = sat_free((void *)pd.tmprel);

      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      if (code10)
	s->supplements = repo_fix_supplements(repo, s->provides, s->supplements, 0);

      /* now for every other arch, clone the product except the architecture */
      for (i = 0; i < numotherarchs; ++i)
	{
	  Solvable *p = pool_id2solvable(pool, repo_add_solvable(repo));
	  repodata_extend(data, p - pool->solvables);
	  p->name = s->name;
	  p->evr = s->evr;
	  p->vendor = s->vendor;
	  p->arch = otherarchs[i];

	  /* self provides */
	  if (s->name && p->arch != ARCH_SRC && p->arch != ARCH_NOSRC)
	      p->provides = repo_addid_dep(repo, p->provides, rel2id(pool, p->name, p->evr, REL_EQ, 1), 0);

	  /* now merge the attributes */
	  repodata_merge_attrs(data, p - pool->solvables, s - pool->solvables);
	}
    }

  if (pd.tmp)
    sat_free(pd.tmp);
  sat_free(line);
  sat_free(otherarchs);
  join_freemem();
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
}