/* Copyright (C) 1996-2001, 2002, 2003, 2004, 2005, 2006, 2008
   Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Thorsten Kukuk <kukuk@suse.de>, 1996.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <rpc/rpc.h>
#include <rpcsvc/nis.h>
#include <rpcsvc/yp.h>
#include <rpcsvc/ypclnt.h>
#include <rpcsvc/ypupd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define __set_errno(err) errno=err;
#define _(S) S
#define N_(S) S

/* This should only be defined on systems with a BSD compatible ypbind */
#ifndef BINDINGDIR
# define BINDINGDIR "/var/yp/binding"
#endif

#define USE_BINDINGDIR 1

struct dom_binding
  {
    struct dom_binding *dom_pnext;
    char dom_domain[YPMAXDOMAIN + 1];
    struct sockaddr_in dom_server_addr;
    int dom_socket;
    CLIENT *dom_client;
  };
typedef struct dom_binding dom_binding;

static const struct timeval RPCTIMEOUT = {60, 0};
static const struct timeval UDPTIMEOUT = {5, 0};
static int const MAXTRIES = 2;
static char ypdomainname[NIS_MAXNAMELEN + 1];
static dom_binding *ypbindlist = NULL;


static void
yp_bind_client_create (const char *domain, dom_binding *ysd,
		       struct ypbind_resp *ypbr)
{
  ysd->dom_server_addr.sin_family = AF_INET;
  memcpy (&ysd->dom_server_addr.sin_port,
	  ypbr->ypbind_resp_u.ypbind_bindinfo.ypbind_binding_port,
	  sizeof (ysd->dom_server_addr.sin_port));
  memcpy (&ysd->dom_server_addr.sin_addr.s_addr,
	  ypbr->ypbind_resp_u.ypbind_bindinfo.ypbind_binding_addr,
	  sizeof (ysd->dom_server_addr.sin_addr.s_addr));
  strncpy (ysd->dom_domain, domain, YPMAXDOMAIN);
  ysd->dom_domain[YPMAXDOMAIN] = '\0';

  ysd->dom_socket = RPC_ANYSOCK;
#ifdef SOCK_CLOEXEC
# define xflags SOCK_CLOEXEC
#else
# define xflags 0
#endif
  ysd->dom_client = clntudp_bufcreate (&ysd->dom_server_addr, YPPROG,
					      YPVERS, UDPTIMEOUT,
					      &ysd->dom_socket,
					      UDPMSGSIZE, UDPMSGSIZE);

  if (ysd->dom_client != NULL)
    {
#ifndef SOCK_CLOEXEC
      /* If the program exits, close the socket */
      if (fcntl (ysd->dom_socket, F_SETFD, FD_CLOEXEC) == -1)
	perror ("fcntl: F_SETFD");
#endif
    }
}

#if USE_BINDINGDIR
static void
yp_bind_file (const char *domain, dom_binding *ysd)
{
  char path[sizeof (BINDINGDIR) + strlen (domain) + 3 * sizeof (unsigned) + 3];

  snprintf (path, sizeof (path), "%s/%s.%u", BINDINGDIR, domain, YPBINDVERS);
  int fd = open (path, O_RDONLY);
  if (fd >= 0)
    {
      /* We have a binding file and could save a RPC call.  The file
	 contains a port number and the YPBIND_RESP record.  The port
	 number (16 bits) can be ignored.  */
      struct ypbind_resp ypbr;

      if (pread (fd, &ypbr, sizeof (ypbr), 2) == sizeof (ypbr))
	yp_bind_client_create (domain, ysd, &ypbr);

      close (fd);
    }
}
#endif

