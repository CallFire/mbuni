/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSC handler functions: Receive and send MMS messages to MMSCs 
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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mmsbox.h"
#include "mms_queue.h"

#include "mmsbox.h"

#define MOD_SUBJECT(msg, mmc,xfrom) do {				\
	  if ((mmc)->reroute_mod_subject) {				\
	       Octstr *s = mms_get_header_value((msg),octstr_imm("Subject")); \
	       Octstr *f = octstr_duplicate(xfrom);			\
	       int _i;							\
	       if (s == NULL) s = octstr_create("");			\
	       if ((_i = octstr_search(f, octstr_imm("/TYPE="), 0)) >= 0) \
		    octstr_delete(f, _i, octstr_len(f));		\
	       octstr_format_append(s, " (from %S)", (f));		\
	       mms_replace_header_value((msg), "Subject", octstr_get_cstr(s)); \
	       octstr_destroy(s); octstr_destroy(f);			\
	  }								\
     } while(0)



static Octstr *get_dlr_notify_url(Octstr *msgid, char *report_type, Octstr *mmc_gid, Octstr *mmc_id,
				  Octstr *status,
				  Octstr **transid)
{

     Octstr *xtransid = NULL, *url = NULL;

     mms_dlr_url_get(msgid, report_type, mmc_gid, &url, &xtransid);

     if (transid)
	  *transid = xtransid;
     else 
	  octstr_destroy(xtransid);
     
     if (octstr_len(url) == 0) {
	  if (url)
	       mms_info(0, "MM7", NULL, 
			"Sending delivery-report skipped: `url' is empty, `group_id'=[%s], `msgid'=[%s]",
			octstr_get_cstr(mmc_gid), octstr_get_cstr(msgid));
	  octstr_destroy(url); 
	  url = NULL;
	  goto done;
     } else if (octstr_search(url, octstr_imm("msgid:"), 0) == 0) { /* a fake one, skip it. */
	  octstr_destroy(url); 
	  url = NULL;
	  
	  goto done;
     }
#if 0
     /* At what point do we delete it? For now, when we get a read report, 
      * and also when we get  a delivery report that is not 'deferred' or sent or forwarded
      */

     if (strcmp(report_type, "read-report") == 0 ||
	 (octstr_case_compare(status, octstr_imm("Deferred")) != 0 &&
	  octstr_case_compare(status, octstr_imm("Forwarded")) != 0))
	  mms_dlr_url_remove(msgid, report_type, mmc_gid);
#endif
done:

     return url;
}


static void fixup_relayed_report(MmsMsg *m, MmscGrp *mmc, char *rtype, Octstr *status)
{
     
     Octstr *value = mms_get_header_value(m, octstr_imm("Message-ID")); 
     Octstr *newmsgid = NULL, *transid = NULL;
     
     /* Firstly, take care to look for the record we saved, and re-write the MessageID. */
     if (value && 
	 mms_dlr_url_get(value, rtype, mmc->group_id, &newmsgid, &transid) == 0) {
	  int x = octstr_search_char(newmsgid, ':', 0);
	  
	  if (x>=0) 
	       octstr_delete(newmsgid, 0, x+1);
	  
	  mms_replace_header_value(m, "Message-ID", octstr_get_cstr(newmsgid));
	  /* Add it back as original. */
	  mms_replace_header_value(m, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value));

#if 0	
	  if (strcmp(rtype, "read-report") == 0 ||
	      (octstr_case_compare(status, octstr_imm("Deferred")) != 0 &&
	       octstr_case_compare(status, octstr_imm("Forwarded")) != 0))	       
	       mms_dlr_url_remove(value, rtype, mmc->group_id); /* only remove if not 
								 * interim status 
								 */
#endif
     }
     octstr_destroy(newmsgid);
     octstr_destroy(transid);
     octstr_destroy(value);
}

/* returns the DLR/RR URL, fills in the queue header info. */
Octstr *mmsbox_get_report_info(MmsMsg *m, MmscGrp *mmsc, Octstr *out_mmc_id, char *report_type, 
			       Octstr *status, List *qhdr, Octstr *uaprof, 
			       time_t uaprof_tstamp, 
			       Octstr *msgid)
{
     Octstr *res;

     if (mmsc == NULL)
	  res = NULL;
     else if (out_mmc_id != NULL) { /* internal routing. */      
	  if (m) 
	       fixup_relayed_report(m, mmsc, report_type, octstr_imm(""));
	  res = NULL;
     } else {
	  Octstr *transid = NULL;
	  
	  res = get_dlr_notify_url(msgid, report_type,mmsc->group_id, mmsc->id, 
				   status, &transid);
	  http_header_add(qhdr, "X-Mbuni-Mmsc-GroupID", octstr_get_cstr(mmsc->group_id));
	  
	  if (transid) {		    
	       http_header_add(qhdr, "X-Mbuni-TransactionID", octstr_get_cstr(transid));		    
	       octstr_destroy(transid);
	  }
	       
	  if (uaprof) {
	       Octstr *sx = date_format_http(uaprof_tstamp);
	       http_header_add(qhdr, "X-Mbuni-UAProf", octstr_get_cstr(uaprof));
	       http_header_add(qhdr, "X-Mbuni-Timestamp", octstr_get_cstr(sx));
	       octstr_destroy(sx);
	  }	  
     }

     return res;
}

/* These functions are very similar to those in mmsproxy */
static int mm7soap_receive(MmsBoxHTTPClientInfo *h)
{

     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     int hstatus = HTTP_OK;
     List *rh = NULL, *lh;
     Octstr *reply_body = NULL;
     
     List *to = NULL;
     Octstr *from = NULL, *subject = NULL,  *vasid = NULL, *msgid = NULL, *uaprof = NULL;
     time_t expiryt = -1, delivert = -1, uaprof_tstamp = -1;
     MmsMsg *m = NULL;
     int status = 1000;
     unsigned char *msgtype = (unsigned char *)"";
     Octstr *qf = NULL, *mmc_id = NULL, *qdir = NULL;
     List *qhdr = http_create_empty_headers();
     Octstr *r, *s, *transid = NULL, *value = NULL;
     Octstr *carrier_id = NULL;
     
     if (h->body)     
	  mreq = mm7_parse_soap(h->headers, h->body);
     if (mreq)  {
	  msgtype = mms_mm7tag_to_cstr(mm7_msgtype(mreq));
	  MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE);
     } else  
	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);
     
     debug("mmsbox.mm7sendinterface", 0,
	   " --> Enterred mm7dispatch interface, mreq=[%s] mtype=[%s] <-- ",
	   mreq ? "Ok" : "Null",
	   mreq ? (char *)msgtype : "Null");
          
     if (!mreq) {
	  mresp = mm7_make_resp(NULL, MM7_SOAP_FORMAT_CORRUPT, NULL,1);
	  status = 4000;
	  goto done;
     } 

     mm7_get_envelope(mreq, &from, &to, &subject, &vasid, 
		      &expiryt, &delivert, &uaprof, &uaprof_tstamp);
     
     if (!from)
	  from = octstr_create("anon@anon");
     
     if (h->m->carrier_id_header)
	  carrier_id = http_header_value(h->headers, h->m->carrier_id_header);

     qdir = get_mmsbox_queue_dir(from, to, h->m, &mmc_id); /* get routing info. */

     switch (mm7_msgtype(mreq)) {
	  Octstr  *value2;

     case MM7_TAG_DeliverReq:
	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  if (m) {
	       /* Store linked id so we use it in response. */
	       Octstr *linkedid = mm7_soap_header_value(mreq, octstr_imm("LinkedID"));
	       List *qh = http_create_empty_headers();
	       int dlr;
	       
	       value = mms_get_header_value(m, octstr_imm("X-Mms-Delivery-Report"));	  

	       if (value && 
		   octstr_case_compare(value, octstr_imm("Yes")) == 0) 
		    dlr = 1;
	       else 
		    dlr = 0;
	       
	       if (delivert < 0)
		    delivert = time(NULL);
	       
	       if (expiryt < 0)
		    expiryt = time(NULL) + DEFAULT_EXPIRE;
	       
	       if (uaprof) {
		    Octstr *sx = date_format_http(uaprof_tstamp);
		    http_header_add(qh, "X-Mbuni-UAProf", octstr_get_cstr(uaprof));
		    http_header_add(qh, "X-Mbuni-Timestamp", octstr_get_cstr(sx));
		    octstr_destroy(sx);
	       }
	       
	       if (carrier_id) 
		    http_header_add(qh, "X-Mbuni-CarrierID", octstr_get_cstr(carrier_id));

	       MOD_SUBJECT(m, h->m, from);
	       
	       qf = qfs->mms_queue_add(from, to, subject, 
				       h->m->id, mmc_id,
				       delivert, expiryt, m, linkedid, 
				       NULL, NULL, 
				       NULL, NULL,
				       qh,
				       dlr, 
				       octstr_get_cstr(qdir),
				       "MM7/SOAP-IN",
				       NULL);

	       if (qf == NULL)  {
		    status = 4000; 
		    mms_error(0, "MM7", h->m->id,
			      "Failed to write queue entry for received MM7/SOAP DeliverReq message from mmc=%s to MMS Message!",
			      octstr_get_cstr(h->m->id));
		    
		    MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	       } else {
		    MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 

		    msgid = mms_make_msgid(octstr_get_cstr(qf), NULL);
		    mms_log("Received", from, to, -1, msgid, NULL, h->m->id, "MMSBox", 
			    h->ua, NULL);
	       }
	       
	       octstr_destroy(linkedid);

	       http_destroy_headers(qh);
	  }  else {
	       mms_error(0, "MM7", h->m->id,
			 "Failed to convert received MM7/SOAP DeliverReq message from mmc=%s to MMS Message!",
			 octstr_get_cstr(h->m->id));
	       status = 4000;	  
	  }
	  mresp = mm7_make_resp(mreq, status, NULL,1);

	  break; 	  
	  
     case MM7_TAG_DeliveryReportReq:
	  value = mm7_soap_header_value(mreq, octstr_imm("MMStatus"));
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  
	  if ((value2 = mm7_soap_header_value(mreq, octstr_imm("StatusText"))) != NULL) {
	       
	       http_header_add(qhdr, "X-Mbuni-StatusText", octstr_get_cstr(value2));
	       octstr_destroy(value2);
	       value2 = NULL;
	  }

	  if ((value2 = mm7_soap_header_value(mreq, octstr_imm("Details"))) != NULL) {
	       
	       http_header_add(qhdr, "X-Mbuni-StatusDetails", octstr_get_cstr(value2));
	       octstr_destroy(value2);
	       value2 = NULL;
	  }
	  
	  if (carrier_id) 
	       http_header_add(qhdr, "X-Mbuni-CarrierID", octstr_get_cstr(carrier_id));
	  
	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  value2 = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, qhdr, uaprof, uaprof_tstamp, msgid);
	  qf = qfs->mms_queue_add(from, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  value2, NULL,
				  qhdr,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/SOAP-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("Received DLR", from, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);
	  } else {
	       status = 4000;
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR,3);			 
	  }
	  mresp = mm7_make_resp(mreq, status, NULL,1);


	  octstr_destroy(value2);
	  break;
     
     case MM7_TAG_ReadReplyReq:

	  m = mm7_soap_to_mmsmsg(mreq, from); 
	  value = mm7_soap_header_value(mreq, octstr_imm("MMStatus"));
	  msgid = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
	  
	  value2 = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", value, qhdr, uaprof, uaprof_tstamp, msgid);

	  if (carrier_id) 
	       http_header_add(qhdr, "X-Mbuni-CarrierID", octstr_get_cstr(carrier_id));

	  qf = qfs->mms_queue_add(from, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  value2, NULL,
				  qhdr,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/SOAP-IN",
				  NULL);
	  if (qf)  {
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);
	       /* Log to access log */
	       mms_log("Received RR", from, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);		    
	  } else {
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);			 
	       status = 4000;
	  }
	  mresp = mm7_make_resp(mreq, status, NULL,1);

	  octstr_destroy(value2);
	  break;
	  
     default:
	  mresp = mm7_make_resp(mreq, MM7_SOAP_UNSUPPORTED_OPERATION, NULL,1);
	  status = MM7_SOAP_UNSUPPORTED_OPERATION;
	  break;	  
     }
     

     /* Invoke call back */
     s = mm7_soap_header_value(mreq, octstr_imm("MM7Version"));
     r = mm7_soap_header_value(mreq, octstr_imm("MessageID"));
     transid = mm7_soap_header_value(mreq, octstr_imm("TransactionID"));
     lh = mm7_soap_headers(mreq);
     mmsbox_event_cb(h->m->id, mm7_msgtype(mreq), 0, s, 200, 
		     octstr_len(h->body), 0, from, 
		     to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL, r, transid, lh, value);
     octstr_destroy(s);
     octstr_destroy(r);
     http_destroy_headers(lh);
done:
     if (mresp && mm7_soapmsg_to_httpmsg(mresp, &h->m->ver, &rh, &reply_body) == 0) 
	  http_send_reply(h->client, hstatus, rh, reply_body);
     else 
	  http_close_client(h->client);

     if (mresp) {
	  Octstr *s = octstr_format("%d.%d.%d", h->m->ver.major, h->m->ver.minor1, h->m->ver.minor2);
	  Octstr *r = mm7_soap_header_value(mresp, octstr_imm("MessageID"));
	  List *lh = mm7_soap_headers(mresp);
	  mmsbox_event_cb(h->m->id, mm7_msgtype(mresp), 0, s, status, 
			  octstr_len(reply_body), 0, to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL, 
			  from, r, transid, lh, NULL);
	  octstr_destroy(s);
	  octstr_destroy(r);
	  http_destroy_headers(lh);
     } else 
	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE, 2);
     
     debug("mmsbox.mm7sendinterface", 0,
	   " --> leaving mm7dispatch interface, mresp=[%s], body=[%s], mm7_status=[%d] <-- ",
	   mresp ? "ok" : "(null)",
	   reply_body ? "ok" : "(null)", status);
     
     octstr_destroy(from);     
     octstr_destroy(subject);
     octstr_destroy(vasid);
     octstr_destroy(msgid);
     octstr_destroy(qf);
     octstr_destroy(uaprof);
     mms_destroy(m);
     http_destroy_headers(rh);
     octstr_destroy(reply_body);
     mm7_soap_destroy(mresp);
     mm7_soap_destroy(mreq);
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);     
     octstr_destroy(mmc_id);
     http_destroy_headers(qhdr);
     octstr_destroy(value);
     octstr_destroy(transid);

     return MM7_SOAP_STATUS_OK(status) ? 0 : -1;
}


