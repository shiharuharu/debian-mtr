/*
    mtr  --  a network diagnostic tool
    Copyright (C) 1997,1998  Matt Kimball

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
    
   1999-08-13 ok Olav@okvittem.priv.no  added -psize

*/

#include <config.h>

#if defined(HAVE_SYS_XTI_H)
#include <sys/xti.h>
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <memory.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <string.h>

#include "mtr.h"
#include "net.h"
#include "display.h"
#include "dns.h"

/*  We can't rely on header files to provide this information, because
    the fields have different names between, for instance, Linux and 
    Solaris  */
struct ICMPHeader {
  uint8 type;
  uint8 code;
  uint16 checksum;
  uint16 id;
  uint16 sequence;
};


/*  Structure of an IP header.  */
struct IPHeader {
  uint8 version;
  uint8 tos;
  uint16 len;
  uint16 id;
  uint16 frag;
  uint8 ttl;
  uint8 protocol;
  uint16 check;
  uint32 saddr;
  uint32 daddr;
};
  

#define ICMP_ECHO		8
#define ICMP_ECHOREPLY		0

#define ICMP_TSTAMP		13
#define ICMP_TSTAMPREPLY	14

#define ICMP_TIME_EXCEEDED	11

#ifndef SOL_IP
#define SOL_IP 0
#endif

struct nethost {
  ip_t addr;
  ip_t addrs[MAXPATH];	/* for multi paths byMin */
  int xmit;
  int returned;
  int sent;
  int up;
  long long var;/* variance, could be overflowed */
  int last;
  int best;
  int worst;
  int avg;	/* average:  addByMin */
  int gmean;	/* geometirc mean: addByMin */
  int jitter;	/* current jitter, defined as t1-t0 addByMin */
//int jbest;	/* min jitter, of cause it is 0, not needed */
  int javg;	/* avg jitter */
  int jworst;	/* max jitter */
  int jinta;	/* estimated variance,? rfc1889's "Interarrival Jitter" */
  int transit;
  int saved[SAVED_PINGS];
  int saved_seq_offset;
};


struct sequence {
  int index;
  int transit;
  int saved_seq;
  struct timeval time;
};


/* Configuration parameter: How many queries to unknown hosts do we
   send? (This limits the amount of traffic generated if a host is not
   reachable) -- REW */
#define MAX_UNKNOWN_HOSTS 5


/* There is something stupid with BSD. We now detect this automatically */
static int BSDfix = 0;
#define saddr_correction(addr) BSDfix ? addr : 0

static struct nethost host[MaxHost];
static struct sequence sequence[MaxSequence];
static struct timeval reset = { 0, 0 };

int    timestamp;
int    sendsock4;
int    recvsock4;
int    sendsock6;
int    recvsock6;
int    sendsock;
int    recvsock;

#ifdef ENABLE_IPV6
struct sockaddr_storage sourcesockaddr_struct;
struct sockaddr_storage remotesockaddr_struct;
struct sockaddr_in6 * ssa6 = (struct sockaddr_in6 *) &sourcesockaddr_struct;
struct sockaddr_in6 * rsa6 = (struct sockaddr_in6 *) &remotesockaddr_struct;
#else
struct sockaddr_in sourcesockaddr_struct;
struct sockaddr_in remotesockaddr_struct;
#endif

struct sockaddr * sourcesockaddr = (struct sockaddr *) &sourcesockaddr_struct;
struct sockaddr * remotesockaddr = (struct sockaddr *) &remotesockaddr_struct;
struct sockaddr_in * ssa4 = (struct sockaddr_in *) &sourcesockaddr_struct;
struct sockaddr_in * rsa4 = (struct sockaddr_in *) &remotesockaddr_struct;

ip_t * sourceaddress;
ip_t * remoteaddress;

/* XXX How do I code this to be IPV6 compatible??? -- REW */
#ifdef ENABLE_IPV6
char localaddr[INET6_ADDRSTRLEN];
#else
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
char localaddr[INET_ADDRSTRLEN];
#endif

static int batch_at = 0;
static int numhosts = 10;

