#if 0
# 
# Copyright (c) 2014 - 2016 Javier Sayago <admin@lonasdigital.com>
# Contact: javilonas@esp-desarrolladores.com
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/ioctl.h>

#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <netdb.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

#include <errno.h>
#include <sys/time.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>

#include <signal.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/un.h>

#include "common.h"
#include <getopt.h>
#include "tools.h"
#include "debug.h"
#include "sockets.h"
#include "threads.h"
#include "convert.h"

#include "cscrypt/bn.h"
#include "cscrypt/aes.h"
#include "cscrypt/des.h"
#include "cscrypt/md5.h"
#include "cscrypt/sha1.h"

#include "msg-newcamd.h"
#ifdef CCCAM
#include "msg-cccam.h"
#endif
#ifdef RADEGAST
#include "msg-radegast.h"
#endif

#include "ecmdata.h"
#include "parser.h"
#include "config.h"
#include "httpserver.h"

#include <linux/dvb/ca.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>

#define CADEV		"/dev/dvb/adapter0/ca1"
#define DMXDEV		"/dev/dvb/adapter0/demux0"
#define CAMDSOCKET	"/tmp/camd.socket"

#ifdef CFG
char config_file[512] = CFG;
#else
char config_file[512] = "/var/etc/newbox.cfg";
#endif

char config_badcw[512] = "/var/etc/badcw.cfg";
char config_channelinfo[512] = "/var/etc/newbox.channelinfo";

char cccam_nodeid[8];
char freecccam_nodeid[8];

int32_t flag_debugscr;
int32_t flag_debugnet;
int32_t flag_debugfile;
char debug_file[256];
char sms_file[256];

///
///
struct config_data cfg;
struct program_data prg;

uint ecm_check_time = 0;

uint cs_dcw_check_time = 0;
#ifdef MGCAMD_SRV
uint mg_dcw_check_time = 0;
#endif
#ifdef CCCAM_SRV
uint cc_dcw_check_time = 0;
#endif
#ifdef FREECCCAM_SRV
uint frcc_dcw_check_time = 0;
#endif
#ifdef RADEGAST_SRV
uint rdgd_dcw_check_time = 0;
#endif

///////////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////////


// 0: different ; 1:~equivalent
int32_t cmp_cards( struct cs_card_data* card1, struct cs_card_data* card2)
{
	int32_t i,j,found;
	int32_t nbsame = 0;
	int32_t nbdiff = 0;

	if (card1->caid!=card2->caid) return 0;

/*
	if ( ((card1->caid & 0xff00)==0x1800)
		|| ((card1->caid & 0xff00)==0x0900)
		|| ((card1->caid & 0xff00)==0x0b00) ) return 1;
*/
	if ( ((card1->caid & 0xff00)!=0x0100) && ((card1->caid & 0xff00)!=0x0500) ) return 1;

	for(i=0; i<card1->nbprov;i++) {
		found = 0;
		for(j=0; j<card2->nbprov;j++)
			if (card1->prov[i]==card2->prov[j]) {
				found = 1;
				break;
			}
		if (found) nbsame++; else nbdiff++;
	}

	if ( (nbsame==card1->nbprov)||(nbsame==card2->nbprov) ) return 1;
	return 0;
}


// Get Any card to decode
struct cs_card_data *srv_findcard( struct cs_server_data *srv, struct cardserver_data *cs, uint16_t ecmcaid, uint32_t ecmprov)
{
	// check for card to decode
	struct cs_card_data *pcard = srv->card;
	struct cs_card_data *selcard = NULL;
	int32_t i;
	while (pcard) {
		if ( ecmcaid == pcard->caid ) {
			for (i=0; i<pcard->nbprov;i++) if (ecmprov==pcard->prov[i]) break;
			if  ( ( i<pcard->nbprov ) || ( cs && cmp_cards(pcard, &cs->card) ) ) {
				if (srv->type==TYPE_NEWCAMD) {
					selcard = pcard;
					break;
				} else if (srv->type==TYPE_MGCAMD) {
					selcard = pcard;
					break;
				}
				else if (srv->type==TYPE_CCCAM) { // select card on hop1 if is there
					if (selcard) {
						if (pcard->uphops<selcard->uphops) selcard = pcard;
						else if (pcard->uphops==selcard->uphops) {
							// Get Stable Card
							if (pcard->shareid<selcard->shareid) selcard = pcard;
						}
					} else selcard = pcard;
				}
				if (srv->type==TYPE_RADEGAST) {
					selcard = pcard;
					break;
				}
				else break;
			}
		}
		pcard = pcard->next;
	}
	return selcard;
}