/* helper function for queueing delivery reports. */
static int queue_dlr(MmscGrp *mmc, Octstr *from, Octstr *to, Octstr *msgid, Octstr *status, char *interf, List *errl)
{
     Octstr *mmc_id = NULL, *qdir;
     MmsMsg *m = mms_deliveryreport(msgid, from, to, time(NULL), status);
     List *lto = gwlist_create();
     int ret;
     Octstr *qf, *rr_uri = NULL;
     List *rqh = http_create_empty_headers(); 

     
     if (errl) 
	  http_header_combine(rqh, errl); /* add status stuff. */

     
     gwlist_append(lto, octstr_duplicate(to));

     
     qdir = get_mmsbox_queue_dir(from, lto, mmc, &mmc_id); /* get routing info. */

     rr_uri = mmsbox_get_report_info(m, mmc, mmc_id, "delivery-report", status, rqh, NULL, 0, msgid);     
     
     mmsbox_event_cb(mmc ? mmc->id : NULL, MM7_TAG_DeliveryReportReq, 0,octstr_imm("5.3.0"), 200, 
		     0, 0, from, 
		     lto && gwlist_len(lto) > 0 ? gwlist_get(lto,0) : NULL,
		     msgid, NULL, NULL, status);
     
     qf = qfs->mms_queue_add(from, lto, NULL, 
			     mmc ? mmc->id : NULL, mmc_id,
			     0, time(NULL) + default_msgexpiry, m, NULL, 
			     NULL, NULL,
			     rr_uri, NULL,
			     rqh,
			     0,
			     octstr_get_cstr(qdir), 				  
			     interf,
			     NULL);
     if (qf)  {
	  /* Log to access log */
	  mms_log("Received DLR", from, lto, -1, msgid, status, mmc ? mmc->id : NULL, "MMSBox", NULL, NULL);
	  ret = 0;
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR);
     }  else {
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);			 
	  ret = -1;
     }
     
     octstr_destroy(qf);
     http_destroy_headers(rqh);     
     octstr_destroy(rr_uri);
     
     gwlist_destroy(lto, (void *)octstr_destroy);
     octstr_destroy(mmc_id);
     mms_destroy(m);

     return ret;
}

static int mm7eaif_receive(MmsBoxHTTPClientInfo *h)
{
     MmsMsg *m = NULL;
     List *mh = NULL;
     int hstatus = HTTP_NO_CONTENT;
     List *rh = http_create_empty_headers(), *lh;
     List *rqh = http_create_empty_headers(); 
     Octstr *reply_body = NULL, *value = NULL, *value2 = NULL;
     
     List *to = gwlist_create(), *hto = NULL;
     Octstr *subject = NULL,  *otransid = NULL, *msgid = NULL, *s = NULL;
     Octstr *hfrom = NULL, *rr_uri = NULL;
     time_t expiryt = -1, deliveryt = -1;
     Octstr *qf = NULL, *xver = NULL, *mmc_id = NULL, *qdir = NULL;
     int msize = h->body ? octstr_len(h->body) : 0;
     int dlr;
     int i, mtype = -1, mm7type = -1;
     
     debug("mmsbox.mm7eaif.sendinterface", 0, 
	   " --> Enterred eaif send interface, blen=[%d] <--- ", 
	   msize);

     hfrom = http_header_value(h->headers, octstr_imm("X-NOKIA-MMSC-From"));     
     if (!h->body ||  /* A body is required, and must parse */
	 (m = mms_frombinary(h->body, hfrom ? hfrom : octstr_imm("anon@anon"))) == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Unexpected MMS message, no body?");

	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE,2);
	  goto done;
     } else 
	  MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE);

     /* XXXX handle delivery reports differently. */
     mtype = mms_messagetype(m);
     mm7type = mm7_msgtype_to_soaptype(mtype, 0);
     mh = mms_message_headers(m);
     /* Now get sender and receiver data. 
      * for now we ignore adaptation flags. 
      */
     mms_collect_envdata_from_msgheaders(mh, &to, &subject, 
					 &otransid, &expiryt, &deliveryt, 
					 DEFAULT_EXPIRE,
					 -1,
					 octstr_get_cstr(unified_prefix), 
					 strip_prefixes);
     
     
     if ((hto = http_header_find_all(h->headers, "X-NOKIA-MMSC-To")) != NULL &&  gwlist_len(hto) > 0) { /* To address is in headers. */
	  int i, n;
	  
	  gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
	  to = gwlist_create();
	  for (i = 0, n = gwlist_len(hto); i < n; i++) {
	       Octstr *h = NULL, *v = NULL;
	       List *l;
	       void *x;
	       
	       http_header_get(hto,i,  &h, &v);	       
	       l = http_header_split_value(v);
	       
	       while ((x = gwlist_extract_first(l)) != NULL)
		    gwlist_append(to, x);
	       
	       gwlist_destroy(l, NULL);
	       octstr_destroy(h);	       
	       octstr_destroy(v);	       	       
	  }
	  
     }
     
     qdir = get_mmsbox_queue_dir(hfrom, to, h->m, &mmc_id); /* get routing info. */
     
     switch(mtype) {
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
       
	  /* Get Message ID */
	  if ((msgid = http_header_value(h->headers, octstr_imm("X-NOKIA-MMSC-Message-Id"))) == NULL)
	       msgid = http_header_value(mh, octstr_imm("Message-ID"));	  
	  else 
	       mms_replace_header_value(m, "Message-ID", octstr_get_cstr(msgid)); /* replace it in the message.*/

	  value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));	  
	  if (value && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       dlr = 1;
	  else 
	       dlr = 0;
	 
	  
	  if (deliveryt < 0)
	       deliveryt = time(NULL);
	  
	  if (expiryt < 0)
	       expiryt = time(NULL) + DEFAULT_EXPIRE;
	  
	  if (hfrom == NULL)
	       hfrom = http_header_value(mh, octstr_imm("From"));
	  
	  mms_remove_headers(m, "Bcc");
	  mms_remove_headers(m, "X-Mms-Delivery-Time");
	  mms_remove_headers(m, "X-Mms-Expiry");
	  mms_remove_headers(m, "X-Mms-Sender-Visibility");
	  
	  MOD_SUBJECT(m, h->m, hfrom);

	  /* Save it,  put message id in header, return. */     
	  qf = qfs->mms_queue_add(hfrom, to, subject, 
				  h->m->id, mmc_id,
				  deliveryt, expiryt, m, NULL, 
				  NULL, NULL,
				  NULL, NULL,
				  NULL,
				  dlr,
				  octstr_get_cstr(qdir),
				  "MM7/EAIF-IN",
				  NULL);
	  
	  if (qf) {
	       /* Log to access log */
	       mms_log("Received", hfrom, to, msize, msgid, NULL, h->m->id, "MMSBox", h->ua, NULL);

	       hstatus = HTTP_NO_CONTENT;
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  } else {
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	  }
	  octstr_destroy(value2);	  
	  break;
     case MMS_MSGTYPE_DELIVERY_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 


	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/EAIF-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("DeliveryReport", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 	       
	       hstatus = HTTP_NO_CONTENT;
	  }  else {
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	  }
	  octstr_destroy(value2);
	  break;
	  
     case MMS_MSGTYPE_READ_ORIG_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Read-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 

	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/EAIF-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("Received RR", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);    
	       hstatus = HTTP_NO_CONTENT;
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  }  else {
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;	  
	  }
	  octstr_destroy(value2);
	  break;
     }

     lh = http_create_empty_headers(); /* Collect extension headers */
     for (i = 0; i<gwlist_len(h->headers); i++) {
	  Octstr *n = NULL, *v = NULL;

	  http_header_get(h->headers, i, &n, &v);
	  if (n && octstr_case_search(n, octstr_imm("X-NOKIA-"), 0) == 0) 
	       http_header_add(lh, octstr_get_cstr(n), octstr_get_cstr(v));

	  if (octstr_case_compare(n, octstr_imm("X-NOKIA-MMSC-Version")) == 0)
	       s = octstr_duplicate(v); /* Get mmsc version */
	  octstr_destroy(n);
	  octstr_destroy(v);
     }
     mmsbox_event_cb(h->m->id, mm7type, 0, s, hstatus, 
		     msize, 0, hfrom, 
		     to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL,
		     msgid, otransid, lh, value);
     octstr_destroy(s);
     http_destroy_headers(lh);     
done:
     
     xver = octstr_format(EAIF_VERSION, h->m->ver.major, h->m->ver.minor1);
     http_header_add(rh, "X-NOKIA-MMSC-Version", octstr_get_cstr(xver));

     mmsbox_event_cb(h->m->id, mm7type >= 0 ? mm7type + 1 : MM7_TAG_VASPErrorRsp, 0, 
		     xver, hstatus, 
		     0, 0, to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL, 
		     hfrom,
		     msgid, otransid, rh, reply_body);

     http_send_reply(h->client, hstatus, rh, reply_body ? reply_body : octstr_imm(""));

     octstr_destroy(xver);
     http_destroy_headers(hto);     
     http_destroy_headers(rqh);     
     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);
     octstr_destroy(hfrom);     
     octstr_destroy(subject);
     octstr_destroy(otransid);
     octstr_destroy(msgid);
     octstr_destroy(qf);
     octstr_destroy(mmc_id);
     octstr_destroy(rr_uri);
     octstr_destroy(value);
     
     http_destroy_headers(mh);
     mms_destroy(m);      

     return http_status_class(hstatus) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}

static int mm7http_receive(MmsBoxHTTPClientInfo *h)
{
     MmsMsg *m = NULL;
     List *mh = NULL;
     int hstatus = HTTP_OK;
     List *rh = http_create_empty_headers();
     Octstr *reply_body = NULL;
     
     List *to = NULL;
     Octstr *hto = NULL, *subject = NULL,  *msgid = NULL;
     Octstr *hfrom = NULL, *body, *rr_uri = NULL, *dlr_uri = NULL;
     time_t expiryt = -1, deliveryt = -1;
     Octstr *qf = NULL, *mmc_id = NULL, *qdir = NULL, *value = NULL, *s;
     int msize;
     int dlr, rr;
     int mtype = -1, mm7type = -1;
     List *cgivars_ctypes = NULL, *rqh = http_create_empty_headers();

     parse_cgivars(h->headers, h->body, &h->cgivars, &cgivars_ctypes);
     
     hfrom = http_cgi_variable(h->cgivars, "from");     
     hto =  http_cgi_variable(h->cgivars, "to");     
     body = http_cgi_variable(h->cgivars, "mms");

     msize = octstr_len(body);

     debug("mmsbox.mm7http.sendinterface", 0, 
	   " --> Enterred http-mmsc send interface, blen=[%d] <--- ", 
	   msize);
     
     if (hto == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Missing 'to' argument");

	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);	  
	  goto done;

     } else if (hfrom == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Missing 'from' argument");
	  
	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);
	  goto done;
	  
     } else if (body == NULL ||  /* A message is required, and must parse */
		(m = mms_frombinary(body, hfrom ? hfrom : octstr_imm("anon@anon"))) == NULL) {
	  http_header_add(rh, "Content-Type", "text/plain"); 
	  hstatus = HTTP_BAD_REQUEST;
	  reply_body = octstr_format("Unexpected MMS message, no content?");
	  
	  MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);
	  goto done;
     }  else 
	  MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_MM7_PARSING_FAILURE);    


     to = octstr_split_words(hto);

     mtype = mms_messagetype(m);
     mm7type = mm7_msgtype_to_soaptype(mtype, 0);
     mh = mms_message_headers(m);

     /* find interesting headers. */
     subject = http_header_value(mh, octstr_imm("Subject"));

     /* Find expiry and delivery times */
     
     if ((s = http_header_value(mh, octstr_imm("X-Mms-Expiry"))) != NULL) {
	  expiryt = date_parse_http(s);
	  octstr_destroy(s);
     } else 
	  expiryt = time(NULL) +  DEFAULT_EXPIRE;
          
     if ((s = http_header_value(mh, octstr_imm("X-Mms-Delivery-Time"))) != NULL) {
	  deliveryt = date_parse_http(s);
	  octstr_destroy(s);
     } else 
	  deliveryt = 0;
     
     qdir = get_mmsbox_queue_dir(hfrom, to, h->m, &mmc_id); /* get routing info. */
     
     switch(mtype) {
	  Octstr *value2;
     case MMS_MSGTYPE_SEND_REQ:
     case MMS_MSGTYPE_RETRIEVE_CONF:
       
	  /* Get/make a Message ID */
	  if ((msgid = mms_get_header_value(m, octstr_imm("Message-ID"))) == NULL) { /* Make a message id for it directly. We need it below. */
	       msgid = mms_make_msgid(NULL, NULL);
	       mms_replace_header_value(m, "Message-ID", octstr_get_cstr(msgid));	       
	  }
	  
	  if ((value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"))) != NULL && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       dlr = 1;
	  else 
	       dlr = 0;
	  octstr_destroy(value);

	  if ((value = http_header_value(mh, octstr_imm("X-Mms-Read-Report"))) != NULL && 
	      octstr_case_compare(value, octstr_imm("Yes")) == 0) 
	       rr = 1;
	  else 
	       rr = 0;

	  
	  if (deliveryt < 0)
	       deliveryt = time(NULL);
	  
	  if (expiryt < 0)
	       expiryt = time(NULL) + DEFAULT_EXPIRE;
	  
	  mms_remove_headers(m, "Bcc");
	  mms_remove_headers(m, "X-Mms-Delivery-Time");
	  mms_remove_headers(m, "X-Mms-Expiry");
	  mms_remove_headers(m, "X-Mms-Sender-Visibility");
	  
	  MOD_SUBJECT(m, h->m, hfrom);
	  

	  if (qdir == outgoing_qdir) { /* We need to remember the old message ID so we can re-write it 
					* if a DLR is relayed backwards. 			
					*/
	       Octstr *t = mms_maketransid(NULL, octstr_imm(MM_NAME)); /* make a fake transaction id  so dlr works*/

	       http_header_add(rqh, "X-Mbuni-TransactionID", octstr_get_cstr(t));
	       if (dlr)
		    dlr_uri = octstr_format("msgid:%S", msgid);
	       if (rr)
		    rr_uri  =  octstr_format("msgid:%S", msgid); 	       	 

	       octstr_destroy(t);
	  }

	  /* Save it,  put message id in header, return. */     
	  qf = qfs->mms_queue_add(hfrom, to, subject, 
				  h->m->id, mmc_id,
				  deliveryt, expiryt, m, NULL, 
				  NULL, NULL,
				  dlr_uri, rr_uri,
				  rqh,
				  dlr,
				  octstr_get_cstr(qdir),
				  "MM7/HTTP-IN",
				  NULL);
	  
	  if (qf) {
	       /* Log to access log */
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	       mms_log("Received", hfrom, to, msize, msgid, NULL, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_OK;
	  } else  {
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	  }
	  break;
     case MMS_MSGTYPE_DELIVERY_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 
	  
	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "delivery-report", 
					  value, rqh, NULL, 0, msgid);
	  if (mmc_id == NULL && value2)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2));		    

	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/HTTP-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("DeliveryReport", hfrom, to, -1, msgid,value, h->m->id, "MMSBox", h->ua, NULL);
	       
	       hstatus = HTTP_OK;
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  }  else {
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	  }
	  octstr_destroy(value2);
	  break;
	  
     case MMS_MSGTYPE_READ_ORIG_IND:
	  msgid = mms_get_header_value(m, octstr_imm("Message-ID")); 
	  value = mms_get_header_value(m, octstr_imm("X-Mms-Read-Status"));
	  value2 = mms_get_header_value(m, octstr_imm("X-Mbuni-Orig-Message-ID")); 

	  rr_uri = mmsbox_get_report_info(m, h->m, mmc_id, "read-report", 
					  value, rqh, NULL, 0, msgid);
	  if (mmc_id == NULL && value2)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2));		    

	  qf = qfs->mms_queue_add(hfrom, to, NULL, 
				  h->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 				  
				  "MM7/HTTP-IN",
				  NULL);
	  if (qf)  {
	       /* Log to access log */
	       mms_log("Received RR", hfrom, to, -1, msgid, value, h->m->id, "MMSBox", h->ua, NULL);		    
	       hstatus = HTTP_NO_CONTENT;
	       MMSC_CLEAR_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  }  else {
	       hstatus = HTTP_INTERNAL_SERVER_ERROR;
	       MMSC_ISSUE_ALARM(h->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	  }
	  octstr_destroy(value2);
	  break;
     }


     mmsbox_event_cb(h->m->id, mm7type, 0, octstr_imm("1.0"), 200, 
		     msize, 0, hfrom, 
		     to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL,
		     msgid, octstr_imm("0000"), NULL, value);     