extern int fstTTL;		/* initial hub(ttl) to ping byMin */
extern int maxTTL;		/* last hub to ping byMin*/
extern int packetsize;		/* packet size used by ping */
extern int bitpattern;		/* packet bit pattern used by ping */
extern int tos;			/* type of service set in ping packet*/
extern int af;			/* address family of remote target */


/* return the number of microseconds to wait before sending the next
   ping */
int calc_deltatime (float waittime)
{
  waittime /= numhosts;
  return 1000000 * waittime;
}


/* This doesn't work for odd sz. I don't know enough about this to say
   that this is wrong. It doesn't seem to cripple mtr though. -- REW */
int checksum(void *data, int sz) 
{
  unsigned short *ch;
  unsigned int sum;

  sum = 0;
  ch = data;
  sz = sz / 2;
  while (sz--) {
    sum += *(ch++);
  }
  
  sum = (sum >> 16) + (sum & 0xffff);  

  return (~sum & 0xffff);  
}


int new_sequence(int index) 
{
  static int next_sequence = 0;
  int seq;

  seq = next_sequence++;
  if (next_sequence >= MaxSequence)
    next_sequence = 0;

  sequence[seq].index = index;
  sequence[seq].transit = 1;
  sequence[seq].saved_seq = ++host[index].xmit;
  memset(&sequence[seq].time, 0, sizeof(sequence[seq].time));
  
  host[index].transit = 1;
  if (host[index].sent)
    host[index].up = 0;
  host[index].sent = 1;
  net_save_xmit(index);
  
  return seq;
}


/*  Attempt to find the host at a particular number of hops away  */
void net_send_query(int index) 
{
  /*ok  char packet[sizeof(struct IPHeader) + sizeof(struct ICMPHeader)];*/
  char packet[MAXPACKET];
  struct IPHeader *ip = (struct IPHeader *) packet;
  struct ICMPHeader *icmp;

  /*ok  int packetsize = sizeof(struct IPHeader) + sizeof(struct ICMPHeader) + datasize;*/
  int rv;
  static int first=1;
  int ttl, iphsize = 0, echotype = 0, salen = 0;

  ttl = index + 1;

  if ( packetsize < MINPACKET ) packetsize = MINPACKET;
  if ( packetsize > MAXPACKET ) packetsize = MAXPACKET;

  memset(packet, (unsigned char) abs(bitpattern), abs(packetsize));

  switch ( af ) {
  case AF_INET:
    iphsize = sizeof (struct IPHeader);

  ip->version = 0x45;
  ip->tos = tos;
  ip->len = BSDfix ? abs(packetsize): htons (abs(packetsize));
  ip->id = 0;
  ip->frag = 0;    /* 1, if want to find mtu size? Min */
    ip->ttl = ttl;
  ip->protocol = IPPROTO_ICMP;
  ip->check = 0;

  /* BSD needs the source address here, Linux & others do not... */
    addrcpy( (void *) &(ip->saddr), (void *) &(ssa4->sin_addr), AF_INET );
    addrcpy( (void *) &(ip->daddr), (void *) remoteaddress, AF_INET );

    echotype = ICMP_ECHO;
    salen = sizeof (struct sockaddr_in);
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    iphsize = 0;
    if ( setsockopt( sendsock, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
                     &ttl, sizeof ttl ) ) {
      perror( "setsockopt IPV6_UNICAST_HOPS" );
      exit( EXIT_FAILURE);
    }
    echotype = ICMP6_ECHO_REQUEST;
    salen = sizeof (struct sockaddr_storage);
    break;
#endif
  }

  icmp = (struct ICMPHeader *)(packet + iphsize);
  icmp->type     = echotype;
  icmp->code     = 0;
  icmp->checksum = 0;
  icmp->id       = getpid();
  icmp->sequence = new_sequence(index);
  icmp->checksum = checksum(icmp, abs(packetsize) - iphsize);

  switch ( af ) {
  case AF_INET:
    ip->check = checksum(packet, abs(packetsize));
    break;
  }

  gettimeofday(&sequence[icmp->sequence].time, NULL);

  rv = sendto(sendsock, packet, abs(packetsize), 0, 
	      remotesockaddr, salen);
  if (first && (rv < 0) && (errno == EINVAL)) {
    ip->len = abs (packetsize);
    rv = sendto(sendsock, packet, abs(packetsize), 0, 
		remotesockaddr, salen);
    if (rv >= 0) {
      fprintf (stderr, "You've got a broken (FreeBSD?) system\n");
      BSDfix = 1;
    }
  }
  first = 0;
}