void srv_cstatadd( struct cs_server_data *srv, int32_t csid, int32_t ok, uint32_t ecmoktime)
{
	int32_t i;
	for(i=0; i<MAX_CSPORTS; i++) {
		if (!srv->cstat[i].csid) {
			srv->cstat[i].csid = csid;
			srv->cstat[i].ecmnb = 1;
			if (ok) {
				srv->cstat[i].ecmok = 1;
				srv->cstat[i].ecmoktime = ecmoktime;
			}
			else {
				srv->cstat[i].ecmok = 0;
				srv->cstat[i].ecmoktime = 0;
			}
			break;
		}
		else if (srv->cstat[i].csid==csid) {
			srv->cstat[i].ecmnb++;
			if (ok) {
				srv->cstat[i].ecmok++;
				srv->cstat[i].ecmoktime += ecmoktime;
			}
			break;
		}
	}
}



///////////////////////////////////////////////////////////////////////////////


// check for card existance
int32_t istherecard(struct cs_server_data *srv, struct cs_card_data *acard)
{
	struct cs_card_data *card = srv->card;
	while(card) {
		if (card==acard) return 1;
		card = card->next;
	}
	return 0;
}


int32_t match_card( uint16_t caid, uint32_t prov, struct cs_card_data* card)
{
	if (caid!=card->caid) return 0;
	// Dont care about provider for caid non via/seca
	if ( ((card->caid & 0xff00)!=0x0100) && ((card->caid & 0xff00)!=0x0500) ) return 1;
	int32_t i;
	for(i=0; i<card->nbprov;i++) if (prov==card->prov[i]) return 1;
	return 0;
}

// Search for a card with best sid val.
// TODO: option validecmtime-ecmtime
int32_t sidata_getval(struct cs_server_data *srv, struct cardserver_data *cs, uint16_t caid, uint32_t prov, uint16_t sid, struct cs_card_data **selcard )
{
	struct cs_card_data *card = NULL;

	*selcard = NULL;
	if ( (srv->type==TYPE_NEWCAMD) || (srv->type==TYPE_MGCAMD) || (srv->type==TYPE_RADEGAST) ) {
		card = srv->card;
		while (card) {
			if ( match_card(caid,prov,card) ) break;
			card = card->next;
		}
		*selcard = card;
		if (card) {
			struct sid_data *sidata = card->sids;
			while (sidata) {
				if (sidata->sid==sid)
				if (sidata->prov==prov) return sidata->val;
				sidata = sidata->next;
			}
		}
		return 0; // Channel not found in sid cache
	}
#ifdef CCCAM_CLI
	else if (srv->type==TYPE_CCCAM) {
		// Search for availabe card cannot be found in sids
		*selcard = NULL;
		int32_t selsidvalue = 0;
		card = srv->card;
		while (card) {
			if ( match_card(caid,prov,card)
				|| ( !prov && cs && cmp_cards(card, &cs->card) ) // use for prov=0
			) {
				// Search fo sid
				int32_t sidvalue = 0; // by default

				struct sid_data *sidata = card->sids;
				while (sidata) {
					if ( (sidata->sid==sid)&&(sidata->prov==prov) ) {
						sidvalue = sidata->val;
						break;
					}
					sidata = sidata->next;
				}

				// Check with Selected card (sidvalue) TODO: classify by ecmtime, decode, stability
				if (*selcard) {
					if ( selsidvalue<0 ) {
						if (sidvalue>=0) { *selcard = card; selsidvalue = sidvalue; }
						else if (card->uphops<(*selcard)->uphops) { *selcard = card; selsidvalue = sidvalue; }
					}
					else if ( selsidvalue==0 ) {
						if (sidvalue>0) { *selcard = card; selsidvalue = sidvalue; }
						else if (sidvalue==0) if (card->uphops<(*selcard)->uphops) { *selcard = card; selsidvalue = sidvalue; }
					}
					else if (sidvalue>0) if (card->uphops<(*selcard)->uphops) { *selcard = card; selsidvalue = sidvalue; }
				}
				else { *selcard = card; selsidvalue = sidvalue; }
			}
			card = card->next;
		}
		return selsidvalue;
	}
#endif
	return 0;
}