static int
yp_bind_ypbindprog (const char *domain, dom_binding *ysd)
{
  struct sockaddr_in clnt_saddr;
  struct ypbind_resp ypbr;
  int clnt_sock;
  CLIENT *client;

  clnt_saddr.sin_family = AF_INET;
  clnt_saddr.sin_port = 0;
  clnt_saddr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  clnt_sock = RPC_ANYSOCK;
  client = clnttcp_create (&clnt_saddr, YPBINDPROG, YPBINDVERS,
			   &clnt_sock, 0, 0);
  if (client == NULL)
    return YPERR_YPBIND;

  /* Check the port number -- should be < IPPORT_RESERVED.
     If not, it's possible someone has registered a bogus
     ypbind with the portmapper and is trying to trick us. */
  if (ntohs (clnt_saddr.sin_port) >= IPPORT_RESERVED)
    {
      clnt_destroy (client);
      return YPERR_YPBIND;
    }

  if (clnt_call (client, YPBINDPROC_DOMAIN,
		 (xdrproc_t) xdr_domainname, (caddr_t) &domain,
		 (xdrproc_t) xdr_ypbind_resp,
		 (caddr_t) &ypbr, RPCTIMEOUT) != RPC_SUCCESS)
    {
      clnt_destroy (client);
      return YPERR_YPBIND;
    }

  clnt_destroy (client);

  if (ypbr.ypbind_status != YPBIND_SUCC_VAL)
    {
      fprintf (stderr, "YPBINDPROC_DOMAIN: %s\n",
	       ypbinderr_string (ypbr.ypbind_resp_u.ypbind_error));
      return YPERR_DOMAIN;
    }
  memset (&ysd->dom_server_addr, '\0', sizeof ysd->dom_server_addr);

  yp_bind_client_create (domain, ysd, &ypbr);

  return YPERR_SUCCESS;
}

static int
__yp_bind (const char *domain, dom_binding **ypdb)
{
  dom_binding *ysd = NULL;
  int is_new = 0;

  if (domain == NULL || domain[0] == '\0')
    return YPERR_BADARGS;

  ysd = *ypdb;
  while (ysd != NULL)
    {
      if (strcmp (domain, ysd->dom_domain) == 0)
	break;
      ysd = ysd->dom_pnext;
    }

  if (ysd == NULL)
    {
      is_new = 1;
      ysd = (dom_binding *) calloc (1, sizeof *ysd);
      if (__builtin_expect (ysd == NULL, 0))
	return YPERR_RESRC;
    }

#if USE_BINDINGDIR
  /* Try binding dir at first if we have no binding */
  if (ysd->dom_client == NULL)
    yp_bind_file (domain, ysd);
#endif /* USE_BINDINGDIR */

  if (ysd->dom_client == NULL)
    {
      int retval = yp_bind_ypbindprog (domain, ysd);
      if (retval != YPERR_SUCCESS)
	{
	  if (is_new)
	    free (ysd);
	  return retval;
	}
    }

  if (ysd->dom_client == NULL)
    {
      if (is_new)
	free (ysd);
      return YPERR_YPSERV;
    }

  if (is_new)
    {
      ysd->dom_pnext = *ypdb;
      *ypdb = ysd;
    }

  return YPERR_SUCCESS;
}

static void
__yp_unbind (dom_binding *ydb)
{
  clnt_destroy (ydb->dom_client);
  free (ydb);
}

int
yp_bind (const char *indomain)
{
  int status;

  status = __yp_bind (indomain, &ypbindlist);

  return status;
}

static void
yp_unbind_locked (const char *indomain)
{
  dom_binding *ydbptr, *ydbptr2;

  ydbptr2 = NULL;
  ydbptr = ypbindlist;

  while (ydbptr != NULL)
    {
      if (strcmp (ydbptr->dom_domain, indomain) == 0)
	{
	  dom_binding *work;

	  work = ydbptr;
	  if (ydbptr2 == NULL)
	    ypbindlist = ypbindlist->dom_pnext;
	  else
	    ydbptr2 = ydbptr->dom_pnext;
	  __yp_unbind (work);
	  break;
	}
      ydbptr2 = ydbptr;
      ydbptr = ydbptr->dom_pnext;
    }
}

void
yp_unbind (const char *indomain)
{
  yp_unbind_locked (indomain);

  return;
}

static int
__ypclnt_call (const char *domain, u_long prog, xdrproc_t xargs,
	       caddr_t req, xdrproc_t xres, caddr_t resp, dom_binding **ydb,
	       int print_error)
{
  enum clnt_stat result;

  result = clnt_call ((*ydb)->dom_client, prog,
		      xargs, req, xres, resp, RPCTIMEOUT);

  if (result != RPC_SUCCESS)
    {
      /* We don't print an error message, if we try our old,
	 cached data. Only print this for data, which should work.  */
      if (print_error)
	clnt_perror ((*ydb)->dom_client, "do_ypcall: clnt_call");

      return YPERR_RPC;
    }

  return YPERR_SUCCESS;
}

