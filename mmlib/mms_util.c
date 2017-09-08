/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * Misc. functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */ 
#include <sys/file.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <dlfcn.h>
#include <strings.h>

#ifdef SunOS
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#endif

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mms_util.h"
#include "mms_queue.h"
#include "mms_uaprof.h"

static int smtp_send(char *relay_host, int port, Octstr *from, List *to, Octstr *msg);

Octstr *_mms_cfg_getx(mCfg *cfg, mCfgGrp *grp, Octstr *item)
{
     Octstr *v = mms_cfg_get(cfg, grp, item);
     
     void *x = v ? v : octstr_create("");

     return x;
}


int mms_load_core_settings(mCfg *cfg, mCfgGrp *cgrp)
{
     Octstr *log, *alog;
     Octstr *http_proxy_host;
     Octstr *our_interface;
     long loglevel = 0;
     
     if (cgrp == NULL)
	  panic(0,"Missing required group `core' in config file!");

     if (mms_cfg_get_int(cfg, cgrp, octstr_imm("log-level"), &loglevel) == -1)
	  loglevel = 0;
     
     /* Set the log file. */
     log = mms_cfg_get(cfg, cgrp, octstr_imm("log-file"));
     if (log != NULL) {
	  log_open(octstr_get_cstr(log), loglevel, GW_NON_EXCL);
	  octstr_destroy(log);
     }
     
     log_set_output_level(loglevel);

     /* Get access log and open it. */
     alog = mms_cfg_get(cfg, cgrp, octstr_imm("access-log"));
     if (alog) {
	  alog_open(octstr_get_cstr(alog), 1, 1);
	  octstr_destroy(alog);
     }

     if ((our_interface = mms_cfg_get(cfg, cgrp, octstr_imm("http-interface-name"))) != NULL) {
	  http_set_interface(our_interface);
	  octstr_destroy(our_interface);
     }

     /* look for http proxy. If set, use it. */
     if ((http_proxy_host = mms_cfg_get(cfg, cgrp, octstr_imm("http-proxy-host"))) != NULL) {
	  
	  Octstr *username = mms_cfg_get(cfg, cgrp, 
					 octstr_imm("http-proxy-username"));
	  Octstr *password = mms_cfg_get(cfg, cgrp, 
					 octstr_imm("http-proxy-password"));
	  List *exceptions = mms_cfg_get_list(cfg, cgrp,
					      octstr_imm("http-proxy-exceptions"));
	  Octstr *except_regex = mms_cfg_get(cfg, cgrp, 
					     octstr_imm("http-proxy-exceptions-regex"));
	  long http_proxy_port = -1; 
	  
	  mms_cfg_get_int(cfg, cgrp, octstr_imm("http-proxy-port"), &http_proxy_port);

	  if (http_proxy_port > 0)
	       http_use_proxy(http_proxy_host, http_proxy_port, 0,
			      exceptions, username, password, except_regex);
	  octstr_destroy(http_proxy_host);
	  octstr_destroy(username);
	  octstr_destroy(password);
	  octstr_destroy(except_regex);
	  gwlist_destroy(exceptions, octstr_destroy_item);
     }

#ifdef HAVE_LIBSSL
     /* We expect that gwlib_init() has been called already, so only need
      * to setup cert files.
      * -- adapted from gwlib/conn.c
      */
     {
	  Octstr *ssl_client_certkey_file = NULL;
	  Octstr *ssl_server_cert_file    = NULL;
	  Octstr *ssl_server_key_file     = NULL;
	  Octstr *ssl_trusted_ca_file     = NULL;
	  
	  /*
	   * check if SSL is desired for HTTP servers and then
	   * load SSL client and SSL server public certificates 
	   * and private keys
	   */    
	  ssl_client_certkey_file = mms_cfg_get(cfg, cgrp, octstr_imm("ssl-client-certkey-file"));
	  if (ssl_client_certkey_file != NULL) 
	       use_global_client_certkey_file(ssl_client_certkey_file);
	  
	  ssl_server_cert_file = mms_cfg_get(cfg, cgrp, octstr_imm("ssl-server-cert-file"));
	  ssl_server_key_file = mms_cfg_get(cfg, cgrp, octstr_imm("ssl-server-key-file"));
	  
	  if (ssl_server_cert_file != NULL && ssl_server_key_file != NULL) 
	       use_global_server_certkey_file(ssl_server_cert_file, 
					      ssl_server_key_file);
	  
	  ssl_trusted_ca_file = mms_cfg_get(cfg, cgrp, octstr_imm("ssl-trusted-ca-file"));
	  
	  use_global_trusted_ca_file(ssl_trusted_ca_file);
	  
	  octstr_destroy(ssl_client_certkey_file);
	  octstr_destroy(ssl_server_cert_file);
	  octstr_destroy(ssl_server_key_file);
	  octstr_destroy(ssl_trusted_ca_file);       
     }
#endif

     return 0;
}


Octstr *mms_maketransid(char *qf, Octstr *mmscname)
{
     Octstr *res;
     Octstr *x, *y = NULL;
     static int ct;
     
     if (!qf) 
	  x = octstr_format("msg.%ld.x%d.%d.%d",
			    (long)time(NULL) % 10000, (++ct % 1000), getpid()%100, random()%100);
     else
	  x = octstr_create(qf);
          
     res = octstr_format("%S-%S", mmscname, x);
     
     octstr_destroy(x);
     octstr_destroy(y);

     return res;
}

Octstr *mms_make_msgid(char *qf, Octstr *mmscname)
{/* Message ID is a little differently done. */
     Octstr *res;
     Octstr *x, *y = NULL;
     static int ct;
     
     if (!qf) 
	  x = octstr_format("msg.%ld.x%d.%d.%d",
			    (long)time(NULL) % 10000, (++ct % 1000), getpid()%100, random()%100);
     else
	  x = octstr_create(qf);
          
     if (mmscname)
	  res = octstr_format("%S@%S", x,mmscname);
     else 
	  res = octstr_duplicate(x);
     
     octstr_destroy(x);
     octstr_destroy(y);

     return res;
}

extern Octstr *mms_getqf_fromtransid(Octstr *transid)
{
     int i;
     
     if (transid == NULL)
	  return NULL;
     i = octstr_search_char(transid, '-', 0);
     if (i < 0)
	  i = octstr_search_char(transid, '@', 0); /* XXX backward compartibility. */
     
     return (i >= 0) ? octstr_copy(transid, i+1, octstr_len(transid)) : octstr_duplicate(transid);
}

extern Octstr *mms_getqf_from_msgid(Octstr *msgid)
{
     int i;
     
     if (msgid == NULL)
	  return NULL;
     if ((i = octstr_search_char(msgid, '@', 0)) > 0)
	  return octstr_copy(msgid, 0, i);
     else 
	  return mms_getqf_fromtransid(msgid); /* For older ones where transid = msgid. */
}

Octstr *mms_isodate(time_t t)
{
     Octstr *current_time;
     struct tm now;

     now = gw_gmtime(t);
     current_time = octstr_format("%04d-%02d-%02dT%02d:%02d:%02dZ", 
				  now.tm_year + 1900, now.tm_mon + 1, 
				  now.tm_mday, now.tm_hour, now.tm_min, 
				  now.tm_sec);

     return current_time;
}

void mms_lib_init(void)
{
     srandom(time(NULL)); /* Seed random number generator. */
     gwlib_init();
     mms_strings_init();
}

void mms_lib_shutdown(void)
{
     mms_strings_shutdown();
     gwlib_shutdown();
}

void strip_quotes(Octstr *s)
{
     int l = s ? octstr_len(s) : 0;

     if (l == 0)
	  return;
     if (octstr_get_char(s, 0) == '"') {
	  octstr_delete(s, 0, 1);
	  l--;
     }
     if (octstr_get_char(s, l-1) == '"')
	  octstr_delete(s, l-1, 1);     
}

List  *get_value_parameters(Octstr *params)
{
     int i,n, k = 0;
     List *h = http_create_empty_headers();
     Octstr *xparams = octstr_duplicate(params);

     octstr_format_append(xparams, ";"); /* So parsing is easier. (aka cheap hack) */

     for (i = 0, n = octstr_len(xparams); i < n; i++) {
	  int c = octstr_get_char(xparams, i);

	  if (c == ';') {
	       int j  = octstr_search_char(xparams, '=', k);
	       Octstr *name, *value;
	       if (j > 0 && j < i) {
		    name = octstr_copy(xparams, k, j - k);
		    value = octstr_copy(xparams, j+1,i-j-1);
		    octstr_strip_blanks(name);
		    octstr_strip_blanks(value);
		    strip_quotes(value);
		    if (octstr_len(name) > 0)
			 http_header_add(h, 
					 octstr_get_cstr(name), 
					 octstr_get_cstr(value));
		    octstr_destroy(name); 
		    octstr_destroy(value);
	       }
	       k = i + 1;
	  } else if (c == '"') 
	       i += http_header_quoted_string_len(xparams, i) - 1;	  
     }
     octstr_destroy(xparams);
     return h;
}

int split_header_value(Octstr *value, Octstr **base_value, Octstr **params)
{

     int i, n;
     for (i = 0, n = octstr_len(value); i < n; i++) {
	  int c = octstr_get_char(value, i);

	  if (c == ';')
	       break;
	  else if (c == '"') 
	       i += http_header_quoted_string_len(value, i) - 1;	  
     }

     *base_value = octstr_duplicate(value);     
     if (i < n) {
	  *params = octstr_copy(value, i+1, octstr_len(value));
	  octstr_delete(*base_value, i, octstr_len(*base_value));
     } else 
	  *params = octstr_create("");
     return 0;

}

int get_content_type(List *hdrs, Octstr **type, Octstr **params)
{
     
     Octstr *v;
     
     v = http_header_find_first(hdrs, "Content-Type");	  
     *params =NULL;

     if (!v) {
	  *type = octstr_create("application/octet-stream");
	  *params = octstr_create("");
	  return -1;          
     }

     split_header_value(v, type, params);

     octstr_destroy(v);
     return 0;
}

static int is_mime_special_char(int ch)
{
     const char *x = "=;<>[]?()@:\\/,";
     char *p;
     for (p = (char *)x; *p; p++)
	  if (ch == *p)
	       return 1;
     return 0;
}
static int needs_quotes(Octstr *s)
{
     int i, n;
     if (!s) 
	  return 0;
     
     for (i = 0, n = octstr_len(s); i<n; i++) {
	  int ch = octstr_get_char(s,i);
	  if (isspace(ch) || is_mime_special_char(ch))
	       return 1;
     }
     return 0;
}

Octstr *make_value_parameters(List *params)
{
     Octstr *s = octstr_create(""), *name, *value;
     int i, n;

     for (i = 0, n = params ? gwlist_len(params) : 0; i<n; i++) {
	  int space;
	  http_header_get(params, i, &name, &value);
	  space = needs_quotes(value);
	  octstr_format_append(s, "%s%S=%s%S%s", 
			       (i==0) ? "" : "; ", 
			       name, 
			       (space) ? "\"" : "",
			       value,
			       (space) ? "\"" : "");
	  octstr_destroy(name);
	  octstr_destroy(value);
     }
     return s;
}

/* Take each header with a comma separated set of values (for To,Cc,Bcc),
 * re-create as a series of header/value pairs.
 * Remove all non-conformant headers (e.g. old unix-style from 
 */
void unpack_mimeheaders(MIMEEntity *mm)
{
     int i, n;
     List *h = http_create_empty_headers();
     List *headers = mime_entity_headers(mm);
     
     
     for (i = 0, n = gwlist_len(headers); i<n; i++) {
	  Octstr *header = NULL, *value = NULL;
	  List *l = NULL;
	  int j, m;
	  int skip;
	  
	  http_header_get(headers, i, &header, &value);
	  
	  if (header == NULL ||
	      octstr_str_compare(header, "X-Unknown") == 0 ||
	      octstr_search_chars(header, octstr_imm(" \n\t"), 0) >= 0) /* Don't allow space in the name. */ 
	       goto loop;
	  
	  if (octstr_case_compare(header, octstr_imm("Cc")) == 0 ||
	      octstr_case_compare(header, octstr_imm("To")) == 0 ||
	      octstr_case_compare(header, octstr_imm("Bcc")) == 0) 
	       skip = 0;
	  else 
	       skip = 1;
	  /* XXX This may not be safe. Need to skip over quotes. */
	  if (!skip && octstr_search_char(value, ',', 0) > 0 && 
	      (l = http_header_split_value(value)) != NULL &&
	      gwlist_len(l) > 1) 
	       for (j = 0, m = gwlist_len(l); j<m; j++) 
		    http_header_add(h, octstr_get_cstr(header), 
				    octstr_get_cstr(gwlist_get(l, j)));
	  else
	       http_header_add(h, octstr_get_cstr(header), 
			       octstr_get_cstr(value));
	  
	  if (l) gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
	  
     loop:
	  octstr_destroy(header);
	  octstr_destroy(value);	  	  
     }

     mime_replace_headers(mm, h);
     http_destroy_headers(headers);
     http_destroy_headers(h);

}