void cardsids_add(struct cs_card_data *card, uint32_t prov, uint16_t sid,int32_t val)
{
	if (!sid) return;
	struct sid_data *sidata = malloc( sizeof(struct sid_data) );
	memset( sidata, 0, sizeof(struct sid_data) );

	sidata->sid=sid;
	sidata->prov=prov;
	sidata->val=val;
	sidata->next = card->sids;
	card->sids = sidata;
}


int32_t cardsids_update(struct cs_card_data *card, uint32_t prov, uint16_t sid,int32_t val)
{
	if (!sid) return 0;

	struct sid_data *sidata = card->sids;
	while (sidata) {
		if (sidata->sid==sid)
		if (sidata->prov==prov) {
			if ( (sidata->val<100) && (sidata->val>-100) ) {
				if (sidata->val>0) {
					if (val>0) sidata->val +=val;
					else sidata->val =0;
				}
				else if (sidata->val<0) {
					if (val<0) sidata->val +=val;
					else sidata->val=0;
				}
				else sidata->val +=val; // else a card that has decode success one time cannot return to decode failed
			}
			return 1;
		}
		sidata = sidata->next;
	}

	if ( !sidata && sid ) {
		cardsids_add( card, prov, sid,val);
	}
	return 1;
}


///////////////////////////////////////////////////////////////////////////////
// Common profile functions
///////////////////////////////////////////////////////////////////////////////

struct cardserver_data *getcsbycaidprov( uint16_t caid, uint32_t prov)
{
	int32_t i;
	if (!caid) return NULL;
	struct cardserver_data *cs = cfg.cardserver;
	while (cs) {
		if (cs->card.caid==caid) {
			for(i=0; i<cs->card.nbprov;i++) if (cs->card.prov[i]==prov) return cs;
			if ( ((cs->card.caid & 0xff00)==0x1800)
				|| ((cs->card.caid & 0xff00)==0x0900)
				|| ((cs->card.caid & 0xff00)==0x0b00) ) return cs;
		}
		cs = cs->next;
	}
	return NULL;
}


struct cardserver_data *getcsbyid(uint32_t id)
{
	if (!id) return NULL;
	struct cardserver_data *cs = cfg.cardserver;
	while (cs) {
		if (cs->id==id) return cs;
		cs = cs->next;
	}
	return NULL;
}


struct cardserver_data *getcsbyport(int32_t port)
{
	struct cardserver_data *cs = cfg.cardserver;
	while (cs) {
		if (cs->port==port) return cs;
		cs = cs->next;
	}
	return NULL;
}


struct cs_server_data *getsrvbyid(uint32_t id)
{
	if (!id) return NULL;
	struct cs_server_data *srv = cfg.server;
	while (srv) {
		if (srv->id==id) return srv;
		srv = srv->next;
	}
	return NULL;
}


///////////////////////////////////////////////////////////////////////////////
// Return
//  0: not accepted
//  1: accepted
int32_t accept_sid(struct cardserver_data *cs, uint16_t sid)
{
	int32_t i;
	// Check for Accepted sids
	if (!sid) {
		if (!cs->faccept0sid) return 0;
	}
	else {
		if (cs->sids) {
			if (!cs->isdeniedsids) {
				int32_t accepted = 0;
				for(i=0;i<MAX_SIDS;i++) {
					if (!cs->sids[i]) break; // end of sids
					else if (cs->sids[i]==sid) {
						accepted = 1;
						break;
					}
				}
				if (!accepted) return 0;
			}
			else {
				int32_t accepted = 1;
				for(i=0;i<MAX_SIDS;i++) {
					if (!cs->sids[i]) break; // end of sids
					else if (cs->sids[i]==sid) {
						accepted = 0;
						break;
					}
				}
				if (!accepted) return 0;
			}
		}
	}
	return 1;
}

