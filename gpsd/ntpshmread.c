/* ntpshmread.c -- monitor the inner end of an ntpshmwrite.connection
 *
 * This file is Copyright 2010 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 *
 * Some of this was swiped from the NTPD distribution.
 */

#include "../include/gpsd_config.h"  /* must be before all includes */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/ntpshm.h"
#include "../include/compiler.h"

/* initialize a SHM segment */
struct shmTime *shm_get(const int unit, const bool create, const bool forall)
{
    struct shmTime *p = NULL;
    int shmid;

    /*
     * Big units will give non-ASCII but that's OK
     * as long as everybody does it the same way.
     */
    shmid = shmget((key_t)(NTPD_BASE + unit), sizeof(struct shmTime),
                   (create ? IPC_CREAT : 0) | (forall ? 0666 : 0600));
    if (shmid == -1) { /* error */
        if (ENOENT != errno) {
            // silently ignore ENOENT (2:No such file or directory)
            // user probably forgot to be root "Permission denied(13)"
            (void)fprintf(stderr, "WARNING: could not open SHM(%d): %s(%d)\n",
                          unit, strerror(errno), errno);
        }
        return NULL;
    }
    p = (struct shmTime *)shmat (shmid, 0, 0);
    if ((struct shmTime *)-1 == p) {     // error
        (void)fprintf(stderr, "WARNING: unit %d, shmat(x%x): %s\n",
                      unit, shmid, strerror(errno));
        return NULL;
    }
    return p;
}

char *ntp_name(const int unit)
/* return the name of a specified segment */
{
    static char name[5] = "NTP\0";

    name[3] = (char)('0' + unit);

    return name;
}

/* try to grab a sample from the specified SHM segment */
enum segstat_t ntp_read(struct shmTime *shm_in, struct shm_stat_t *shm_stat,
                        const bool consume)
{
    volatile struct shmTime shmcopy, *shm = shm_in;
    volatile int cnt;

    unsigned int cns_new, rns_new;

    if (shm == NULL) {
        shm_stat->status = NO_SEGMENT;
        return NO_SEGMENT;
    }

    shm_stat->tvc.tv_sec = shm_stat->tvc.tv_nsec = 0;

    /* relying on word access to be atomic here */
    if (shm->valid == 0) {
        shm_stat->status = NOT_READY;
        return NOT_READY;
    }

    cnt = shm->count;

    /*
     * This is proof against concurrency issues if either (a) the
     * memory_barrier() call works on this host, or (b) memset
     * compiles to an uninterruptible single-instruction bitblt (this
     * will probably cease to be true if the structure exceeds your VM
     * page size).
     */
    memory_barrier();
    memcpy((void *)&shmcopy, (void *)shm, sizeof(struct shmTime));

    /*
     * An update consumer such as ntpd should zero the valid flag at this point.
     * A program snooping the updates to collect statistics should not, lest
     * it make the data unavailable for consumers.
     */
    if (consume)
        shm->valid = 0;
    memory_barrier();

    /*
     * Clash detection in case neither (a) nor (b) was true.
     * Not supported in mode 0, and word access to the count field
     * must be atomic for this to work.
     */
    if (shmcopy.mode > 0 && cnt != shm->count) {
        shm_stat->status = CLASH;
        return shm_stat->status;
    }

    shm_stat->status = OK;

    switch (shmcopy.mode) {
    case 0:
        shm_stat->tvr.tv_sec    = shmcopy.receiveTimeStampSec;
        shm_stat->tvr.tv_nsec   = shmcopy.receiveTimeStampUSec * 1000;
        rns_new         = shmcopy.receiveTimeStampNSec;
        shm_stat->tvt.tv_sec    = shmcopy.clockTimeStampSec;
        shm_stat->tvt.tv_nsec   = shmcopy.clockTimeStampUSec * 1000;
        cns_new         = shmcopy.clockTimeStampNSec;

        /* Since the following comparisons are between unsigned
        ** variables they are always well defined, and any
        ** (signed) underflow will turn into very large unsigned
        ** values, well above the 1000 cutoff.
        **
        ** Note: The usecs *must* be a *truncated*
        ** representation of the nsecs. This code will fail for
        ** *rounded* usecs, and the logic to deal with
        ** wrap-arounds in the presence of rounded values is
        ** much more convoluted.
        */
        if (((cns_new - (unsigned)shm_stat->tvt.tv_nsec) < 1000)
               && ((rns_new - (unsigned)shm_stat->tvr.tv_nsec) < 1000)) {
            shm_stat->tvt.tv_nsec = cns_new;
            shm_stat->tvr.tv_nsec = rns_new;
        }
        /* At this point shm_stat->tvr and shm_stat->tvt contain valid ns-level
        ** timestamps, possibly generated by extending the old
        ** us-level timestamps
        */
        break;

    case 1:
        shm_stat->tvr.tv_sec    = shmcopy.receiveTimeStampSec;
        shm_stat->tvr.tv_nsec   = shmcopy.receiveTimeStampUSec * 1000;
        rns_new         = shmcopy.receiveTimeStampNSec;
        shm_stat->tvt.tv_sec    = shmcopy.clockTimeStampSec;
        shm_stat->tvt.tv_nsec   = shmcopy.clockTimeStampUSec * 1000;
        cns_new         = shmcopy.clockTimeStampNSec;

        /* See the case above for an explanation of the
        ** following test.
        */
        if (((cns_new - (unsigned)shm_stat->tvt.tv_nsec) < 1000)
               && ((rns_new - (unsigned)shm_stat->tvr.tv_nsec) < 1000)) {
            shm_stat->tvt.tv_nsec = cns_new;
            shm_stat->tvr.tv_nsec = rns_new;
        }
        /* At this point shm_stat->tvr and shm_stat->tvt contains valid ns-level
        ** timestamps, possibly generated by extending the old
        ** us-level timestamps
        */
        break;

    default:
        shm_stat->status = BAD_MODE;
        break;
    }

    /*
     * leap field is not a leap offset but a leap notification code.
     * The values are magic numbers used by NTP and set by GPSD, if at all, in
     * the subframe code.
     */
    shm_stat->leap = shmcopy.leap;
    shm_stat->precision = shmcopy.precision;

    return shm_stat->status;
}

/* end */
// vim: set expandtab shiftwidth=4