/* Undo base64 content coding for mime entities that need it. */
void unbase64_mimeparts(MIMEEntity *m)
{
     int i, n;
     
     if ((n = mime_entity_num_parts(m)) > 0)
	  for (i = 0; i<n; i++) {
	       MIMEEntity *x = mime_entity_get_part(m, i);
	       unbase64_mimeparts(x);
	       mime_entity_replace_part(m, i, x);
	       mime_entity_destroy(x);
	  }
     else { /* A non-multipart message .*/
	  List *headers = mime_entity_headers(m);
	  Octstr *ctype = http_header_value(headers, octstr_imm("Content-Type"));
	  Octstr *te = http_header_value(headers, octstr_imm("Content-Transfer-Encoding"));
	  
	  if (DRM_CONTENT_TYPE(ctype))
	       goto done; /* leave it alone! */
	  if (ctype && te &&
	      octstr_case_compare(te,octstr_imm("base64")) == 0) {
	       Octstr *s = mime_entity_body(m);
	       if (s) {
		    octstr_base64_to_binary(s);
		    mime_entity_set_body(m, s);
		    octstr_destroy(s);
	       }
	  }
	  if (headers) {
	       http_header_remove_all(headers, "Content-Transfer-Encoding"); /* Remove it in all cases (?).*/
	       mime_replace_headers(m, headers);
	  }
     done:
	  /* XXX may be we should deal with other transfer encodings here as well... */
	  
	  octstr_destroy(ctype);	  
	  octstr_destroy(te);
	  http_destroy_headers(headers);
     }
}

int _mms_gw_isprint(int c)
{
     return isprint(c) || isspace(c);
}


/* Change content coding for mime entities that need it. */
void base64_mimeparts(MIMEEntity *m, int all)
{
     int i, n;

     if ((n = mime_entity_num_parts(m)) > 0)
	  for (i = 0; i<n; i++) {
	       MIMEEntity *x = mime_entity_get_part(m, i);
	       base64_mimeparts(x, all);
	       mime_entity_replace_part(m, i, x);
	       mime_entity_destroy(x);
	  }
     else { /* A non-multipart message .*/
	  List *headers = mime_entity_headers(m);
	  Octstr *ctype = http_header_value(headers, octstr_imm("Content-Type"));
	  Octstr *te = http_header_value(headers, octstr_imm("Content-Transfer-Encoding"));
	  Octstr *body = mime_entity_body(m);
	  if (ctype && 
	      (te == NULL || octstr_str_case_compare(te, "binary") == 0) &&  
	      body && 
	      (all || octstr_check_range(body, 0, octstr_len(body), _mms_gw_isprint) == 0) &&
	      !DRM_CONTENT_TYPE(ctype)) { /* don't touch drm content object. */
	       octstr_binary_to_base64(body);	      
	       
	       http_header_remove_all(headers, "Content-Transfer-Encoding");
	       http_header_add(headers, "Content-Transfer-Encoding", "base64");
	       mime_entity_set_body(m, body);
	       mime_replace_headers(m, headers);
	  }
	  octstr_destroy(ctype);	  
	  octstr_destroy(te);
	  octstr_destroy(body);
	  http_destroy_headers(headers);
     }
}

void addmmscname(Octstr *s, char *myhostname)
{
     int j;
     int len = octstr_len(s);
     
     if (octstr_search_char(s, '@', 0) >= 0)
	  return; /* Nothing to do. */
     
     j = octstr_case_search(s, octstr_imm("/TYPE=PLMN"), 0);
     if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len)  /* A proper number. */	
	  octstr_format_append(s, "@%s", myhostname);
     
     
}

static int send2email(List *xto, Octstr *from, Octstr *subject, 
		      Octstr *msgid,
		      MIMEEntity *m, int append_hostname, Octstr **error, 
		      char *sendmail_cmd, Octstr *myhostname,
		      Octstr *relay_host, int relay_port)
{
     Octstr *s;
     FILE *f;
     int ret = MMS_SEND_OK, i, n, fd = -1;
     char fname[] = "/tmp/mms.XXXXXX";

     
     Octstr *cmd = octstr_create(""); 
     List *headers = mime_entity_headers(m); /* we don't want the mime version header removed. */
     Octstr *to = NULL;

     /* Make to address */
     
     if (append_hostname) { /* Add our hostname to all phone numbers. */
	  List *l = http_create_empty_headers();
	  Octstr *xfrom = http_header_value(headers, octstr_imm("From"));
	  List *lto = http_header_find_all(headers, "To");
	  List *lcc = http_header_find_all(headers, "Cc");
	  
	  if (xfrom) {
	       addmmscname(xfrom, octstr_get_cstr(myhostname));
	       http_header_add(l, "From", octstr_get_cstr(xfrom));
	       octstr_destroy(xfrom);
	  }
	  http_header_remove_all(headers, "From");
	
	  for (i = 0, n = gwlist_len(lto); i < n; i++) {	 
	       Octstr *name, *value;	 
	       
	       http_header_get(lto, i, &name, &value);	 
	       
	       if (!value || !name ||	 
		   octstr_case_compare(name, octstr_imm("To")) != 0)	 
		    goto loop;	 
	       
	       addmmscname(value, octstr_get_cstr(myhostname));	 
	       http_header_add(l, "To", octstr_get_cstr(value));	 
	  loop:	 
	       octstr_destroy(value);	 
	       octstr_destroy(name);	 
	  }	 
 	 
	  http_destroy_headers(lto);	 
	  http_header_remove_all(headers, "To");	 
	  
	  for (i = 0, n = gwlist_len(lcc); i < n; i++) {	 
	       Octstr *name, *value;	 
	       
	       http_header_get(lcc, i, &name, &value);	 
	       
	       if (!value || !name ||	 
		   octstr_case_compare(name, octstr_imm("Cc")) != 0)	 
		    goto loop2;	 
	       
	       addmmscname(value, octstr_get_cstr(myhostname));	 
	       http_header_add(l, "Cc", octstr_get_cstr(value));	 
	  loop2:	 
	       octstr_destroy(value);	 
	       octstr_destroy(name);	 
	  }	 
	  
	  http_destroy_headers(lcc);	 
	  http_header_remove_all(headers, "Cc");	 
   
	  http_append_headers(headers, l); /* combine old with new. */
	  http_destroy_headers(l);
     }
	  
     /* Pack headers, get string rep of mime entity. */
     http_header_pack(headers); 
     mime_replace_headers(m, headers);
     s = mime_entity_to_octstr(m);

     if (relay_host && relay_port > 0) {
	  ret = smtp_send(octstr_get_cstr(relay_host), relay_port, from, xto, s);
	  goto done;
     }
     /* 
      *	Make the command: Transpose % formatting characters:
      * f - from address
      * t - recipient
      * s - subject
      * m - message id 
      */

     LINEARISE_STR_LIST(to,xto," ");
     i = 0;
     for (;;) {
	  Octstr *tmp;
	  while (sendmail_cmd[i]) {
	       char c = sendmail_cmd[i];
	       if (c == '%' && sendmail_cmd[i + 1])
		    break;
	       octstr_append_char(cmd, c);
	       i++;
	  }
	  if (!sendmail_cmd[i])
	       break;
	  
	  switch(sendmail_cmd[i+1]) {
	  case 't':
	       tmp = octstr_duplicate(to);
	       escape_shell_chars(tmp);
	       octstr_append(cmd, tmp);
	       octstr_destroy(tmp);
	       break;
	  case 'f':
	       if (append_hostname) {
		    Octstr *xfrom = octstr_duplicate(from);
		    addmmscname(xfrom, octstr_get_cstr(myhostname));
		    escape_shell_chars(xfrom);

		    octstr_append(cmd, xfrom);
		    octstr_destroy(xfrom);
	       } else {
		    tmp = octstr_duplicate(from);
		    escape_shell_chars(tmp);		    
		    octstr_append(cmd, tmp);
		    octstr_destroy(tmp);
	       }
	       break;
	  case 's':
	       tmp = octstr_duplicate(subject);
	       escape_shell_chars(tmp);
	       octstr_append(cmd, subject);
	       octstr_destroy(tmp);
	       break;
	  case 'm':
	       tmp = octstr_duplicate(msgid);
	       escape_shell_chars(tmp);
	       octstr_append(cmd, msgid);
	       octstr_destroy(tmp);
	       break;
	  case '%':
	       octstr_format_append(cmd, "%%");
	       break;
	  default:
	       octstr_format_append(cmd, "%%%c", sendmail_cmd[i+1]);
	       break;
	  }
	  i += 2;
     }


     if ((fd = mkstemp(fname)) < 0) {
	  *error = octstr_format("mkstemp: Failed to create temporary file: %s", 
				 strerror(errno));
	  ret = MMS_SEND_ERROR_TRANSIENT;
	  goto done;
     }
     if ((f = fopen(fname, "w"))  == NULL) {
	  *error = octstr_format("fopen failed for %s:  %s",
				 fname, strerror(errno));
	  ret = MMS_SEND_ERROR_TRANSIENT;
	  goto done;
     }

     if (octstr_print(f, s) < 0) {
	  *error = octstr_format("send email failed in write temp file %d: %s",
				 errno, strerror(errno));
	  fclose(f);
	  ret = MMS_SEND_ERROR_TRANSIENT;	  
	  goto done;
     } else 
	  fclose(f);
     
     octstr_format_append(cmd, " < '%s'", fname);
     
     debug("mms.sendtoemail", 0, "preparing to execute %s to send to email: ", octstr_get_cstr(cmd));
     
     if (system(octstr_get_cstr(cmd)) != 0) {
	  *error = octstr_format("system(%S) failed: %d: %s",
				 cmd, errno, strerror(errno));
	  ret = MMS_SEND_ERROR_TRANSIENT;
	  goto done;
     } else 
	  ret = MMS_SEND_QUEUED;

done:
     if (fd >= 0) {
	  close(fd);
	  unlink(fname);
     }
     http_destroy_headers(headers);
     octstr_destroy(cmd);
     octstr_destroy(s);
     octstr_destroy(to);
     return ret;
}

static int smtp_send(char *relay_host, int port, Octstr *from, List *to, Octstr *msg)
{
     int fd = tcpip_connect_to_server(relay_host, port, NULL);
     Connection *c;
     Octstr *l;
     List *xl;
     int i, ret = -1, code;
     char tbuf[512];
     
     if (fd < 0 || (c = conn_wrap_fd(fd,0)) == NULL)
	  return -1;

     socket_set_blocking(fd,1); /* Because we want each line as it comes */

     l = conn_read_line(c); /* Get greeting. */

     if (l == NULL)
	  goto done;
     tbuf[0] = 0;
     sscanf(octstr_get_cstr(l), "%*d %128s", tbuf);
     octstr_destroy(l);
     
#define SANDC(fmt,arg,val,cont) do {			\
	  Octstr *y, *x = octstr_format(fmt,arg);	\
	  conn_write(c,x);				\
	  octstr_destroy(x);				\
	  if ((y = conn_read_line(c)) == NULL)		\
	       goto done;				\
	  sscanf(octstr_get_cstr(y), "%d", &code);	\
	  octstr_destroy(y);				\
	  if (code != (val) && !(cont))			\
	       goto done;				\
     } while (0)					\
							
     SANDC("HELO %s\r\n", tbuf, 250,0);
     SANDC("MAIL FROM:<%S>\r\n", from, 250,0);

     for (i = 0; i<gwlist_len(to); i++) {
	  Octstr *xto = gwlist_get(to, i);
	  
	  SANDC("RCPT TO:<%S>\r\n", xto, 250, 1);

	  if (code == 250) {
	       octstr_destroy(xto); /* Sent */
	       gwlist_delete(to, i, 1);
	       i--;
	  }
     }
     SANDC("DATA\r\n", 0, 354, 0);

     xl = octstr_split(msg, octstr_imm("\n"));
     
     while ((l = gwlist_extract_first(xl)) != NULL) {
	  
	  if (octstr_get_char(l, 0) == '.')
	       octstr_insert_char(l, 0, '.');

	  if (octstr_get_char(l, octstr_len(l) - 1) != '\r')
	       octstr_append_char(l, '\r');
	  octstr_append_char(l, '\n');

	  conn_write(c, l);	  
	  octstr_destroy(l);
     }

     gwlist_destroy(xl, NULL);
     SANDC(".\r\n", 0, 250, 0);
     conn_write(c, octstr_imm("QUIT\r\n"));
     ret = 0;
done:
     conn_destroy(c);
     return ret;
}

int mm_send_to_email(Octstr *to, Octstr *from, Octstr *subject, 
		     Octstr *msgid,
		     MIMEEntity *m, int append_hostname, Octstr **error, 
		     char *sendmail_cmd, Octstr *myhostname,
		     Octstr *relay_host, int relay_port)
{
     List *lto = gwlist_create_ex(to);
     int ret = send2email(lto,from,subject,msgid,m,append_hostname,error,sendmail_cmd,myhostname, relay_host, relay_port);
     gwlist_destroy(lto, NULL);
     return ret;
}