done:


     mmsbox_event_cb(h->m->id, mm7type >= 0 ? mm7type + 1 : MM7_TAG_VASPErrorRsp, 0, 
		     octstr_imm("1.0"), hstatus, 
		     0, 0, to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL, 
		     hfrom,
		     msgid, octstr_imm("0001"), NULL, reply_body);
     
     http_header_add(rh, "X-Mbuni-Version", VERSION);
     
     http_send_reply(h->client, hstatus, rh, msgid ? msgid : reply_body ? reply_body : qf ? qf : octstr_imm(""));

     gwlist_destroy(to, (gwlist_item_destructor_t *)octstr_destroy);

     octstr_destroy(subject);

     octstr_destroy(qf);
     octstr_destroy(mmc_id);
     octstr_destroy(msgid);
     octstr_destroy(reply_body);
     http_destroy_headers(mh);
     http_destroy_headers(rh);
     http_destroy_headers(rqh);
     octstr_destroy(value);

     mms_destroy(m);      
     
     http_destroy_cgiargs(cgivars_ctypes);
     
     return http_status_class(hstatus) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}

static int mm7mm1_receive(MmsBoxHTTPClientInfo *);
static void dispatch_mm7_recv(List *rl) 
{

     MmsBoxHTTPClientInfo *h;
     
     /* hmon->register_thread( "dispatch_mm7" ); */
     while ((h = gwlist_consume(rl)) != NULL) {
	  MmscGrp *m = h->m;
	  char *user = m->incoming.user ? octstr_get_cstr(m->incoming.user) : NULL;
	  char *pass = m->incoming.pass ? octstr_get_cstr(m->incoming.pass) : NULL;
	  int ret = -1, has_auth = (m->type != MM1_MMSC); /* We dont authenticate mm1. right? */

	  if (!has_auth && http_auth_check(user, 
			 pass, 
			 h->headers, &has_auth) != 0) { /* Ask it to authenticate... */
	       List *hh = http_create_empty_headers();
	       http_header_add(hh, "WWW-Authenticate", 
			       "Basic realm=\"" MM_NAME "\"");
	       http_send_reply(h->client, HTTP_UNAUTHORIZED, hh, 
			       octstr_imm("Authentication failed"));			   
	       http_destroy_headers(hh);
	       if (!has_auth)
		    mms_info_ex("auth",0, "MM7", m->id, "Auth failed, incoming connection, MMC group=[%s]",
				m->id ? octstr_get_cstr(m->id) : "(none)");
	       else 
		    mms_error_ex("auth",0, "MM7", m->id, "Auth failed, incoming connection, MMC group=[%s]",
				 m->id ? octstr_get_cstr(m->id) : "(none)");	       
	  } else if (h->m->type == SOAP_MMSC)
	       ret = mm7soap_receive(h);
	  else if (h->m->type == EAIF_MMSC)
	       ret = mm7eaif_receive(h);
	  else if (h->m->type == MM1_MMSC)
	       ret = mm7mm1_receive(h);
	  else
	       ret = mm7http_receive(h);

	  h->m->last_pdu = time(NULL);

	  if (ret == 0)
	       h->m->mo_pdus++;
	  else 
	       h->m->mo_errors++;
	  free_mmsbox_http_clientInfo(h, 1);
     }
     /* hmon->unregister_thread( "dispatch_mm7" ); */
}

void mmsc_receive_func(MmscGrp *m)
{
     int i;
     MmsBoxHTTPClientInfo h = {NULL};
     List *mmsc_incoming_reqs = gwlist_create();
     long *thids = gw_malloc((maxthreads + 1)*sizeof thids[0]);

     gwlist_add_producer(mmsc_incoming_reqs);
     
     hmon->register_thread( "mmsc_receive" );

     for (i = 0; i<maxthreads; i++)
	  thids[i] = gwthread_create((gwthread_func_t *)dispatch_mm7_recv, mmsc_incoming_reqs);
     
     h.m = m;
     while(rstop == 0 &&
	   (h.client = http_accept_request(m->incoming.port, 
					   &h.ip, &h.url, &h.headers, 
					   &h.body, &h.cgivars)) != NULL) 
	  if (is_allowed_ip(m->incoming.allow_ip, m->incoming.deny_ip, h.ip)) {
	       MmsBoxHTTPClientInfo *hx = gw_malloc(sizeof hx[0]); 

	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));	       

	       *hx = h;		    

	       debug("mmsbox", 0, 
		     " MM7 Incoming, IP=[%s], MMSC=[%s], dest_port=[%ld] ", 
		     h.ip ? octstr_get_cstr(h.ip) : "", 
		     octstr_get_cstr(m->id),
		     m->incoming.port);  
	      	       
	       /* Dump headers, url etc. */
#if 0
	       http_header_dump(h.headers);
	       if (h.body) octstr_dump(h.body, 0);
	       if (h.ip) octstr_dump(h.ip, 0);
#endif

	       gwlist_produce(mmsc_incoming_reqs, hx);	      
	  } else {
	       h.ua = http_header_value(h.headers, octstr_imm("User-Agent"));
	       
	       mms_error_ex("auth",0,  "MM7", m->id, "HTTP: Incoming IP denied MMSC[%s] ip=[%s], ua=[%s], disconnected",
			    m->id ? octstr_get_cstr(m->id) : "(none)", 
			    h.ip ? octstr_get_cstr(h.ip) : "(none)",
			    h.ua ? octstr_get_cstr(h.ua) : "(none)");
               
               http_send_reply(h.client, HTTP_FORBIDDEN, NULL,
                               octstr_imm("Access denied."));
	       free_mmsbox_http_clientInfo(&h, 0);
	  }
     
     hmon->unregister_thread( "mmsc_receive" );

     debug("proxy", 0, "MMSBox: MM7 receiver [mmc=%s] Shutting down...", octstr_get_cstr(m->id));          
     gwlist_remove_producer(mmsc_incoming_reqs);

     for (i = 0; i<maxthreads; i++)
	  if (thids[i] >= 0)
	       gwthread_join(thids[i]);
     
     gwlist_destroy(mmsc_incoming_reqs, NULL);
     gw_free(thids);
     
     debug("proxy", 0, "MMSBox: MM7 receiver [mmc=%s] Shutting down complete.", octstr_get_cstr(m->id));
}


/* This function check if there is custom retry statuses list setup for mmc, 
 * if not it returns -1, 
 * if it is set check if current status should be retried (returns 1) or not (returns 0)
 */

int check_custom_retry_statuses(MmscGrp *mmc, int hstatus) {
    int retry_sending = -1, i, n;
    Octstr *res_str = octstr_format("%d", hstatus);
    if (mmc && mmc->retry_statuses != NULL) {
        retry_sending = 0;
        for (i = 0, n = gwlist_len(mmc->retry_statuses); i<n; i++) {
    	    Octstr *retry_status = gwlist_get(mmc->retry_statuses, i);
    	    if (octstr_compare(res_str, retry_status) == 0) {
    		retry_sending = 1;
    		break;
    	    }
	}
    }
    octstr_destroy(res_str);
    
    return retry_sending;
}


/* XXX Returns msgid in mmsc or NULL if error. Caller uses this for DLR issues. 
 * Caller must make sure throughput issues
 * are observed!
 * Don't remove from queue on fail, just leave it to expire. 
 */
static Octstr *mm7soap_send(MmscGrp *mmc, Octstr *from, List *lto, 
			    Octstr *transid,
			    Octstr *linkedid, 
			    char *vasid,
			    Octstr *service_code,
			    MmsEnvelope *e,
			    MmsMsg *m, Octstr **error,
			    List **errl,
			    int *retry)
{
     List *hdrs = e ? e->hdrs : NULL;
     Octstr *ret = NULL;
     int  mtype = mms_messagetype(m);
     int hstatus = HTTP_OK, tstatus  = -1;
     MSoapMsg_t *mreq = NULL, *mresp = NULL;
     List *rh = NULL, *ph = NULL;
     Octstr *body = NULL, *rbody = NULL, *url = NULL; 
     Octstr *s, *r, *status_details = NULL;
     char *xvasid = vasid ? vasid : (mmc->default_vasid ? octstr_get_cstr(mmc->default_vasid) : NULL);
     Octstr *to;
     int retry_sending = -1;

     LINEARISE_STR_LIST(to,lto,", ");

     if (e == NULL || mmc == NULL)
	  goto done1;
     mms_info(0, "MM7", mmc->id,  "MMSBox: Send[soap] to MMSC[%s], msg type [%s], from %s, to %s", 
	      mmc->id ? octstr_get_cstr(mmc->id) : "", 
	      mms_message_type_to_cstr(mtype), 
	      octstr_get_cstr(from), octstr_get_cstr(to));    
     
     
     if ((mreq = mm7_mmsmsg_to_soap(m, (mmc == NULL || mmc->no_senderaddress == 0) ? from : NULL, 
				    lto, transid,
				    service_code, 
				    linkedid, 
				    1, octstr_get_cstr(mmc->vasp_id), xvasid, NULL, 0,/* UA N/A on this side. */
				    hdrs)) == NULL) {
	  *error = octstr_format("Failed to convert Msg[%S] 2 SOAP message!",
				 mms_message_type_to_string(mtype));
	  goto done1;
     }
     
     if (mm7_soapmsg_to_httpmsg(mreq, &mmc->ver, &rh, &body) < 0) {
	  *error = octstr_format("Failed to convert SOAP message to HTTP Msg!");
	  goto done1;
     } 

     if (hdrs)
	  http_header_combine(rh, hdrs);  /* If specified, then update and pass on. */
     
     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph,&rbody);     
     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status=[%d]!",
				 mmc->mmsc_url, hstatus);
	  if (hstatus < 0)
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED, 3);
	
	  /* Now check if we should retry */
	  retry_sending = check_custom_retry_statuses(mmc, hstatus);
	
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT, 3);
	  goto done1;
     } else {
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED);
     }
     /* Invoke call back */
     s = mm7_soap_header_value(mreq, octstr_imm("MM7Version"));
     r = mm7_soap_header_value(mreq, octstr_imm("MessageID"));

     mmsbox_event_cb(mmc->id, mm7_msgtype(mreq), 0, s, hstatus, 
		     mms_msgsize(m), e->attempts, e->from, 
		     to,r, transid, hdrs, NULL);
     octstr_destroy(s);
     octstr_destroy(r);

     
     if ((mresp = mm7_parse_soap(ph, rbody)) == NULL) {
	  *error = octstr_format("Failed to parse MMSC[url=%S, id=%S]  response!",
				 mmc->mmsc_url,  mmc->id);

	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);			 
	  goto done1;
     } else 
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_PARSING_FAILURE);			 

     if (errl) { /* Pick up status stuff -- for DLR */
	  if (*errl == NULL)
	       *errl = http_create_empty_headers();
	  if ((s = mm7_soap_header_value(mresp, octstr_imm("StatusText"))) != NULL) {	  
	       http_header_add(*errl, "X-Mbuni-StatusText", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }

	  if ((s = mm7_soap_header_value(mresp, octstr_imm("Details"))) != NULL) {	  
	       http_header_add(*errl, "X-Mbuni-StatusDetails", octstr_get_cstr(s));
	       octstr_destroy(s);
	  }
     }

     /* Now look at response code and use it to tell you what you want. */
     if ((s = mm7_soap_header_value(mresp, octstr_imm("StatusCode"))) != NULL) {
	  tstatus = atoi(octstr_get_cstr(s));
	  octstr_destroy(s);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_PARSING_FAILURE);			 
     } else if ((s = mm7_soap_header_value(mresp, octstr_imm("faultstring"))) != NULL) {
	  tstatus = atoi(octstr_get_cstr(s));
	  octstr_destroy(s);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_PARSING_FAILURE);			 
     } else {
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_PARSING_FAILURE, 3);			 
	  tstatus = MM7_SOAP_FORMAT_CORRUPT; 
     }
     
     /* Now check if we should retry */
     retry_sending = check_custom_retry_statuses(mmc, tstatus);
     
     if (!MM7_SOAP_STATUS_OK(tstatus) && tstatus != MM7_SOAP_COMMAND_REJECTED) {
	  char *tmp = (char *)mms_soap_status_to_cstr(tstatus);

	  Octstr *detail = mm7_soap_header_value(mresp, octstr_imm("Details"));
	  if (detail == NULL)
	       detail = mm7_soap_header_value(mresp, octstr_imm("faultcode"));
	  ret = NULL;
	  mms_info(0, "MM7", mmc->id, "Send to MMSC[%s], failed, code=[%d=>%s], detail=[%s]", 
		   mmc ? octstr_get_cstr(mmc->id) : "", 
		   tstatus, tmp ? tmp : "", 
		   detail ? octstr_get_cstr(detail) : "");

	  *error = octstr_format("Failed to deliver to MMC[url=%S, id=%S], status=[%d=>%s]!",
				 mmc->mmsc_url, 
				 mmc->id,
				 tstatus, 
				 tmp ? tmp : "");
	  status_details = detail ? octstr_duplicate(detail) : tmp ? octstr_create(tmp) : octstr_imm("");
	  octstr_destroy(detail);	  	  
     } else {	  
	  ret = mm7_soap_header_value(mresp, octstr_imm("MessageID"));	  
	  mms_info(0, "MM7", NULL, "Sent to MMC[%s], code=[%d=>%s], msgid=[%s]", octstr_get_cstr(mmc->id), 
		   tstatus, mms_soap_status_to_cstr(tstatus), ret ? octstr_get_cstr(ret) : "(none)");
     }

     s = mm7_soap_header_value(mresp, octstr_imm("MM7Version"));

     mmsbox_event_cb(mmc->id, mm7_msgtype(mresp), 0, s, tstatus, 
		     0, e->attempts, e->from, 
		     to, ret, transid,hdrs, status_details);
     octstr_destroy(s);


     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);
done1:
     //DEFAULT when retry_sending < 0
     //otherwise we rely on retry_status list
     if (retry_sending < 0) *retry = (ret == NULL && (!MM7_SOAP_CLIENT_ERROR(tstatus) || tstatus < 0));
     else *retry = retry_sending;
     
     mm7_soap_destroy(mreq);
     mm7_soap_destroy(mresp);	  
     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(url);
     octstr_destroy(to);
     octstr_destroy(status_details);
     
     return ret;
}

