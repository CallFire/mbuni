/*
 * Mbuni - Open  Source MMS Gateway 
 * 
 * MMSBOX CFG: MMC configuration and misc. functions
 * 
 * Copyright (C) 2003 - 2008, Digital Solutions Ltd. - http://www.dsmagic.com
 *
 * Paul Bagyenda <bagyenda@dsmagic.com>
 * 
 * This program is free software, distributed under the terms of
 * the GNU General Public License, with a few exceptions granted (see LICENSE)
 */
#ifndef __MMSBOX_CFG_INCLUDED__
#define __MMSBOX_CFG_INCLUDED__
#include "mms_util.h"
#include "mmsbox_mt_filter.h"
#include "mms_queue.h"
#include "mmsbox_resolve.h"

#include "mmsbox_mmsc.h"
#include "mmsbox_cdr.h"
#include "mms_cfg.h"

/* Alarm callback mechanism */
enum MmsBoxAlarms {
     MMSBOX_ALARM_HTTP_DOWN = 0,
     MMSBOX_ALARM_FILE_WRITE_ERROR,
     MMSBOX_ALARM_SOCKET_CONNECT_FAILED,
     MMSBOX_ALARM_QUEUE_WRITE_ERROR,
     MMSBOX_ALARM_STORAGE_API_ERROR,
     MMSBOX_ALARM_MM7_PARSING_FAILURE,
     MMSBOX_ALARM_MM7_NON_200_RESULT,
     MMSBOX_ALARM_RETRIEVE_MMS_ERROR,
     MMSBOX_ALARM_MM4_PARSING_FAILURE,
     MMSBOX_ALARM_MM1_ADDRESS_RESOLUTION_ERROR,
     MMSBOX_ALARM_MAX_ALARM /* Must be last one */
};

typedef struct MmscGrp {
     Octstr *id;       /* MMSC id (for logging). */
     Octstr *group_id; /* GROUP MMSC id (used for qf). */
     Octstr *vasp_id;  /* vasp id for SOAP mmsc */
     Octstr *mmsc_url; /* URL at which MMSC can be reached. */
     struct {
	  Octstr *user, *pass;
	  Octstr *allow_ip;
	  Octstr *deny_ip;     	  
	  long port;
	  int ssl;
     } incoming;      /* user, pass, port (and whether SSL) that MMSC uses to connect to us. */
     Octstr *allowed_prefix,  *denied_prefix;
     Octstr *allowed_sender_prefix,  *denied_sender_prefix;
     Octstr *carrier_id_header; /* http header that has carrier id */
     enum {UNKNOWN_MMSC = -1, CUSTOM_MMSC, SOAP_MMSC, EAIF_MMSC, MM4_MMSC, HTTP_MMSC, MM1_MMSC} type; /* type of connection. */
     double  throughput;  /* Max send rate.  */
     long threadid;   /* handler thread. */
  
     int reroute;     /* whether messages from this mmsc are re-routed outwards. */
     Octstr *reroute_mmsc_id;
     int no_senderaddress; /* used by SOAP interface: Don't add a sender address. */
     int reroute_mod_subject; /* Set to true if we'll change subject line on reroute. */
     MM7Version_t ver; /* supported MM7/SOAP version. */
     int use_mt_filter; /* whether to use MT filter on this connection. */
     Mutex *mutex;

     Octstr *default_vasid; /* default vasid  for mm7/soap */
     
     MmsBoxMmscFuncs *fns; /* pointer to functions for handling this mmsc connection type */
     Octstr *settings;     /* settings for the above module. */
     void *data;           /* data for above module. */
     int started;          /* Whether it is active */
     unsigned long mt_pdus;   /* number of MT PDUs since start. */
     unsigned long mo_pdus;   /* number of MO PDUs since start. */
     unsigned long mt_errors; /* number of MT errors since start */
     unsigned long mo_errors; /* number of MO errors since start */
     
     time_t last_pdu;         /* time of last PDU */
     time_t start_time;       /* when was this connection started */
     
     time_t last_alarm[MMSBOX_ALARM_MAX_ALARM];
     int use_count;        /* use counter. */
     time_t delete_after;  /* used to control deletion of object -- not very clean, but... */

     long max_pkt_size;
     
     int strip_domain;       /* MM4 only */

     long max_recipients; /* Max recpients per transaction */

     List *strip_prefixes; /* List of prefixes to be stripped before sending out*/

     List *retry_statuses; /* List of HTTP/MM7 statuses we do retry on, MM7 statuses are checked only on HTTP 200 - they do not overlap*/

     struct MM1Info_t {               /* Stuff used only by the MM1 MMSC */
	  Octstr *proxy;    /* Proxy within the operator network, form of host:port */
	  Octstr *gprs_on;  /* Command to start GPRS link. Must not exit. */
	  Octstr *gprs_off; /* Command to stop GPRS link. */
	  Octstr *gprs_pid; /* command to call to get PID of GPRS for stopping GPRS link (i.e. pppd). */
	  Octstr *smsc_on;  /* command to start smsc connection */
	  Octstr *smsc_off; /* commadn to stop smsc connection  */
	  Octstr *msisdn;   /* Our msisdn */
	  Octstr *ua;       /* User agent string, if given */ 
	  List *requests;   /* list of requests. */
	  long d_tid;/* thread ID for mm1 handler.  */
	  
	  int sender_alive;	  
     } mm1;
} MmscGrp;