/* Send this message to email recipient. */
int mms_sendtoemail(Octstr *from, List *lto, 
		    Octstr *subject, Octstr *msgid,
		    MmsMsg *msg, int dlr, 
		    Octstr **error, char *sendmail_cmd,		    
		    Octstr *myhostname, 
		    int trans_msg,
		    int trans_smil, char *txt, char *html,
		    int mm4, 
		    char *transid, 
		    List *extra_headers, 
		    Octstr *relay_host, int relay_port)
{
     MIMEEntity *m = NULL;
     List *headers = NULL;
     List *newhdrs = http_create_empty_headers();
     int  ret = 0, mtype;
     Octstr *to;
     
     gw_assert(msg);     
     mtype = mms_messagetype(msg);

     /* Make 'to' header field in case we need it. */
     LINEARISE_STR_LIST(to, lto, ", ");
     
     if (!to || 
	 octstr_search_char(to, '@', 0) < 0) {
	  *error = octstr_format("Invalid email address %S!", to);
	  octstr_destroy(to);
	  return MMS_SEND_ERROR_FATAL;
     }
   
     if (!trans_msg)
	  m = mms_tomime(msg,0);
     else if ((ret = mms_format_special(msg, trans_smil, txt, html, &m)) < 0 ||
	      m == NULL) {
	  mms_warning(0, "send2email", NULL, "Failed to format message (msg=%s,ret=%d)",
		      m ? "OK" : "Not transformed",ret);
	  octstr_destroy(to);
	  return -ret;
     }      
     
     base64_mimeparts(m,0); /* make sure parts are base64 formatted. */

     if (extra_headers) /* add any other headers into the mix. */
	  http_header_combine(newhdrs, extra_headers);

     headers = mime_entity_headers(m);
     
     /* Before we send it, we insert some email friendly headers if they are missing. */
     if (!mm4) {
	  http_header_add(newhdrs, "Subject", subject ? octstr_get_cstr(subject) : "MMS Message");
	  http_header_remove_all(headers, "From");
	  http_header_add(newhdrs, "From", octstr_get_cstr(from));
#if 0 /* Should we reveal recipients ?? */
	  http_header_remove_all(headers, "To");
	  http_header_add(newhdrs, "To", octstr_get_cstr(to));
#endif
	  http_header_add(newhdrs, "Message-ID", msgid ? octstr_get_cstr(msgid) : "");
	  http_header_add(newhdrs, "MIME-Version", "1.0");
     } else {
	  char *x, tmp[32];	  
	  Octstr *xsender = octstr_format("system-user@%S", myhostname);
	  Octstr *y;

	  if (msgid) {
	       y =  (octstr_get_char(msgid, 0) == '"') ? octstr_duplicate(msgid) : 
		    octstr_format("\"%S\"", msgid);
	       
	       http_header_add(newhdrs, "X-Mms-Message-ID", octstr_get_cstr(y));
	       octstr_destroy(y);
	  }

	  /* fixup messageid */
	  if ((y = http_header_value(headers, octstr_imm("Message-ID"))) != NULL) {
	       if (octstr_get_char(y, 0) != '<') {
		    octstr_insert_char(y, 0, '<');
		    octstr_append_char(y, '>');
	       }
	       http_header_remove_all(headers, "Message-ID");

	       http_header_add(newhdrs, "Message-ID", octstr_get_cstr(y));
	       octstr_destroy(y); 	       	       
	  }	 
	  sprintf(tmp, "%d.%d.%d", 
		  MAJOR_VERSION(MMS_3GPP_VERSION),
		  MINOR1_VERSION(MMS_3GPP_VERSION),
		  MINOR2_VERSION(MMS_3GPP_VERSION));
	  http_header_add(newhdrs, "X-Mms-3GPP-MMS-Version", tmp);
	  http_header_add(newhdrs, "X-Mms-Originator-System",
			  octstr_get_cstr(xsender));
	  http_header_remove_all(headers, "X-Mms-Message-Type");
	  if (mtype == MMS_MSGTYPE_SEND_REQ ||
	      mtype == MMS_MSGTYPE_RETRIEVE_CONF)
	       x = "MM4_forward.REQ";
	  else if (mtype == MMS_MSGTYPE_DELIVERY_IND) {
	       Octstr *s = http_header_value(headers, octstr_imm("X-Mms-Status"));
	       x  = "MM4_delivery_report.REQ";
#if 0
	       /* insert FROM address as recipient as per spec */
	       http_header_add(newhdrs, "From", octstr_get_cstr(to));
#else
	       http_header_add(newhdrs, "To", octstr_get_cstr(to));
#endif
	       /* rename status header. */
	       http_header_remove_all(headers, "X-Mms-Status");
	       http_header_add(newhdrs, "X-Mms-MM-Status-Code", 
			       s ? octstr_get_cstr(s) : "Unrecognised");
	       if (!s)
		    mms_warning(0, NULL, NULL, "MMS Delivery report with missing Status!");
	       octstr_destroy(s);
	  } else if (mtype == MMS_MSGTYPE_READ_REC_IND) {
	       x = "MM4_read_reply_report.REQ";
#if 0
	       /* insert FROM address as recipient as per spec */
	       http_header_add(newhdrs, "From", octstr_get_cstr(to));
#else
	       http_header_add(newhdrs, "To", octstr_get_cstr(to));
#endif
	  } else {
	       *error = octstr_format("Invalid message type %s on MM4 outgoing interface!", 
				      mms_message_type_to_cstr(mtype));
	       x = "";
	       ret =  MMS_SEND_ERROR_FATAL;
	       goto done;
	  }


	  http_header_add(newhdrs, "X-Mms-Message-Type", x);
	  /* Add a few more MM4 headers. */
	  http_header_add(newhdrs, "X-Mms-Ack-Request", dlr ? "Yes" : "No");
	  http_header_add(newhdrs, "Sender", octstr_get_cstr(from));

	  y =  (transid && transid[0] == '"') ? octstr_create(transid) : 
	       octstr_format("\"%s\"", transid ? transid : "x");

	  http_header_add(newhdrs, "X-Mms-Transaction-ID", octstr_get_cstr(y));

	  octstr_destroy(y);
	  octstr_destroy(xsender);
     }
     
     http_header_combine(headers, newhdrs); 
     mime_replace_headers(m, headers);
     
done:
     http_destroy_headers(headers);
     http_destroy_headers(newhdrs);
     octstr_destroy(to);
     if (ret == 0)
	  ret = send2email(lto, 
			   from, subject, msgid, m, mm4 == 0, error, sendmail_cmd, myhostname, relay_host, relay_port);
     mime_entity_destroy(m);

     return ret;
}

void mms_log2(char *logmsg, Octstr *from, Octstr *to, 
	      int msize, Octstr *msgid,
	      Octstr *acct,
	      Octstr *viaproxy,
	      char *interface, Octstr *ua, Octstr *mmboxloc)
{
     List *l;
     if (to) {
	  l = gwlist_create();
	  gwlist_append(l, to);
     } else
	  l = NULL;
     
     mms_log(logmsg, from,l,msize,msgid,acct,viaproxy,interface,ua,mmboxloc);
     
     if (l) 
	  gwlist_destroy(l, NULL);
}

void mms_log(char *logmsg, Octstr *from, List *to, 
	     int msize, Octstr *msgid,
	     Octstr *acct,
	     Octstr *viaproxy,
	     char *interface, Octstr *ua, Octstr *mmboxloc)
{
     Octstr *xto = octstr_create("");
     int i, n = to ? gwlist_len(to) : 0;
     Octstr *xfrom = from ? octstr_duplicate(from) : NULL;
     int j = xfrom ? octstr_case_search(xfrom, octstr_imm("/TYPE=PLMN"), 0) : -1;

     if (j >= 0)
	  octstr_delete(xfrom, j, octstr_len(xfrom));
	
     for (i = 0; i < n; i++) {
	  void *y;
	  Octstr *x = (y = gwlist_get(to,i)) ? octstr_duplicate(y) : NULL;
	  int   j = x ? octstr_case_search(x, octstr_imm("/TYPE=PLMN"), 0) : -1;
	  
	  if (j >= 0)
	       octstr_delete(x, j, octstr_len(x));
	  
	  octstr_format_append(xto, 
			       "%s%S",
			       (i == 0) ? "" : ", ",
			       x);
	  
	  octstr_destroy(x);
     }
       
     alog("%s MMS [INT:%s] [ACT:%s] [MMSC:%s] [from:%s] [to:%s] [msgid:%s] [size=%d] [UA:%s] [MMBox:%s]", 
	  logmsg, interface, 
	  acct ? octstr_get_cstr(acct) : "",
	  viaproxy ? octstr_get_cstr(viaproxy) : "",
	  xfrom ? octstr_get_cstr(xfrom) : "",
	  octstr_get_cstr(xto),
	  msgid ? octstr_get_cstr(msgid) : "",
	  msize,
	  ua ? octstr_get_cstr(ua) : "", 
	  mmboxloc ? octstr_get_cstr(mmboxloc) : "");

     octstr_destroy(xto);
     octstr_destroy(xfrom);
}


/* Compare a file_lock(lhs) to the file_key(rhs)
 *   and see if they match
 */
int file_lock_inode_cmp(void *_lhs, void *_rhs);

/* Each file gets a condition, there is only a single file_loc
   for each inode number.  Assumes a uni
*/
typedef struct {
     dev_t dev;
     ino_t inode;
} file_key;

typedef struct {
     file_key key;
     pthread_cond_t condition;
     int fd;
} file_lock;

static List *openFileList = NULL;
static pthread_mutex_t listMutex = PTHREAD_MUTEX_INITIALIZER;

void release_file_lock(int fd, file_key *key)
{
     debug("mm_util",0,"----->Locked");
     pthread_mutex_lock(&listMutex);

     if (openFileList == NULL) {
	  openFileList = (List *)gwlist_create();
     }
    
     file_lock *item = (file_lock*)gwlist_search(openFileList, key, file_lock_inode_cmp);
     if (item && item->fd == fd) {
	  /* we own the lock */
	  gwlist_delete_equal(openFileList, item);
	  pthread_cond_broadcast(&item->condition);
	  pthread_cond_destroy(&item->condition);
	  gw_free(item);
     }
     debug("mm_util",0,"<-----UnLocked");
     pthread_mutex_unlock(&listMutex);
}

int unlock_and_close(int fd)
{
#ifdef SunOS
     struct stat buf;
     if (fstat(fd, &buf)) {
	  perror("Unable to fstat file for lock");
	  return close(fd);
     }

     file_key key;
     key.inode = buf.st_ino;
     key.dev   = buf.st_dev;
    
     release_file_lock(fd, &key);
#endif

     return close(fd);
}

int unlock_and_fclose(FILE *fp) 
{
#ifdef SunOS
     int   fd = fileno(fp);
     struct stat buf;
     if (fstat(fd, &buf)) {
	  perror("Unable to fstat file for lock");
	  return fclose(fp);
     }

     file_key key;
     key.inode = buf.st_ino;
     key.dev   = buf.st_dev;

     release_file_lock(fd, &key);
#endif
     return fclose(fp);
}

/* Compare a file_lock(lhs) to the file_key(rhs)
   and see if they match
*/
int file_lock_inode_cmp(void *_lhs, void *_rhs) {
     file_key *rhs = (file_key*)_rhs;
     file_lock *lhs = (file_lock *)_lhs;

     return (
	  lhs && 
	  lhs->key.inode == rhs->inode && 
	  lhs->key.dev   == rhs->dev
	  );
}

int sun_lockfile(int fd, int shouldblock)
{

#ifdef SunOS
     int n, stop;     
     int flg = shouldblock ? F_SETLKW : F_SETLK;
     flock_t lock;

     struct stat buf;
     if (fstat(fd, &buf)) {
	  int e = errno;
	  perror("Unable to fstat file for lock");
	  errno = e;
	  return(-1);
     }

     file_key key;
     key.inode = buf.st_ino;
     key.dev   = buf.st_dev;
    
     debug("mm_util",0,"----->Locked");
     pthread_mutex_lock(&listMutex);
    
     if (openFileList == NULL) {
	  openFileList = (List *)gwlist_create();
     }

     /* See if the inode exists in the list */
     file_lock *item = NULL;
     do {
	  item = (file_lock*)gwlist_search(openFileList, &key, file_lock_inode_cmp);
	  if (item) {
	       /* It exists, that means that someone has already locked the file */
	       if (!shouldblock) {
		    n = -1;
		    debug("mm_util",0,"<-----UnLocked");
		    pthread_mutex_unlock(&listMutex);
		    errno = EWOULDBLOCK;
		    return n;
	       }
            
	       pthread_cond_wait(&item->condition, &listMutex);
	       /* O.k. we've got the file, but now item is invalid, 
		  the unlock_and_close removes it.
	       */
	  } 
     } while (item != NULL);

     /* No one else has locked the file, create the condition for 
	anyone else. 
     */
     item = (file_lock*)gw_malloc(sizeof(file_lock));
     item->key.inode = key.inode;
     item->key.dev = key.dev;
     item->fd = fd;
     pthread_cond_init(&item->condition, NULL);
     gwlist_append(openFileList, item);

     /* Release the global lock so that we don't block the 
	entire system waiting for fnctl to return
     */
     debug("mm_util",0,"<-----UnLocked");
     pthread_mutex_unlock(&listMutex);

     do {
	  lock.l_whence = SEEK_SET;
	  lock.l_start = 0;
	  lock.l_len = 0;
	  lock.l_type = F_WRLCK;
	  n = fcntl(fd, flg, &lock);
	  if (n < 0) {
	       if (errno == EINTR)
		    stop = 0;
	       else
		    stop = 1;
	  } else	    
	       stop = 1;
     } while (!stop);

     /* If we failed to get the fcntl lock, then we need to 
	release the local lock */
     if (n != 0) {
	  release_file_lock(fd, &key);
     }

     return (n == 0) ? 0 : errno; 
#else
     panic(0, "Attempt to call sun_lockfile on a non-solaris system");
     return 0;
#endif
}