static Octstr  *mm7eaif_send(MmscGrp *mmc, Octstr *from, List *lto, 
			     Octstr *transid,
			     char *vasid,
			     MmsEnvelope *e,
			     MmsMsg *m, Octstr **error,
			     int *retry)
{
     List *hdrs  = e ? e->hdrs : NULL;
     Octstr *ret = NULL, *resp = NULL;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK;
     List *rh = http_create_empty_headers(), *ph = NULL;
     Octstr *body = NULL, *rbody = NULL, *xver = NULL; 
     char *msgtype;
     MmsMsg *mresp = NULL;
     int mresp_type = -1, i;
     Octstr *to;
     int retry_sending = -1;

     LINEARISE_STR_LIST(to,lto,", ");

     if (e == NULL || mmc == NULL)
	  goto done;
     mms_info(0, "MM7", mmc->id,  "MMSBox: Send [eaif] to MMC[%s], msg type [%s], from %s, to %s", 
	      mmc && mmc->id ? octstr_get_cstr(mmc->id) : "", 
	      mms_message_type_to_cstr(mtype), 
	      octstr_get_cstr(from), octstr_get_cstr(to));

     http_header_remove_all(rh, "X-Mms-Allow-Adaptations");	
     for (i = 0; i < gwlist_len(lto); i++) {
	  Octstr *to = gwlist_get(lto, i);
	  http_header_add(rh, "X-NOKIA-MMSC-To", octstr_get_cstr(to));
     }
     http_header_add(rh, "X-NOKIA-MMSC-From", octstr_get_cstr(from));

     xver = octstr_format(EAIF_VERSION, mmc->ver.major, mmc->ver.minor1);
     http_header_add(rh, "X-NOKIA-MMSC-Version", octstr_get_cstr(xver));


     if (mtype == MMS_MSGTYPE_SEND_REQ || 
	 mtype == MMS_MSGTYPE_RETRIEVE_CONF) {
	  msgtype = "MultiMediaMessage";
	  mms_make_sendreq(m); /* ensure it is a sendreq. */
     } else if (mtype == MMS_MSGTYPE_DELIVERY_IND)
	  msgtype = "DeliveryReport";
     else
	  msgtype = "ReadReply";
     http_header_add(rh, "X-NOKIA-MMSC-Message-Type", msgtype);

     if (hdrs)
	  http_header_combine(rh, hdrs);  /* If specified, then update and pass on. */

     http_header_add(rh, "Content-Type", "application/vnd.wap.mms-message");

     /* Patch the message FROM and TO fields. */
     mms_replace_header_value(m, "From", octstr_get_cstr(from));
#if 0     
     mms_replace_header_value(m, "To", octstr_get_cstr(to));
#endif
     mms_replace_header_value(m,"X-Mms-Transaction-ID",
			      transid ? octstr_get_cstr(transid) : "000");
     body = mms_tobinary(m);	       
     
     mmsbox_event_cb(mmc->id, mm7_msgtype_to_soaptype(mtype,1), 0, xver, 0, 
		     octstr_len(body), e->attempts, e->from, 
		     to, NULL, transid,hdrs, NULL);

     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph, &rbody);

     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status = %d !",
				 mmc->mmsc_url, hstatus);
	  if (hstatus < 0)
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED, 3);
	
	  /* Now check if we should retry */
	  retry_sending = check_custom_retry_statuses(mmc, hstatus);
	
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT, 3);
     } else {
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED);

	  mresp = rbody ? mms_frombinary(rbody, octstr_imm("anon@anon")) : NULL;
	  mresp_type = mresp ? mms_messagetype(mresp) : -1;
	  resp = octstr_imm("Ok");
	  if (mresp_type == MMS_MSGTYPE_SEND_CONF)
	       resp = mms_get_header_value(mresp, octstr_imm("X-Mms-Response-Status"));	  
	  if (octstr_case_compare(resp, octstr_imm("ok")) != 0)
	       hstatus = HTTP_STATUS_SERVER_ERROR; /* error. */
	  else if (mresp)
	       ret = mms_get_header_value(mresp, octstr_imm("Message-ID"));
     }

     if (hstatus < 0)
	  ret = NULL; 
     else {
	  hstatus = http_status_class(hstatus);	  
	  if (hstatus == HTTP_STATUS_SERVER_ERROR ||
	      hstatus == HTTP_STATUS_CLIENT_ERROR) 
	       ret = NULL;
	  else if (!ret) 
	       ret = http_header_value(ph, octstr_imm("X-Nokia-MMSC-Message-Id"));
     }
     
     //DEFAULT when retry_sending < 0
     //otherwise we rely on retry_status list
     if (retry_sending < 0) *retry = (ret == NULL && (hstatus == HTTP_STATUS_SERVER_ERROR || hstatus < 0));
     else *retry = retry_sending;
     
     mmsbox_event_cb(mmc->id, 
		     mm7_msgtype_to_soaptype(mresp_type,1), 0, xver, hstatus, 
		     0, e->attempts, e->from, 
		     to, ret, transid, hdrs, resp);     
     
     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);

#if 0
     mms_info(0, "MM7", mmc->id,"Sent to MMC[%s], code=[%d], resp=[%s] msgid [%s]", 
	      octstr_get_cstr(mmc->id), 
	      hstatus, resp ? octstr_get_cstr(resp) : "(none)", ret ? octstr_get_cstr(ret) : "(none)");
#endif 

done:
     mms_destroy(mresp);
     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(to);
     octstr_destroy(resp);
     octstr_destroy(xver);
     return ret;
}


static Octstr  *mm7http_send(MmscGrp *mmc, 
			     MmsEnvelope *e,
			     Octstr *from, List *lto,
			     MmsMsg *m, Octstr **error,
			     int *retry)
{
     List *hdrs = e ? e->hdrs : NULL;
     Octstr *ret = NULL;
     int mtype = mms_messagetype(m);
     int hstatus = HTTP_OK;
     List *rh = NULL, *ph = NULL;
     Octstr *body = NULL, *rbody = NULL; 
     Octstr *mms = NULL;
     MIMEEntity *form_data = make_multipart_formdata();
     Octstr *transid = e ? octstr_create(e->xqfname) : NULL;
     int mm7type = mm7_msgtype_to_soaptype(mtype,1);
     Octstr *to;
     int retry_sending = -1;

     LINEARISE_STR_LIST(to,lto," ");

     if (e == NULL || mmc == NULL)
	  goto done;
     mms_info(0, "MM7", mmc->id,  "MMSBox: Send [http] to MMC[%s], msg type [%s], from %s, to %s", 
	      mmc && mmc->id ? octstr_get_cstr(mmc->id) : "", 
	      mms_message_type_to_cstr(mtype), 
	      octstr_get_cstr(from), octstr_get_cstr(to));

     mms = mms_tobinary(m);
     
     add_multipart_form_field(form_data, "to", "text/plain", NULL, to);
     add_multipart_form_field(form_data, "from", "text/plain", NULL, from);
     add_multipart_form_field(form_data, "mms", "application/vnd.wap.mms-message", NULL, mms);

     mmsbox_event_cb(mmc->id, mm7type, 0, octstr_imm("1.0"), 0, 
		     octstr_len(mms), e->attempts, from, 
		     to, NULL, transid, hdrs, NULL);
		     
     rh = mime_entity_headers(form_data);
     body = mime_entity_body(form_data);

     hstatus = mmsbox_url_fetch_content(HTTP_METHOD_POST, mmc->mmsc_url, rh, body, &ph, &rbody);

     if (http_status_class(hstatus) != HTTP_STATUS_SUCCESSFUL) {
	  *error = octstr_format("Failed to contact MMC[url=%S] => HTTP returned status = %d !",
				 mmc->mmsc_url, hstatus);
	  if (hstatus < 0)
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED, 3);
	
	  /* Now check if we should retry */
	  retry_sending = check_custom_retry_statuses(mmc, hstatus);
	
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT, 3);
     } else {
	  ret = rbody ? octstr_duplicate(rbody) : NULL;
	  if (ret)
	       octstr_strip_blanks(ret);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT);
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_SOCKET_CONNECT_FAILED);
     }
     
     mmsbox_event_cb(mmc->id, mm7type + 1, /* Always represents response type */
		     0, octstr_imm("1.0"), hstatus, 
		     0, e->attempts, e->from, 
		     to, ret, transid, hdrs, NULL);     

     //DEFAULT when retry_sending < 0
     //otherwise we rely on retry_status list
     if (retry_sending < 0) *retry = (ret == NULL && (http_status_class(hstatus) == HTTP_STATUS_SERVER_ERROR || hstatus < 0));
     else *retry = retry_sending;

     if (ret)
	  mms_log2("Sent", from, to, -1, ret, NULL, mmc->id, "MMSBox", NULL, NULL);

done:
     http_destroy_headers(rh);
     octstr_destroy(body);
     http_destroy_headers(ph);
     octstr_destroy(rbody);
     octstr_destroy(mms);
     mime_entity_destroy(form_data);
     octstr_destroy(transid);
     octstr_destroy(to);

     return ret;
}

static Octstr *mm7mm1_send(MmscGrp *mmc, Octstr *from, List *lto, 
			   Octstr *transid, 
			   Octstr *linkedid, char *vasid, Octstr *service_code,
			   MmsMsg *m, List *hdrs, Octstr **err, int *retry);
static int mms_sendtommsc(MmscGrp *mmc, MmsEnvelope *e, 
			  List *lto, /* Of Octstr * */
			  Octstr *orig_transid,
			  MmsMsg *m, 
			  Octstr **new_msgid,
			  List **errhdrs) 
{
     Octstr *id = NULL, *groupid = NULL;
     int ret = 0, retry  = 0;
     double throughput = 0;
     Octstr *from = e->from;
     Octstr *transid = e->msgId;
     Octstr *linkedid = e->token; /* token = linkedid */
     char *vasid = e->vasid ? octstr_get_cstr(e->vasid) : NULL;
     Octstr *service_code = e->vaspid;
     Octstr *dlr_url = e->url1;
     Octstr *rr_url = e->url2;
     List *hdrs = e->hdrs;
     
     mutex_lock(mmc->mutex); { /* Grab a lock on it. */
	  Octstr *err = NULL;
	  if (mmc->type == SOAP_MMSC)
	       id = mm7soap_send(mmc, from, lto, transid, linkedid, vasid, service_code, e, m, &err, errhdrs, &retry);
	  else if (mmc->type == EAIF_MMSC)
	       id = mm7eaif_send(mmc, from, lto, transid, vasid, e, m, &err, &retry);
	  else if (mmc->type == HTTP_MMSC)
	       id = mm7http_send(mmc,e, from, lto, m, &err, &retry);
	  else if (mmc->type == MM1_MMSC)
	       id = mm7mm1_send(mmc, from, lto, transid, linkedid, vasid, 
				service_code, m, hdrs, &err, &retry);
	  else if (mmc->type == CUSTOM_MMSC && mmc->started) {
	       Octstr *to = gwlist_get(lto, 0); /* XXX Send one at a time*/
	       id = mmc->fns->send_msg(mmc->data, 
				       from, to, transid, linkedid, vasid, 
				       service_code, m, hdrs, &err, &retry);
	  }
#if 0
	  else if (mmc->type == MM4_MMSC && mmc->started)
	       (void )0; /* Already sent above */
#endif
	  else	       
	       mms_error_ex("MT", 0,  "MM7", mmc->id, "Can't send to MMC[%s]!", 
			    mmc->id ? octstr_get_cstr(mmc->id) : ""); 	       
	
	  throughput = mmc->throughput;
	  groupid = mmc->group_id ? octstr_duplicate(mmc->group_id) : NULL;
	  
	  if (err && errhdrs) {
	       if (*errhdrs == NULL) 
		    *errhdrs = http_create_empty_headers();
	       http_header_add(*errhdrs, "X-Mbuni-Error", octstr_get_cstr(err));
	  }
	  octstr_destroy(err);
     }  mutex_unlock(mmc->mutex);  /* release lock */

     if (id) {
	  if (dlr_url)  /* remember the url's for reporting purposes. */
	       mms_dlr_url_put(id, "delivery-report", groupid, dlr_url, orig_transid);
	  if (rr_url)
	       mms_dlr_url_put(id, "read-report", groupid, rr_url, orig_transid);	  
	  ret = MMS_SEND_OK;
     } else {
	  ret = retry ? MMS_SEND_ERROR_TRANSIENT : MMS_SEND_ERROR_FATAL; 
	  if (!retry && dlr_url)
	       mms_dlr_url_put(e->msgId, "delivery-report", groupid, dlr_url, orig_transid);	  
     }
     *new_msgid = id;
          
     octstr_destroy(groupid);
     if (throughput > 0) 
	  gwthread_sleep(1.0/throughput);          
     return ret;
}

typedef struct MRcpt_t {
     int smtp_flag;
     MmscGrp *mmc;
     List *rto; /* List of envelope */
} MRcpt_t;

static int cmp_mrcpt(struct MRcpt_t *m, MmscGrp *mmc)
{

     if (mmc == NULL && m->smtp_flag)
	  return 1;
     else 
	  return (mmc && mmc->id && m->mmc && m->mmc->id) && (octstr_case_compare(m->mmc->id, mmc->id) == 0);
}

static void process_send_res(MmsEnvelope *e, MmsMsg *msg, 
			     MmsEnvelopeTo *to, MmscGrp *mmc, 
			     int res, Octstr *err, List *errl, Octstr *new_msgid, 
			     int first_one)
{
     Octstr *rcpt = to->_x ? to->_x : to->rcpt; /* Might have a cleaned up recipient address */
     time_t tnow = time(NULL);
     Octstr *xfrom = octstr_duplicate(e->from); /* Because it might change below */

     if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED) {
	  to->process = 0;
	  
	  if (e->msgtype == MMS_MSGTYPE_SEND_REQ ||
	      e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF) /* queue dlr as needed. */
	       queue_dlr(mmc, e->from, rcpt, new_msgid, octstr_imm("Forwarded"), "MM7-Out", errl);
     } else if (res == MMS_SEND_ERROR_FATAL && mmc) {
	  if (e->msgtype == MMS_MSGTYPE_SEND_REQ ||
	      e->msgtype == MMS_MSGTYPE_RETRIEVE_CONF) /* queue dlr as needed. */		    
	       queue_dlr(mmc, e->from, rcpt, e->msgId, 			
			 (e->expiryt != 0 && e->expiryt < tnow) ? 
			 octstr_imm("Expired") : octstr_imm("Rejected"), 
			 "MM7-Out", errl);
     }
     
     if (mmc) {
	  if (first_one) {
	       if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED)
		    mmc->mt_pdus++;
	       else
		    mmc->mt_errors++;	  
	  }
	  mmc->last_pdu = time(NULL);
	  return_mmsc_conn(mmc); /* important. */
     }
     
     if (res == MMS_SEND_ERROR_FATAL)
	  to->process = 0; /* No more attempts. */        
     
     
     /* handle CDR */
     if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED || res == MMS_SEND_ERROR_FATAL) {
	  Octstr *mclass = mms_get_header_value(msg, octstr_imm("X-Mms-Message-Class"));
	  Octstr *prio = mms_get_header_value(msg, octstr_imm("X-Mms-Priority"));
	  Octstr *mstatus  = mms_get_header_value(msg, octstr_imm("X-Mms-Status"));
	  
	  /* Do CDR */
	  cdrfs->logcdr(e->created, 
			octstr_get_cstr(xfrom),
			octstr_get_cstr(rcpt),
			octstr_get_cstr(e->msgId),
			mmc ? octstr_get_cstr(mmc->id) : NULL, /* Should we touch mmc here? XXX */ 
			e->src_interface, 
			"MM7",
			e->msize, 
			(char *)mms_message_type_to_cstr(e->msgtype),
			
			prio ? octstr_get_cstr(prio) : NULL,
			mclass ? octstr_get_cstr(mclass) : NULL,
			res == MMS_SEND_ERROR_FATAL ? "dropped" : (mstatus ? octstr_get_cstr(mstatus) : "sent"),
			e->dlr,
			0);
	  
	  octstr_destroy(mclass);
	  octstr_destroy(prio);
	  octstr_destroy(mstatus);	       
     }
     
     if (err == NULL)
	  mms_info(0, "MM7", NULL, "%s MMSBox Outgoing Queue MMS Send: From %s, to %s, msgsize=%ld: msgid=[%s]", 
		   SEND_ERROR_STR(res),
		   octstr_get_cstr(xfrom), octstr_get_cstr(rcpt), e->msize,
		   new_msgid ? octstr_get_cstr(new_msgid) : "N/A");
     else 
	  mms_error_ex("MT", 0, 
		       "MM7", NULL, 
		       "%s MMSBox Outgoing Queue MMS Send: From %s, to %s, msgsize=%ld: %s", 
		       SEND_ERROR_STR(res),
		       octstr_get_cstr(xfrom), octstr_get_cstr(rcpt), e->msize, octstr_get_cstr(err));     
     octstr_destroy(xfrom);
}