int32_t accept_prov(struct cardserver_data *cs, uint32_t prov)
{
	int32_t i;
	// Check for provid, accept provid==0
	if (prov) {
		for (i=0; i<cs->card.nbprov;i++) if (prov==cs->card.prov[i]) break;
		if (i>=cs->card.nbprov) return 0;
	}
	else {
		if (!cs->faccept0provider) return 0;
	}
	return 1;
}

int32_t accept_caid(struct cardserver_data *cs, uint16_t caid)
{
	// Check for caid, accept caid=0
	if (caid) {
		if (caid!=cs->card.caid) return 0;
	}
	else {
		if (!cs->faccept0caid) return 0;
	}
	return 1;
}

int32_t accept_ecmlen(int32_t ecmlen)
{
	if ( (ecmlen<20)||(ecmlen>300) ) return 0;
	return 1;
}

///////////////////////////////////////////////////////////////////////////////
void ecm_setdcw( struct cardserver_data *cs, ECM_DATA *ecm, uchar dcw[16], int32_t srctype, int32_t srcid);

#include "dcwfilter.c"

#include "pipe.c"

#include "clustredcache.c"

#include "cli-newcamd.c"
#ifdef CCCAM_CLI
#include "cli-cccam.c"
#endif

#include "srv-newcamd.c"
#ifdef MGCAMD_SRV
#include "srv-mgcamd.c"
#endif

#ifdef CCCAM_SRV
#include "srv-cccam.c"
#endif
#ifdef FREECCCAM_SRV
#include "srv-freecccam.c"
#endif

#ifdef RADEGAST_CLI
#include "cli-radegast.c"
#endif
#ifdef RADEGAST_SRV
#include "srv-radegast.c"
#endif


#include "th-srv.c"  // Servers Connnection
#include "th-dns.c"  // Dns Resolving
#include "th-ecm.c"  // Check/send ecm request to servers & Check/send dcw to clients
#include "th-cfg.c"  // Reread Config
#include "th-date.c"


///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////

pthread_t cli_tid;

int32_t checkthread;

unsigned int seed2;

uint8_t fastrnd2()
{
	unsigned int offset = 12923+(GetTickCount()&0xff);
	unsigned int multiplier = 4079+(GetTickCount()&0xff);
	seed2 = seed2 * multiplier + offset;
	return (uint8_t)(seed2 % 0xFF);
}