int lockfile(int fd, int shouldblock)
{
#ifdef SunOS
     return sun_lockfile(fd, shouldblock);
#else 
     int n, stop;     
     unsigned flg = shouldblock ? 0 : LOCK_NB;

     do {
	  n = flock(fd, LOCK_EX|flg);
	  if (n < 0) {
	       if (errno == EINTR)
		    stop = 0;
	       else
		    stop = 1;
	  } else	    
	       stop = 1;
     } while (!stop);
     
     return (n == 0) ? 0 : errno; 
#endif
}

static int check_lock(int fd, char *fname)
{
     struct stat fs = {0}, ds = {0};
     
     /* You might grab a lock on a file, but the file 
      * might be changed just before you grabbed the lock. Detect that and fail..
      */
     if (fstat(fd, &ds) < 0 || 
	 stat(fname, &fs) < 0 ||
	 
	 ds.st_nlink != fs.st_nlink ||
	 memcmp(&ds.st_dev,&fs.st_dev, sizeof ds.st_dev) != 0 ||
	 memcmp(&ds.st_ino,&fs.st_ino, sizeof ds.st_ino) != 0 ||
	 ds.st_uid != fs.st_uid ||
	 ds.st_gid != fs.st_gid ||
	 ds.st_size != fs.st_size)
	  return -1;
     else 
	  return 0;
}

int mm_lockfile(int fd, char *fname, int shouldblock)
{
     int ret = lockfile(fd,shouldblock);

     if (ret != 0 && errno != EWOULDBLOCK) {
	  debug("mm_util", 0, "Unable to lock '%s', error= %d, %s, shouldblock=%d", 
		fname, errno, strerror(errno), shouldblock);
	  perror("Unable to lock file");
     }

     if (ret != 0 || 
	 (ret = check_lock(fd,fname)) != 0)
	  return ret;     
     return 0;
}

void mms_collect_envdata_from_msgheaders(List *mh, List **xto, 
					 Octstr **subject, 
					 Octstr **otransid, time_t *expiryt, 
					 time_t *deliveryt, long default_msgexpiry, 
					 long max_msgexpiry,
					 char *unified_prefix, List *strip_prefixes)
{

     Octstr *s;
     List *l;
  
     if (xto) {
	  l = http_header_find_all(mh, "To");
	  if (l != NULL) { 
	       int i, n;
	       for (i = 0, n = gwlist_len(l); i<n; i++) {
		    Octstr *name, *value;
		    http_header_get(l, i, &name, &value);
		    _mms_fixup_address(&value, unified_prefix, strip_prefixes, 1);
		    gwlist_append(*xto, value);
		    octstr_destroy(name);
	       }
	       http_destroy_headers(l);
	  }
       
	  l = http_header_find_all(mh, "Cc");
	  if (l) { 
	       int i, n;
	       for (i = 0, n = gwlist_len(l); i<n; i++) {
		    Octstr *name, *value;
		    http_header_get(l, i, &name, &value);
		    _mms_fixup_address(&value, unified_prefix, strip_prefixes, 1);
		    gwlist_append(*xto, value);
		    octstr_destroy(name);
		 
	       }
	       http_destroy_headers(l);
	  }
       
       
	  l = http_header_find_all(mh, "Bcc");
	  if (l) { 
	       int i, n;
	    
	       for (i = 0, n = gwlist_len(l); i<n; i++) {
		    Octstr *name, *value;
		    http_header_get(l, i, &name, &value);
		    _mms_fixup_address(&value, unified_prefix, strip_prefixes, 1);
		    gwlist_append(*xto, value);
		    octstr_destroy(name);
		 
	       }
	       http_destroy_headers(l);
	  }
     }
                   
     /* Find expiry and delivery times */
     if (expiryt) {
	  s = http_header_value(mh, octstr_imm("X-Mms-Expiry"));
	  if (s) {
	       *expiryt = date_parse_http(s);
	       octstr_destroy(s);
	  } else 
	       *expiryt = time(NULL) + default_msgexpiry;
       
	  if (max_msgexpiry > 0 
	      && (*expiryt - time(NULL)) > max_msgexpiry)
	       *expiryt = time(NULL) + max_msgexpiry;
     }
  
     if (deliveryt) {
	  s = http_header_value(mh, octstr_imm("X-Mms-Delivery-Time"));
	  if (s) {
	       *deliveryt = date_parse_http(s);
	       octstr_destroy(s);
	  } else 
	       *deliveryt = 0;
     }
     if (subject)
	  *subject = http_header_value(mh, octstr_imm("Subject"));
  
     if (otransid)
	  *otransid = http_header_value(mh, octstr_imm("X-Mms-Transaction-ID"));   
}

unsigned long _mshash(char *s)
{
     unsigned h = 0;
     
     while (*s) {
	  unsigned int ch = tolower(*s);
	  s++;
	  h += ((unsigned)(ch) << 4) + 1249;
     }
     return h;
}

int isphonenum_ex(Octstr *s, int offset)
{
     int i = offset, n = octstr_len(s);
     char *cs;

     if (s && octstr_len(s) >= 1 && 
	 octstr_get_char(s, i) == '+')
	  i++;
     for ( cs = octstr_get_cstr(s); i<n; i++)
	  if (!gw_isdigit(cs[i]))
	       return 0;
     return 1;
}

int isphonenum(Octstr *s)
{
     return isphonenum_ex(s, 0);
}
void mms_normalize_phonenum(Octstr **num, char *unified_prefix, List *strip_prefixes)
{
     int i, n;

     if (num == NULL ||
	 *num == NULL)
	  return;
     
     /* stip prefix first. */
     for (i = 0, n = gwlist_len(strip_prefixes); i<n;i++) {
	  Octstr *x  = gwlist_get(strip_prefixes, i);
	  if (octstr_search(*num, x, 0) == 0) {
	       octstr_delete(*num, 0, octstr_len(x));
	       break;
	  }
     }
     
     if (unified_prefix)
	  normalize_number(unified_prefix, num);
}

/* Doesn't handle IP addresses very well */
void _mms_fixup_address(Octstr **address, char *unified_prefix, List *strip_prefixes, int keep_suffix)
{
     int i;
     Octstr *typ;
     
     if (!address || !*address) return;
     i = octstr_search_char(*address, '@', 0);     
     if (i>0) /* an email address. */
	  return;
     
     i = octstr_case_search(*address, octstr_imm("/TYPE="), 0);
     if (i > 0) {
	  typ = octstr_copy(*address, i, octstr_len(*address));
	  octstr_delete(*address, i, octstr_len(*address));
     } else 
	  typ = NULL;

     if (isphonenum(*address) || (typ && octstr_str_case_compare(typ, "/TYPE=PLMN") == 0)) {
	  mms_normalize_phonenum(address, unified_prefix, strip_prefixes);
	  octstr_append(*address, keep_suffix ? octstr_imm("/TYPE=PLMN") : octstr_imm(""));
     } else if (typ) 
	  octstr_append(*address, keep_suffix ? typ : octstr_imm(""));     
     else
	  octstr_append(*address, keep_suffix ? octstr_imm("@unknown") : octstr_imm(""));     
     octstr_destroy(typ);
}

/* compare, reversed result! */
static int comp_fn(void *item, void *pattern)
{
     return (octstr_case_compare(item, pattern) == 0) ? 1 : 0;
}
int is_allowed_host(Octstr *host, Octstr *host_list)
{
     List *l;
     int ret;
     gw_assert(host_list);
     gw_assert(host);
     
     l = octstr_split(host_list, octstr_imm(";"));

     ret = (gwlist_search(l, host, comp_fn) != NULL) ? 1 : 0;
     
     gwlist_destroy(l, (void *)octstr_destroy);
     
     return ret;
}

#define SHELLCHARS "'|\"()[]{}$&!?*><%`\n \t\\"
void escape_shell_chars(Octstr *str)
{
     Octstr *tmp;
     int i, n;

     octstr_strip_blanks(str);

     tmp = octstr_duplicate(str);
     octstr_delete(str, 0, octstr_len(str));

     for (i = 0, n = octstr_len(tmp); i < n; i++) {
	  int ch = octstr_get_char(tmp,i);

	  if (strchr(SHELLCHARS, ch) != NULL)
	       octstr_append_char(str, '\\');
	  octstr_append_char(str, ch);	
     }
     octstr_destroy(tmp);
}

int parse_cgivars(List *request_headers, Octstr *request_body,
		  List **cgivars, List **cgivar_ctypes)
{
     Octstr *ctype = NULL, *charset = NULL; 
     int ret = 0;
     
     if (request_body == NULL || 
	 octstr_len(request_body) == 0 || cgivars == NULL)
	  return 0; /* Nothing to do, this is a normal GET request. */
     
     http_header_get_content_type(request_headers, &ctype, &charset);

     if (*cgivars == NULL)
	  *cgivars = gwlist_create();

     if (*cgivar_ctypes == NULL)
	  *cgivar_ctypes = gwlist_create();

     if (!ctype) {
	  mms_warning(0, NULL, NULL, "MMS: Parse CGI Vars: Missing Content Type!");
	  ret = -1;
	  goto done;
     }

     if (octstr_case_compare(ctype, octstr_imm("application/x-www-form-urlencoded")) == 0) {
	  /* This is a normal POST form */
	  List *l = octstr_split(request_body, octstr_imm("&"));
	  Octstr *v;

	  while ((v = gwlist_extract_first(l)) != NULL) {
	       List *r = octstr_split(v, octstr_imm("="));
	       
	       if (gwlist_len(r) == 0)
		    mms_warning(0, NULL, NULL, "MMS: Parse CGI Vars: Missing CGI var name/value in POST data: %s",
				octstr_get_cstr(request_body));
	       else {
		    HTTPCGIVar *x = gw_malloc(sizeof *x);
		    x->name =  gwlist_extract_first(r);
		    x->value = gwlist_extract_first(r);
		    if (!x->value)
			 x->value = octstr_create("");
		
		    octstr_strip_blanks(x->name);
		    octstr_strip_blanks(x->value);
		    
		    octstr_url_decode(x->name);
		    octstr_url_decode(x->value);
		    
		    gwlist_append(*cgivars, x);
	       }
	       octstr_destroy(v);
	       gwlist_destroy(r, octstr_destroy_item);
	  }
	  gwlist_destroy(l, NULL);
     } else if (octstr_case_compare(ctype, octstr_imm("multipart/form-data")) == 0) {
	  /* multi-part form data */
	  MIMEEntity *m = mime_http_to_entity(request_headers, request_body);
	  int i, n;
	  
	  if (!m) {
	       mms_warning(0, NULL, NULL, "MMS: Parse CGI Vars: Failed to parse multipart/form-data body: %s",
			   octstr_get_cstr(request_body));
	       ret = -1;
	       goto done;
	  }
	  /* Go through body parts, pick out what we need. */
	  for (i = 0, n = mime_entity_num_parts(m); i < n; i++) {
	       MIMEEntity *mp = mime_entity_get_part(m, i);
	       List   *headers = mime_entity_headers(mp);
	       Octstr *body = mime_entity_body(mp);
	       Octstr *ct = http_header_value(headers, 
					      octstr_imm("Content-Type"));      
	       Octstr *cd = http_header_value(headers, 
					      octstr_imm("Content-Disposition"));
	       Octstr *name = http_get_header_parameter(cd, octstr_imm("name"));

	       if (name) {
		    HTTPCGIVar *x = gw_malloc(sizeof *x);
		    
		    /* Strip quotes */
		    if (octstr_get_char(name, 0) == '"') {
			 octstr_delete(name, 0, 1);		    
			 octstr_truncate(name, octstr_len(name) - 1);
		    }
		    
		    x->name = octstr_duplicate(name);
		    x->value = octstr_duplicate(body);
		    
		    gwlist_append(*cgivars, x);
		    
		    if (ct) { /* If the content type is set, use it. */
			 x = gw_malloc(sizeof *x);
			 x->name = octstr_duplicate(name);
			 x->value = octstr_duplicate(ct);
			 
			 gwlist_append(*cgivar_ctypes, x);
		    }
		    octstr_destroy(name);
	       }

	       octstr_destroy(ct);	       
	       octstr_destroy(cd);	       
	       octstr_destroy(body);
	       http_destroy_headers(headers);
	       mime_entity_destroy(mp);
	  }
	  mime_entity_destroy(m);
	  
     } else /* else it is nothing that we know about, so simply go away... */
	  ret = -1;
done:
     octstr_destroy(ctype);
     octstr_destroy(charset);	       
     return ret;
}

/* get content-ID header, fix: WAP decoder may leave \" at beginning */
Octstr *_x_get_content_id(List *headers)
{
     Octstr *cid = http_header_value(headers, octstr_imm("Content-ID"));

     if (cid)
	  if (octstr_get_char(cid, 0) == '"' && 
	      octstr_get_char(cid, octstr_len(cid) - 1) != '"')
	       octstr_delete(cid, 0,1);
     return cid;
}

/* Utility: Take a header list, remove any boundary parameter from the content-type
 * element. We don't want this in the WSP packed content.
 */