/* Make a list of recpients up to max_rcpt */
static inline List *make_srcpt_list(List *rto, int max_rcpt) 
{
     List  *l = gwlist_create();
     MmsEnvelopeTo *xto;
     while ((max_rcpt > 0) && (xto = gwlist_extract_first(rto)) != NULL) {
	  gwlist_append(l, xto);
	  max_rcpt--;
     }

     if (gwlist_len(l) == 0)  {
	  gwlist_destroy(l, NULL);
	  l = NULL;
     }
     return l;
}

static int sendMsg(MmsEnvelope *e)
{
     MmsMsg *msg = NULL;
     int i, n;     
     Octstr *otransid = e->hdrs ? http_header_value(e->hdrs, octstr_imm("X-Mbuni-TransactionID")) : NULL;
     MmscGrp _mmc_smtp = {.max_recipients = DEFAULT_SIMUL_RCPTS, .mutex = mutex_create()};
     MRcpt_t smtp_h = {.mmc = &_mmc_smtp, .smtp_flag = 1, .rto = gwlist_create()}; /* Signals that recipient is on other end of smtp pipe */
     List *mlist = NULL; /* List of MMScs */
     MmsEnvelopeTo *to;
     time_t tnow = time(NULL);
     
     if ((msg = qfs->mms_queue_getdata(e)) == NULL)  {
	  mms_error(0,  "MM7", NULL, "MMSBox queue error: Failed to load message for queue id [%s]!", e->xqfname);
	  MMSC_ISSUE_ALARM(NULL, MMSBOX_ALARM_RETRIEVE_MMS_ERROR, 4);
	  goto done2;
     } else 
	  MMSC_CLEAR_ALARM(NULL, MMSBOX_ALARM_RETRIEVE_MMS_ERROR);

     mlist = gwlist_create_ex(&smtp_h);

     /* First split by mmc */
     for (i = 0, n = gwlist_len(e->to); i<n; i++) 
	  if ((to = gwlist_get(e->to, i)) != NULL && to->process && to->rcpt) {
	       Octstr *x, *err = NULL;
	       int is_email = (octstr_search_char(to->rcpt, '@', 0) > 0);
	       Octstr *requested_mmsc = NULL;
	       MmscGrp *mmc = NULL;
	       int res = MMS_SEND_OK;
	       MRcpt_t *m;
	       
	       to->_x = NULL; /* Clear it. */
	       if (e->expiryt != 0 &&  /* Handle message expiry. */
		   e->expiryt < tnow) {
		    err = octstr_format("MMSC error: Message expired while sending to %S!", to->rcpt);
		    res = MMS_SEND_ERROR_FATAL;
		    
		    goto done_route;
	       } else if (e->attempts >= maxsendattempts) {
		    err = octstr_format("MMSBox error: Failed to deliver to "
					"%S after %ld attempts. (max attempts allowed is %ld)!", 
					to->rcpt, e->attempts, 
					maxsendattempts);
		    res = MMS_SEND_ERROR_FATAL;
		    goto done_route;
	       }

	       x = octstr_format("X-Mbuni-Via-%d", i);
	       requested_mmsc = e->hdrs ? http_header_value(e->hdrs, x) : NULL;
	       octstr_destroy(x);


	       if ((mmc = get_handler_mmc(requested_mmsc ? requested_mmsc : e->viaproxy, to->rcpt, e->from)) == NULL && 
		   !is_email) {
		    err = octstr_format("MMSBox error: Failed to deliver to "
					"%S. Don't know how to route!", 
					to->rcpt);
		    res = MMS_SEND_ERROR_TRANSIENT;
		    goto done_route;
	       }
	       

	       /* We know how to route: If mmc is null at this point, means mm4 recipient. */
	       if ((m = gwlist_search(mlist, mmc, (void *)cmp_mrcpt)) == NULL) { /* A new route, add recipient zone */
		    m = gw_malloc(sizeof *m);
		    m->smtp_flag = 0;
		    m->mmc = mmc;
		    m->rto = gwlist_create();

		    gwlist_append(mlist, m);
	       }
	       
	       gwlist_append(m->rto, to); /* Record route. */

	  done_route:
	       if (res != MMS_SEND_OK)
		    process_send_res(e, msg, to, mmc, res, err, NULL, NULL, 0);
	       octstr_destroy(err);
	       octstr_destroy(requested_mmsc);
	  }
     
     for (i = 0, n = gwlist_len(mlist); i<n && (e != NULL); i++) { /* Pass through MMSCs, delivering to one at a time. */
	  MRcpt_t *m = gwlist_get(mlist, i);
	  int maxrcpt = m->mmc->max_recipients;
	  List *lto;

	  /* Hive off maxrcpt each time and send. */
	  while (e != NULL && (lto = make_srcpt_list(m->rto, maxrcpt)) != NULL) {
	       MmscGrp *mmc = m->mmc;
	       Octstr *err = NULL;
	       Octstr *new_msgid = NULL;
	       List *errl = NULL;
	       int j, res = MMS_SEND_OK;
	       int is_email = m->smtp_flag;
	       int is_mm4 = (mmc && mmc->type == MM4_MMSC && mmc->started); 
	       List *xto = gwlist_create();
	       Octstr *zto = NULL;
	       Octstr *oldfrom = octstr_duplicate(e->from); /* Save old from address */
	       
	       if (mmc && mmc->strip_prefixes) /* strip prefixes from sender address */
		    _mms_fixup_address(&e->from, NULL, mmc->strip_prefixes, 1);
	       
	       /* Make recipient list */
	       for (j = 0; j < gwlist_len(lto); j++) {
		    MmsEnvelopeTo *to = gwlist_get(lto, j);
		    Octstr *x = octstr_duplicate(to->rcpt);
		    int is_email = (octstr_search_char(to->rcpt, '@', 0) > 0);		    
		    if (is_mm4 && !is_email)  /* Add host name of recipient domain */
			 octstr_format_append(x, "@%S",
					      mmc && octstr_str_compare(mmc->mmsc_url, "*") != 0 ? mmc->mmsc_url : 
					      octstr_imm("unknown"));

		    if (mmc && mmc->strip_prefixes) /* strip prefixes */
			 _mms_fixup_address(&x, NULL, mmc->strip_prefixes, 1);
		    if (x)
			 gwlist_append(xto, x);
	       }
	       
	       LINEARISE_STR_LIST(zto,xto,", ");	       
	       if (is_mm4 || is_email) { /* Handle mm4 as well */
		    int j = octstr_case_search(e->from, octstr_imm("/TYPE=PLMN"), 0);
		    int k = octstr_case_search(e->from, octstr_imm("/TYPE=IPv"), 0);
		    int len = octstr_len(e->from);
		    Octstr *pfrom;
		    
		    
		    if (j > 0 && j - 1 +  sizeof "/TYPE=PLMN" == len) 
			 pfrom = octstr_copy(e->from, 0, j);
		    else if (k > 0 && k + sizeof "/TYPE=IPv" == len) 
			 pfrom = octstr_copy(e->from, 0, k);
		    else
			 pfrom = octstr_duplicate(e->from);
		    
		    if (octstr_search_char(e->from, '@', 0) < 0)
			 octstr_format_append(pfrom,"@%S", myhostname);
	       
		    res = mms_sendtoemail(pfrom, xto, 
					  e->subject ? e->subject : octstr_imm(""),
					  e->msgId, msg, 0, &err, octstr_get_cstr(sendmail_cmd),
					  myhostname, 0, 0, 
					  "", 
					  "", 0, 
					  e->xqfname, 
					  e->hdrs, smtp_relay.host, smtp_relay.port);	       
		    if (res == MMS_SEND_OK || res == MMS_SEND_QUEUED) {
			 new_msgid = e->msgId ? octstr_duplicate(e->msgId) : octstr_create("00001"); /* Fake it */
			 mmsbox_event_cb(NULL, MM7_TAG_SubmitReq, 1, octstr_imm("1.0"), 200, 
					 mms_msgsize(msg), e->attempts, pfrom, 
					 zto,NULL, NULL, e->hdrs, NULL);
		    }
		    octstr_destroy(pfrom);
	       } else {
		    res = mms_sendtommsc(mmc, e, 
					 xto, 
					 otransid,
					 msg, 
					 &new_msgid,
					 &errl);
		    if (errl)
			 err = http_header_value(errl, octstr_imm("X-Mbuni-Error"));
		    if (new_msgid && e->hdrs) { /* Record it */
			 Octstr *x = octstr_format("X-Mbuni-Received-Message-Id-%d", i);
			 
			 http_header_remove_all(e->hdrs, octstr_get_cstr(x));
			 http_header_add(e->hdrs, octstr_get_cstr(x), octstr_get_cstr(new_msgid));
			 
			 octstr_destroy(x);
		    }
	       }

	       /* For each recipient, process result */
	       for (j = 0; j < gwlist_len(lto); j++) {
		    MmsEnvelopeTo *to = gwlist_get(lto, j);
		    if (to)
			 to->_x = gwlist_get(xto, j);
		    process_send_res(e, msg, to, mmc, res, err, errl, new_msgid, j == 0);		    
	       }
	       octstr_destroy(zto);
	       gwlist_destroy(xto, (void *)octstr_destroy);
	       octstr_destroy(new_msgid);
	       octstr_destroy(err);
	       http_destroy_headers(errl);
	       gwlist_destroy(lto, NULL);
	       
	       e->lasttry = tnow;

	       octstr_destroy(e->from);
	       e->from = oldfrom; /* restore old from address */
	       if (qfs->mms_queue_update(e) == 1) { 
		    e = NULL;
		    break; /* Queue entry gone. */             
	       }
	  }

     }

done2:
     mms_destroy(msg);     
     octstr_destroy(otransid);

     /* Clear out mlist: */
     for (i = 0; i < gwlist_len(mlist); i++) {
	  MRcpt_t *m = gwlist_get(mlist, i);
	  
	  gwlist_destroy(m->rto, NULL); /* Clear list */
	  if (m != &smtp_h)
	       gw_free(m);
	  else 
	       mutex_destroy(smtp_h.mmc->mutex);
     }
     gwlist_destroy(mlist, NULL);
     
     if (e) { /* Update the queue if it is still valid (e.g. recipients not handled) 
	       * XXX can this happen here??... 
	       */
	  e->lasttry = time(NULL);
	  e->attempts++;  /* Update count of number of delivery attempts. */   
	  e->sendt = e->lasttry + mmsbox_send_back_off * e->attempts;
	  
	  if (qfs->mms_queue_update(e) != 1)
	       qfs->mms_queue_free_env(e);
     }

     
     return 1; /* always delete queue entry. */
}

void mmsbox_outgoing_queue_runner(volatile sig_atomic_t *rstop)
{
     hmon->register_thread( "outgoing_queue" );
     qfs->mms_queue_run(octstr_get_cstr(outgoing_qdir), 
			sendMsg, queue_interval, maxthreads, rstop);
     hmon->unregister_thread( "outgoing_queue" );
}


/* MM4 handler stuff */
static void fixup_addresses(List *headers)
{
     fixup_address_type(headers, "To",
			octstr_get_cstr(unified_prefix), strip_prefixes);
     fixup_address_type(headers, "From",
			octstr_get_cstr(unified_prefix), strip_prefixes);
}


static int send_mm4_res(int mtype, Octstr *to, Octstr *sender, Octstr *transid, 
			char *status, Octstr *msgid, 
			char *sendmail_cmd)
{
     char tmp[32];
     List *h = http_create_empty_headers();
     MIMEEntity *m = mime_entity_create();
     Octstr *err  = NULL;

     /* Make headers */
     sprintf(tmp, "%d.%d.%d", 
	     MAJOR_VERSION(MMS_3GPP_VERSION),
	     MINOR1_VERSION(MMS_3GPP_VERSION),
	     MINOR2_VERSION(MMS_3GPP_VERSION));
     
     http_header_add(h, "X-Mms-3GPP-MMS-Version", tmp);
     http_header_add(h, "X-Mms-Transaction-ID", octstr_get_cstr(transid));
     http_header_add(h, "X-Mms-Message-Type", mm4_types[mtype].mm4str);
     if (msgid) 
	  http_header_add(h, "X-Mms-Message-ID", octstr_get_cstr(msgid));	  
     http_header_add(h, "X-Mms-Request-Status-Code", status);
     http_header_add(h, "Sender", octstr_get_cstr(sender));     
     http_header_add(h, "To", octstr_get_cstr(to));     
     
     mime_replace_headers(m, h);
     http_destroy_headers(h);
     
     mm_send_to_email(to, sender, octstr_imm(""), msgid, m, 0, 
		      &err, sendmail_cmd,
		      myhostname, smtp_relay.host, smtp_relay.port);
     if (err) {
	  mms_warning(0, "MM4", NULL, "send.RES reported: %s!", octstr_get_cstr(err));
	  octstr_destroy(err);
     }
     mime_entity_destroy(m);

     return 0;
}