/*   We got a return on something we sent out.  Record the address and
     time.  */
void net_process_ping(int seq, void * addr, struct timeval now) 
{
  int index;
  int totusec;
  int oldavg;	/* usedByMin */
  int oldjavg;	/* usedByMin */
  int i;	/* usedByMin */
#ifdef ENABLE_IPV6
  char addrcopy[sizeof(struct in6_addr)];
#else
  char addrcopy[sizeof(struct in_addr)];
#endif

  /* Copy the from address ASAP because it can be overwritten */
  addrcpy( (void *) &addrcopy, addr, af );
  
  if (seq < 0 || seq >= MaxSequence)
    return;

  if (!sequence[seq].transit)
    return;
  sequence[seq].transit = 0;

  index = sequence[seq].index;

  totusec = (now.tv_sec  - sequence[seq].time.tv_sec ) * 1000000 +
            (now.tv_usec - sequence[seq].time.tv_usec);
  /* impossible? if( totusec < 0 ) totusec = 0 */;

  if ( addrcmp( (void *) &(host[index].addr),
		(void *) &unspec_addr, af ) == 0 ) {
    // should be out of if as addr can change
    addrcpy( (void *) &(host[index].addr), addrcopy, af );
    display_rawhost(index, (void *) &(host[index].addr));

  /* multi paths by Min */
    addrcpy( (void *) &(host[index].addrs[0]), addrcopy, af );
  } else {
    for( i=0; i<MAXPATH; ) {
      if( addrcmp( (void *) &(host[index].addrs[i]), (void *) &addrcopy,
                   af ) == 0 ||
          addrcmp( (void *) &(host[index].addrs[i]),
		   (void *) &unspec_addr, af ) == 0 ) break;
      i++;
    }
    if( addrcmp( (void *) &(host[index].addrs[i]), addrcopy, af ) != 0 && 
        i<MAXPATH ) {
      addrcpy( (void *) &(host[index].addrs[i]), addrcopy, af );
    }
  /* end multi paths */
  }

  host[index].jitter = totusec - host[index].last;
  if (host[index].jitter < 0 ) host[index].jitter = - host[index].jitter;
  host[index].last = totusec;

  if (host[index].returned < 1) {
    host[index].best = host[index].worst = host[index].gmean = totusec;
    host[index].avg  = host[index].var  = 0;

    host[index].jitter = host[index].jworst = host[index].jinta= 0;
  }

  /* some time best can be too good to be true, experienced 
   * at least in linux 2.4.x.
   *  safe guard 1) best[index]>=best[index-1] if index>0
   *             2) best >= average-20,000 usec (good number?)
   *  Min
  if (index > 0) {
    if (totusec < host[index].best &&
       totusec>= host[index-1].best) host[index].best  = totusec;
  } else {
    if(totusec < host[index].best) host[index].best  = totusec;
  }
   */
  if (totusec < host[index].best ) host[index].best  = totusec;
  if (totusec > host[index].worst) host[index].worst = totusec;

  if (host[index].jitter > host[index].jworst)
	host[index].jworst = host[index].jitter;

  host[index].returned++;
  /* begin addByMin do more stats */
  oldavg = host[index].avg;
  host[index].avg += (totusec - oldavg +.0) / host[index].returned;
  host[index].var += (totusec - oldavg +.0) * (totusec - host[index].avg);

  oldjavg = host[index].javg;
  host[index].javg += (host[index].jitter - oldjavg) / host[index].returned;
  /* below algorithm is from rfc1889, A.8 */
  host[index].jinta += host[index].jitter - ((host[index].jinta + 8) >> 4);

  if ( host[index].returned > 1 )
  host[index].gmean = pow( (double) host[index].gmean, (host[index].returned-1.0)/host[index].returned )
			* pow( (double) totusec, 1.0/host[index].returned );
  /* end addByMin*/
  host[index].sent = 0;
  host[index].up = 1;
  host[index].transit = 0;

  net_save_return(index, sequence[seq].saved_seq, totusec);
  display_rawping(index, totusec);
}