void strip_boundary_element(List *headers, char *s)
{
     Octstr *ctype = NULL, *params = NULL;     
     Octstr *value;
     int n;
     
     gw_assert(headers);     

     if ((n = get_content_type(headers, &ctype, &params)) < 0) {
	  octstr_destroy(ctype);
	  ctype = NULL; /* no ctype found, so do not replace it! */
     } else if (ctype && 
		DRM_CONTENT_TYPE(ctype)) { 
	  octstr_destroy(ctype);
	  ctype = NULL; /* leave drm alone! */
     }

     if (s) {/* we are replacing the content type as well as stripping */
	  octstr_destroy(ctype);
	  ctype = octstr_create(s);
     }
	  
     if (params && ctype) {
	  List *h = get_value_parameters(params);
	  Octstr *ps;
	  http_header_remove_all(h,"boundary"); /* We  don't need the boundary param if it is there. */
	  ps = make_value_parameters(h);
	  
	  value = octstr_format("%S%s%S", ctype, 
				(ps && octstr_len(ps) > 0) ? "; " : "", 
				ps);
	  octstr_destroy(ps);
	  http_destroy_headers(h);
     } else 
	  value = ctype;
     if (value) {
	  http_header_remove_all(headers, "Content-Type");
	  http_header_add(headers, "Content-Type", octstr_get_cstr(value));     
     }
     if (ctype != value)
	  octstr_destroy(ctype);
     octstr_destroy(value);     
     octstr_destroy(params);
}


/* Mapping file extensions to content types. */
static struct {
     char *ctype,  *file_ext;
} exts[] = {
     {"text/plain", "txt"},
     {"image/jpeg",  "jpg"},
     {"image/jpeg",  "jpeg"},
     {"image/png",  "png"},
     {"image/tiff",  "tiff"},
     {"image/gif",  "gif"},
     {"image/bmp",  "bmp"},
     {"image/vnd.wap.wbmp",  "wbmp"},
     {"image/x-bmp",  "bmp"},
     {"image/x-wmf",  "bmp"},
     {"image/vnd.wap.wpng",  "png"},
     {"image/x-up-wpng",  "png"},
     {"audio/mpeg",  "mp3"},
     {"audio/wav",  "wav"},
     {"audio/basic",  "au"},
     {"audio/amr",  "amr"},
     {"audio/x-amr",  "amr"},
     {"audio/amr-wb",  "amr"},
     {"audio/midi",  "mid"},
     {"audio/sp-midi",  "mid"},  
     {"application/smil", "smil"},
     {"application/smil", "smi"},
     {"application/vnd.wap.mms-message", "mms"},
     {"application/java-archive", "jar"},
     {"video/3gpp", "3gp"},
     {"video/3gpp", "3gp2"},
     {"video/3gpp2","3g2"},
     {"audio/vnd.qcelp", "qcp"},
     
     {MBUNI_MULTIPART_TYPE, "urls"}, /* mbuni url list type. */
     {NULL, NULL}
};

/* Some of Web languages used for generating content, but can't be a content itself. */
static struct {
     char *language, *file_ext; 
} l_exts[] = {
     {"Perl", "pl"},
     {"Php", "php"},
     {"Python", "py"},
     {"Common Gateway Interface", "cgi"},
     {"Active Server Page", "asp"},
     {"Java Server Page", "jsp"},
     {"Ruby on Rails", "rb"},            
     {"Tool Command Language", "tcl"},      
     {"Shell Command Language", "sh"},
     {"Executables", "exe"},
     {NULL, NULL}
};

Octstr *filename2content_type(char *fname)
{
     char *p = strrchr(fname, '.');
     int i;
     
     if (p) 
	  for (i = 0; exts[i].file_ext; i++)
	       if (strcasecmp(p+1, exts[i].file_ext) == 0)
		    return octstr_imm(exts[i].ctype);
     
     return octstr_imm("application/octet-stream");          
}

static char *content_type2file_ext(Octstr *ctype)
{
     int i, j;     

     /* Take the first value, expecting content-type! */
     if ((j = octstr_search_char(ctype, ';', 0)) != -1)
	  octstr_delete(ctype, j, octstr_len(ctype));
     
     for (i = 0; exts[i].file_ext; i++)
	  if (octstr_str_case_compare(ctype, exts[i].ctype) == 0)
	       return exts[i].file_ext;
     
     return "dat";          
}

char *make_file_ext(Octstr *url, Octstr *ctype, char fext[5])
{
     int i;
     fext[0] = 0;
     if (url) {
	  HTTPURLParse *h = parse_url(url);	  
	  char *s, *p;
	  if (!h)
	       goto done;
	  
	  s = h->path ? octstr_get_cstr(h->path) : "";

	  if ((p = strrchr(s, '.')) != NULL)
	       strncpy(fext, p+1, 4); /* max length of 4. */
	  
	  http_urlparse_destroy(h);
        
	  for (i = 0; l_exts[i].file_ext; i++)
	       if (strcasecmp(fext, l_exts[i].file_ext) == 0)
		    return content_type2file_ext(ctype);

	  if (fext[0]) 
	       return fext;
     } 
done:
     return content_type2file_ext(ctype);
}
static int fetch_url_with_auth(HTTPCaller *c, int method, Octstr *url, List *request_headers, 
			       Octstr *body, Octstr *auth_hdr, List **reply_headers, Octstr **reply_body);

int mms_url_fetch_content(int method, Octstr *url, List *request_headers, 
			  Octstr *body, List **reply_headers, Octstr **reply_body)
{

     int status = 0;
     Octstr *furl = NULL;

     if (octstr_search(url, octstr_imm("data:"), 0) == 0) {
	  int i = octstr_search_char(url, ',',0);
	  Octstr *ctype = (i >= 0) ? octstr_copy(url, 5, i-5) : octstr_create("text/plain; charset=us-ascii");
	  Octstr *data = (i >= 0) ? octstr_copy(url, i+1, octstr_len(url)) : octstr_copy(url,5,octstr_len(url));

	  Octstr *n = NULL, *h = NULL;
	  
	  if (octstr_len(ctype) == 0)
	       octstr_append_cstr(ctype, "text/plain; charset=us-ascii");

	  split_header_value(ctype, &n, &h);
	  
	  if (h) {
	       List *ph = get_value_parameters(h);
	       Octstr *v = NULL;

	       if ((ph && (v = http_header_value(ph, octstr_imm("base64"))) != NULL) || 
		   octstr_case_search(h, octstr_imm("base64"), 0) >= 0) { /* has base64 item */
		    Octstr *p = NULL;

		    octstr_base64_to_binary(data);
		    http_header_remove_all(ph, "base64");
		    
		    octstr_destroy(ctype);
		    
		    if (gwlist_len(ph) > 0) {
			 p = make_value_parameters(ph);
			 ctype = octstr_format("%S; %S",
					       n,p);
			 octstr_destroy(p);
		    } else 
			 ctype = octstr_format("%S", n);
	       }
	       
	       if (ph)
		    http_destroy_headers(ph);

	       octstr_destroy(v);
	       octstr_destroy(h);
	  }
	  
	  octstr_destroy(n);

	  *reply_body = data;
	  *reply_headers = http_create_empty_headers();
	  http_header_add(*reply_headers, "Content-Type", octstr_get_cstr(ctype));

	  octstr_destroy(ctype);
	  status = HTTP_OK;
     } else  if (octstr_search(url, octstr_imm("file://"), 0) == 0) {
	  char *file = octstr_get_cstr(url) + 6;
	  Octstr *ctype = filename2content_type(file);
	  Octstr *data = octstr_read_file(file);

	  *reply_body = data;
	  *reply_headers = http_create_empty_headers();
	  http_header_add(*reply_headers, "Content-Type", octstr_get_cstr(ctype));

	  status = data ? HTTP_OK : HTTP_NOT_FOUND;	  
	  octstr_destroy(ctype);	  
     } else {
	  HTTPCaller *c = http_caller_create();
	  http_start_request(c, method, url, request_headers, body, 1, NULL, NULL);	  
	  if (http_receive_result_real(c, &status, &furl, reply_headers, reply_body,1) == NULL)
	       status = -1;
	  if (status == HTTP_UNAUTHORIZED) { 
	       Octstr *v = http_header_value(*reply_headers, octstr_imm("WWW-Authenticate"));
	       
	       status = fetch_url_with_auth(c, method, url, request_headers, body, v, 
					    reply_headers, reply_body);

	       octstr_destroy(v);
	  }
	  http_caller_destroy(c);
     }

     octstr_destroy(furl);

     return status;
}

Octstr *get_stripped_param_value(Octstr *value, Octstr *param)
{
     Octstr *x = http_get_header_parameter(value, param);

     if (x != NULL && 
	 octstr_get_char(x, 0) == '"' &&
	 octstr_get_char(x, octstr_len(x) - 1) == '"') {
	  octstr_delete(x, 0, 1);
	  octstr_delete(x, octstr_len(x) - 1, 1);
     }
     return x;    
}


static Octstr *make_url(HTTPURLParse *h);

/* Fetch a url with authentication as necessary. */
static int fetch_url_with_auth(HTTPCaller *c, int method, Octstr *url, List *request_headers, 
			       Octstr *body, Octstr *auth_hdr,  List **reply_headers, Octstr **reply_body)
{
     Octstr *xauth_value = auth_hdr ? octstr_duplicate(auth_hdr) : octstr_create("");
     Octstr *domain = NULL, *nonce = NULL, *opaque = NULL, *algo = NULL, *auth_type = NULL, *x;
     Octstr *realm = NULL, *xurl = NULL;
     Octstr *cnonce = NULL;
     char *nonce_count = "00000001";
     Octstr *A1 = NULL, *A2 = NULL, *rd = NULL;
     List *qop = NULL, *l = NULL;
     int i, status = HTTP_UNAUTHORIZED, has_auth = 0, has_auth_int = 0;
     HTTPURLParse *h = parse_url(url);
     char *m_qop = NULL;
     time_t t  = time(NULL);
     
     /* Check that there is a username and password in the URL! */

     if (h == NULL || h->user == NULL || octstr_len(h->user) == 0) 
	  goto done;
          
     /* First we get the auth type: */
     
     if ((i = octstr_search_char(xauth_value, ' ', 0)) < 0) {
	  mms_warning(0, NULL, NULL, "Mal-formed WWW-Authenticate header (%s) received while fetching %s!",
		      octstr_get_cstr(xauth_value), url ? octstr_get_cstr(url) : "");
	  status = -1;
	  goto done;
     }
     auth_type = octstr_copy(xauth_value, 0, i);
     octstr_delete(xauth_value, 0, i+1);

     if (octstr_str_case_compare(auth_type, "Basic") == 0) {
	  status = HTTP_UNAUTHORIZED; /* suported by default by GWLIB so if we get here, means bad passwd. */
	  goto done;
     } /* else digest. */

     /* Put back some fake data so what we have can be parsed easily. */
     if ((l =  http_header_split_auth_value(xauth_value)) != NULL) {
	  Octstr *x = gwlist_get(l, 0);
	  octstr_insert(x, octstr_imm("_none; "), 0); /* make it easier to parse. */
	  octstr_destroy(xauth_value);
	  xauth_value = octstr_duplicate(x);
	  
	  gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
     } else 
	  mms_warning(0, NULL, NULL, "Mal-formed Digest header (%s) while fetching (%s)!", 
		      octstr_get_cstr(xauth_value), url ? octstr_get_cstr(url) : "");
     
     realm = get_stripped_param_value(xauth_value, octstr_imm("realm"));
     domain = get_stripped_param_value(xauth_value, octstr_imm("domain"));
     nonce = get_stripped_param_value(xauth_value, octstr_imm("nonce"));
     opaque = get_stripped_param_value(xauth_value, octstr_imm("opaque"));     
     algo = get_stripped_param_value(xauth_value, octstr_imm("algorithm"));

     if ((x = get_stripped_param_value(xauth_value, octstr_imm("qop"))) != NULL) {
	  int i;
	  qop = octstr_split(x, octstr_imm(","));
	  octstr_destroy(x);
	  for (i = 0; i<gwlist_len(qop); i++) { /* find qop options. */
	       Octstr *s = gwlist_get(qop, i);
	       if (!s) continue;
	       if (octstr_str_case_compare(s, "auth") == 0)
		    has_auth = 1;
	       else if (octstr_str_case_compare(s, "auth-int") == 0)
		    has_auth_int = 1;
	  }
     }
     
     if (qop || 
	 (algo != NULL && octstr_str_case_compare(algo, "MD5-sess") == 0)) {
	  cnonce = octstr_create_from_data((void *)&t, sizeof t);
	  octstr_binary_to_hex(cnonce,0);
     }

     /* Make A1 */
     x = octstr_format("%S:%S:%S",
		       h->user, realm, h->pass ? h->pass : octstr_imm(""));
     A1 = md5(x);
     octstr_destroy(x);

     if (algo != NULL && octstr_str_case_compare(algo, "MD5-sess") == 0) {
	  x = octstr_format("%S:%S:%S", 
			    A1, nonce, cnonce);
	  octstr_destroy(A1);
	  A1 = md5(x);
	  octstr_destroy(x);	  
     }
     octstr_binary_to_hex(A1,0);

     /* Make A2. */
     x = octstr_format("%s:%S",
		       http_method2name(method), 
		       h->path);
     if (qop != NULL && has_auth_int && !has_auth) { /* if qop, and qop=auth-int */
	  Octstr *y; 
	  m_qop = "auth-int";
	  
	  y = md5(body);
	  octstr_binary_to_hex(y,0);

	  octstr_append_char(x, ':');
	  octstr_append(x, y);

	  octstr_destroy(y);
     } else if (qop)
	  m_qop = "auth";

     A2 = md5(x);
     octstr_destroy(x);
     octstr_binary_to_hex(A2,0);
     
     /* Finally make the digest response */
     if (qop) 
	  x = octstr_format("%S:%S:%s:%S:%s:%S",
			    A1, nonce, nonce_count, cnonce,
			    m_qop, A2);
     else 
	  x = octstr_format("%S:%S:%S", A1, nonce, A2);

     rd = md5(x);
     octstr_destroy(x);
     octstr_binary_to_hex(rd, 0);
     
     
     /* make the header value */
     x = octstr_format("Digest username=\"%S\", realm=\"%S\", response=\"%S\", nonce=\"%S\", uri=\"%S\"",
		       h->user, realm, rd, nonce, h->path);

     if (opaque) 
	  octstr_format_append(x, ", opaque=\"%S\"", opaque);
     
     if (cnonce) 
	  octstr_format_append(x, ", cnonce=\"%S\", nc=%s", cnonce, nonce_count);
     if (m_qop)
	  octstr_format_append(x,", qop=%s", m_qop);
     if (algo)
	  octstr_format_append(x,", algorithm=%S", algo);

     http_header_remove_all(request_headers, "Authorization");
     http_header_add(request_headers, "Authorization", octstr_get_cstr(x));
     octstr_destroy(x);

     /* Remove username, password, then remake URL */
     octstr_destroy(h->user);
     h->user = NULL;
	 
     octstr_destroy(h->pass);
     h->pass = NULL;

     xurl = make_url(h);
     x = NULL;
     http_start_request(c, method, xurl, request_headers, body, 1, NULL, NULL);	  
     if (http_receive_result_real(c, &status, &x, reply_headers, reply_body,1) == NULL)
	  status = -1;
     if (x)
	  octstr_destroy(x);
done:
     octstr_destroy(xauth_value);     
     octstr_destroy(realm);     
     octstr_destroy(domain);
     octstr_destroy(nonce);
     octstr_destroy(opaque);
     octstr_destroy(algo);
     octstr_destroy(xurl);
     octstr_destroy(cnonce);
     gwlist_destroy(qop, (gwlist_item_destructor_t *)octstr_destroy);     
     if (h)
	  http_urlparse_destroy(h);
     
     return status;
}