static Octstr *handle_msg(MIMEEntity *mm, Octstr *from, List *to, MmscGrp *mmc)
{
     char *err = NULL;
     List *headers = NULL, *h2;
     Octstr  *mm4_type;
     Octstr *transid = NULL, *orig_sys = NULL, *ack = NULL, *res = NULL, *me = NULL, *rstatus = NULL;
     int i, mtype = -1, mm1_type = -1, mm7type;
     MmsMsg *msg = NULL;
     Octstr *qdir = NULL, *mmc_id = NULL, *value = NULL, *msgid = NULL;

     Octstr *xproxy = mmc != NULL && octstr_str_compare(mmc->mmsc_url, "*") != 0 ? mmc->mmsc_url : NULL;
     
     /* Taken largely from mmsfromeamil.c */
     /* Take the entity, recode it --> remove base64 stuff, split headers. */
     unbase64_mimeparts(mm);
     unpack_mimeheaders(mm);
     
     /* Delete some headers... */
     headers = mime_entity_headers(mm);
     http_header_remove_all(headers, "Received");
     http_header_remove_all(headers, "X-MimeOLE");
     http_header_remove_all(headers, "X-Mailer");
     
     /* rebuild headers, removing nasty looking ones. */
     h2 = http_create_empty_headers();
     for (i = 0; i<gwlist_len(headers); i++) {
	  Octstr *name = NULL, *value = NULL;
	  http_header_get(headers, i, &name, &value);
	  
	  if (!name ||
	      octstr_case_search(name, octstr_imm("spam"), 0) >= 0 ||
	      octstr_case_search(name, octstr_imm("mailscanner"), 0) >= 0)
	       goto loop;
	  
	  http_header_add(h2, octstr_get_cstr(name), octstr_get_cstr(value));
     loop:
	  octstr_destroy(name);
	  octstr_destroy(value);	  
     }
     http_destroy_headers(headers);
     headers = h2;

     /* Look for MM4 headers... */
     mm4_type = http_header_value(headers, octstr_imm("X-Mms-Message-Type"));
     ack = http_header_value(headers, octstr_imm("X-Mms-Ack-Request"));
     rstatus = http_header_value(headers, octstr_imm("X-Mms-Request-Status-Code"));
     
     if ((transid = http_header_value(headers, octstr_imm("X-Mms-Transaction-ID"))) == NULL)
	  transid = octstr_create("001");
     /* get originator system. */
     if ((orig_sys = http_header_value(headers, octstr_imm("X-Mms-Originator-System"))) == NULL) 
	  orig_sys = http_header_value(headers, octstr_imm("Sender"));
          
     if ((msgid =  http_header_value(headers, octstr_imm("X-Mms-Message-ID"))) == NULL)
	  msgid = http_header_value(headers, octstr_imm("Message-ID"));
     
     strip_quoted_string(msgid);     
     strip_quoted_string(transid);
     
     
     debug("mmsbox.MM4receive", 0,
	   "Received [message type: %s] [transaction id: %s] [msgid: %s]",
	   mm4_type ? octstr_get_cstr(mm4_type) : "",
	   transid ? octstr_get_cstr(transid) : "",
	   msgid ? octstr_get_cstr(msgid) : "");

     /* ... and remove non-essential ones */
     http_header_remove_all(headers, "X-Mms-3GPP-MMS-Version");
     http_header_remove_all(headers, "MIME-Version");
     http_header_remove_all(headers, "X-Mms-Message-ID");
     http_header_remove_all(headers, "Message-ID");
     http_header_remove_all(headers, "X-Mms-Ack-Request");
     http_header_remove_all(headers, "X-Mms-Originator-System");

     http_header_remove_all(headers, "Sender");
     
     /* msgid was there, put it back in proper form. */
     if (msgid)
	  http_header_add(headers, "Message-ID", octstr_get_cstr(msgid));
     
     fixup_addresses(headers);
     
     
     if (mm4_type) {
	  unsigned char *x = NULL;
	  Octstr *y;
	  int i;
	  
	  http_header_remove_all(headers, "X-Mms-Message-Type");
	  for (i = 0; mm4_types[i].mm4str; i++)
	       if (octstr_str_case_compare(mm4_type, mm4_types[i].mm4str) == 0) {
		    mtype = i;
		    mm1_type = mm4_types[i].mm1_type;
		    x = mms_message_type_to_cstr(mm1_type);
		    break;
	       }
	  
	  if (x) {
	       http_header_add(headers, "X-Mms-Message-Type", (char *)x);  
	       if (orig_sys == NULL) /* Make it up! */
		    orig_sys = octstr_format("system-user@%S", 
					     xproxy ? xproxy : octstr_imm("unknown"));	       
	  } else {
	       octstr_destroy(mm4_type);     
	       mm4_type = NULL; /* So that we assume normal message below. */
	  }
	  
	  if ((y = http_header_value(headers, octstr_imm("X-Mms-MM-Status-Code"))) != NULL) {
	       /* This field is different on MM1. */
	       http_header_remove_all(headers, "X-Mms-MM-Status-Code");
	       http_header_add(headers, "X-Mms-Status", octstr_get_cstr(y));
	       octstr_destroy(y);
	  }
     } 
     if (mm4_type == NULL && mtype < 0) { /* else assume a normal send message. */
	  http_header_add(headers, "X-Mms-Message-Type", "m-send-req");  
	  mm1_type = MMS_MSGTYPE_SEND_REQ;
	  mtype = MM4_FORWARD_REQ;
     }

     mime_replace_headers(mm, headers);

     /* Now convert from mime to MMS message, if we should */
     if (mm1_type >= 0) {
	  if ((msg = mms_frommime(mm)) == NULL) {
	       mms_error(0, "MM4", mmc ? mmc->id : NULL, "Unable to parse Message!");
	       MMSC_ISSUE_ALARM(mmc ? mmc : NULL, MMSBOX_ALARM_MM4_PARSING_FAILURE, 3);
	       res =  NULL;

	       goto done;
	  } else
	       MMSC_CLEAR_ALARM(mmc ? mmc : NULL, MMSBOX_ALARM_MM4_PARSING_FAILURE);
     } else 
	  msg = NULL;     

     mm7type = mm7_msgtype_to_soaptype(mm1_type,0);
     me = octstr_format("system-user@%S", myhostname);

     qdir = get_mmsbox_queue_dir(from, to, mmc, &mmc_id); /* get routing info. */
     switch(mtype) {	  
     case MM4_FORWARD_REQ:
     {
	  Octstr *qf;
	  Octstr *dreport = mms_get_header_value(msg, octstr_imm("X-Mms-Delivery-Report"));	  
	  Octstr *subject = mms_get_header_value(msg, octstr_imm("Subject"));	  
	  int dlr;
	  
	  if (dreport && 
	      octstr_case_compare(dreport, octstr_imm("Yes")) == 0) 
	       dlr = 1;
	  else 
	       dlr = 0;

	  qf = qfs->mms_queue_add(from, to, subject, mmc ? mmc->id : NULL, mmc_id,
				  0, time(NULL) + default_msgexpiry, msg, NULL, 
				  NULL, NULL,
				  NULL, NULL,
				  NULL,
				  dlr,
				  octstr_get_cstr(qdir), 
				  "MM7/MM4-IN",
				  NULL);
	  if (qf) {
	       res = mms_make_msgid(octstr_get_cstr(qf), NULL);
	       mms_log("Received", from, to, -1, res, NULL, mmc->id, "MMSBox", octstr_imm("smtp"), NULL);	      
	       err = "Ok";		    
	       MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR);
	  } else {
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	       err = "Error-network-problem";
	  }	  
	  octstr_destroy(dreport);
	  octstr_destroy(subject);	       
	  octstr_destroy(qf);
     }
     break;
     case MM4_DELIVERY_REPORT_REQ: 
     {
	  Octstr *qf;
	  Octstr *value2 = NULL, *rr_uri = NULL;
	  List *rqh = http_create_empty_headers(); 
	  
	  value = mms_get_header_value(msg, octstr_imm("X-Mms-Status"));
	  value2 = mms_get_header_value(msg, octstr_imm("X-Mbuni-Orig-Message-ID")); 


	  rr_uri = mmsbox_get_report_info(msg, mmc, mmc_id, "delivery-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  qf = qfs->mms_queue_add(from, to, NULL,
				  mmc ? mmc->id : NULL, mmc_id,
				  0, time(NULL) + default_msgexpiry, msg, NULL,
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 
				  "MM7/MM4-IN",
				  NULL);
	  
	  if (qf) {
	       mms_log("DeliveryReport", from, to, -1, msgid, value, mmc->id, "MMSBox", octstr_imm("smtp"), NULL);
	       err = "Ok";
	       res = octstr_duplicate(qf);
	       MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 	       
	  } else {
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);			 	       
	       err = "Error-network-problem";
	  }
	       
	  octstr_destroy(qf);
	  octstr_destroy(value2);
	  octstr_destroy(rr_uri);
	  http_destroy_headers(rqh);
     }          
     break;

     case MM4_READ_REPLY_REPORT_REQ:
     {
	  Octstr *qf;
	  Octstr *value2 = NULL, *rr_uri = NULL;
	  List *rqh = http_create_empty_headers(); 
	  
	  value = mms_get_header_value(msg, octstr_imm("X-Mms-Read-Status"));
	  value2 = mms_get_header_value(msg, octstr_imm("X-Mbuni-Orig-Message-ID")); 

	  rr_uri = mmsbox_get_report_info(msg, mmc, mmc_id, "read-report", 
					  value, rqh, NULL, 0, msgid);
	  if (value2 && mmc_id == NULL)
	       http_header_add(rqh, "X-Mbuni-Orig-Message-ID", octstr_get_cstr(value2)); 
	  
	  
	  qf = qfs->mms_queue_add(from, to, NULL,
				  mmc->id,mmc_id,
				  0, time(NULL) + default_msgexpiry, msg, NULL,
				  NULL, NULL,
				  rr_uri, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir), 
				  "MM7/MM4-IN",
				  NULL);
	  if (qf) {
	       mms_log("Received RR", from, to, -1, msgid, value, mmc->id, "MMSBox", octstr_imm("smtp"), NULL);    
	       res = octstr_duplicate(qf);
	       err = "Ok";
	       MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  } else {
	       err = "Error-network-problem";
	       MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
	  }
	  octstr_destroy(qf);
	  octstr_destroy(value2);
	  octstr_destroy(rr_uri);
	  http_destroy_headers(rqh);

     }

     break;
     case MM4_FORWARD_RES:
     case MM4_READ_REPLY_REPORT_RES:
     case MM4_DELIVERY_REPORT_RES: /* remove corresponding queue entry. */     
	  /* Do nothing */	  
	  break;
     default: 
	  mms_warning(0, "MM4", xproxy, "Unexpected message type: %s", 
		      mm4_type  ? octstr_get_cstr(mm4_type) : "not given!");
	  break;
     }
     

     if (mm7type >= 0)  /* Issue event call back */
	  mmsbox_event_cb(mmc->id, mm7type, 1, octstr_imm("1.0"), 200, 
			  mms_msgsize(msg), 0, from, 
			  to && gwlist_len(to) > 0 ? gwlist_get(to,0) : NULL,
			  msgid, transid, headers, value);
     
     /* respond to the sender as necessary. */
     if (mm4_type && 
	 err && 
	 ack && octstr_str_case_compare(ack, "Yes") == 0) {
	  int size = send_mm4_res(mtype+1, orig_sys, me, transid, err, res, octstr_get_cstr(sendmail_cmd));     
	 
	  mmsbox_event_cb(mmc->id, mm7type >= 0 ? mm7type + 1 : MM7_TAG_VASPErrorRsp, 
			  1, octstr_imm("1.0"), 200, 
			  size, 0, me, orig_sys, res, NULL, NULL, NULL);	 	 
     } else if (mtype == MM4_FORWARD_REQ) /* Or straight up SMTP */
	  mmsbox_event_cb(mmc->id, mm7type >= 0 ? mm7type + 1 : MM7_TAG_VASPErrorRsp, 
			  1, octstr_imm("1.0"), 200, 
			  0, 0, me, orig_sys, res, NULL, NULL, NULL);	 	 
     
     octstr_destroy(mm4_type);     
     octstr_destroy(transid);     
     octstr_destroy(orig_sys);
     octstr_destroy(msgid);

     octstr_destroy(rstatus);
     octstr_destroy(ack);

     octstr_destroy(me);

     octstr_destroy(mmc_id);
     octstr_destroy(value);
     mms_destroy(msg);
     http_destroy_headers(headers);
done:

     return res;
}

static void clean_address(Octstr **addr, int *isphone, Octstr **xproxy)
{
     int i;

     octstr_strip_blanks(*addr);
     *isphone = 1;
     if ((i = octstr_search_char(*addr, '<', 0)) >= 0) {
	  octstr_delete(*addr, 0, i+1);
	  
	  i = octstr_search_char(*addr, '>', 0);
	  if (i > 0)
	       octstr_delete(*addr, i, octstr_len(*addr));
			      
     }
     /* Find number type */
     i = octstr_case_search(*addr, octstr_imm("/TYPE="), 0);
     if (i>0) {
	  int j = octstr_search_char(*addr, '@', 0);
	  if (j > i) { /* we have @, remove it */
	       *xproxy = octstr_copy(*addr, j+1, octstr_len(*addr));
	       octstr_delete(*addr, j, octstr_len(*addr));
	  }           
     } else if (isphonenum(*addr))  /* Add the TYPE element if missing. */			       
	  octstr_append(*addr, octstr_imm("/TYPE=PLMN"));     
     else {
	  i = octstr_search_char(*addr, '@', 0);     
	  if (i<0) 
	       octstr_format_append(*addr, "@unknown");
	  else if (*xproxy == NULL)
	       *xproxy = octstr_copy(*addr, i+1, octstr_len(*addr));
	  *isphone = 0;
     }
     /* clean the number. */
     if (*isphone)
	  _mms_fixup_address(addr, 
			     octstr_get_cstr(unified_prefix), 
			     strip_prefixes, 1);                 
     
}