/*  We know a packet has come in, because the main select loop has called us,
    now we just need to read it, see if it is for us, and if it is a reply 
    to something we sent, then call net_process_ping()  */
void net_process_return(void) 
{
  char packet[MAXPACKET];
#ifdef ENABLE_IPV6
  struct sockaddr_storage fromsockaddr_struct;
  struct sockaddr_in6 * fsa6 = (struct sockaddr_in6 *) &fromsockaddr_struct;
#else
  struct sockaddr_in fromsockaddr_struct;
#endif
  struct sockaddr * fromsockaddr = (struct sockaddr *) &fromsockaddr_struct;
  struct sockaddr_in * fsa4 = (struct sockaddr_in *) &fromsockaddr_struct;
  int fromsockaddrsize;
  int num;
  struct ICMPHeader *header = NULL;
  struct timeval now;
  ip_t * fromaddress = NULL;
  int echoreplytype = 0, timeexceededtype = 0;

  gettimeofday(&now, NULL);
  switch ( af ) {
  case AF_INET:
    fromsockaddrsize = sizeof (struct sockaddr_in);
    fromaddress = (ip_t *) &(fsa4->sin_addr);
    echoreplytype = ICMP_ECHOREPLY;
    timeexceededtype = ICMP_TIME_EXCEEDED;
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    fromsockaddrsize = sizeof (struct sockaddr_storage);
    fromaddress = (ip_t *) &(fsa6->sin6_addr);
    echoreplytype = ICMP6_ECHO_REPLY;
    timeexceededtype = ICMP6_TIME_EXCEEDED;
    break;
#endif
  }

  num = recvfrom(recvsock, packet, MAXPACKET, 0, 
		 fromsockaddr, &fromsockaddrsize);

  switch ( af ) {
  case AF_INET:
    if((size_t) num < sizeof(struct IPHeader) + sizeof(struct ICMPHeader))
      return;
    header = (struct ICMPHeader *)(packet + sizeof(struct IPHeader));
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    if(num < sizeof(struct ICMPHeader))
      return;

    header = (struct ICMPHeader *) packet;
    break;
#endif
  }
  if (header->type == echoreplytype) {
    if(header->id != (uint16)getpid())
      return;

    net_process_ping (header->sequence, (void *) fromaddress, now);
  } else if (header->type == timeexceededtype) {
    switch ( af ) {
    case AF_INET:

      if ((size_t) num < sizeof(struct IPHeader) + 
                         sizeof(struct ICMPHeader) + 
                         sizeof (struct IPHeader) + 
                         sizeof (struct ICMPHeader))
        return;
      header = (struct ICMPHeader *)(packet + sizeof (struct IPHeader) + 
                                              sizeof (struct ICMPHeader) + 
                                              sizeof (struct IPHeader));
    break;
#ifdef ENABLE_IPV6
    case AF_INET6:
      if ( num < sizeof (struct ICMPHeader) + 
                 sizeof (struct ip6_hdr) + sizeof (struct ICMPHeader) )
        return;
      header = (struct ICMPHeader *) ( packet + 
                                       sizeof (struct ICMPHeader) +
                                       sizeof (struct ip6_hdr) );
      break;
#endif
    }

    if (header->id != (uint16)getpid())
      return;

    net_process_ping(header->sequence, (void *)fromaddress, now);
  }
}


ip_t *net_addr(int at) 
{
  return (ip_t *)&(host[at].addr);
}


ip_t *net_addrs(int at, int i) 
{
  return (ip_t *)&(host[at].addrs[i]);
}


int net_loss(int at) 
{
  if ((host[at].xmit - host[at].transit) == 0) 
    return 0;
  /* times extra 1000 */
  return 1000*(100 - (100.0 * host[at].returned / (host[at].xmit - host[at].transit)) );
}