#define DEFAULT_MAX_PKT_SIZE 1024*1024

#define MMSBOX_MMSC_MARK_INUSE(mmc) do {\
	  mutex_lock((mmc)->mutex); \
	  (mmc)->use_count++; \
	  mutex_unlock(mmc->mutex); \
     } while (0)

#define MMSBOX_MMSC_UNMARK_INUSE(mmc) do {\
	  mutex_lock((mmc)->mutex); \
	  (mmc)->use_count--; \
	  mutex_unlock(mmc->mutex); \
     } while (0)


typedef struct MmsServiceUrlParam {
     Octstr *name;
     enum {NO_PART, AUDIO_PART, IMAGE_PART, VIDEO_PART, 
	   TEXT_PART, SMIL_PART , OTHER_PART, 
	   ANY_PART, WHOLE_BINARY, KEYWORD_PART,

	   RCPT_PART, FROM_PART, SUBJECT_PART, MMSCID_PART,
	   MSGTYPE_PART, MSGID_PART} type;
     Octstr *value; /* for generic value (type == NO_PART), 
		     * or for value that follows spec (e.g. %Tisatest is allowed) 
		     */
} MmsServiceUrlParam;

typedef struct MmsService {
     Octstr *name;         /* name of service. */
     int isdefault;
     int omitempty;
     int noreply;
     int accept_x_headers;
     List *passthro_headers;
     
     int assume_plain_text;
     List   *keywords;  /* List of keywords matched. */
     enum {TRANS_TYPE_GET_URL, TRANS_TYPE_POST_URL, TRANS_TYPE_FILE, TRANS_TYPE_EXEC, 
	   TRANS_TYPE_TEXT} type;
     Octstr *url;        /* The value. */
     List   *params;     /* of MmsServiceUrlParam */
     
     Octstr *faked_sender;
     List   *allowed_mmscs; /* List of MMSCs allowed to access this service (by ID). */
     List   *denied_mmscs;  /* List of MMSCs allowed to access this service (by ID). */
     Octstr *service_code;  /* Service code (MM7/SOAP only) */

     Octstr *allowed_receiver_prefix,  *denied_receiver_prefix;
     Octstr *special_header; /* To be added to each content element. */
} MmsService;

typedef struct SendMmsUser {
     Octstr *user, *pass;
     Octstr *faked_sender;
     Octstr *dlr_url, *rr_url, *mmsc;
} SendMmsUser;

/* Basic settings for the mmsbox. */
extern List *sendmms_users; /* list of SendMmsUser structs */
extern List *mms_services;  /* list of MMS Services */
extern Octstr *incoming_qdir, *outgoing_qdir, *dlr_dir;	  
extern Octstr *unified_prefix;
extern Octstr *sendmail_cmd;
extern Octstr *myhostname;
extern List *strip_prefixes;
extern long svc_maxsendattempts, maxsendattempts, mmsbox_send_back_off, default_msgexpiry, max_msgexpiry;
extern long  maxthreads;
extern double queue_interval;
extern struct SendMmsPortInfo {
     long port; /* Might be ssl-ed. */
     Octstr *allow_ip;
     Octstr *deny_ip;     
} sendmms_port;

extern struct MmsBoxMTfilter *mt_filter;
extern  MmsQueueHandlerFuncs *qfs;
extern int mt_multipart;

extern MmsBoxResolverFuncStruct *rfs; /* resolver functions. */
extern void *rfs_data;
extern Octstr *rfs_settings; 

extern   struct SMTPRelay {
     Octstr *host;
     int port;
} smtp_relay;

extern MmsBoxCdrFuncStruct *cdrfs;


extern struct MmsBoxHealthMonitors {
     void (*register_thread)( char *name);   /* Called by each thread to register itself with the health monitor */
     void (*unregister_thread)(char *name); /* Called by each thread to unregister itself with the health monitor */
     void (*register_port)(short port);     /* Called to register each port on which we listen */
     void (*unregister_port)(short port);     /* Called to unregister each port on which we listen */
} *hmon; /* This should be set if you want to do any thread and port monitoring */

