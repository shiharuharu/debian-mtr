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
*/

/* Typedefs */

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long dword;

typedef unsigned int ip_t;



/*  Prototypes for dns.c  */

void dns_open();
int dns_waitfd();
void dns_ack();
void dns_events(double *sinterval);
char *dns_lookup(ip_t address);
char *dns_lookup2(ip_t address);
int dns_forward(char *name);
char *strlongip(ip_t ip);

extern int dns;