static int
do_ypcall (const char *domain, u_long prog, xdrproc_t xargs,
	   caddr_t req, xdrproc_t xres, caddr_t resp)
{
  dom_binding *ydb;
  int status;
  int saved_errno = errno;

  status = YPERR_YPERR;

  ydb = ypbindlist;
  while (ydb != NULL)
    {
      if (strcmp (domain, ydb->dom_domain) == 0)
	{
          if (__yp_bind (domain, &ydb) == 0)
	    {
	      /* Call server, print no error message, do not unbind.  */
	      status = __ypclnt_call (domain, prog, xargs, req, xres,
				      resp, &ydb, 0);
	      if (status == YPERR_SUCCESS)
	        {
	          __set_errno (saved_errno);
	          return status;
	        }
	    }
	  /* We use ypbindlist, and the old cached data is
	     invalid. unbind now and create a new binding */
	  yp_unbind_locked (domain);

	  break;
	}
      ydb = ydb->dom_pnext;
    }

  /* First try with cached data failed. Now try to get
     current data from the system.  */
  ydb = NULL;
  if (__yp_bind (domain, &ydb) == 0)
    {
      status = __ypclnt_call (domain, prog, xargs, req, xres,
			      resp, &ydb, 1);
      __yp_unbind (ydb);
    }

#if USE_BINDINGDIR
  /* If we support binding dir data, we have a third chance:
     Ask ypbind.  */
  if (status != YPERR_SUCCESS)
    {
      ydb = calloc (1, sizeof (dom_binding));
      if (ydb != NULL && yp_bind_ypbindprog (domain, ydb) == YPERR_SUCCESS)
	{
	  status = __ypclnt_call (domain, prog, xargs, req, xres,
				  resp, &ydb, 1);
	  __yp_unbind (ydb);
	}
      else
	free (ydb);
    }
#endif

  __set_errno (saved_errno);

  return status;
}

/* Like do_ypcall, but translate the status value if necessary.  */
static int
do_ypcall_tr (const char *domain, u_long prog, xdrproc_t xargs,
	      caddr_t req, xdrproc_t xres, caddr_t resp)
{
  int status = do_ypcall (domain, prog, xargs, req, xres, resp);
  if (status == YPERR_SUCCESS)
    /* We cast to ypresp_val although the pointer could also be of
       type ypresp_key_val or ypresp_master or ypresp_order or
       ypresp_maplist.  But the stat element is in a common prefix so
       this does not matter.  */
    status = ypprot_err (((struct ypresp_val *) resp)->stat);
  return status;
}


int
yp_get_default_domain (char **outdomain)
{
  int result = YPERR_SUCCESS;;
  *outdomain = NULL;

  if (ypdomainname[0] == '\0')
    {
      if (getdomainname (ypdomainname, NIS_MAXNAMELEN))
	result = YPERR_NODOM;
      else if (strcmp (ypdomainname, "(none)") == 0)
	{
	  /* If domainname is not set, some systems will return "(none)" */
	  ypdomainname[0] = '\0';
	  result = YPERR_NODOM;
	}
      else
	*outdomain = ypdomainname;
    }
  else
    *outdomain = ypdomainname;

  return result;
}

int
__yp_check (char **domain)
{
  char *unused;

  if (ypdomainname[0] == '\0')
    if (yp_get_default_domain (&unused))
      return 0;

  if (domain)
    *domain = ypdomainname;

  if (yp_bind (ypdomainname) == 0)
    return 1;
  return 0;
}

int
yp_match (const char *indomain, const char *inmap, const char *inkey,
	  const int inkeylen, char **outval, int *outvallen)
{
  ypreq_key req;
  ypresp_val resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0' ||
      inmap == NULL || inmap[0] == '\0' ||
      inkey == NULL || inkey[0] == '\0' || inkeylen <= 0)
    return YPERR_BADARGS;

  req.domain = (char *) indomain;
  req.map = (char *) inmap;
  req.key.keydat_val = (char *) inkey;
  req.key.keydat_len = inkeylen;

  *outval = NULL;
  *outvallen = 0;
  memset (&resp, '\0', sizeof (resp));

  result = do_ypcall_tr (indomain, YPPROC_MATCH, (xdrproc_t) xdr_ypreq_key,
			 (caddr_t) &req, (xdrproc_t) xdr_ypresp_val,
			 (caddr_t) &resp);

  if (result != YPERR_SUCCESS)
    return result;

  *outvallen = resp.val.valdat_len;
  *outval = malloc (*outvallen + 1);
  int status = YPERR_RESRC;
  if (__builtin_expect (*outval != NULL, 1))
    {
      memcpy (*outval, resp.val.valdat_val, *outvallen);
      (*outval)[*outvallen] = '\0';
      status = YPERR_SUCCESS;
    }

  xdr_free ((xdrproc_t) xdr_ypresp_val, (char *) &resp);

  return status;
}

