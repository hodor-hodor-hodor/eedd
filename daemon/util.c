/*
 * Name: util.c
 *
 * Description: This file contains FD read/write demultiplexing, timers and 
 *              the log utilities for the empty daemon.
 *
 * Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc.
 *              All rights reserved.
 *
 * License:     This program is free software; you can redistribute it and/or
 *              modify it under the terms of the Version 2 of the GNU General
 *              Public License as published by the Free Software Foundation.
 *              GPL2.txt in the top level directory is a copy of this license.
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *              GNU General Public License for more details.
 *
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <stdarg.h>      // for va_arg
#include <sys/time.h>    // for gettimeofday
#include "main.h"

/***************************************************************************
 *  - Defines
 ***************************************************************************/


/***************************************************************************
 *  - Statically allocated variables and arrays
 ***************************************************************************/
int      fdcount;      // Number open file descriptors
int      mxfd;         // Highest numbered FD
fd_set   gRfds;        // read FDs
fd_set   gWfds;        // write FDs
fd_set   gXfds;        // exception FDs
int      ntimers = 0;  // number of timers in use


/***************************************************************************
 *  - Forward references
 ***************************************************************************/
static void      update_fdsets(); // set fd_set before use by select()
struct timeval  *doTimer();
static long long tv2us(struct timeval *);

extern SLOT      Slots[];   // table of plug-in info
extern ED_FD     Ed_Fd[];   // Array of open FDs and callbacks
extern ED_TIMER  Timers[];  // Array of timers and callbacks
extern char     *CmdName;
extern int       UseStderr;



/***************************************************************************
 *  muxmain():  The main dpserver event loop.
 ***************************************************************************/
void muxmain()
{
    struct timeval *ptv;
    fd_set   readset;
    fd_set   writeset;
    fd_set   exceptset;
    ED_FD   *pin;
    int      sret;     // return value from select();
    int      activity; // type of select activity (read,write,except)
    int      i;

    update_fdsets();

    while (1) {
        // init the local fd sets from the global ones
        memcpy(&readset, &gRfds, sizeof(fd_set));
        memcpy(&writeset, &gWfds, sizeof(fd_set));
        memcpy(&exceptset, &gXfds, sizeof(fd_set));

        // Process timers
        ptv = doTimer();

        // wait for FD activity
        sret = select(mxfd + 1, &readset, &writeset, &exceptset, ptv);

        if (sret < 0) {
            // select error -- bail out on all but EINTR
            if (errno != EINTR) {
                edlog(strerror(errno));
                exit(-1);
            }
        }

        // Walk the table of FDs looking for read,write,except activity
        for (i = 0; i < MX_FD; i++) {
            pin = &Ed_Fd[i];
            if (pin < 0) {
                continue;
            }
            activity = 0;
            if (FD_ISSET(pin->fd, &readset)) {
                activity = ED_READ;
            }
            if (FD_ISSET(pin->fd, &writeset)) {
                activity |= ED_WRITE;
            }
            if (FD_ISSET(pin->fd, &exceptset)) {
                activity |= ED_EXCEPT;
            }
            if ((activity != 0) && (pin->scb != NULL)) {
                pin->scb(pin->fd, pin->pcb_data, activity);
            }
        }
    }
}


/***************************************************************************
 * add_fd(): - add a file descriptor to the select list
 ***************************************************************************/
void add_fd(
    int      fd,        // FD to add
    int      stype,     // type of activity to watch for
    void     (*scb) (), // activity callback
    void    *pcb_data)  // callback data 
{
    ED_FD   *pinfo = 0;
    int      i;         // loop counter

    // Sanity check: fd must be positive and callback must be defined
    if ((scb == NULL) || (fd <= 0)) {
        // LOG(LOG_WARNING, SL, E_Bad_FD);
        return;
    }

    // Find the first free entry in Ed_Fd
    for (i = 0; i < MX_FD; i++) {
        if (Ed_Fd[i].fd == -1) {
            pinfo = &Ed_Fd[i];
            break;
        }
    }
    if (i == MX_FD) {
        edlog(M_NOMOREFD);
        exit(-1);
    }

    // At this point we've walked the ED_FD array and have
    // found an empty ED_FD.  Add the new entry.
    pinfo->fd = fd;
    pinfo->stype = stype;
    pinfo->scb = scb;
    pinfo->pcb_data = pcb_data;

    update_fdsets();
}