static Octstr *make_url(HTTPURLParse *h)
{
     Octstr *url = octstr_duplicate(h->scheme);
     
     if (h->user) {
	  octstr_format_append(url, "%S", h->user);
	  
	  if (h->pass)
	       octstr_format_append(url, ":%S", h->pass);	       
	  octstr_format_append(url, "@");
     }
     octstr_format_append(url, "%S:%d%S", h->host, h->port, h->path);
     
     if (h->query)
	  octstr_format_append(url, "?%S", h->query);

     if (h->fragment)
	  octstr_format_append(url, "#%S", h->fragment);
     return url;
}

static int is_separator_char(int c)
{
     switch (c) {
     case '(':
     case ')':
     case '<':
     case '>':
     case '@':
     case ',':
     case ';':
     case ':':
     case '\\':
     case '"':
     case '/':
     case '[':
     case ']':
     case '?':
     case '=':
     case '{':
     case '}':
     case 32:  /* SP */
     case 9:   /* HT */
	  return 1;
     default:
	  return 0;
     }
}

/* Is this char part of a 'token' as defined by HTTP? */
static int is_token_char(int c)
{
     return c >= 32 && c < 127 && !is_separator_char(c);
}

/* Is this string a 'token' as defined by HTTP? */
int  mms_is_token(Octstr *token)
{
     return octstr_len(token) > 0 &&
	  octstr_check_range(token, 0, octstr_len(token), is_token_char);
}


int has_node_children(xmlNodePtr node)
{
     xmlNodePtr x;
     
     for (x = node->xmlChildrenNode; x; x = x->next) 
	  if (x->type == XML_ELEMENT_NODE) 
	       return 1;
     return 0;
}

/* strip all but content-type, content-id, content-transfer-disposition, content-location */
void strip_non_essential_headers(MIMEEntity *mime)
{
     Octstr *v;
     List *h, *h2;

     if (!mime) return;
     
     h = mime_entity_headers(mime);
     h2 = http_create_empty_headers();

     if ((v = http_header_value(h, octstr_imm("Content-Type"))) != NULL) {
	  http_header_add(h2, "Content-Type", octstr_get_cstr(v));
	  octstr_destroy(v);
     }

     if ((v = http_header_value(h, octstr_imm("Content-ID"))) != NULL) {
	  http_header_add(h2, "Content-ID", octstr_get_cstr(v));
	  octstr_destroy(v);
     }

     if ((v = http_header_value(h, octstr_imm("Content-Location"))) != NULL) {
	  http_header_add(h2, "Content-Location", octstr_get_cstr(v));
	  octstr_destroy(v);
     }

     if ((v = http_header_value(h, octstr_imm("Content-Transfer-Encoding"))) != NULL) {
	  http_header_add(h2, "Content-Transfer-Encoding", octstr_get_cstr(v));
	  octstr_destroy(v);
     }

     mime_replace_headers(mime,h2);
     http_destroy_headers(h);
     http_destroy_headers(h2);

}

void *_mms_load_module(mCfg *cfg, mCfgGrp *grp, char *config_key, char *symbolname,
		       void *shell_builtin)
{
     Octstr *s = NULL;
     void *retval = NULL;
     
     s = mms_cfg_get(cfg, grp, octstr_imm(config_key));
     
     if (s) {
	  void *x; 
	  void *y = NULL;
#ifdef __APPLE__	       
	  char sbuf[512];
#endif  
	  /* First look for the builtin: keyword. 
	   * For now only builtin:shell is supported. 
	   */
	  if (octstr_case_search(s, octstr_imm("builtin:shell"), 0) >= 0)
	       retval = shell_builtin;
	  else {
	       x = dlopen(octstr_get_cstr(s), RTLD_LAZY);
#ifdef __APPLE__	       
	       sprintf(sbuf, "_%s", symbolname);
#endif  
	       if (x == NULL || ((y = dlsym(x, symbolname)) == NULL 
#ifdef __APPLE__ /* fink version of dlsym has issues it seems. */
				 && (y = dlsym(x, sbuf)) == NULL
#endif
			))
	       
		    panic(0, "Unable to load dynamic libary (%s): %s",
			  octstr_get_cstr(s),
			  dlerror());
	       else
		    retval = y;	  
	  }
	  octstr_destroy(s);
     }
	
     return retval;
}

Octstr *extract_phonenum(Octstr *num, Octstr *unified_prefix)
{
     Octstr *phonenum;
     int j = octstr_case_search(num, octstr_imm("/TYPE=PLMN"), 0);
     
     if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == octstr_len(num)) 
	  phonenum = octstr_copy(num, 0, j);
     else 
	  phonenum = octstr_duplicate(num);
      
     if (unified_prefix)
	  normalize_number(octstr_get_cstr(unified_prefix), &phonenum);

     return phonenum;

}

void strip_quoted_string(Octstr *s)
{
     if (s == NULL) return;

     octstr_strip_blanks(s);
     if (octstr_get_char(s, 0) == '"') {
	  octstr_delete(s, 0, 1);
	  octstr_delete(s, octstr_len(s)-1, 1);
     }
}

MIMEEntity *make_multipart_formdata(void)
{
     MIMEEntity *x = mime_entity_create();
     List *rh = http_create_empty_headers();
     
     http_header_add(rh, "User-Agent", MM_NAME "/" VERSION);	       
     http_header_add(rh, "Accept", "*/*");	            
     http_header_add(rh, "Content-Type", "multipart/form-data");
     mime_replace_headers(x, rh);
     http_destroy_headers(rh);
     
     return x;
}

void add_multipart_form_field(MIMEEntity *multipart, char *field_name, char *ctype, char *content_loc, 
			      Octstr *data)
{
     MIMEEntity *p = mime_entity_create();
     List *xh = http_create_empty_headers();
     Octstr *cd = octstr_format("form-data; name=\"%s\"", field_name);
     
     if (content_loc) 
	  octstr_format_append(cd, "; filename=\"%s\"", content_loc);
     
     http_header_add(xh, "Content-Disposition", octstr_get_cstr(cd));	  
     if (ctype) /* This header must come after the above it seems. */
	  http_header_add(xh, "Content-Type", ctype);

     mime_entity_set_body(p, data);     
     mime_replace_headers(p, xh);
     
     mime_entity_add_part(multipart, p); /* add it to list so far. */

     mime_entity_destroy(p);
     http_destroy_headers(xh);
     octstr_destroy(cd);

}

void fixup_address_type(List *headers, char *hdr, 
			char *unified_prefix, List *strip_prefixes)
{
     List *l;
     int i, n;
     
     l = http_header_find_all(headers, hdr);
     
     http_header_remove_all(headers,hdr);
     for (i = 0, n = gwlist_len(l); i<n; i++) {
	  Octstr *name, *value;
	  int j, k;
	  http_header_get(l, i, &name, &value);
	  
	  if (!value || !name ||	 
	      octstr_case_compare(name, octstr_imm(hdr)) != 0)	 
	       goto loop;
	  if ((j = octstr_search(value, octstr_imm("/TYPE="), 0))>0) {
	       k = octstr_search_char(value, '@', 0);
	       if (k > j) { /* we have @, after TYPE=PLMN remove it */
		    octstr_delete(value, k, octstr_len(value));
	       }
	  }
	  _mms_fixup_address(&value, 
			     unified_prefix, 
			     strip_prefixes, 1);     	       
	  
	  http_header_add(headers, octstr_get_cstr(name), octstr_get_cstr(value));
     loop:	 
	  octstr_destroy(value);	 
	  octstr_destroy(name);	 	  
     }
     http_destroy_headers(l);     
}

MIMEEntity *multipart_from_urls(List *url_list)
{
     int i, n;
     List *rh = http_create_empty_headers();
     MIMEEntity *m = mime_entity_create();
               
     http_header_add(rh, "User-Agent", MM_NAME "/" VERSION);	       
     for (i = 0, n = gwlist_len(url_list); i<n; i++) {
	  List *rph = NULL;
	  Octstr *rbody = NULL;
	  Octstr *url = gwlist_get(url_list, i);
	  if (mms_url_fetch_content(HTTP_METHOD_GET, 
				    url, rh, NULL, &rph, &rbody) == HTTP_OK) {
	       List *mh = http_create_empty_headers();
	       Octstr *x;
	       MIMEEntity *mx;
	    
	       if ((x = http_header_value(rph, octstr_imm("Content-Type"))) != NULL) {
		    http_header_add(mh, "Content-Type", octstr_get_cstr(x));
		    octstr_destroy(x);
	       } else 
		    http_header_add(mh, "Content-Type", "application/content-stream");
	    
	       if ((x = http_header_value(rph, octstr_imm("Content-ID"))) != NULL) {
		    http_header_add(mh, "Content-ID", octstr_get_cstr(x));
		    octstr_destroy(x);
	       }
	    
	       if ((x = http_header_value(rph, octstr_imm("Content-Location"))) != NULL) {
		    http_header_add(mh, "Content-Location", octstr_get_cstr(x));
		    octstr_destroy(x);
	       }
	       mx = mime_http_to_entity(mh, rbody);
	    
	       mime_entity_add_part(m, mx);
	    
	       http_destroy_headers(mh);
	       mime_entity_destroy(mx);
	  } else 
	       mms_error(0, "multipart_from_urls", NULL, "Failed to load URL content for URL [%s]", 
			 octstr_get_cstr(url));
	  octstr_destroy(rbody);
	  http_destroy_headers(rph);
     }
     
     http_destroy_headers(rh);
     
     /* Now change the content type on this baby. */
     rh = http_create_empty_headers();
     http_header_add(rh, "Content-Type", "multipart/mixed");	       
     mime_replace_headers(m, rh);

     http_destroy_headers(rh);
     
     return m;
}

const char *mibenum_to_charset(int val)
{
#define CharMIBEnum(c,cset) else if (val == (c)) return #cset;
     
     if (0)
	  return "";
#include "charmibenum.def"
     
     return "ASCII"; /* Assume default ASCII ? */
}

int charset_to_mibenum(char *charset)
{
     
#define CharMIBEnum(c,cset) else if (strcasecmp(charset,(#cset)) == 0) return (c);
     
     if (0)
	  return US_ASCII_MIB_VAL;
#include "charmibenum.def"
     
     return US_ASCII_MIB_VAL; /* Assume default ASCII ? */
     
     
}

/* XXX right now we do not honour the 75 char length limit */
static Octstr *parse_rfc2047_atom(Octstr *in, int offset, int *mibenum, int *end_marker)
{
     Octstr *xs;
     int enc, n, i, ch, ch2;
     
     *end_marker = offset;

#if 0
     if (in == NULL || 
	 octstr_get_char(in, offset) != '=' ||
	 octstr_get_char(in, offset + 1) != '?') {
	  *mibenum = US_ASCII_MIB_VAL;
	  return in ? octstr_duplicate(in) : NULL;
     }
#endif

     offset += 2;
     xs = octstr_copy(in, offset, octstr_len(in));
     
     /* Get charset */
     if ((n = octstr_search_char(xs, '?', 0)) >= 0) {
	  Octstr *charset = octstr_copy(xs, 0, n);

	  octstr_delete(xs, 0, 1+octstr_len(charset));
	  *mibenum = charset_to_mibenum(octstr_get_cstr(charset));
	  octstr_destroy(charset);

	  offset += n+1;
     } else 
	  *mibenum =  US_ASCII_MIB_VAL;

     /* Get encoding */
     if ((n = octstr_search_char(xs, '?', 0)) >= 0) {
	  enc = octstr_get_char(xs, n-1);
	  octstr_delete(xs, 0, 2); /* Delete the encoding, and the following '?' */
	  offset += n+1;	  
     } else 
	  enc = 'q';


     /* Look for end marker */
     if ((n = octstr_search(xs, octstr_imm("?="), 0)) >= 0) {
	  octstr_delete(xs, n, octstr_len(xs)); /* Remove everything after that */
	  offset += n+2;
     } else 
	  offset += octstr_len(xs);
	 
     if (toupper(enc) == 'B') 
	  octstr_base64_to_binary(xs);
     else /* Assume plain text, deal with =20 and _ issues */
	  for (i = 0; i < octstr_len(xs); i++)
	       if (octstr_get_char(xs, i) == '=' && 
		   (ch = octstr_get_char(xs,i+1)) > 0 && 
		   (ch2 = octstr_get_char(xs,i+2)) > 0 && 
		   isxdigit(ch) && isxdigit(ch2)) { 
		    sscanf(octstr_get_cstr(xs) + i + 1, "%2x", &ch);
		    
		    octstr_delete(xs, i, 3); 
		    octstr_insert_char(xs, i,ch);
	       } else if (octstr_get_char(xs, i) == '_')  {
		    octstr_delete(xs, i, 1); 
		    octstr_insert_char(xs, i,0x20);
	       }
	 
     *end_marker = offset;
     return xs;
}