static void smtp_process(int fd, Octstr *ip, 
			 Octstr *(*handle_msg)(MIMEEntity *m, Octstr *from, List *to, MmscGrp *mmc))
{
     enum smtp_state_t {MLISTEN, MFROM, MTO, MDATA,MERROR};
     int i, state = MLISTEN;
     Connection *c = conn_wrap_fd(fd,0);
     Octstr *from = NULL;
     MmscGrp *mmc = NULL;
     List *to = NULL;
     Octstr *body = NULL;
     MIMEEntity *m  = NULL;
     Octstr *line;
     
     socket_set_blocking(fd,1); /* Because we want each line as it comes */
     
     /* Issue greeting */
     line = octstr_format("220 %S SMTP Mbuni %s\r\n", myhostname, MMSC_VERSION);
     conn_write(c, line);
     octstr_destroy(line);
     
     do {
	  Octstr *res;
	  Octstr *cmd = NULL, *arg = NULL;
	  int stop, max_size;

	  line = conn_read_line(c);	  	  
	  if (line == NULL)
	       state = MERROR;
	  else if (state != MDATA) {
	       i = octstr_search_char(line, ' ', 0);   
	       if (i > 0) {
		    cmd = octstr_copy(line, 0, i);
		    arg = octstr_copy(line, i+1, octstr_len(line));		    
	       } else {
		    cmd = octstr_copy(line, 0, octstr_len(line));
		    arg = octstr_create("");
	       }
	       octstr_strip_blanks(cmd);
	       octstr_strip_blanks(arg);
	  }
	  switch(state) {

	  case MLISTEN:
	       if (octstr_str_case_compare(cmd, "HELO") == 0)
		    conn_write(c, octstr_imm("250 Hello and welcome\r\n")); /* ... and stay in same state */
	       else if (octstr_str_case_compare(cmd, "MAIL") == 0) { /* Sender */
		    if ((i = octstr_case_search(arg, octstr_imm("FROM:"), 0)) < 0)  /* No From? */ 
			 conn_write(c, octstr_imm("500 Error. Missing FROM:\r\n"));			 
		    else {
			 int isphone = 1;
			 Octstr *xproxy = NULL;			 
			 from = octstr_copy(arg, i+5, octstr_len(arg));
			 
			 clean_address(&from, &isphone, &xproxy);
			  
			 /* We now have the sender domain and the number. Find a matching MMSC */
			 if (!isphone || xproxy == NULL)
			      mmc = mmsbox_get_mmsc_by_url(octstr_imm("*"));
			 else 
			      mmc = mmsbox_get_mmsc_by_url(xproxy);

			 /* Check that we have an mmsc and the sender IP is allowed. */
			 if (mmc == NULL || xproxy == NULL || 
			     !is_allowed_ip(mmc->incoming.allow_ip, mmc->incoming.deny_ip, ip))  {
			      conn_write(c, octstr_imm("421 Sender not allowed\r\n"));
			      state = MERROR;			      
			 } else {
			  
			      if (!mmc->strip_domain && isphone) 
				   octstr_format_append(from, "@%S", xproxy);
			      conn_write(c, octstr_imm("250 Ok\r\n"));
			      state = MTO;
			 }
			 octstr_destroy(xproxy);
		    }
	       } else 
		    conn_write(c, octstr_imm("500 Error.\r\n"));			 
	       break;

	  case MTO:
	       if (octstr_str_case_compare(cmd, "DATA") == 0) {
		    if (gwlist_len(to) == 0)  {
			 conn_write(c, octstr_imm("421 No recipients? Go away!\r\n"));
			 state = MERROR;
		    } else {
			 state = MDATA;
			 conn_write(c, octstr_imm("354 Proceed\r\n")); 
			 body = octstr_create("");
		    }
	       } else if (octstr_str_case_compare(cmd, "RCPT") != 0)  /* recipient */
		    conn_write(c, octstr_imm("500 send recipient please\r\n"));  /* ... and stay in same state */
	       else if ((i = octstr_case_search(arg, octstr_imm("TO:"), 0)) < 0)  /* No To? */ 
		    conn_write(c, octstr_imm("500 send TO field\r\n"));  /* ... and stay in same state */
	       else { /* Got it. Clean it up */
		    Octstr *xto = octstr_copy(arg, i+3, octstr_len(arg));		    
		    int isphone = 1;
		    Octstr *xproxy = NULL;			 

		    clean_address(&xto, &isphone, &xproxy);

		    if (mmc && !mmc->strip_domain && isphone && xproxy) 
			 octstr_format_append(from, "@%S", xproxy);
		    
		    if (xproxy == NULL)
			 conn_write(c, octstr_imm("500 send correct recipient please\r\n"));  /* ... and stay in same state */
		    else { /* Stay in same state */
			 conn_write(c, octstr_imm("250 Ok\r\n")); /* Accept recipient */
			 if (to == NULL)
			      to = gwlist_create();
			 gwlist_append(to, xto);
			 xto = NULL;
		    }		        
		    octstr_destroy(xproxy);   
		    octstr_destroy(xto);
	       }
	       break;
	  case MDATA:
	       stop = 0;
	       max_size = (mmc ? mmc->max_pkt_size : DEFAULT_MAX_PKT_SIZE);
	       do {
		    if (octstr_get_char(line, 0) == '.') {
			 if (octstr_get_char(line, 1) != '.') {
			      stop = 1; 
			      goto end_loop;
			 } else 
			      octstr_delete(line, 0, 1); /* Remove period */
		    }
		    
		    if (octstr_len(body) < max_size)
			 octstr_format_append(body, "%S\r\n", line);
		    else if (octstr_len(body) == max_size)
			 octstr_append_char(body, ' '); /* So we exceed length */
	       end_loop:
		    octstr_destroy(line);
		    line = NULL; /* So no double free below */
	       } while (!stop && !conn_eof(c) && (line = conn_read_line(c)) != NULL);
	       
	       if (octstr_len(body) > max_size) {
		    conn_write(c, octstr_imm("452 Message too large\r\n"));
		    state  = MERROR;
		    goto loop;
	       }
	       
	       /* We got message, time to process it */
	       if ((m = mime_octstr_to_entity(body)) == NULL) {
		    conn_write(c, octstr_imm("501 Invalid MIME content\r\n"));
		    state = MERROR;
		    goto loop;
	       } else {
		    octstr_destroy(body); /* Release space */
		    body = NULL;
	       }

	       if ((res = handle_msg(m, from, to, mmc)) != NULL) {
		    Octstr *x = octstr_format("250 Ok: %S\r\n", res);
	       
		    conn_write(c, x);
		    octstr_destroy(x);
		    octstr_destroy(res);
	       } else 
		    conn_write(c, octstr_imm("451 Invalid message content\r\n"));
	       state = MERROR; /* Not really, but we gotta go */
	       break;	       	      	       
	  default:
	       /* Do nothing */
	       break;
	  }
	  
     loop:
	  octstr_destroy(cmd);
	  octstr_destroy(arg);
	  octstr_destroy(line);
     } while (state != MERROR);
     
     octstr_destroy(body);
     octstr_destroy(from);
     gwlist_destroy(to, (void *)octstr_destroy);
     if (m)
	  mime_entity_destroy(m);
     
     conn_destroy(c);
     if (mmc)
	  return_mmsc_conn(mmc);
     return ;
}

struct mm4_req_t {
     Octstr *ip;
     int fd;
};

static List *slist = NULL; /* Request list */

static void smtp_thread(void *unused)
{
     struct mm4_req_t *m;
     
     while ((m = gwlist_consume(slist)) != NULL) {
	  smtp_process(m->fd, m->ip, handle_msg); 
	  octstr_destroy(m->ip);
	  gw_free(m);
     }
}

void mm4_receive_func(int *sock)
{
     int i, fd, tcount = 0;
     Octstr *ip;
     /* Start the threads, then receive requests, build, go */
     slist = gwlist_create();

     gwlist_add_producer(slist);
     
     for (i = 0; i < maxthreads; i++)
	  if (gwthread_create((void *)smtp_thread, NULL) >= 0)
	       tcount++;
     if (tcount > 0)
	  while (!rstop && ((fd = gw_accept(*sock, &ip)) >= 0 ||
			    errno == EINTR))  {
	       struct mm4_req_t *m;
	       
	       if (fd < 0)
		    continue;
	       
	       m = gw_malloc(sizeof *m);
	       m->ip = ip;
	       m->fd = fd;
	       
	       gwlist_produce(slist, m);
	  }
     else 
	  mms_error(0, "MM4", NULL, "Failed to start SMTP listener threads: %s!", strerror(errno));
     gwlist_remove_producer(slist);
     
     gwthread_join_every((void *)smtp_thread);

     gwlist_destroy(slist, NULL);     
}



/* MM1 functions and data */

typedef struct {
     enum {MM1_GET, MM1_PUSH} type;
     int waiter_exists;     /* set to true if after handling, should signal and NOT destroy struct.*/
     pthread_cond_t   cond;
     pthread_mutex_t mutex;
     union {
	  MmsMsg *m;   /* for push. */
	  Octstr *url;  /* for get   */
     } u;
     void   *result;  /* set to the result for a PUSH */
     Octstr *err;
} MM1Request; 

static long start_gprs(Octstr *cmd, Octstr *id, Octstr *pid_cmd);
static Octstr *fetch_content_with_curl(MmscGrp *mmc, Octstr *url, Octstr *body, int *hstatus);
static void stop_gprs(Octstr *cmd);

static Octstr *mm7mm1_send(MmscGrp *mmc, Octstr *from, List *lto, 
			   Octstr *transid, 
			   Octstr *linkedid, char *vasid, Octstr *service_code,
			   MmsMsg *m, List *hdrs, Octstr **err, int *retry)
{
     MM1Request *r = gw_malloc(sizeof *r);
     Octstr *id;
     struct MM1Info_t *s = &mmc->mm1;
     
     gw_assert(m);
     
     if (!s->sender_alive) {
	  *err =  octstr_imm("internal error, mm1 notify not started!");
	  *retry  = 1;
	  return NULL;
     }

     /* Remove the from address first of all, replace the to address as well */
     mms_replace_header_value(m, "From", "#insert");
     mms_replace_header_values(m, "To", lto); /* Put in recipient list */
     mms_remove_headers(m, "Message-ID");

     r->u.m = m;
     pthread_cond_init(&r->cond, NULL);
     pthread_mutex_init(&r->mutex, NULL);
     r->waiter_exists = 1;
     r->type = MM1_PUSH;
     r->result = NULL;
     r->err = NULL;
     
     pthread_mutex_lock(&r->mutex); /* at pickup, must grab mutex before signalling. otherwise race condition.*/

     gwlist_produce(s->requests, r);
     
     pthread_cond_wait(&r->cond, &r->mutex);
     
     *err = r->err;
     
     id = r->result;
     mms_info(0, "MM1", mmc->id, "mm1_send: sent message, type=%s, result=%s", 
	      mms_message_type_to_cstr(mms_messagetype(m)),
	      r->err ? octstr_get_cstr(r->err) : "(none)");
     /* destroy the structure. */
     if(r->err && (octstr_compare(r->err, octstr_imm("Error-service-denied")) == 0 ||
		     octstr_compare(r->err, octstr_imm("Error-permanent-failure")) == 0))
	  *retry = 0;
     else
	  *retry = 1;
     
     pthread_cond_destroy(&r->cond);
     pthread_mutex_destroy(&r->mutex);
     gw_free(r);
     
     return id;
}

void handle_mm1_mt_requests(MmscGrp *mmsc)
{
     /* stop smsc, start GPRS, transact, stop GPRS, start SMSC. And so on. */
     MM1Request *r;

     mms_info(0, "MM7", mmsc->id, "handle_mm1 [%s] started", octstr_get_cstr(mmsc->id));
     mmsc->mm1.sender_alive++;
     while ((r = gwlist_consume(mmsc->mm1.requests)) != NULL) {
	  long n, pid = -1;
	  if (mmsc->mm1.smsc_off) {
	       n = system(octstr_get_cstr(mmsc->mm1.smsc_off));
	       gwthread_sleep(5); /* allow it to die. */
	  }
	  
	  if (mmsc->mm1.gprs_on) 
	       pid = start_gprs(mmsc->mm1.gprs_on, mmsc->id, mmsc->mm1.gprs_pid);
	  
	  if (pid  < 0) {
	       mms_warning(0, "MM7", mmsc->id,
			   "Failed to start GPRS connection. waiting...");
	       gwthread_sleep(2);
	       goto kill_gprs;
	  } else 
	       mms_info(0, "MM7", mmsc->id, "handle_mm1 [start_gprs] returned PID: %ld", pid);
	  
	  do {
	       Octstr *body;
	       Octstr *url;
	       int hstatus  = 0;
	       Octstr *ms;
	       MmsMsg *m;
	       int msize;
	       pid_t wp;
	       int st;

	       if (r->waiter_exists) 
		    pthread_mutex_lock(&r->mutex); /* grab lock to avoid race condition */

	       body = (r->type == MM1_PUSH) ? mms_tobinary(r->u.m) : NULL;
	       url  = (r->type == MM1_PUSH) ? mmsc->mmsc_url : r->u.url;
	       ms   = fetch_content_with_curl(mmsc, url, body, &hstatus);
	       msize = ms ? octstr_len(ms) : 0;
	       m  = (hstatus == 0 && ms)  ? mms_frombinary(ms, mmsc->mm1.msisdn) : NULL;

	       if (r->type == MM1_GET) {
		    if (m == NULL)
			 mms_error(0, "MM7", mmsc->id, "failed to fetch mms from URL: %s!",
				   octstr_get_cstr(url));
		    else {
			 List *mh = mms_message_headers(m), *to = gwlist_create();
			 Octstr *subject = NULL, *otransid = NULL, *msgid = NULL, *value;
			 Octstr *hfrom = mh ? http_header_value(mh, octstr_imm("From")) : octstr_imm("anon@anon");
			 Octstr *qf = NULL, *qdir = NULL, *mmc_id = NULL;
			 time_t expiryt = -1, deliveryt = -1;
			 int dlr;
			 
			 /* we assume it is a true message (send_req|retrieve_conf) */
			 mms_collect_envdata_from_msgheaders(mh, &to, &subject, 
							     &otransid, &expiryt, &deliveryt, 
							     DEFAULT_EXPIRE, -1,
							     octstr_get_cstr(unified_prefix), 
							     strip_prefixes);
			 
			 msgid = http_header_value(mh, octstr_imm("Message-ID"));	  
			 value = http_header_value(mh, octstr_imm("X-Mms-Delivery-Report"));	  
			 if (value && 
			     octstr_case_compare(value, octstr_imm("Yes")) == 0) 
			      dlr = 1;
			 else 
			      dlr = 0;
			 octstr_destroy(value);
			 
			 if (deliveryt < 0)
			      deliveryt = time(NULL);
			 
			 if (expiryt < 0)
			      expiryt = time(NULL) + DEFAULT_EXPIRE;
			 
			 if (hfrom == NULL)
			      hfrom = http_header_value(mh, octstr_imm("From"));
			 
			 mms_remove_headers(m, "Bcc");
			 mms_remove_headers(m, "X-Mms-Delivery-Time");
			 mms_remove_headers(m, "X-Mms-Expiry");
			 mms_remove_headers(m, "X-Mms-Sender-Visibility");
			 
			 qdir = get_mmsbox_queue_dir(hfrom, to, mmsc, &mmc_id); /* get routing info. */
			 /* Save it,  put message id in header, return. */     
			 qf = qfs->mms_queue_add(hfrom, to, subject, 
						 mmsc->id, mmc_id,
						 deliveryt, expiryt, m, NULL, 
						 NULL, NULL,
						 NULL, NULL,
						 NULL,
						 dlr,
						 octstr_get_cstr(qdir),
						 "MM7/MM1-IN",
						 octstr_imm(MM_NAME));
	  
			 if (qf) {
			      /* Log to access log */
			      mms_log("Received", hfrom, to, msize, 
				      msgid, NULL, mmsc->id, "MMSBox",octstr_imm("MM1"), NULL);
			      MMSC_CLEAR_ALARM(mmsc, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
			 } else {
			      mms_error(0, "MM7", mmsc->id, "handle_mm1: failed to create queue entry for URL %s",
					octstr_get_cstr(url));
			      MMSC_ISSUE_ALARM(mmsc, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);
			 }

			 if (otransid) { /* tell mmsc that we fetched fine. */
			      int _status;
			      MmsMsg *mresp = mms_notifyresp_ind(octstr_get_cstr(otransid),
								 mms_message_enc(m), "Retrieved", 1);
			      Octstr *sm = mms_tobinary(mresp);
			      Octstr *_x = fetch_content_with_curl(mmsc, 
								   NULL,
								   sm,
								   &_status);

			      octstr_destroy(_x);
			      octstr_destroy(sm);
			      mms_destroy(mresp);
			 }
			 gwlist_destroy(to, (void *)octstr_destroy);
			 octstr_destroy(hfrom);     
			 octstr_destroy(subject);
			 octstr_destroy(otransid);
			 octstr_destroy(msgid);
			 octstr_destroy(qf);
			 octstr_destroy(mmc_id);

			 http_destroy_headers(mh);
		    }
		    octstr_destroy(r->u.url); /* For GET only, because caller doesn't wait  */
	       }  else if (r->type ==  MM1_PUSH) {
		    Octstr *xs = NULL;
		    /* we expect a send-conf. */
		    if (ms) {
			 octstr_dump(ms, 0);

			 mms_msgdump(m, 1);
		    } else 
			 mms_warning(0, "MM7", mmsc->id,"handle_mm1: No send-conf returned by operator");

		    if (m == NULL ||
			(r->result = mms_get_header_value(m, octstr_imm("Message-ID"))) == NULL ||
			octstr_compare((xs = mms_get_header_value(m, octstr_imm("X-Mms-Response-Status"))),
				       octstr_imm("Ok")) != 0) {
			 Octstr *err = m ? mms_get_header_value(m, octstr_imm("X-Mms-Response-Text")) : NULL;
			 Octstr *status = m ? mms_get_header_value(m, octstr_imm("X-Mms-Response-Status")) : NULL;
			 if(status && (octstr_compare(status, octstr_imm("Error-service-denied")) == 0 ||
				       octstr_compare(status, octstr_imm("Error-permanent-failure")) == 0)) {
			      r->err = octstr_duplicate(status);
			 }
			 r->result = NULL; /* indicate failure to bearerbox */
			 mms_error(0, "MM7", mmsc->id, "Sending failed: %s, %s!", 
				   err ? octstr_get_cstr(err) : "(none)", 
				   status ? octstr_get_cstr(status) : "(none)");
			 octstr_destroy(err);
			 octstr_destroy(status);
		    }
		    octstr_destroy(xs);
	       } else 
		    mms_error(0, "MM7", mmsc->id, "unknown type: %d", r->type);
	       
	       if (r->waiter_exists) {
		    pthread_mutex_unlock(&r->mutex);
		    pthread_cond_signal(&r->cond);
	       } else  /* no waiter, so we free it ourselves. */
		    gw_free(r);
	       
	       octstr_destroy(body);
	       octstr_destroy(ms);
	       mms_destroy(m);
	       r = NULL;

	       if (pid > 0) {
		    wp = waitpid(pid, &st, WNOHANG);
		    if(wp == pid && WIFEXITED(st)) {
			 mms_info(0, "MM7", mmsc->id, "GPRS pid (%d) appears to be dead - quitting loop", pid);
			 goto after_gprs_dead;
		    }
	       }
	       gwthread_sleep(2); /* according to Piotr Isajew, this makes life better */
	  } while (gwlist_len(mmsc->mm1.requests) > 0 &&
		   (r = gwlist_consume(mmsc->mm1.requests)) != NULL);
	  
     kill_gprs:
	  if(r != NULL) {
	       if(r->waiter_exists) {
		    pthread_mutex_unlock(&r->mutex);
		    pthread_cond_signal(&r->cond);
	       } else{
		    gw_free(r);
	       }
	  }
          
	  if (mmsc->mm1.gprs_off) {
	       stop_gprs(mmsc->mm1.gprs_off);
	  } else if (pid > 0) { /* stop GPRS, restart SMSC connection. */
	       int xkill, status;
	       pid_t wpid;
	       do { 
		    xkill = kill(pid, SIGTERM);
		    mms_info(0, "MM7", mmsc->id, "GPRS turned off returned: %d", xkill);
		    if (xkill < 0 && errno == ESRCH) 
			 break;
		    wpid = waitpid(pid, &status, 0);
		    if (wpid == pid && WIFEXITED(status)) 
			 break;
		    else if (wpid < 0 && errno == ECHILD) 
			 break;
	       } while (1);
	       gwthread_sleep(2);
	  }
     after_gprs_dead:
	  if (mmsc->mm1.smsc_on) {
	       system(octstr_get_cstr(mmsc->mm1.smsc_on));
	       gwthread_sleep(5);
	       mms_info(0, "MM7", mmsc->id, "SMSC turned on");
	  }
	  
     }
     mmsc->mm1.sender_alive--;
     mms_info(0, "MM7", mmsc->id, "handle_mm1 exits");
}

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
static int write_octstr_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
     Octstr *out = userp;
     
     octstr_append_data(out, buffer, size*nmemb);
     mms_info(0, "mmsbox-mm1", NULL,  "write_data called with nmemn=%ld, size=%ld",
	      nmemb, size);
     return size*nmemb;
}
#endif