/***************************************************************************
 * del_fd(): - delete a file descriptor from the select list
 ***************************************************************************/
void del_fd(
    int      fd)        // FD to delete
{
    int      i;         // loop counter

    // Find the FD and mark the entry as unused.
    for (i = 0; i < MX_FD; i++) {
        if (Ed_Fd[i].fd == fd) {
            Ed_Fd[i].fd = -1;
            break;
        }
    }
    update_fdsets();

    return;
}


/***************************************************************************
 * update_fdsets(): - refresh the list of read and write FDs
 ***************************************************************************/
void update_fdsets()
{
    int      i;         // loop counter

    FD_ZERO(&gRfds);
    FD_ZERO(&gWfds);
    FD_ZERO(&gXfds);
    fdcount = 0;
    mxfd = -1;

    for (i = 0; i < MX_FD; i++) {
        if (Ed_Fd[i].fd == -1)
            continue;

        fdcount++;
        mxfd = (Ed_Fd[i].fd > mxfd) ? Ed_Fd[i].fd : mxfd;
        if ((Ed_Fd[i].stype & ED_READ) != 0)
            FD_SET(Ed_Fd[i].fd, &gRfds);
        if ((Ed_Fd[i].stype & ED_WRITE) != 0)
            FD_SET(Ed_Fd[i].fd, &gWfds);
        if ((Ed_Fd[i].stype & ED_EXCEPT) != 0)
            FD_SET(Ed_Fd[i].fd, &gXfds);
    }
    return;
}


/***************************************************************************
 *  edlog():  Print logmessages to stderr or syslog
 ***************************************************************************/
void edlog(
    char    *format, ...) // printf format string
{
    va_list  ap;
    char    *s1;        // first optional argument
    char    *s2;        // second optional argument
    char    *s3;        // third optional argument
    char    *sptr;      // used to look for %s

    s1 = (char *) 0;
    s2 = (char *) 0;
    s3 = (char *) 0;

    /* Get the optional parameters if any */
    va_start(ap, format);
    sptr = strstr(format, "%s");
    if (sptr) {
        s1 = va_arg(ap, char *);
        sptr++;
        if (sptr) {
            s2 = va_arg(ap, char *);
            sptr++;
            if (sptr) {
                s3 = va_arg(ap, char *);
            }
        }
    }
    va_end(ap);

    /* Send to stderr if so configured */
    if (UseStderr) {
        fprintf(stderr, "%s: ", CmdName);
        fprintf(stderr, format, s1, s2, s3);
        fprintf(stderr, "\n");
    }
    else {
        syslog(LOG_WARNING, format, s1, s2, s3);
    }
}


/***************************************************************************
 * Timers in the ED Daemon
 * 
 * #define ED_ONESHOT  0
 * #define ED_PERIODIC 1
 * 
 * void * add_timer(
 *       int type;             // one-shot or periodic
 *       int ms;               // milliseconds to timeout
 *       void *(*callback)();  // called at timeout
 *       void *cb_data)        // blindly passed to callback
 * 
 * The 'add_timer' routine registers a subroutine ('callback')
 * for execution a set number of milliseconds from the time
 * of registration.  The timeout will occur repeatedly if the
 * type is ED_PERIODIC and will occur once if ED_ONESHOT.
 * Add_timer returns a 'handle' to help identify the timer
 * in other calls.
 * 
 * The callback returns nothing.  The callback routine has 
 * two parameters, the handle of the timer (void *) and the
 * callback data registered with the timer (cb_data).
 * 
 * A timer can be canceled with a call to del_timer()
 * which is passed a single parameter, the handle returned
 * from add_timer().
 *
 * The 'handle' is, of course, a pointer to the timer
 * structure.   Timers can be scheduled at most 2**31
 * milliseconds in the future on machines with 32 bit
 * ints.  This is about 24 days.
 ***************************************************************************/