int
yp_first (const char *indomain, const char *inmap, char **outkey,
	  int *outkeylen, char **outval, int *outvallen)
{
  ypreq_nokey req;
  ypresp_key_val resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0' ||
      inmap == NULL || inmap[0] == '\0')
    return YPERR_BADARGS;

  req.domain = (char *) indomain;
  req.map = (char *) inmap;

  *outkey = *outval = NULL;
  *outkeylen = *outvallen = 0;
  memset (&resp, '\0', sizeof (resp));

  result = do_ypcall (indomain, YPPROC_FIRST, (xdrproc_t) xdr_ypreq_nokey,
		      (caddr_t) &req, (xdrproc_t) xdr_ypresp_key_val,
		      (caddr_t) &resp);

  if (result != RPC_SUCCESS)
    return YPERR_RPC;
  if (resp.stat != YP_TRUE)
    return ypprot_err (resp.stat);

  int status;
  if (__builtin_expect ((*outkey  = malloc (resp.key.keydat_len + 1)) != NULL
			&& (*outval = malloc (resp.val.valdat_len
					      + 1)) != NULL, 1))
    {
      *outkeylen = resp.key.keydat_len;
      memcpy (*outkey, resp.key.keydat_val, *outkeylen);
      (*outkey)[*outkeylen] = '\0';

      *outvallen = resp.val.valdat_len;
      memcpy (*outval, resp.val.valdat_val, *outvallen);
      (*outval)[*outvallen] = '\0';

      status = YPERR_SUCCESS;
    }
  else
    {
      free (*outkey);
      status = YPERR_RESRC;
    }

  xdr_free ((xdrproc_t) xdr_ypresp_key_val, (char *) &resp);

  return status;
}

int
yp_next (const char *indomain, const char *inmap, const char *inkey,
	 const int inkeylen, char **outkey, int *outkeylen, char **outval,
	 int *outvallen)
{
  ypreq_key req;
  ypresp_key_val resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0' ||
      inmap == NULL || inmap[0] == '\0' ||
      inkeylen <= 0 || inkey == NULL || inkey[0] == '\0')
    return YPERR_BADARGS;

  req.domain = (char *) indomain;
  req.map = (char *) inmap;
  req.key.keydat_val = (char *) inkey;
  req.key.keydat_len = inkeylen;

  *outkey = *outval = NULL;
  *outkeylen = *outvallen = 0;
  memset (&resp, '\0', sizeof (resp));

  result = do_ypcall_tr (indomain, YPPROC_NEXT, (xdrproc_t) xdr_ypreq_key,
			 (caddr_t) &req, (xdrproc_t) xdr_ypresp_key_val,
			 (caddr_t) &resp);

  if (result != YPERR_SUCCESS)
    return result;

  int status;
  if (__builtin_expect ((*outkey  = malloc (resp.key.keydat_len + 1)) != NULL
			&& (*outval = malloc (resp.val.valdat_len
					      + 1)) != NULL, 1))
    {
      *outkeylen = resp.key.keydat_len;
      memcpy (*outkey, resp.key.keydat_val, *outkeylen);
      (*outkey)[*outkeylen] = '\0';

      *outvallen = resp.val.valdat_len;
      memcpy (*outval, resp.val.valdat_val, *outvallen);
      (*outval)[*outvallen] = '\0';

      status = YPERR_SUCCESS;
    }
  else
    {
      free (*outkey);
      status = YPERR_RESRC;
    }

  xdr_free ((xdrproc_t) xdr_ypresp_key_val, (char *) &resp);

  return status;
}

