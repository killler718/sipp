/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Author : Guillaume TEISSIER from FTR&D 02/02/2006
 */
#include <pcap.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#ifdef __HPUX
#include <netinet/in_systm.h>
#endif
#include <netinet/ip.h>
#include <netinet/ip6.h>
#ifdef __HPUX
#include <sys/socket.h>
#include <net/if_arp.h>
#include <netinet/if_ether.h>
#else
#include <net/ethernet.h>
#endif
#include <string.h>

#include "prepare_pcap.h"
#include "screen.hpp"

#ifdef __HPUX
int check(u_int16_t *buffer, int len){
#else
inline int check(u_int16_t *buffer, int len){
#endif
  int sum;
  int i;
  sum = 0;

  for (i=0; i<(len&~1); i+= 2)
    sum += *buffer++;

  if (len & 1) {
    sum += htons( (*(const u_int8_t *)buffer) << 8);
  }
  return sum;
}

#ifdef __HPUX
u_int16_t checksum_carry(int s) {
#else
inline u_int16_t checksum_carry(int s) {
#endif
	int s_c = (s >> 16) + (s & 0xffff);
	return (~(s_c + (s_c >> 16)) & 0xffff);
}

char errbuf[PCAP_ERRBUF_SIZE];

/* prepare a pcap file
 */
int prepare_pkts(char *file, pcap_pkts *pkts) {
  pcap_t *pcap;
  struct pcap_pkthdr *pkthdr = NULL;
  u_char *pktdata = NULL;
  int n_pkts = 0;
  u_long max_length = 0;
  u_int16_t base = 0xffff;
  u_int16_t *chk_buffer;
  u_long pktlen;
  pcap_pkt *pkt_index;
  struct ether_header *ethhdr;
  struct iphdr *iphdr;
  struct ip6_hdr *ip6hdr;
  struct udphdr *udphdr;

  // to be suppressed
  int check_s = 0;
  int diff;

  pkts->pkts = NULL;

  pcap = pcap_open_offline(file, errbuf);
  if (!pcap) 
    ERROR_P1("Can't open PCAP file '%s'", file);

#if HAVE_PCAP_NEXT_EX
  while (pcap_next_ex (pcap, &pkthdr, (const u_char **) &pktdata) == 1)
  {
#else
#ifdef __HPUX
  pkthdr = (pcap_pkthdr *) malloc (sizeof (*pkthdr));
#else
  pkthdr = malloc (sizeof (*pkthdr));
#endif
  if (!pkthdr)
    ERROR("Can't allocate memory for pcap pkthdr");
  while ((pktdata = (u_char *) pcap_next (pcap, pkthdr)) != NULL)
  {
#endif
    ethhdr = (struct ether_header *)pktdata;
    if (ntohs(ethhdr->ether_type) != ETHERTYPE_IP
          && ntohs(ethhdr->ether_type) != 0x86dd) {
      fprintf(stderr, "Ignoring non IP{4,6} packet!\n");
      continue;
    }
    iphdr = (struct iphdr *)((char *)ethhdr + sizeof(*ethhdr));
    if (iphdr && iphdr->version == 6) {
      //ipv6
      pktlen = (u_long) pkthdr->len - sizeof(*ethhdr) - sizeof(*ip6hdr);
      ip6hdr = (struct ip6_hdr *)(void *) iphdr;
      if (ip6hdr->ip6_nxt != IPPROTO_UDP) {
        fprintf(stderr, "prepare_pcap.c: Ignoring non UDP packet!\n");
	     continue;
      }
      udphdr = (struct udphdr *)((char *)ip6hdr + sizeof(*ip6hdr));
    } else {
      //ipv4
      pktlen = (u_long) pkthdr->len - sizeof(*ethhdr) - sizeof(*iphdr);
      if (iphdr->protocol != IPPROTO_UDP) {
        fprintf(stderr, "prepare_pcap.c: Ignoring non UDP packet!\n");
        continue;
      }
#ifdef __DARWIN
      udphdr = (struct udphdr *)((char *)iphdr + (iphdr->ihl << 2) + 4);
#else
      udphdr = (struct udphdr *)((char *)iphdr + (iphdr->ihl << 2));
#endif
    }
    if (pktlen > PCAP_MAXPACKET) {
      ERROR("Packet size is too big! Recompile with bigger PCAP_MAXPACKET in prepare_pcap.h");
    }
    pkts->pkts = (pcap_pkt *) realloc(pkts->pkts, sizeof(*(pkts->pkts)) * (n_pkts + 1));
    if (!pkts->pkts)
      ERROR("Can't re-allocate memory for pcap pkt");
    pkt_index = pkts->pkts + n_pkts;
    pkt_index->pktlen = pktlen;
    pkt_index->ts = pkthdr->ts;
    pkt_index->data = (unsigned char *) malloc(pktlen);
    if (!pkt_index->data) 
      ERROR("Can't allocate memory for pcap pkt data");
    memcpy(pkt_index->data, udphdr, pktlen);

#if defined(__HPUX) || defined(__DARWIN)
    udphdr->uh_sum = 0 ;      
#else
    udphdr->check = 0;
#endif

      // compute a partial udp checksum
      // not including port that will be changed
      // when sending RTP
#if defined(__HPUX) || defined(__DARWIN)
    pkt_index->partial_check = check((u_int16_t *) &udphdr->uh_ulen, pktlen - 4) + ntohs(IPPROTO_UDP + pktlen);
#else
    pkt_index->partial_check = check((u_int16_t *) &udphdr->len, pktlen - 4) + ntohs(IPPROTO_UDP + pktlen);
#endif
    if (max_length < pktlen)
      max_length = pktlen;
#if defined(__HPUX) || defined(__DARWIN)
    if (base > ntohs(udphdr->uh_dport))
      base = ntohs(udphdr->uh_dport);
#else
    if (base > ntohs(udphdr->dest))
      base = ntohs(udphdr->dest);
#endif
    n_pkts++;
  }
  pkts->max = pkts->pkts + n_pkts;
  pkts->max_length = max_length;
  pkts->base = base;
  fprintf(stderr, "In pcap %s, npkts %d\nmax pkt length %d\nbase port %d\n", file, n_pkts, max_length, base);
  pcap_close(pcap);

  return 0;
}

void free_pkts(pcap_pkts *pkts) {
  pcap_pkt *pkt_index;
  while (pkt_index < pkts->max) {
    free(pkt_index->data);
  }
  free(pkts->pkts);
}