static Octstr *fetch_content_with_curl(MmscGrp *mmc,  Octstr *url, Octstr *body, int *hstatus)
{
     Octstr *s = octstr_create("");
#ifdef HAVE_LIBCURL
    
     Octstr *proxy = mmc->mm1.proxy;
     Octstr *ua = mmc->mm1.ua;

     CURL *cl;
     struct curl_slist *h = NULL;
     char errbuf[512];
     static int curl_inited = 0;
     
     Octstr *xurl =  url ? url : mmc->mmsc_url;
     if (curl_inited == 0) {
	  curl_global_init(CURL_GLOBAL_ALL);
	  curl_inited = 1;
     }

     cl = curl_easy_init();
     curl_easy_setopt(cl, CURLOPT_URL, octstr_get_cstr(xurl));
     if (octstr_len(proxy) > 0)
	  curl_easy_setopt(cl, CURLOPT_PROXY, octstr_get_cstr(proxy));
     curl_easy_setopt(cl, CURLOPT_WRITEFUNCTION, write_octstr_data);
     curl_easy_setopt(cl, CURLOPT_WRITEDATA, s);
     curl_easy_setopt(cl, CURLOPT_NOSIGNAL, 1L);
     curl_easy_setopt(cl, CURLOPT_TIMEOUT, 120L);
     curl_easy_setopt(cl, CURLOPT_FORBID_REUSE, 1L);
     curl_easy_setopt(cl, CURLOPT_CONNECTTIMEOUT, 40L);
     
     h = curl_slist_append(h, "Accept: */*");
     if (body) { /* POST. */
	  h = curl_slist_append(h, "Content-Type: application/vnd.wap.mms-message");
	  curl_easy_setopt(cl, CURLOPT_POSTFIELDS, octstr_get_cstr(body));
	  curl_easy_setopt(cl, CURLOPT_POSTFIELDSIZE, octstr_len(body));
     }
     
     if (ua) {
	  Octstr *x = octstr_format("User-Agent: %S", ua);
	  h = curl_slist_append(h, octstr_get_cstr(x));
	  octstr_destroy(x);
     }

     curl_easy_setopt(cl, CURLOPT_HTTPHEADER, h);         
     curl_easy_setopt(cl, CURLOPT_ERRORBUFFER, errbuf);         

     *hstatus = curl_easy_perform(cl); /* post away! */
     if (*hstatus != 0) {
	  MMSC_ISSUE_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT, 3);

	  mms_error(0, "mmsbox-mm1", NULL, "failed to fetch/post content to host %s [proxy: %s] [http_status=%d] : %.256s",
		    octstr_get_cstr(url), octstr_len(proxy) > 0 ? octstr_get_cstr(proxy) : "n/a",
		    *hstatus, errbuf);
     } else 
	  MMSC_CLEAR_ALARM(mmc, MMSBOX_ALARM_MM7_NON_200_RESULT);
     curl_slist_free_all(h); /* free the header list */
     curl_easy_setopt(cl, CURLOPT_NOSIGNAL, 0L); /* Stop blocking signals */
     curl_easy_cleanup(cl);
#else
     panic(0, "Libcurl not linked in.");
#endif
     return s;
}

static void stop_gprs(Octstr *cmd)
{
     char *xcmd = octstr_get_cstr(cmd);
     FILE *f = popen(xcmd, "r");
     
     if (f)
	  pclose(f);
}

#define MAX_GPRS_WAIT 80
#define GPRS_POLL  5
static long start_gprs(Octstr *cmd, Octstr *id, Octstr *pid_cmd)
{
     int ct = 0;
     char *xcmd = octstr_get_cstr(cmd);
     char *pcmd = pid_cmd ? octstr_get_cstr(pid_cmd) : NULL;
     FILE *f = popen(xcmd, "r");

     if (f == NULL) {
	  mms_error(0, "MM7", id, "start_gprs: failed to start process!");
	  return -1;
     }

     pclose(f);

     if (pid_cmd != NULL)
	  do { /* Wait for it. */
	       long xpid = -1;
	       
	       gwthread_sleep(GPRS_POLL); /* wait a little. */
	       if ((f = popen(pcmd, "r")) != NULL) {
		    fscanf(f, "%ld", &xpid);
		    pclose(f);
		    if (xpid >= 0) 
			 return xpid;
	       }
	       mms_info(0, "MM7", id, 
			"start_gprs: waiting for connection: %d",ct);
	  } while (GPRS_POLL*ct++ < MAX_GPRS_WAIT);
     else 
	  return 0;
     mms_error(0, "MM7", id, "start_gprs: failed to get PID!");
     return -1;
}


static int mm7mm1_receive(MmsBoxHTTPClientInfo *hci)
{
     HTTPClient *client = hci->client;
     List *cgivar_ctypes = NULL;
     Octstr *text, *rb = NULL, *s = NULL, *loc = NULL;
     MmsMsg *m = NULL;
     int hdrlen, status = HTTP_ACCEPTED, mtype;
     List *mh = NULL, *to = gwlist_create(), *rh = http_create_empty_headers();
     time_t expiryt = -1, deliveryt = -1;
     Octstr *from = NULL, *subject = NULL, *otransid = NULL, *mmc_id = NULL;
     Octstr *qdir;
     
     parse_cgivars(hci->headers, hci->body, &hci->cgivars, &cgivar_ctypes);
     
     if ((text = http_cgi_variable(hci->cgivars, "text")) == NULL) {
	  rb = octstr_imm("mmsbox-mm1: missing 'text' CGI parameter!");
	  status = HTTP_NOT_FOUND;	       
	  MMSC_ISSUE_ALARM(hci->m, MMSBOX_ALARM_MM7_PARSING_FAILURE,2);
	  goto done;
     } else 
	  MMSC_CLEAR_ALARM(hci->m, MMSBOX_ALARM_MM7_PARSING_FAILURE);
     
     hdrlen = octstr_get_char(text, 2);
     if ((s = octstr_copy(text, 3 + hdrlen, octstr_len(text))) != NULL)
	  m = mms_frombinary(s, hci->m->mm1.msisdn);
     else 
	  m = NULL;
	  
     if (m == NULL) {
	  rb = octstr_imm("mmsbox-mm1: mal-formed mms packet on interface!");
	  status = HTTP_FORBIDDEN;
	  goto done;
     } else 
	  mms_msgdump(m, 1);

     /* rest of this copied largely from EAIF code. */
     mh = mms_message_headers(m);
     mtype = mms_messagetype(m);
     mms_collect_envdata_from_msgheaders(mh, &to, &subject, 
					 &otransid, &expiryt, &deliveryt, 
					 DEFAULT_EXPIRE, -1,
					 octstr_get_cstr(unified_prefix), 
					 strip_prefixes);
     from = http_header_value(mh, octstr_imm("From"));
     qdir = get_mmsbox_queue_dir(from, to, hci->m, &mmc_id); /* get routing info. */
     switch (mtype) {
	  Octstr *qf;
	  Octstr *dlr_url, *status_value, *msgid;
	  List *rqh;
     case MMS_MSGTYPE_DELIVERY_IND: /* notification of a delivery. */
     case MMS_MSGTYPE_READ_ORIG_IND: /* message read. */
	  msgid = http_header_value(mh, octstr_imm("Message-ID"));
	  status_value = http_header_value(mh, 
					   (mtype == MMS_MSGTYPE_DELIVERY_IND) ? 
					   octstr_imm("X-Mms-Status") : 
					   octstr_imm("X-Mms-Read-Status"));

	  rqh = http_create_empty_headers();
	  
	  dlr_url = mmsbox_get_report_info(m, hci->m, mmc_id, 
					   (mtype == MMS_MSGTYPE_DELIVERY_IND) ?  
					   "delivery-report" : "read-report",
					   status_value, rqh, NULL, 0, msgid);
	  
	  qf = qfs->mms_queue_add(from, to, NULL, 
				  hci->m->id, mmc_id,
				  0, time(NULL) + default_msgexpiry, m, NULL, 
				  NULL, NULL,
				  dlr_url, NULL,
				  rqh,
				  0,
				  octstr_get_cstr(qdir),
				  "MM7/MM1-IN",
				  octstr_imm(MM_NAME));
	  if (qf)  {
	       /* Log to access log */
	       mms_log((mtype == MMS_MSGTYPE_DELIVERY_IND) ? "Received DLR" : "Received RR", 
		       from, to, -1, msgid, status_value, hci->m->id, 
		       "MMSBox", octstr_imm("MM1"), NULL);			
	       MMSC_CLEAR_ALARM(hci->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR);			 
	  } else {
	       MMSC_ISSUE_ALARM(hci->m, MMSBOX_ALARM_QUEUE_WRITE_ERROR, 4);	   
	       status = HTTP_INTERNAL_SERVER_ERROR;
	  }
	  octstr_destroy(qf);
	  octstr_destroy(msgid);
	  octstr_destroy(dlr_url);
	  octstr_destroy(status_value);
	  http_destroy_headers(rqh);
	  break;
	  
     case MMS_MSGTYPE_NOTIFICATION_IND: /* notification of an incoming message. */
	  if ((loc = http_header_value(mh, octstr_imm("X-Mms-Content-Location"))) != NULL) {
	       MM1Request *r = gw_malloc(sizeof *r);
		    
	       memset(r, 0, sizeof *r);
	       r->type = MM1_GET;
	       r->u.url = loc;		    
	       r->waiter_exists = 0;
	       loc = NULL;
	       gw_assert(hci->m->mm1.requests);
	       gwlist_produce(hci->m->mm1.requests, r); 
	  } else 
	       rb = octstr_format("mmsbox-mm1: notification with content-location??");
	  break;
     default:
	  rb = octstr_format("mmsbox-mm1: unexpected message type: %s",
			     mms_message_type_to_cstr(mtype));
	  status = HTTP_NOT_FOUND;
	  break;
     }
	  
	  
done:
     /* send reply. */
     http_header_add(rh, "Content-Type", "text/plain");
     http_send_reply(client, status, rh, rb ? rb : octstr_imm(""));
	  
     octstr_destroy(s);
     octstr_destroy(loc);
     octstr_destroy(mmc_id);
     octstr_destroy(from);
     octstr_destroy(subject);
     octstr_destroy(otransid);

     octstr_destroy(rb);
     gwlist_destroy(to, (void *)octstr_destroy);
  
     http_destroy_headers(rh);
     http_destroy_headers(mh);
     http_destroy_cgiargs(cgivar_ctypes);
     mms_destroy(m);

     return http_status_class(status) == HTTP_STATUS_SUCCESSFUL ? 0 : -1;
}