int
yp_master (const char *indomain, const char *inmap, char **outname)
{
  ypreq_nokey req;
  ypresp_master resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0' ||
      inmap == NULL || inmap[0] == '\0')
    return YPERR_BADARGS;

  req.domain = (char *) indomain;
  req.map = (char *) inmap;

  memset (&resp, '\0', sizeof (ypresp_master));

  result = do_ypcall_tr (indomain, YPPROC_MASTER, (xdrproc_t) xdr_ypreq_nokey,
			 (caddr_t) &req, (xdrproc_t) xdr_ypresp_master,
			 (caddr_t) &resp);

  if (result != YPERR_SUCCESS)
    return result;

  *outname = strdup (resp.peer);
  xdr_free ((xdrproc_t) xdr_ypresp_master, (char *) &resp);

  return *outname == NULL ? YPERR_YPERR : YPERR_SUCCESS;
}

int
yp_order (const char *indomain, const char *inmap, unsigned int *outorder)
{
  struct ypreq_nokey req;
  struct ypresp_order resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0' ||
      inmap == NULL || inmap[0] == '\0')
    return YPERR_BADARGS;

  req.domain = (char *) indomain;
  req.map = (char *) inmap;

  memset (&resp, '\0', sizeof (resp));

  result = do_ypcall_tr (indomain, YPPROC_ORDER, (xdrproc_t) xdr_ypreq_nokey,
			 (caddr_t) &req, (xdrproc_t) xdr_ypresp_order,
			 (caddr_t) &resp);

  if (result != YPERR_SUCCESS)
    return result;

  *outorder = resp.ordernum;
  xdr_free ((xdrproc_t) xdr_ypresp_order, (char *) &resp);

  return result;
}

struct ypresp_all_data
{
  unsigned long status;
  void *data;
  int (*foreach) (int status, char *key, int keylen,
		  char *val, int vallen, char *data);
};

static bool_t
__xdr_ypresp_all (XDR *xdrs, struct ypresp_all_data *objp)
{
  while (1)
    {
      struct ypresp_all resp;

      memset (&resp, '\0', sizeof (struct ypresp_all));
      if (!xdr_ypresp_all (xdrs, &resp))
	{
	  xdr_free ((xdrproc_t) xdr_ypresp_all, (char *) &resp);
	  objp->status = YP_YPERR;
	  return FALSE;
	}
      if (resp.more == 0)
	{
	  xdr_free ((xdrproc_t) xdr_ypresp_all, (char *) &resp);
	  objp->status = YP_NOMORE;
	  return TRUE;
	}

      switch (resp.ypresp_all_u.val.stat)
	{
	case YP_TRUE:
	  {
	    char key[resp.ypresp_all_u.val.key.keydat_len + 1];
	    char val[resp.ypresp_all_u.val.val.valdat_len + 1];
	    int keylen = resp.ypresp_all_u.val.key.keydat_len;
	    int vallen = resp.ypresp_all_u.val.val.valdat_len;

	    /* We are not allowed to modify the key and val data.
	       But we are allowed to add data behind the buffer,
	       if we don't modify the length. So add an extra NUL
	       character to avoid trouble with broken code. */
	    objp->status = YP_TRUE;
	    *((char *) __mempcpy (key, resp.ypresp_all_u.val.key.keydat_val,
				  keylen)) = '\0';
	    *((char *) __mempcpy (val, resp.ypresp_all_u.val.val.valdat_val,
				  vallen)) = '\0';
	    xdr_free ((xdrproc_t) xdr_ypresp_all, (char *) &resp);
	    if ((*objp->foreach) (objp->status, key, keylen,
				  val, vallen, objp->data))
	      return TRUE;
	  }
	  break;
	default:
	  objp->status = resp.ypresp_all_u.val.stat;
	  xdr_free ((xdrproc_t) xdr_ypresp_all, (char *) &resp);
	  /* Sun says we don't need to make this call, but must return
	     immediately. Since Solaris makes this call, we will call
	     the callback function, too. */
	  (*objp->foreach) (objp->status, NULL, 0, NULL, 0, objp->data);
	  return TRUE;
	}
    }
}