/* Output charset will be forced to UTF8 if multiple atoms exist */
Octstr *parse_rfc2047_text(Octstr *in, int *mibenum)
{
     int n1, mib, end, last_mib = -1, offset = 0;
     Octstr *x = NULL, *xs = NULL;
     
     *mibenum = US_ASCII_MIB_VAL; /* Default */
     if (in == NULL)
	  goto done;
     x = octstr_create("");
     while ((n1 = octstr_search(in, octstr_imm("=?"), offset)) >= 0 && 
	    (xs = parse_rfc2047_atom(in, n1, &mib, &end)) != NULL) {
	  Octstr *x1 = octstr_copy(in, offset, n1 - offset); /* Copy all the stuff up to this point. */
	  
	  octstr_append(x, x1);

	  if (last_mib >= 0 && mib != last_mib) { 
	       /* Force all to be utf-8 encoded */
	       const char *charset1 = mibenum_to_charset(mib);
	       Octstr *xcharset1 = octstr_imm(charset1);

	       const char *charset2 = mibenum_to_charset(last_mib);
	       Octstr *xcharset2 = octstr_imm(charset2);
	       Octstr *xout = NULL;
	       
	       /* Convert old stuff to utf8 */
	       if (last_mib != UTF8_MIB_VAL) {
		    if (charset_to_utf8(x, &xout, xcharset2) > 0 && xout) {
			 octstr_destroy(x);
			 x  = xout;
		    } else 
			 octstr_destroy(xout);
		    xout = NULL;
	       }
	       
	       /* Convert new string and append it */
	       if (mib != UTF8_MIB_VAL) {
		    if (charset_to_utf8(xs, &xout, xcharset1) > 0 && xout) {
			 octstr_destroy(xs);
			 xs  = xout;
		    } else 
			 octstr_destroy(xout);
		    xout = NULL;
	       }
	       
	       mib = UTF8_MIB_VAL; /* Force all subsequent to utf8 */	       
	  } 
	  
	  octstr_append(x, xs); /* Put it in */

	  last_mib = *mibenum = mib;
	  
	  offset = end;

	  octstr_destroy(x1);
	  octstr_destroy(xs);
	  xs = NULL;
     }

     octstr_destroy(xs); /* Because it might have escaped */
     if (offset < octstr_len(in)) {
	  Octstr *x1 = octstr_copy(in, offset, octstr_len(in)); /* Copy all the stuff up to this point. Ignore charset*/
	  octstr_append(x, x1);
	  octstr_destroy(x1);
     }

done:
     return x;
}

Octstr *pack_rfc2047_text(Octstr *in, int charset_mib_enum)
{
     int i;
     Octstr *xs;
     if (charset_mib_enum == US_ASCII_MIB_VAL)
	  return octstr_duplicate(in);
     
     xs = octstr_format("=?%s?q?", mibenum_to_charset(charset_mib_enum));

     for (i = 0; i<octstr_len(in); i++) {
	  int ch = octstr_get_char(in, i);
	  if (isspace(ch) || ch > 127) 
	       octstr_format_append(xs,"=%02X", ch);
	  else 
	       octstr_append_char(xs, ch);
     }
     octstr_append_cstr(xs, "?=");
     return xs;
}

List *gwlist_create_ex_real(const char *file, const char *func, int line,...)
{
     va_list ap;
     List *l  = gwlist_create();
     void *v;
     
     va_start(ap, line);

     while ((v = va_arg(ap, void *)) != NULL)
          gwlist_append(l, v);
     return l;
}

void mygw_free(void *x)
{
     gw_free(x);
}


/* Finds text part, returns copy. */
static MIMEEntity *find_textpart(MIMEEntity *m)
{
     Octstr *ctype = NULL, *params = NULL;
     MIMEEntity *res = NULL;
     List *headers;
     int i, n;
     
     if (!m) return NULL; 

     headers = mime_entity_headers(m);
     get_content_type(headers, &ctype, &params);
     http_destroy_headers(headers);
     
     if (ctype && octstr_str_compare(ctype, "text/plain") == 0) {
	  res = mime_entity_duplicate(m);
	  goto done;
     }
     
     if ((n = mime_entity_num_parts(m)) > 0) {
	  for (i = 0; i < n; i++) {
	       MIMEEntity *x = mime_entity_get_part(m, i);
	       res = find_textpart(x);
	       mime_entity_destroy(x);
	       
	       if (res != NULL) 
		    goto done2;
	  }
     }
done:
     if (res) { /* We got it! Convert charset if needed. */
	  List *params_h = get_value_parameters(params);
	  Octstr *charset = http_header_value(params_h, octstr_imm("charset"));
	  Octstr *body = mime_entity_body(res);
	  if (charset == NULL || 
	      octstr_str_compare(charset, "unknown") == 0) {
	       octstr_destroy(charset);
	       charset = octstr_create(DEFAULT_CHARSET);
	  }
	  
	  if (octstr_str_case_compare(charset, DEFAULT_CHARSET) != 0) {
	       charset_convert(body, DEFAULT_CHARSET, octstr_get_cstr(charset)); /* XXX error ignored? */	  
	       mime_entity_set_body(res, body);
	  }
	  octstr_destroy(body);
	  http_destroy_headers(params_h);
	  octstr_destroy(charset);
     } 

done2:
     
     octstr_destroy(ctype);     
     octstr_destroy(params);
     return res;
}


/* Gets the keyword, if any, from the text part of the message. 
 * converts charset as needed. 
 */
Octstr *get_keyword_from_mime(MIMEEntity *me)
{

     MIMEEntity *t = find_textpart(me);
     Octstr *txt = t ? mime_entity_body(t) : NULL;
     List *l = t ? octstr_split_words(txt) : NULL;
     Octstr *keyword = l ? gwlist_extract_first(l) : NULL;
     
     if (t)
	  mime_entity_destroy(t);
     gwlist_destroy(l, (gwlist_item_destructor_t *)octstr_destroy);
     octstr_destroy(txt);

     return keyword;
}

int http_auth_check(char *user, char *pass, List *headers, int *has_auth_hdr)
{
     int i, res = -1;
     Octstr *v = http_header_value(headers, octstr_imm("Authorization"));
     Octstr *p = NULL, *q = NULL;

     *has_auth_hdr = (v != NULL);
     if (user == NULL || user[0] == 0) {
	  res = 0;
	  goto done;
     }

     if (!v ||
	 octstr_search(v, octstr_imm("Basic "), 0) != 0)
	  goto done;
     p = octstr_copy(v, sizeof "Basic", octstr_len(v));
     octstr_base64_to_binary(p);
     
     i = octstr_search_char(p, ':', 0);
     q = octstr_copy(p, i+1, octstr_len(p));
     octstr_delete(p, i, octstr_len(p));
     
     /* p = user, q = pass. */

     if (octstr_str_compare(p,user) != 0 ||
	 octstr_str_compare(q,pass) != 0)
	  res = -1;
     else 
	  res = 0;
done:
     octstr_destroy(v);
     octstr_destroy(p);     
     octstr_destroy(q);
     return res;
}

/* little dirty method to see if file begins with url scheme. */
static int has_url_scheme(char *url, int *supported_scheme)
{
     char *p = strstr(url, "://");

     *supported_scheme = 1;
     
     if (strstr(url, "data:") == url || /* data: url scheme */
	 strstr(url, "http://") == url ||
#ifdef HAVE_LIBSSL
	 strstr(url, "https://") == url ||
#endif
	 strstr(url, "file://") == url)
	  return 1;
     
     if (p) {

	  for (p--; p >= url; p--)
	       if (!isalpha(*p))
		    break;
	  if (p < url) {
	       *supported_scheme = 0; /* we don't support this one. */
	       return 1;
	  } else 
	       return 0;	  
     } else 
	  return 0;
}