/***************************************************************************
 * doTimers(): - Scan the timer queue looking for expired timers.
 * Call the callbacks for any expired timers and either remove
 * them (ED_ONESHOT) or reschedule them (ED_PERIODIC).
 *   Output a NULL timeval pointer if there are no timer or a
 * pointer to a valid timeval struct if there are timers.
 *
 * Input:        none
 * Output:       pointer to a timeval struct
 * Effects:      none
 ***************************************************************************/
struct timeval *doTimer()
{
    struct timeval tv;  // timeval struct to hold "now"
    long long now;      // "now" in milliseconds since Epoch
    long long nextto;   // Next timeout
    int    i;           // loop counter
    int    count;       // how many timers we've checked

    /* the following is the allocation for the tv used in select() */
    static struct timeval select_tv;

    /* Just return if there are no timers in use */
    if (ntimers == 0) {
        return ((struct timeval *) 0);
    }

    /* Get "now" in milliseconds since the Epoch */
    if (gettimeofday(&tv, 0)) {
        // LOG(LOG_WARNING, TM, E_No_Date);
        return ((struct timeval *) 0);
    }
    now = tv2us(&tv);

    /* Walk the array looking for timers with a timeout less than now */
    /* We can stop looking at timers when we've looked at ntimers of them */
    count = 0;
    for (i = 0; i < MX_TIMER; i++) {
        // Done if we've looked at all the timers
        if (count == ntimers)
            break;

        // Ignore unused timers
        if (Timers[i].type == ED_UNUSED)
            continue;

        // Found a timer in use
        count++;

        // Ignore if not expired
        if (Timers[i].to > now)
            continue;


        // Is it a PERIODIC timer ?
        if (Timers[i].type == ED_PERIODIC) { /* Periodic, so reschedule */
            (Timers[i].cb) ((void *) &Timers[i], Timers[i].pcb_data); /* Do the callback */
            Timers[i].to += Timers[i].us;
            if (Timers[i].to < now) { /* CPU hog made us miss a period? */
                edlog(M_MISSTO, i);
                Timers[i].to = now;
            }
        }
        else {             // must be a ONESHOT
            if (Timers[i].cb == 0) {
                break;
            }
            else {
                Timers[i].type = ED_UNUSED;
                ntimers--;
                (Timers[i].cb) ((void *) &Timers[i], Timers[i].pcb_data); // Do callback 
            }
        }
    }

    /* We processed all the expired timers.  Now we need to set the
       timeval struct to be used in the next select call.  This is null 
       if there are no timers.  If there are timers then the
       select timeval is based on the next timer timeout value. */

    // No timeout if no timers
    if (ntimers == 0) {
        return ((struct timeval *) 0);
    }

    // Walk the timer array again to find the nearest timeout
    nextto = -1;
    count = 0;
    for (i = 0; i < MX_TIMER; i++) {
        if (count == ntimers)
            break;

        if (Timers[i].type == ED_UNUSED)
            continue;

        count++;

        if ((nextto == -1) || (Timers[i].to < nextto)) {
            nextto = Timers[i].to;
        }
    }
    // Return null if no timers are set
    if (nextto == -1) {
        // This indicates an internal error.  no timers but ntimers not zero
        edlog("eedd internal timer error");
        return ((struct timeval *) 0);
    }
    // Return 0 for an immediate timeout if we missed a timeoout
    if ((nextto - now) < 0L) {     // next timeout is in the past (CPU hog?)
        nextto = now;
    }
    select_tv.tv_sec = (nextto - now) / 1000000;
    select_tv.tv_usec = (suseconds_t) ((nextto - now) % 1000000);
    return (&select_tv);
}