int net_drop(int at) 
{
  return (host[at].xmit - host[at].transit) - host[at].returned;
}


int net_last(int at) 
{
  return (host[at].last);
}


int net_best(int at) 
{
  return (host[at].best);
}


int net_worst(int at) 
{
  return (host[at].worst);
}


int net_avg(int at) 
{
  return (host[at].avg);
}


int net_gmean(int at) 
{
  return (host[at].gmean);
}


int net_stdev(int at) 
{
  if( host[at].returned > 1 ) {
    return ( sqrt( host[at].var/(host[at].returned -1.0) ) );
  } else {
    return( 0 );
  }
}


/* jitter stuff */
int net_jitter(int at) 
{ 
  return (host[at].jitter); 
}


int net_jworst(int at) 
{ 
  return (host[at].jworst); 
}


int net_javg(int at) 
{ 
  return (host[at].javg); 
}


int net_jinta(int at) 
{ 
  return (host[at].jinta); 
}
/* end jitter */


int net_max(void) 
{
  int at;
  int max;

  max = 0;
  // replacedByMin
  // for(at = 0; at < MaxHost-2; at++) {
  for(at = 0; at < maxTTL-1; at++) {
    if ( addrcmp( (void *) &(host[at].addr),
                  (void *) remoteaddress, af ) == 0 ) {
      return at + 1;
    } else if ( addrcmp( (void *) &(host[at].addr),
			 (void *) &unspec_addr, af ) != 0 ) {
      max = at + 2;
    }
  }

  return max;
}


/* add by Min (wonder its named net_min;-)) because of ttl stuff */
int net_min (void) 
{
  return ( fstTTL - 1 );
}


/* Added by Brian Casey December 1997 bcasey@imagiware.com*/
int net_returned(int at) 
{ 
  return host[at].returned;
}


int net_xmit(int at) 
{ 
  return host[at].xmit;
}


int net_transit(int at) 
{ 
  return host[at].transit;
}


int net_up(int at) 
{
   return host[at].up;
}


char * net_localaddr (void)
{
  return localaddr;
}


void net_end_transit(void) 
{
  int at;
  
  for(at = 0; at < MaxHost; at++) {
    host[at].transit = 0;
  }
}

int net_send_batch(void) 
{
  int n_unknown=0, i;

  /* randomized packet size and/or bit pattern if packetsize<0 and/or 
     bitpattern<0.  abs(packetsize) and/or abs(bitpattern) will be used 
  */
  if( batch_at < fstTTL ) {
    if( packetsize < 0 ) {
      packetsize = 
	- (int)(MINPACKET + (MAXPACKET-MINPACKET)*(rand()/(RAND_MAX+0.1)));
    }
    if( bitpattern < 0 ) {
      bitpattern = - (int)(256 + 255*(rand()/(RAND_MAX+0.1)));
    }
  }

  net_send_query(batch_at);

  for (i=fstTTL-1;i<batch_at;i++) {
    if ( addrcmp( (void *) &(host[i].addr), (void *) &unspec_addr, af ) == 0 )
      n_unknown++;

    /* The second condition in the next "if" statement was added in mtr-0.56, 
	but I don't remember why. It makes mtr stop skipping sections of unknown
	hosts. Removed in 0.65. 
	If the line proves neccesary, it should at least NOT trigger that line 
	when host[i].addr == 0 -- REW */
    if ( ( addrcmp( (void *) &(host[i].addr),
                    (void *) remoteaddress, af ) == 0 )
	/* || (host[i].addr == host[batch_at].addr)  */)
      n_unknown = MaxHost; /* Make sure we drop into "we should restart" */
  }

  if (	// success in reaching target
     ( addrcmp( (void *) &(host[batch_at].addr),
                (void *) remoteaddress, af ) == 0 ) ||
      // fail in consecuitive MAX_UNKNOWN_HOSTS (firewall?)
      (n_unknown > MAX_UNKNOWN_HOSTS) ||
      // or reach limit 
      (batch_at >= maxTTL-1)) {
    numhosts = batch_at+1;
    batch_at = fstTTL - 1;
    return 1;
  }

  batch_at++;
  return 0;
}