static int add_msg_part(MIMEEntity *res, xmlNodePtr node, Octstr *base_url,
			Octstr *top_url,  
			int type, Octstr *svc_name,
			Octstr *mmc_id,
			Dict *url_map, Octstr *xmhdr, Octstr *xmhdr_val,
			int (*filter)(MIMEEntity **msg, Octstr *loc_url, Octstr *mmc_id))
{
     Octstr *curl = NULL, *ctype = NULL, *body = NULL, *xcid = NULL;
     char *src = NULL;
     int isurl, slash_prefix;
     Octstr *cid = NULL;
     int supported_url_scheme = 0;
     
     /* For each node in the smil doc, if it has an src attribute, then: 
      * - if our type of base_url is FILE *and the src attribute does not look 
      *   like a url, then file the file referenced, load it into the message and go
      * - if our type is URL and the url scheme is http/https (or has no scheme)
      *   then fetch it and put into message. 
      */
     
     if (!node || node->type != XML_ELEMENT_NODE ||
	 (src = (char *)xmlGetProp(node, (unsigned char *)"src")) == NULL)
	  return 0; /* Nothing to do. */
     
     if (src[0] == '\\') { /* User can escape url to prevent substitution. */
	  xmlSetProp(node, (xmlChar *)"src", (xmlChar *)(src + 1));
	  goto done;
     }

     isurl = has_url_scheme(src, &supported_url_scheme);
     slash_prefix = (src[0] == '/');

     if (isurl && !supported_url_scheme)
	  goto done;
     else if (isurl)
	  curl = octstr_create(src);
     else if (slash_prefix) {
	  if (type == URL_TYPE)
	       curl = octstr_format("%S%s",
				    top_url, src);
	  else 
	       curl = octstr_create(src);	       	       
     } else 
	  curl = octstr_format("%S/%s",base_url, src);

     if ((cid = dict_get(url_map, curl)) != NULL) { /* We've seen it before. */
	  xmlSetProp(node, (xmlChar *)"src", (xmlChar *)octstr_get_cstr(cid));
	  /* Don't delete cid! */
	  goto done;
     }
     
     isurl |= (type == URL_TYPE); /* From now on, this flag tells us whether we are fetching a url.*/

     if (isurl) {
	  List *rh = http_create_empty_headers(), *rph = NULL;
	  
	  http_header_add(rh, "User-Agent", MM_NAME "/" VERSION);	       
	  if (mms_url_fetch_content(HTTP_METHOD_GET, curl, rh, NULL, &rph, &body) == HTTP_OK) {
	       ctype = http_header_value(rph, octstr_imm("Content-Type"));
	       xcid = http_header_value(rph, octstr_imm("Content-ID"));
	  } else 
	       mms_error(0,  "mmsbox", NULL, "Failed to load url %s within SMIL content from service %s!",
			 octstr_get_cstr(curl), 
			 svc_name ? octstr_get_cstr(svc_name) : "unknown");
	  if (rph)
	       http_destroy_headers(rph);
	  http_destroy_headers(rh);
     } else {
	  body = octstr_read_file(octstr_get_cstr(curl));
	  ctype = filename2content_type(src);
     }
     
     if (ctype && body) { /* If we got it, put it in. */
	  static int cntr;  /* For generating cids */
	  char _fext[5] = {0}, *fext = make_file_ext(curl, ctype, _fext);
	  Octstr *attr = xcid ? octstr_format("cid:%S", xcid) : octstr_format("cid:%06d.%s", ++cntr,fext);
	  char  *p = octstr_get_cstr(attr) + 4;
	  Octstr *cid_header_val = octstr_format("<%s>", p);
	  MIMEEntity *x = mime_entity_create();
	  List *headers = http_create_empty_headers();
	  
	  http_header_add(headers, "Content-Type", octstr_get_cstr(ctype));
	  http_header_add(headers, "Content-ID", octstr_get_cstr(cid_header_val));
	  http_header_add(headers, "Content-Location", p);

	  if (xmhdr)
	       http_header_add(headers, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
	  
	  mime_replace_headers(x, headers);
	  mime_entity_set_body(x, body);
	  
	  
	  if (filter && mmc_id)
	       filter(&x, curl, mmc_id);

	  mime_entity_add_part(res, x);
	  mime_entity_destroy(x);

	  dict_put_once(url_map, curl, octstr_duplicate(attr)); /* Store the cid. */

	  xmlSetProp(node, (xmlChar *)"src", (xmlChar *)octstr_get_cstr(attr));

	  octstr_destroy(attr);
	  octstr_destroy(cid_header_val);
	  http_destroy_headers(headers);
     }

done:

     octstr_destroy(curl);
     octstr_destroy(ctype);
     octstr_destroy(body);
     octstr_destroy(xcid);
     xmlFree(src);
     return 0;
}

/* Traverse the tree doing the above. */
static void add_msg_parts(MIMEEntity *res, xmlNodePtr node, Octstr *base_url, 
			  Octstr *top_url, 
			  int type, Octstr *svc_name, 
			  Octstr *mmc_id,
			  Dict *url_map, Octstr *xmhdr, Octstr *xmhdr_val,
			  int (*filter)(MIMEEntity **msg, Octstr *loc_url, Octstr *mmc_id))
{
     xmlNodePtr n;
     /* Do all the children recursively, then come back and do parent. */
     for (n = node; n; n = n->next)
	  if (n->type != XML_COMMENT_NODE) {
	       add_msg_part(res, n, base_url,  top_url, type, svc_name, mmc_id, url_map, xmhdr, 
			    xmhdr_val, filter);
	       add_msg_parts(res, n->xmlChildrenNode, base_url, top_url, type, 
			     svc_name, mmc_id, url_map, xmhdr, xmhdr_val, filter);	       
	  }          
}
/* Get's just the host:port part, leaving out UrI */
static Octstr *get_toplevel_url(Octstr *url)
{
     int i, len = octstr_len(url);
     char *p = octstr_get_cstr(url);
     
     i = octstr_search(url, octstr_imm("://"),0);
     
     if (i > 0)
	  i += 3;
     else 
	  i = 0;

     for ( ; i < len; i++)
	  if (p[i] == '/')
	       break;
     
     return octstr_copy(url, 0, i);     
}

static Octstr *url_path_prefix(Octstr *url, int type)
{
     int i, j,  len = octstr_len(url);
     char *p = octstr_get_cstr(url);

     /* Set lower/upper limit of search. */
     if (type == URL_TYPE)  { /* then skip first slashes. */
          char *x;
          i = octstr_search(url, octstr_imm("://"),0);
          if (i > 0)
               i += 3;
          else 
               i = 0;
          x = rindex(p, '#'); /* look for fragment if any. */
          
          if (x)
               j = x - p  - 1;
          else 
               j = len - 1;
     } else {
          i = 0;
          j = len - 1;
     }

     /* Now search backwards for the last '/'. 
      * if you don't find one, set to end of string. 
      */
     
     for (;j > i; j--)
          if (p[j] == '/')
               break;
     if (j <= i)
          j = len;

     return octstr_copy(url, 0, j);
}


void get_content_from_sendmms_request(Octstr *body, List *headers, List *cgivars, List *cgivar_ctypes,
				      List **rh, Octstr **ctype, Octstr **data, 
				      Octstr **rb,
				      Octstr **base_url, Octstr **data_url)
{
     Octstr *s;
     /* Now get the data. */
     if ((s = http_cgi_variable(cgivars, "text")) != NULL) { /* text. */
	  Octstr *charset = http_cgi_variable(cgivars, "charset");
	  
	  *ctype = octstr_format("text/plain");
	  if (charset) 
	       octstr_format_append(*ctype, "; charset=%S", charset);
	  *data = octstr_duplicate(s);
     } else if ((s = http_cgi_variable(cgivars, "smil")) != NULL) {  /* smil. */
	  *ctype = octstr_create("application/smil");
	  *data = octstr_duplicate(s);
     } else if ((s = http_cgi_variable(cgivars, "content-url")) != NULL) {  /* arbitrary content. */
	  List *reqh = http_create_empty_headers(), *rph = NULL;	
	  Octstr *reply = NULL, *params = NULL;
		    
	  http_header_add(reqh, "User-Agent", MM_NAME "/" VERSION);	       

	  *ctype = NULL;
	  if (mms_url_fetch_content(HTTP_METHOD_GET, s, reqh, NULL, &rph, &reply) == HTTP_OK)
	       get_content_type(rph, ctype, &params);
	  else 
	       *rb = octstr_format("failed to fetch content from url [%S]!", s);		    
	  *base_url = url_path_prefix(s, URL_TYPE);
	  
	  *data_url = octstr_duplicate(s); /* the URL of the message. */
	  *data = reply;
	  
	  http_destroy_headers(reqh);
	  http_destroy_headers(*rh);
	  *rh = rph ? rph : http_create_empty_headers(); /* replace as real reply headers. */
	  if (*ctype == NULL)
	       *ctype = http_header_value(*rh, octstr_imm("Content-Type"));
	  octstr_destroy(params);
     } else if ((s = http_cgi_variable(cgivars, "content")) != NULL) { /* any content. */		    
	  Octstr *_xctype = NULL; /* ... because cgi var stuff is destroyed elsewhere, we must dup it !! */
	  
	  *data = octstr_duplicate(s);
	  /* If the user sent us a content element, then they should have
	   * sent us its type either as part of the HTTP POST (multipart/form-data)
	   * or as CGI VAR called content_type 
	   */
	  if ((_xctype = http_cgi_variable(cgivars, "content_type")) == NULL)
	       if (cgivar_ctypes) 
		    _xctype = http_cgi_variable(cgivar_ctypes, "content");  
	  if (_xctype) 
	       *ctype = octstr_duplicate(_xctype); 
	  else 
	       *ctype = octstr_create("application/octet-stream");
     } else if (body) { /* if all above fails, 
			   use send-mms msg body if any. */
	  *data = octstr_duplicate(body);
	  *ctype = http_header_value(headers, octstr_imm("Content-Type"));
     } else
	  *rb = octstr_create("Missing content");
}

MmsMsg *make_msg_from_sendmms_request(Octstr *from, List *to,
				      Octstr *msgId,
				      Octstr *subject,
				      Octstr *data, Octstr *ctype, 
				      List *reply_headers, 
				      Octstr *msg_url,
				      Octstr *base_url, int type, 
				      Octstr *mmc,
				      Octstr *svc_name,
				      time_t expiryt,
				      int do_multipart,
				      int do_rr,
				      int do_dlr,
				      char *allow_adaptations,
				      int (*filter)(MIMEEntity **msg, Octstr *loc_url, Octstr *mmc_id),
				      Octstr **err)
{
     Octstr *mclass = http_header_value(reply_headers, octstr_imm("X-Mbuni-MessageClass"));
     Octstr *prio = http_header_value(reply_headers, octstr_imm("X-Mbuni-Priority"));
     Octstr *turl = get_toplevel_url(base_url);
     MmsMsg *m = NULL;
     MIMEEntity *me = mime_entity_create();
     Octstr *xmhdr = NULL, *xmhdr_val = NULL;
     List *xheaders = NULL;
     int i, n;

     if (octstr_str_case_compare(ctype, "application/vnd.wap.mms-message") == 0)
	  m = mms_frombinary(data, from);
     else if (octstr_case_search(ctype, octstr_imm("multipart/"), 0) == 0) { /* treat it differently. */
	  MIMEEntity *mime = mime_http_to_entity(reply_headers, data);
	  if (mime) {
	       mime_entity_destroy(me);
	       me = mime;
	  } 	       
     } else if (octstr_case_search(ctype, 
				   octstr_imm(MBUNI_MULTIPART_TYPE), 0) == 0) { /* Mbuni multipart.*/
	  List *l = octstr_split_words(data);
	  MIMEEntity *mime = multipart_from_urls(l);

	  if (mime) {
	       mime_entity_destroy(me);
	       me = mime;
	  } 	       
	  gwlist_destroy(l, (void *)octstr_destroy);
     } else if (octstr_case_search(ctype, octstr_imm("application/smil"), 0) == 0) {
	  xmlDocPtr smil;
	  xmlChar *buf = NULL;
	  int bsize = 0;
	  Dict *url_map = dict_create(97, (void (*)(void *))octstr_destroy);
	  List *xh = http_create_empty_headers();
	  
	  /* This is the hard bit: Fetch each external reference in smil, add it to message! */
	  http_header_add(xh, "Content-Type", "multipart/related; "
			  "type=\"application/smil\"; start=\"<presentation>\"");
	  mime_replace_headers(me,xh);
	  http_destroy_headers(xh);

	  /* Parse the smil as XML. */
	  smil = xmlParseMemory(octstr_get_cstr(data), octstr_len(data));
	  if (!smil || !smil->xmlChildrenNode) {
	       *err = octstr_format("MMSBox: Error parsing SMIL response from service[%s], "
				    " msgid %s!", octstr_get_cstr(svc_name), 
				    msgId ? octstr_get_cstr(msgId) : "(none)");
	       dict_destroy(url_map);
	       goto done;
	  }
	  
	  add_msg_parts(me, smil->xmlChildrenNode, 
			base_url, turl, type, svc_name, 
			mmc, url_map, xmhdr, xmhdr_val,
			filter);

	  dict_destroy(url_map);
	  /* SMIL has been modified, convert it to text, put it in. */
	  xmlDocDumpFormatMemory(smil, &buf, &bsize, 1);
	  xmlFreeDoc(smil);
	  if (buf) {
	       MIMEEntity *sm = mime_entity_create();
	       List *xh = http_create_empty_headers();
	       Octstr *s;
	       
	       http_header_add(xh, "Content-Type", "application/smil");
	       http_header_add(xh, "Content-ID", "<presentation>"); /* identify it as start element. */
	       if (xmhdr)
		    http_header_add(xh, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
	       s = octstr_create_from_data((char *)buf, bsize);
	       
	       mime_replace_headers(sm, xh);
	       mime_entity_set_body(sm, s);
	       
	       mime_entity_add_part(me, sm);

	       mime_entity_destroy(sm);
	       http_destroy_headers(xh);
	       octstr_destroy(s);
	       
	       xmlFree(buf);
	  } else {
	       *err = octstr_format("MMSBox: Error writing converted SMIL "
				    "for response from service[%s], "
				    " msgid %s!", 
				    octstr_get_cstr(svc_name), 
				    (msgId) ? octstr_get_cstr(msgId) : "(none)");
	       goto done;	       
	  }	  
     } else { /* all others, make the message as-is and hope for the best! */
	  List *xh = http_create_empty_headers();
	  
	  if (do_multipart) { /* if it's going to be multi-part, add some headers. */
	       static int cntr = 0;
	       char _fext[5] = {0}, *fext = make_file_ext(msg_url, ctype, _fext);
	       Octstr *attr = octstr_format("%06d.%s", ++cntr,fext);
	       Octstr *cid_header_val = octstr_format("<%S>", attr);
	  
	       http_header_add(xh, "Content-ID", octstr_get_cstr(cid_header_val));
	       http_header_add(xh, "Content-Location", octstr_get_cstr(attr));
	       
	       octstr_destroy(attr);
	       octstr_destroy(cid_header_val);
	  }

	  http_header_add(xh, "Content-Type", octstr_get_cstr(ctype));	  
	  if (xmhdr)
	       http_header_add(xh, octstr_get_cstr(xmhdr), octstr_get_cstr(xmhdr_val));		    
	  
	  mime_replace_headers(me, xh);

	  http_destroy_headers(xh);
	  mime_entity_set_body(me, data);	  
	  
	  if (filter && mmc) /* filter it too. */
	       filter(&me, msg_url, mmc);

	  if (do_multipart && 
	      octstr_case_search(ctype, octstr_imm("multipart/"), 0) != 0) { /* requires multipart but this one is not, wrap it */
	       MIMEEntity *xm = mime_entity_create();
	       List *h1 = http_create_empty_headers();
	       
	       http_header_add(h1, "Content-Type", "multipart/related");	  	       
	       mime_replace_headers(xm, h1);
	       mime_entity_add_part(xm, me);
	       
	       http_destroy_headers(h1);
	       
	       me = xm;
	  }
     }
     
     /* Add some nice headers. */
     xheaders = mime_entity_headers(me);
     http_header_add(xheaders, "From", octstr_get_cstr(from));
     
     for (i = 0, n = gwlist_len(to); i < n; i++) {
	  Octstr *v;
	  v = gwlist_get(to, i);
	  http_header_add(xheaders, "To", octstr_get_cstr(v));
     }
     
     if (do_dlr)
	  http_header_add(xheaders, "X-Mms-Delivery-Report", "Yes");	  
     if (do_rr)
	  http_header_add(xheaders, "X-Mms-Read-Report", "Yes");	  
     if (allow_adaptations)
	  http_header_add(xheaders, "X-Mms-Allow-Adaptations", 
			  (strcasecmp(allow_adaptations, "true") == 0) ? "true" : "false");
     
     if (mclass)
	  http_header_add(xheaders, "X-Mms-Message-Class", octstr_get_cstr(mclass));	  

     if (prio)
	  http_header_add(xheaders, "X-Mms-Priority", octstr_get_cstr(prio));	  
     
     if (subject)
	  http_header_add(xheaders, "Subject", octstr_get_cstr(subject));	  

     if (expiryt > 0) {
	  Octstr *x = date_format_http(expiryt);
	  http_header_add(xheaders, "X-Mms-Expiry", octstr_get_cstr(x));
	  octstr_destroy(x);
     }
     mime_replace_headers(me, xheaders);


     if (me && !m) /* Set m if it hasn't been set yet. */
	  m = mms_frommime(me);     
     if (!m) 
	  *err = octstr_format("MMSBox: Failed to convert Mms Message from service %s!",
			       octstr_get_cstr(svc_name));

done:          
     octstr_destroy(mclass);
     octstr_destroy(prio);
     if (me)
	  mime_entity_destroy(me);
     octstr_destroy(xmhdr_val);
     octstr_destroy(turl);
     http_destroy_headers(xheaders);
     return m;
}