int32_t mainprocess()
{
	struct utsname info;
	gettimeofday( &startime, NULL );
	debugf("\n");
	debugf("Starting NewBox...\n");
	debugf("\n");
	debugf("\n");
	debugf("CardServer NewBox v%s, build %s ", VERSION, DATE_BUILD);
	gettimeofday( uname(&info), NULL );
	debugf("(%s-", info.sysname);
	//debugf("nodename = %s\n", info.nodename);
	//debugf("release = %s\n", info.release);
	//debugf("version = %s\n", info.version);
	debugf("%s).\n\n", info.machine);
	debugf("Copyright (C) 2014 - 2016 developed by Javilonas.\n");
	debugf("This program is distributed under GPLv3.\n");
	debugf("Source Code NewBox: https://github.com/javilonas/NewBox\n");
	debugf("Visit https://www.lonasdigital.com for more details.\n\n");

// INIT
	pthread_mutex_init(&prg.lock, NULL);
	pthread_mutex_init(&prg.lockecm, NULL);

	pthread_mutex_init(&prg.lockcli, NULL);
	pthread_mutex_init(&prg.locksrv, NULL);

#ifdef CCCAM_SRV
	pthread_mutex_init(&prg.locksrvcc, NULL); // CC Client connection
	pthread_mutex_init(&prg.lockcccli, NULL);
#endif
#ifdef FREECCCAM_SRV
	pthread_mutex_init(&prg.locksrvfreecc, NULL); // CC Client connection
	pthread_mutex_init(&prg.lockfreecccli, NULL);
#endif

#ifdef MGCAMD_SRV
	pthread_mutex_init(&prg.locksrvmg, NULL); // Client connection
	pthread_mutex_init(&prg.lockclimg, NULL);
#endif

#ifdef RADEGAST_SRV
	pthread_mutex_init(&prg.lockrdgdsrv, NULL); // Client connection
	pthread_mutex_init(&prg.lockrdgdcli, NULL);
#endif

	// Main Loops(THREADS)
	pthread_mutex_init(&prg.lockdnsth, NULL); // DNS lookup Thread

	pthread_mutex_init(&prg.locksrvth, NULL); // Connection to cardservers
	pthread_mutex_init(&prg.lockmain, NULL); // Messages Recv

	pthread_mutex_init(&prg.locksrvcs, NULL); // CS Client connection
	pthread_mutex_init(&prg.lockhttp, NULL); // HTTP Server

	pthread_mutex_init(&prg.lockdns, NULL);

	pthread_mutex_init(&prg.lockdcw, NULL);

	pthread_mutex_init(&prg.lockcache, NULL);

	pthread_mutex_init(&prg.lockthreaddate, NULL);

	gettimeofday( &prg.exectime, NULL );

	memset(&trace, 0, sizeof(struct trace_data) );

	/* Create the pipe. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, srvsocks) < 0) { perror("opening stream socket pair"); exit(1); }

#ifdef CCCAM
	cccam_nodeid[0] = 0x11;
	cccam_nodeid[1] = 0x22;
	cccam_nodeid[2] = 0x33;
	cccam_nodeid[3] = 0x44;
	cccam_nodeid[4] = 0xff & fastrnd2();
	cccam_nodeid[5] = 0xff & fastrnd2();
	cccam_nodeid[6] = 0xff & fastrnd2();
	cccam_nodeid[7] = 0xff & fastrnd2();
#endif

#ifdef FREECCCAM_SRV
	freecccam_nodeid[0] = 0x11;
	freecccam_nodeid[1] = 0x22;
	freecccam_nodeid[2] = 0x33;
	freecccam_nodeid[3] = 0x44;
	freecccam_nodeid[4] = 0xff & fastrnd2();
	freecccam_nodeid[5] = 0xff & fastrnd2();
	freecccam_nodeid[6] = 0xff & fastrnd2();
	freecccam_nodeid[7] = 0xff & fastrnd2();
#endif

	init_config(&cfg);
	read_config(&cfg);

	//check for config data
	if ( check_config(&cfg) ) return -1;

	read_chinfo(&prg);

	read_badcw(&prg);

	init_ecmdata();

// THREADS - detached
	start_thread_dns();
	sleep(3);
	start_thread_srv();
	start_thread_config();
	start_thread_date();
#ifdef HTTP_SRV
	start_thread_http();
#endif
	start_thread_recv_msg();
	sleep(5); // wait for dns & servers connections

	start_thread_cache();

	create_prio_thread(&cli_tid, (threadfn)cs_connect_cli_thread, NULL, 50);
#ifdef RADEGAST_SRV
	pthread_t rdgd_cli_tid;
	create_prio_thread(&rdgd_cli_tid, (threadfn)rdgd_connect_cli_thread, NULL, 50); // Lock server
#endif
#ifdef CCCAM_SRV
	pthread_t cc_cli_tid;
	create_prio_thread(&cc_cli_tid, (threadfn)cc_connect_cli_thread, NULL, 50); // Lock server
#endif
#ifdef FREECCCAM_SRV
	pthread_t freecc_cli_tid;
	create_prio_thread(&freecc_cli_tid, (threadfn)freecc_connect_cli_thread, NULL, 30); // Lock server
#endif
#ifdef MGCAMD_SRV
	start_thread_mgcamd();
#endif
	return 0;
}


#ifdef SIG_HANDLER

#include <execinfo.h>
#include <ucontext.h>

#ifdef REG_EIP
#define INDEX_IP REG_EIP
#else
#ifdef REG_RIP
#define INDEX_IP REG_RIP
#else
#ifdef PT_NIP
#define INDEX_IP PT_NIP
#endif
#endif
#endif

void sighandler(int32_t sig, siginfo_t *info, void *secret) {

	ucontext_t *uc = (ucontext_t *)secret;
	printf(" Got signal %d ", sig);
	printf(" faulty address is %p ", info->si_addr);
#ifdef PT_NIP
	printf(" from 0x%016X\n", uc->uc_mcontext.uc_regs->gregs[PT_NIP]);
#else
	printf(" from 0x%016X\n", uc->uc_mcontext.gregs[INDEX_IP]);
#endif

	fdebugf(" Got signal %d ", info->si_signo);
	fdebugf(" faulty address is %p ", info->si_addr);
#ifdef PT_NIP
	fdebugf(" from 0x%016X\n", uc->uc_mcontext.uc_regs->gregs[PT_NIP]);
#else
	fdebugf(" from 0x%016X\n", uc->uc_mcontext.gregs[INDEX_IP]);
#endif

	exit(0);
}

void install_handler (void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sigaction));
	sa.sa_sigaction = (void *)sighandler;
	sigemptyset (&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
}
#endif

int32_t main(int32_t argc, char *argv[])
{
	int32_t option_background = 0; // default
	int32_t fork_return;
	char *args;
	int32_t i,j;

#ifdef SIG_HANDLER
	install_handler();
#endif

	flag_debugscr = 0;
	flag_debugfile = 0;
	flag_debugnet = 0;

	// Extract filename
	char *p = argv[0];
	char *slash = p;
	char *dot = NULL;
	while (*p) {
		if (*p=='/') slash = p+1;
		else if (*p=='.') dot = p;
		p++;
	}
	char path[255];
	if (dot>slash) memcpy( path, slash, dot-slash); else strcpy(path, slash);
	// Set Config name
	sprintf( config_file, "/var/etc/%s.cfg", path);
	sprintf( config_channelinfo, "/var/etc/%s.channelinfo", path);
	sprintf( config_badcw, "/var/etc/%s.cfg", path);
//	sprintf( sid_file, "/var/etc/%s.sid", path);
//	sprintf( card_file, "/var/etc/%s.card", path);
	sprintf( debug_file, "/var/tmp/%s.log", path);
	sprintf( sms_file, "/var/tmp/%s.sms", path);

	// Parse Options
	for (i=1;i<argc;i++) {
		args = *(argv+i);
		if (args[0]=='-') {
			if (args[1]=='h') {
				printf("USAGE\n\tnewbox [-b] [-v] [-f] [-n] [-C <configfile>]\n\
OPTIONS\n\
\t-b               run in background\n\
\t-C <configfile>  use <configfile> instead of default config file (/var/etc/newbox.cfg)\n\
\t-f               write to log file (/var/tmp/newbox.log)\n\
\t-n               print network packets\n\
\t-v               print on screen\n\
\t-h               this help message\n");
				return 0;
			}
			else if (args[1]=='C') {
				i++;
				if (i<argc) {
					args = *(argv+i);
					strcpy( config_file, args );
				}
			}
			else {
				for(j=1; j<strlen(args); j++) {
					if (args[j]=='b') option_background = 1;
					else if (args[j]=='v') flag_debugscr = 1;
					else if (args[j]=='n') flag_debugnet = 1;
					else if (args[j]=='f') flag_debugfile = 1;
				}
			}
		}
	}

	if (option_background==1) {
		fork_return = fork();
		if( fork_return < 0) {
			debugf(" unable to create child process, exiting.\n");
			exit(-1);
		}
		if (fork_return>0) {
			//debugf(" main process, exiting.\n");
			exit(0);
		}
		//else mainprocess();
	}

	prg.pid_main = getpid();

	mainprocess();

	sleep(1);

	prg.restart = 0;
	while (1) {
			if (prg.restart==1) { // restart()
				debugf(" Restarting '%s'...\n", argv[0]);
				//TODO:stop threads
				int32_t fork_return;
				done_config(&cfg);
				fork_return = vfork();
				if( fork_return < 0) {
					printf("unable to create child process, exiting.\n");
				}
				else if (fork_return==0) {
					fork_return = vfork();
					if (fork_return < 0) {
						printf("unable to create child process, exiting.\n");
					}
					else if (fork_return==0) {
						execvp( argv[0], argv );
						perror("execvp");
					}
					exit(0);
				}
				debugf(" NewBox stopped.\n");
				exit(0);
			}
			sleep(2);
	}
	return 0;
}