int
yp_all (const char *indomain, const char *inmap,
	const struct ypall_callback *incallback)
{
  struct ypreq_nokey req;
  dom_binding *ydb = NULL;
  int try, res;
  enum clnt_stat result;
  struct sockaddr_in clnt_sin;
  CLIENT *clnt;
  struct ypresp_all_data data;
  int clnt_sock;
  int saved_errno = errno;

  if (indomain == NULL || indomain[0] == '\0'
      || inmap == NULL || inmap[0] == '\0')
    return YPERR_BADARGS;

  try = 0;
  res = YPERR_YPERR;

  while (try < MAXTRIES && res != YPERR_SUCCESS)
    {
      if (__yp_bind (indomain, &ydb) != 0)
	{
	  __set_errno (saved_errno);
	  return YPERR_DOMAIN;
	}

      clnt_sock = RPC_ANYSOCK;
      clnt_sin = ydb->dom_server_addr;
      clnt_sin.sin_port = 0;

      /* We don't need the UDP connection anymore.  */
      __yp_unbind (ydb);
      ydb = NULL;

      clnt = clnttcp_create (&clnt_sin, YPPROG, YPVERS, &clnt_sock, 0, 0);
      if (clnt == NULL)
	{
	  __set_errno (saved_errno);
	  return YPERR_PMAP;
	}
      req.domain = (char *) indomain;
      req.map = (char *) inmap;

      data.foreach = incallback->foreach;
      data.data = (void *) incallback->data;

      result = clnt_call (clnt, YPPROC_ALL, (xdrproc_t) xdr_ypreq_nokey,
			  (caddr_t) &req, (xdrproc_t) __xdr_ypresp_all,
			  (caddr_t) &data, RPCTIMEOUT);

      if (__builtin_expect (result != RPC_SUCCESS, 0))
	{
	  /* Print the error message only on the last try.  */
	  if (try == MAXTRIES - 1)
	    clnt_perror (clnt, "yp_all: clnt_call");
	  res = YPERR_RPC;
	}
      else
	res = YPERR_SUCCESS;

      clnt_destroy (clnt);

      if (res == YPERR_SUCCESS && data.status != YP_NOMORE)
	{
	  __set_errno (saved_errno);
	  return ypprot_err (data.status);
	}
      ++try;
    }

  __set_errno (saved_errno);

  return res;
}

int

yp_maplist (const char *indomain, struct ypmaplist **outmaplist)
{
  struct ypresp_maplist resp;
  enum clnt_stat result;

  if (indomain == NULL || indomain[0] == '\0')
    return YPERR_BADARGS;

  memset (&resp, '\0', sizeof (resp));

  result = do_ypcall_tr (indomain, YPPROC_MAPLIST, (xdrproc_t) xdr_domainname,
			 (caddr_t) &indomain, (xdrproc_t) xdr_ypresp_maplist,
			 (caddr_t) &resp);

  if (__builtin_expect (result == YPERR_SUCCESS, 1))
    {
      *outmaplist = resp.maps;
      /* We don't free the list, this will be done by ypserv
	 xdr_free((xdrproc_t)xdr_ypresp_maplist, (char *)&resp); */
    }

  return result;
}

const char *
yperr_string (const int error)
{
  const char *str;
  switch (error)
    {
    case YPERR_SUCCESS:
      str = N_("Success");
      break;
    case YPERR_BADARGS:
      str = N_("Request arguments bad");
      break;
    case YPERR_RPC:
      str = N_("RPC failure on NIS operation");
      break;
    case YPERR_DOMAIN:
      str = N_("Can't bind to server which serves this domain");
      break;
    case YPERR_MAP:
      str = N_("No such map in server's domain");
      break;
    case YPERR_KEY:
      str = N_("No such key in map");
      break;
    case YPERR_YPERR:
      str = N_("Internal NIS error");
      break;
    case YPERR_RESRC:
      str = N_("Local resource allocation failure");
      break;
    case YPERR_NOMORE:
      str = N_("No more records in map database");
      break;
    case YPERR_PMAP:
      str = N_("Can't communicate with portmapper");
      break;
    case YPERR_YPBIND:
      str = N_("Can't communicate with ypbind");
      break;
    case YPERR_YPSERV:
      str = N_("Can't communicate with ypserv");
      break;
    case YPERR_NODOM:
      str = N_("Local domain name not set");
      break;
    case YPERR_BADDB:
      str = N_("NIS map database is bad");
      break;
    case YPERR_VERS:
      str = N_("NIS client/server version mismatch - can't supply service");
      break;
    case YPERR_ACCESS:
      str = N_("Permission denied");
      break;
    case YPERR_BUSY:
      str = N_("Database is busy");
      break;
    default:
      str = N_("Unknown NIS error code");
      break;
    }
  return _(str);
}