int net_preopen(void) 
{
  int trueopt = 1;

  sendsock4 = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sendsock4 < 0) 
    return -1;
#ifdef ENABLE_IPV6
  sendsock6 = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
#endif

#ifdef IP_HDRINCL
  /*  FreeBSD wants this to avoid sending out packets with protocol type RAW
      to the network.  */
  if (setsockopt(sendsock4, SOL_IP, IP_HDRINCL, &trueopt, sizeof(trueopt))) {
    perror("setsockopt(IP_HDRINCL,1)");
    return -1;
  }
#endif /* IP_HDRINCL */

  recvsock4 = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (recvsock4 < 0)
    return -1;
#ifdef ENABLE_IPV6
  recvsock6 = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
#endif

  return 0;
}

 
int net_open(struct hostent * host) 
{
#ifdef ENABLE_IPV6
  struct sockaddr_storage name_struct;
#else
  struct sockaddr_in name_struct; 
#endif
  struct sockaddr * name = (struct sockaddr *) &name_struct;
  int len; 

  net_reset();

  remotesockaddr->sa_family = host->h_addrtype;

  switch ( host->h_addrtype ) {
  case AF_INET:
    sendsock = sendsock4;
    recvsock = recvsock4;
    addrcpy( (void *) &(rsa4->sin_addr), host->h_addr, AF_INET );
    sourceaddress = (ip_t *) &(ssa4->sin_addr);
    remoteaddress = (ip_t *) &(rsa4->sin_addr);
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    if (sendsock6 < 0 || recvsock6 < 0) {
      fprintf( stderr, "Could not open IPv6 socket\n" );
      exit( EXIT_FAILURE );
    }
    sendsock = sendsock6;
    recvsock = recvsock6;
    addrcpy( (void *) &(rsa6->sin6_addr), host->h_addr, AF_INET6 );
    sourceaddress = (ip_t *) &(ssa6->sin6_addr);
    remoteaddress = (ip_t *) &(rsa6->sin6_addr);
    break;
#endif
  default:
    fprintf( stderr, "net_open bad address type\n" );
    exit( EXIT_FAILURE );
  }

  len = sizeof name_struct; 
  getsockname (recvsock, name, &len);
  sockaddrtop( name, localaddr, sizeof localaddr );
#if 0
  printf ("got localaddr: %s\n", localaddr); 
#endif

  return 0;
}


void net_reopen(struct hostent * addr) 
{
  int at;

  for(at = 0; at < MaxHost; at++) {
    memset(&host[at], 0, sizeof(host[at]));
  }

  remotesockaddr->sa_family = addr->h_addrtype;

  switch ( addr->h_addrtype ) {
  case AF_INET:
    addrcpy( (void *) remoteaddress, addr->h_addr, AF_INET );
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    addrcpy( (void *) &(rsa6->sin6_addr), addr->h_addr, AF_INET6 );
    break;
#endif
  default:
    fprintf( stderr, "net_reopen bad address type\n" );
    exit( EXIT_FAILURE );
  }

  net_reset ();
  net_send_batch();
}


void net_reset(void) 
{
  int at;
  int i;

  batch_at = fstTTL - 1;	/* above replacedByMin */
  numhosts = 10;

  for (at = 0; at < MaxHost; at++) {
    host[at].xmit = 0;
    host[at].transit = 0;
    host[at].returned = 0;
    host[at].sent = 0;
    host[at].up = 0;
    host[at].last = 0;
    host[at].avg  = 0;
    host[at].best = 0;
    host[at].worst = 0;
    host[at].gmean = 0;
    host[at].var = 0;
    host[at].jitter = 0;
    host[at].javg = 0;
    host[at].jworst = 0;
    host[at].jinta = 0;
    for (i=0; i<SAVED_PINGS; i++) {
      host[at].saved[i] = -2;	/* unsent */
    }
    host[at].saved_seq_offset = -SAVED_PINGS+2;
  }
  
  for (at = 0; at < MaxSequence; at++) {
    sequence[at].transit = 0;
  }

  gettimeofday(&reset, NULL);
}