/***************************************************************************
 * add_timer(): - register a subroutine to be executed after a
 * specified number of milliseconds.
 *
 * Input:        int   type -- ED_ONESHOT or ED_PERIODIC
 *               void  (*cb)() -- pointer to subroutine called
 *                     when the timer expires
 *               void *pcbdata -- callback data which is passed
 *                     transparently to the callback
 * Output:       void * to be used as a 'handle' in the cancel call.
 * Effects:      No side effects
 ***************************************************************************/
void *add_timer(        // address of timer struct allocated
    int      type,      // oneshot or periodic
    int      ms,        // milliseconds to timeout
    void     (*cb) (),  // timeout callback
    void    *pcb_data)  // callback data
{
    struct timeval tv;  // timeval struct to hold "now"
    int      i;         // loop counter

    /* Sanity checks */
    if (cb == (void *) 0) {
        edlog("Adding timer with null callback");
        return((void *) 0);
    }
    if (ms == 0 && type == ED_PERIODIC) {
        edlog("Periodic timer with period = 0");
        return ((ED_TIMER *) 0);
    }

    // Walk the array of timers and find a free one
    for (i = 0; i < MX_TIMER; i++) {
        if (Timers[i].type == ED_UNUSED)
            break;
    }
    if (i == MX_TIMER) {
        edlog("No free timers");
        return ((void *) 0);
    }

    /* Get "now" */
    if (gettimeofday(&tv, 0)) {
        // LOG(LOG_WARNING, TM, E_No_Date);
        return ((ED_TIMER *) 0);
    }

    /* OK, we've got the ED_TIMER struct, now fill it in */
    ntimers++;       /* increment number of ED_TIMER structs alloc'ed */
    Timers[i].type = type;          /* one-shot or periodic */
    Timers[i].to = tv2us(&tv) + (ms * 1000); /* us from Epoch to timeout */
    Timers[i].us = ms * 1000;       /* period or interval in uS */
    Timers[i].cb = cb;              /* callback routine */
    Timers[i].pcb_data = pcb_data;  /* callback data */

    return ((void *) &Timers[i]);
}


/***************************************************************************
 * del_timer(): - mark a timer for deletion
 *
 * Input:        Pointer to a timer structure
 * Output:       Nothing
 * Effects:      No side effects
 ***************************************************************************/
void del_timer(
    void    *ptimer)
{
    // Verify pointer is in range and on struct boundry
    if ((ptimer < (void *) &Timers[0]) || (ptimer > (void *) &Timers[MX_TIMER -1])) {
        return;
    }
    if (((ptimer - (void *) &Timers[0]) % sizeof(ED_TIMER)) != 0) {
        return;
    }
    if (((ED_TIMER *) ptimer)->type == ED_UNUSED) {
        return;
    }

    ((ED_TIMER *) ptimer)->type = ED_UNUSED;

    ntimers--;
}


/***************************************************************************
 * tv2us(): - convert a timeval struct to long long of microseconds.
 *
 * Input:        Pointer to a timer structure
 * Output:       long long
 * Effects:      No side effects
 ***************************************************************************/
static long long tv2us(
    struct timeval *ptv)
{
    return ((((long long)ptv->tv_sec) * 1000000) + ptv->tv_usec);
}


/***************************************************************************
 * getslotbyid(): - return a slot pointer given its index.
 *   This routine is used by plug-ins to help find what other
 * plug-ins are in the system.
 *
 ***************************************************************************/
const SLOT * getslotbyid(
    int    id)
{
    if ((id >= 0) && (id < MX_PLUGIN))
        return(&(Slots[id]));
    else
        return((const SLOT *) 0);
}


/* End of util.c */