static const int8_t yp_2_yperr[] =
  {
#define YP2YPERR(yp, yperr)  [YP_##yp - YP_VERS] = YPERR_##yperr
    YP2YPERR (TRUE, SUCCESS),
    YP2YPERR (NOMORE, NOMORE),
    YP2YPERR (FALSE, YPERR),
    YP2YPERR (NOMAP, MAP),
    YP2YPERR (NODOM, DOMAIN),
    YP2YPERR (NOKEY, KEY),
    YP2YPERR (BADOP, YPERR),
    YP2YPERR (BADDB, BADDB),
    YP2YPERR (YPERR, YPERR),
    YP2YPERR (BADARGS, BADARGS),
    YP2YPERR (VERS, VERS)
  };
int
ypprot_err (const int code)
{
  if (code < YP_VERS || code > YP_NOMORE)
    return YPERR_YPERR;
  return yp_2_yperr[code - YP_VERS];
}

const char *
ypbinderr_string (const int error)
{
  const char *str;
  switch (error)
    {
    case 0:
      str = N_("Success");
      break;
    case YPBIND_ERR_ERR:
      str = N_("Internal ypbind error");
      break;
    case YPBIND_ERR_NOSERV:
      str = N_("Domain not bound");
      break;
    case YPBIND_ERR_RESC:
      str = N_("System resource allocation failure");
      break;
    default:
      str = N_("Unknown ypbind error");
      break;
    }
  return _(str);
}

#define WINDOW 60

int
yp_update (char *domain, char *map, unsigned ypop,
	   char *key, int keylen, char *data, int datalen)
{
  union
    {
      ypupdate_args update_args;
      ypdelete_args delete_args;
    }
  args;
  xdrproc_t xdr_argument;
  unsigned res = 0;
  CLIENT *clnt;
  char *master;
  struct sockaddr saddr;
  char servername[MAXNETNAMELEN + 1];
  int r;

  if (!domain || !map || !key || (ypop != YPOP_DELETE && !data))
    return YPERR_BADARGS;

  args.update_args.mapname = map;
  args.update_args.key.yp_buf_len = keylen;
  args.update_args.key.yp_buf_val = key;
  args.update_args.datum.yp_buf_len = datalen;
  args.update_args.datum.yp_buf_val = data;

  if ((r = yp_master (domain, map, &master)) != YPERR_SUCCESS)
    return r;

  if (!host2netname (servername, master, domain))
    {
      fputs (_("yp_update: cannot convert host to netname\n"), stderr);
      free (master);
      return YPERR_YPERR;
    }

  clnt = clnt_create (master, YPU_PROG, YPU_VERS, "tcp");

  /* We do not need the string anymore.  */
  free (master);

  if (clnt == NULL)
    {
      clnt_pcreateerror ("yp_update: clnt_create");
      return YPERR_RPC;
    }

  if (!clnt_control (clnt, CLGET_SERVER_ADDR, (char *) &saddr))
    {
      fputs (_("yp_update: cannot get server address\n"), stderr);
      return YPERR_RPC;
    }

  switch (ypop)
    {
    case YPOP_CHANGE:
    case YPOP_INSERT:
    case YPOP_STORE:
      xdr_argument = (xdrproc_t) xdr_ypupdate_args;
      break;
    case YPOP_DELETE:
      xdr_argument = (xdrproc_t) xdr_ypdelete_args;
      break;
    default:
      return YPERR_BADARGS;
      break;
    }

  clnt->cl_auth = authdes_create (servername, WINDOW, &saddr, NULL);

  if (clnt->cl_auth == NULL)
    clnt->cl_auth = authunix_create_default ();

again:
  r = clnt_call (clnt, ypop, xdr_argument, (caddr_t) &args,
		 (xdrproc_t) xdr_u_int, (caddr_t) &res, RPCTIMEOUT);

  if (r == RPC_AUTHERROR)
    {
      if (clnt->cl_auth->ah_cred.oa_flavor == AUTH_DES)
	{
	  auth_destroy (clnt->cl_auth);
	  clnt->cl_auth = authunix_create_default ();
	  goto again;
	}
      else
	return YPERR_ACCESS;
    }
  if (r != RPC_SUCCESS)
    {
      clnt_perror (clnt, "yp_update: clnt_call");
      return YPERR_RPC;
    }
  return res;
}