int net_set_interfaceaddress (char *InterfaceAddress)
{
  int i1, i2, i3, i4;
  char dummy;

  if (!InterfaceAddress) return 0; 

  sourcesockaddr->sa_family = af;
  switch ( af ) {
  case AF_INET:
    ssa4->sin_port = 0;
    ssa4->sin_addr.s_addr = 0;

  if(sscanf(InterfaceAddress, "%u.%u.%u.%u%c", &i1, &i2, &i3, &i4, &dummy) != 4) {
    printf("mtr: bad interface address: %s\n", InterfaceAddress);
    exit(1);
  }

    ((unsigned char*)&ssa4->sin_addr)[0] = i1;
    ((unsigned char*)&ssa4->sin_addr)[1] = i2;
    ((unsigned char*)&ssa4->sin_addr)[2] = i3;
    ((unsigned char*)&ssa4->sin_addr)[3] = i4;
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    ssa6->sin6_port = 0;
    inet_pton( af, InterfaceAddress, &(ssa6->sin6_addr) );
    break;
#endif
  }

  if (bind(sendsock, sourcesockaddr, sizeof sourcesockaddr_struct) == -1) {
    perror("mtr: failed to bind to interface");
    exit(1);
  }
  return 0; 
}



void net_close(void)
{
  if (sendsock4 >= 0) close(sendsock4);
  if (recvsock4 >= 0) close(recvsock4);
  if (sendsock6 >= 0) close(sendsock6);
  if (recvsock6 >= 0) close(recvsock6);
}


int net_waitfd(void)
{
  return recvsock;
}


int* net_saved_pings(int at)
{
  return host[at].saved;
}


void net_save_increment(void)
{
  int at;
  for (at = 0; at < MaxHost; at++) {
    memmove(host[at].saved, host[at].saved+1, (SAVED_PINGS-1)*sizeof(int));
    host[at].saved[SAVED_PINGS-1] = -2;
    host[at].saved_seq_offset += 1;
  }
}


void net_save_xmit(int at)
{
  if (host[at].saved[SAVED_PINGS-1] != -2) 
    net_save_increment();
  host[at].saved[SAVED_PINGS-1] = -1;
}


void net_save_return(int at, int seq, int ms)
{
  int idx;
  idx = seq - host[at].saved_seq_offset;
  if (idx < 0 || idx > SAVED_PINGS) {
    return;
  }
  host[at].saved[idx] = ms;
}

/* Similar to inet_ntop but uses a sockaddr as it's argument. */
void sockaddrtop( struct sockaddr * saddr, char * strptr, size_t len ) {
  struct sockaddr_in *  sa4;
#ifdef ENABLE_IPV6
  struct sockaddr_in6 * sa6;
#endif

  switch ( saddr->sa_family ) {
  case AF_INET:
    sa4 = (struct sockaddr_in *) saddr;
    strncpy( strptr, inet_ntoa( (struct in_addr) sa4->sin_addr ), len );
    return;
#ifdef ENABLE_IPV6
  case AF_INET6:
    sa6 = (struct sockaddr_in6 *) saddr;
    inet_ntop( sa6->sin6_family, &(sa6->sin6_addr), strptr, len );
    return;
#endif
  default:
    fprintf( stderr, "sockaddrtop unknown address type\n" );
    strptr[0] = '\0';
    return;
  }
}

/* Address comparison. */
int addrcmp( char * a, char * b, int af ) {
  int rc = -1;

  switch ( af ) {
  case AF_INET:
    rc = memcmp( a, b, sizeof (struct in_addr) );
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    rc = memcmp( a, b, sizeof (struct in6_addr) );
    break;
#endif
  }

  return rc;
}

/* Address copy. */
void addrcpy( char * a, char * b, int af ) {

  switch ( af ) {
  case AF_INET:
    memcpy( a, b, sizeof (struct in_addr) );
    break;
#ifdef ENABLE_IPV6
  case AF_INET6:
    memcpy( a, b, sizeof (struct in6_addr) );
    break;
#endif
  }
}