/* mmsbox_event_cb: Called with each mmsc event. 
 * mmsc - mmsc ID
 * mm7_pkt_type - MM7 packet type (using MM7/SOAP codes)
 * mm7_ver - value for SOAP or EAIF only
 * status - 0 for Ok, -1 if failed (e.g. for submitack
 * msg_size - size of mms
 * num_retries - number of retries so far
 * from - sender (can be NULL)
 * to - recipient (can be NULL)
 * transid - transaction ID
 * message_id - Message ID (for submitack, deliverreq, deliveryreport or readreport)
 * hdrs - List of envelope headers
 * value - value associated with pkt type: e.g. for deliveryreport, report type
 */
extern void (*mmsbox_event_cb)(Octstr *mmsc, int mm7_pkt_type, 	
			       int is_mm4,
			       Octstr *mm7_ver, int status, 
			       int msg_size, int num_retries, 
			       Octstr *from, Octstr *to, Octstr *message_id, Octstr *transid,
			       List *hdrs, Octstr *value);

/* mmsbox_alarm_cb: Called when an alarm is raised or cleared
 * mmsc - MMSC ID
 * alarm - alarm type
 * alarm_state - 0 = alarm cleared, 1 = alarm raised
 * lev = severity level, 1 = warning, 2 = minor, 3 = major, 4+ = critical 
 */
extern void (*mmsbox_alarm_cb)(Octstr*mmsc, enum MmsBoxAlarms alarm, int alarm_state, int lev);

#define MMSC_ISSUE_ALARM(mmc,alarm,lev) do {		\
	  MmscGrp *_mmc = (mmc);			\
	  if (_mmc)					 \
	       _mmc->last_alarm[(alarm)] = time(NULL); \
	  mmsbox_alarm_cb(_mmc ? _mmc->id : NULL, (alarm), 1, (lev));	\
     } while (0)
#define MMSC_CLEAR_ALARM(mmc,alarm) do {				\
	  MmscGrp *_mmc = (mmc);					\
	  if (_mmc && _mmc->last_alarm[(alarm)] > 0 )	{		\
	       mmsbox_alarm_cb(_mmc->id, (alarm), 0, 0);		\
	       _mmc->last_alarm[(alarm)] = 0;				\
	  }								\
     } while (0)

extern int mms_load_mmsbox_settings(struct mCfgImpFuncs *cfg_funcs, Octstr *init, 
				    gwthread_func_t *mmsc_handler_func, 
				    MmsQueueHandlerFuncs *, 
				    MmsEventLoggerFuncs *);
extern void mmsbox_settings_cleanup(void);
extern MmscGrp *get_handler_mmc(Octstr *id, Octstr *to, Octstr *from);
extern void return_mmsc_conn(MmscGrp *m);

extern Octstr  *get_mmsbox_queue_dir(Octstr *from, List *to, MmscGrp *m,  Octstr **mmc_id);
#if 0
MmscGrp *start_mmsc_from_conf(mCfg *cfg, mCfgGrp *x, List *errors, List *warnings);
#endif
int mmsbox_stop_mmsc(Octstr  *mmc);

int mmsbox_start_mmsc(Octstr *mmc_id);

MmscGrp *mmsbox_get_mmsc_by_url(Octstr *mmc_url);
void mmsbox_stop_all_mmsc_conn(void);
typedef struct MmsBoxHTTPClientInfo {
     HTTPClient *client;
     Octstr *ua;
     Octstr *ip;
     List   *headers;
     Octstr *url;
     Octstr *body;
     List   *cgivars;
     MmscGrp *m;     
} MmsBoxHTTPClientInfo;
void free_mmsbox_http_clientInfo(MmsBoxHTTPClientInfo *h, int freeh);
/* parse service url params in list of MmsServiceUrlParm structs */
List *parse_service_url_params(Octstr *input, Octstr *id);

#define add_all_matching_parts(a,b,c,d,e,f,g) add_all_matching_parts_real((a),(b),(c),(d),(e),(f),(g),NULL,NULL,NULL,NULL,-1,NULL)

void add_all_matching_parts_real(MIMEEntity *plist, MmsServiceUrlParam *pm, 
				 Octstr *keyword,
				 MIMEEntity *me, 
				 MmsMsg *msg, int lev, int count, 
				 Octstr *from, List *lto,  char *subject,
				 char *mmsc_id, int mtype, Octstr *msgid);

MIMEEntity *mmsbox_add_service_url_params(List *params,  
					  Octstr *from, List *lto,  char *subject,
					  char *mmsc_id, int mtype, Octstr *msgid,
					  MmsMsg *m);

extern volatile sig_atomic_t rstop;
#endif
