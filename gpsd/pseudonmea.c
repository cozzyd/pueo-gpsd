/*
 * Create psuedo NMEA (and AIVDM) messaegs.
 *
 * This file is Copyright 2010 by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 */

#include "../include/gpsd_config.h"   // must be before all includes

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/gpsd.h"
#include "../include/strfuncs.h"

/*
 * Support for generic binary drivers.  These functions dump NMEA for passing
 * to the client in raw mode.  They assume that (a) the public gps.h structure
 * members are in a valid state, (b) that the private members hours, minutes,
 * and seconds have also been filled in, (c) that if the private member
 * mag_var is not NAN it is a magnetic variation in degrees that should be
 * passed on, and (d) if the private member separation does not have the
 * value NAN, it is a valid WGS84 geoidal separation in meters for the fix.
 */

#define BUF_SZ 20
/* decimal degrees to GPS-style, degrees first followed by minutes
 *
 * Return: pointer to passed in buffer (buf) with string
 */
static char *degtodm_str(double angle, const char *fmt, char *buf)
{
    if (0 == isfinite(angle)) {
        buf[0] = '\0';
    } else {
        double fraction, integer;

        angle = fabs(angle);
        fraction = modf(angle, &integer);
        (void)snprintf(buf, BUF_SZ, fmt, floor(angle) * 100 + fraction * 60);
    }
    return buf;
}

/* format a float/lon/alt into a string, handle NAN, INFINITE
 *
 * Return: pointer to passed in buffer (buf) with string
 */
static char *f_str(double f, const char *fmt, char *buf)
{
    if (0 == isfinite(f)) {
        buf[0] = '\0';
    } else {
        (void)snprintf(buf, BUF_SZ, fmt, f);
    }
    return buf;
}

// make size of time string buffer large to shut up paranoid cc's
#define TIMESTR_SZ 48

/* convert UTC to time str (hh:mm:ss.ss) and tm
 *
 * Return: pointer to passed in buffer (buf) with string
 */
static void utc_to_hhmmss(timespec_t time, char *buf, ssize_t buf_sz,
                          struct tm *tm)
{
    time_t integer;
    long fractional;

    if (0 >= time.tv_sec) {
        buf[0] = '\0';
        return;
    }

    integer = time.tv_sec;
    // round to 100ths
    fractional = (time.tv_nsec + 5000000L) / 10000000L;
    if (99 < fractional) {
        integer++;
        fractional = 0;
    }

    if (NULL == gmtime_r(&integer, tm)) {
        // WTF?
        buf[0] = '\0';
        return;
    }


    (void)snprintf(buf, buf_sz, "%02d%02d%02d.%02ld",
                   tm->tm_hour, tm->tm_min, tm->tm_sec, fractional);
    return;
}

/* dbl_to_str() -- convert a double to a string in bufp
 *
 * Return: void
 */
static void dbl_to_str(const char *fmt, double val, char *bufp, size_t len,
                       const char *suffix)
{
    if (0 == isfinite(val)) {
        (void)strlcat(bufp, ",", len);
    } else {
        str_appendf(bufp, len, fmt, val);
    }
    if (NULL != suffix) {
        str_appendf(bufp, len, "%s", suffix);
    }
}

/* GPS Quality Indicator values for xxGGA
 * Almost, not quite, the same as STATUS_* from gps.h */
#define FIX_QUALITY_INVALID 0
#define FIX_QUALITY_GPS 1
#define FIX_QUALITY_DGPS 2
#define FIX_QUALITY_PPS 3
#define FIX_QUALITY_RTK 4
#define FIX_QUALITY_RTK_FLT 5
#define FIX_QUALITY_DR 6
#define FIX_QUALITY_MANUAL 7
#define FIX_QUALITY_SIMULATED 8

/* Dump a $GPGGA.
 * looks like this is only called from net_ntrip.c and nmea_tpv_dump()
 */
void gpsd_position_fix_dump(struct gps_device_t *session,
                            char bufp[], size_t len)
{
    struct tm tm;
    char time_str[TIMESTR_SZ];

    utc_to_hhmmss(session->gpsdata.fix.time, time_str, sizeof(time_str), &tm);

    if (MODE_NO_FIX < session->gpsdata.fix.mode) {
        unsigned char fixquality;
        char lat_str[BUF_SZ];
        char lon_str[BUF_SZ];

        switch(session->gpsdata.fix.status) {
        case STATUS_UNK:
            fixquality = FIX_QUALITY_INVALID;
            break;
        case STATUS_GPS:
            fixquality = FIX_QUALITY_GPS;
            break;
        case STATUS_DGPS:
            fixquality = FIX_QUALITY_DGPS;
            break;
        case STATUS_RTK_FIX:
            fixquality = FIX_QUALITY_RTK;
            break;
        case STATUS_RTK_FLT:
            fixquality = FIX_QUALITY_RTK_FLT;
            break;
        case STATUS_DR:
            FALLTHROUGH
        case STATUS_GNSSDR:
            fixquality = FIX_QUALITY_DR;
            break;
        case STATUS_TIME:
            fixquality = FIX_QUALITY_MANUAL;
            break;
        case STATUS_SIM:
            fixquality = FIX_QUALITY_SIMULATED;
            break;
        default:
            fixquality = FIX_QUALITY_INVALID;
            break;
        }

        (void)snprintf(bufp, len,
                       "$GPGGA,%s,%s,%c,%s,%c,%d,%02d,",
                       time_str,
                       degtodm_str(session->gpsdata.fix.latitude, "%09.4f",
                                   lat_str),
                       ((session->gpsdata.fix.latitude > 0) ? 'N' : 'S'),
                       degtodm_str(session->gpsdata.fix.longitude, "%010.4f",
                                   lon_str),
                       ((session->gpsdata.fix.longitude > 0) ? 'E' : 'W'),
                       fixquality,
                       session->gpsdata.satellites_used);
        dbl_to_str("%.2f,", session->gpsdata.dop.hdop, bufp, len, NULL);
        dbl_to_str("%.2f,", session->gpsdata.fix.altMSL, bufp, len, "M,");
        dbl_to_str("%.3f,", session->gpsdata.fix.geoid_sep, bufp, len, "M,");
        /* empty place holders for Age of correction data, and
         * Differential base station ID */
        (void)strlcat(bufp, ",", len);
        nmea_add_checksum(bufp);
    }
}

static void gpsd_transit_fix_dump(struct gps_device_t *session,
                                  char bufp[], size_t len)
{
    char time_str[TIMESTR_SZ];
    char time2_str[TIMESTR_SZ];
    char lat_str[BUF_SZ];
    char lon_str[BUF_SZ];
    char speed_str[BUF_SZ];
    char track_str[BUF_SZ];
    char var_str[BUF_SZ];
    char *var_dir = "";
    struct tm tm;
    char valid;
    char faa_mode;
    char nav_stat;

    utc_to_hhmmss(session->gpsdata.fix.time, time_str, sizeof(time_str), &tm);
    if ('\0' == time_str[0]) {
        time2_str[0] = '\0';
    } else {
        tm.tm_mon++;
        tm.tm_year %= 100;

        (void)snprintf(time2_str, sizeof(time2_str),
                       "%02d%02d%02d",
                       tm.tm_mday, tm.tm_mon, tm.tm_year);
    }
    if (0 == isfinite(session->gpsdata.fix.magnetic_var)) {
        var_str[0] = '\0';
        var_dir = "";
    } else {
        f_str(session->gpsdata.fix.magnetic_var, "%.1f", var_str),
        var_dir = (session->gpsdata.fix.magnetic_var > 0) ? "E" : "W";
    }

    //handle FAA-mode indicator
    //new in NMEA 2.3
    if (MODE_NO_FIX < session->gpsdata.fix.mode) {
        bool is_valid_mode = false;
        // only 'A' and 'D' create a valid status as per NMEA 3.01
        // default to false
        switch(session->gpsdata.fix.status) {
        case STATUS_GPS:
            faa_mode = 'A'; //FAA_MODE_AUTONOMOUS
            is_valid_mode = true;
            break;
        case STATUS_RTK_FIX:
            // NMEA 4.10+
            faa_mode = 'R'; //FAA_MODE_RTK_FIX
            break;
        case STATUS_RTK_FLT:
            // NMEA 4.10+
            faa_mode = 'F'; //FAA_MODE_RTK_FLT
            break;
        case STATUS_PPS_FIX:
            // NMEA 4.10+
            faa_mode = 'P'; //FAA_MODE_PRECISE
            break;
        case STATUS_DGPS:
            faa_mode = 'D'; //FAA_MODE_DIFFERENTIAL
            is_valid_mode = true;
            break;
        case STATUS_DR:
            FALLTHROUGH
        case STATUS_GNSSDR:
            faa_mode = 'E'; //FAA_MODE_ESTIMATED
            break;
        case STATUS_TIME:
            faa_mode = 'M'; //FAA_MODE_MANUAL
            break;
        case STATUS_SIM:
            faa_mode = 'S'; //FAA_MODE_SIMULATED
            break;
        case STATUS_UNK:
            FALLTHROUGH
        default:
            faa_mode = 'N'; //FAA_MODE_NOT_VALID
            break;
        }
        //handle data validity status
        if(is_valid_mode) {
            valid = 'A';      // Status: Valid
        } else {
            valid = 'V';      // Status: Warning
        }
    } else {
        // enable clients to use mode field to check for NO_FIX
        faa_mode='N';     //FAA_MODE_NOT_VALID
        // no fix -> not valid
        valid = 'V';      // Status: Warning
    }
    //Navigational Status Indicator
    //new in NMEA 4.10
    //S = Safe - accuracy, integrity and age of fix meets requirements
    //C = Caution - integrity is not available
    //U = Unsafe - requirements not met
    //V = Navigational invalid, No navigational status indication
    //we don't evaluate this field atm.
    //we report that we don't provide valid information in this field
    nav_stat='V'
    //this does't invalidate the fix!

    //NMEA 4.10
    (void)snprintf(bufp, len,
                   "$GPRMC,%s,%c,%s,%c,%s,%c,%s,%s,%s,%s,%s,%c,%c",
                   time_str,
                   valid,
                   degtodm_str(session->gpsdata.fix.latitude, "%09.4f",
                               lat_str),
                   ((session->gpsdata.fix.latitude > 0) ? 'N' : 'S'),
                   degtodm_str(session->gpsdata.fix.longitude, "%010.4f",
                               lon_str),
                   ((session->gpsdata.fix.longitude > 0) ? 'E' : 'W'),
                   f_str(session->gpsdata.fix.speed * MPS_TO_KNOTS, "%.4f",
                            speed_str),
                   f_str(session->gpsdata.fix.track, "%.3f", track_str),
                   time2_str, var_str, var_dir, faa_mode, nav_stat);
    nmea_add_checksum(bufp);
}

static void gpsd_binary_satellite_dump(struct gps_device_t *session,
                                       char bufp[], size_t len)
{
    int i;                  // index into skyview[]
    int j;                  // index into GPGSV
    char *bufp2 = bufp;
    int satellites_visible = 0;
    bufp[0] = '\0';

    // check skyview[] for valid sats first
    for (i = 0; i < session->gpsdata.satellites_visible; i++) {
        if (1 > session->gpsdata.skyview[i].PRN) {
            // bad PRN, ignore
            continue;
        }
        if (0 == isfinite(session->gpsdata.skyview[i].elevation) ||
            90 < fabs(session->gpsdata.skyview[i].elevation)) {
            // bad elevation, ignore
            continue;
        }
        if (0 == isfinite(session->gpsdata.skyview[i].azimuth) ||
            0 > session->gpsdata.skyview[i].azimuth ||
            359 < session->gpsdata.skyview[i].azimuth) {
            // bad azimuth, ignore
            continue;
        }
        satellites_visible++;
    }
    for (i = 0, j= 0; i < session->gpsdata.satellites_visible; i++) {
        if (1 > session->gpsdata.skyview[i].PRN) {
            // bad prn, skip
            continue;
        }
        if (0 == isfinite(session->gpsdata.skyview[i].elevation) ||
            90 < fabs(session->gpsdata.skyview[i].elevation)) {
            // bad elevation, ignore
            continue;
        }
        if (0 == isfinite(session->gpsdata.skyview[i].azimuth) ||
            0 > session->gpsdata.skyview[i].azimuth ||
            359 < session->gpsdata.skyview[i].azimuth) {
            // bad azimuth, ignore
            continue;
        }
        if (0 == isfinite(session->gpsdata.skyview[i].ss)) {
            // bad snr, ignore
            continue;
        }
        if (0 == (j % 4)) {
            bufp2 = bufp + strlen(bufp);
            str_appendf(bufp, len,
                            "$GPGSV,%d,%d,%02d",
                            ((satellites_visible - 1) / 4) + 1, (j / 4) + 1,
                            satellites_visible);
        }
        str_appendf(bufp, len, ",%02d,%02.0f,%03.0f,%02.0f",
                    session->gpsdata.skyview[i].PRN,
                    session->gpsdata.skyview[i].elevation,
                    session->gpsdata.skyview[i].azimuth,
                    session->gpsdata.skyview[i].ss);

        if (3 == (j % 4) ||
            (satellites_visible - 1) == j) {
            nmea_add_checksum(bufp2);
        }
        j++;
    }

#ifdef ZODIAC_ENABLE
    if (ZODIAC_PACKET == session->lexer.type &&
        0 != session->driver.zodiac.Zs[0]) {
        bufp2 = bufp + strlen(bufp);
        str_appendf(bufp, len, "$PRWIZCH");
        for (i = 0; i < ZODIAC_CHANNELS; i++) {
            str_appendf(bufp, len,
                        ",%02u,%X",
                        session->driver.zodiac.Zs[i],
                        session->driver.zodiac.Zv[i] & 0x0f);
        }
        nmea_add_checksum(bufp2);
    }
#endif  // ZODIAC_ENABLE
}

static void gpsd_binary_quality_dump(struct gps_device_t *session,
                                     char bufp[], size_t len)
{
    char *bufp2;
    bufp[0] = '\0';

    if (NULL != session->device_type) {
        int i, j;
        int max_channels = session->device_type->channels;
        int mode = session->gpsdata.fix.mode;

        // GPGSA commonly has exactly 12 channels, enforce that as a MAX
        if (12 < max_channels) {
            // what to do with the excess channels?
            max_channels = 12;
        }

        bufp2 = bufp + strlen(bufp);
        // fix.mode can be zero, legal values are only 1, 2 or 3
        if (MODE_NO_FIX > mode) {
            mode = MODE_NO_FIX;    // 1
        } else if (MODE_3D < mode) {
            mode = MODE_3D;        // 3
        }
        // We don't know if we are manual (M) or automatic (A), force "A".
        (void)snprintf(bufp, len, "$GPGSA,%c,%d,", 'A', mode);
        j = 0;
        for (i = 0; i < max_channels; i++) {
            if (true == session->gpsdata.skyview[i].used){
                str_appendf(bufp, len, "%d,", session->gpsdata.skyview[i].PRN);
                j++;
            }
        }
        for (i = j; i < max_channels; i++) {
            // fill out the empty slots
            (void)strlcat(bufp, ",", len);
        }
        if (MODE_NO_FIX == session->gpsdata.fix.mode) {
            (void)strlcat(bufp, ",,,", len);
        } else {
            // output the DOPs, NaN as blanks
            if (0 != isfinite(session->gpsdata.dop.pdop)) {
                str_appendf(bufp, len, "%.1f,", session->gpsdata.dop.pdop);
            } else {
                (void)strlcat(bufp, ",", len);
            }
            if (0 != isfinite(session->gpsdata.dop.hdop)) {
                str_appendf(bufp, len, "%.1f,", session->gpsdata.dop.hdop);
            } else {
                (void)strlcat(bufp, ",", len);
            }
            if (0 != isfinite(session->gpsdata.dop.vdop)) {
                str_appendf(bufp, len, "%.1f*", session->gpsdata.dop.vdop);
            } else {
                (void)strlcat(bufp, "*", len);
            }
        }
        nmea_add_checksum(bufp2);
    }

    /* create $GPGBS if we have time, epx and epy.  Optional epv.
     * Not really kosher, not have enough info to compute the RAIM
     */
    if (0 != isfinite(session->gpsdata.fix.epx) &&
        0 != isfinite(session->gpsdata.fix.epy) &&
        0 != session->gpsdata.fix.time.tv_sec) {

        struct tm tm;
        char time_str[TIMESTR_SZ];
        char epv_str[BUF_SZ];

        (void)utc_to_hhmmss(session->gpsdata.fix.time,
                            time_str, sizeof(time_str), &tm);

        bufp2 = bufp + strlen(bufp);
        str_appendf(bufp, len,
                    "$GPGBS,%s,%.3f,%.3f,%s,,,,",
                    time_str,
                    session->gpsdata.fix.epx,
                    session->gpsdata.fix.epy,
                    f_str(session->gpsdata.fix.epv, "%.3f", epv_str));
        nmea_add_checksum(bufp2);
    }
}

// Dump $GPZDA if we have time and a fix
// return length added
static ssize_t gpsd_binary_time_dump(struct gps_device_t *session,
                                     char bufp[], size_t len)
{

    ssize_t blen = 0;

    if (MODE_NO_FIX < session->gpsdata.fix.mode &&
        0 <= session->gpsdata.fix.time.tv_sec) {
        // shut up clang about uninitialized
        struct tm tm = {0};
        char time_str[TIMESTR_SZ];

        utc_to_hhmmss(session->gpsdata.fix.time, time_str, sizeof(time_str),
                      &tm);
        /* There used to be confusion, but we now know NMEA times are UTC,
         * when available. */
        blen = snprintf(bufp, len,
                        "$GPZDA,%s,%02d,%02d,%04d,00,00",
                        time_str,
                        tm.tm_mday,
                        tm.tm_mon + 1,
                        tm.tm_year + 1900);
        nmea_add_checksum(bufp);
        blen += 5;
    }
    return blen;
}

static void gpsd_binary_almanac_dump(struct gps_device_t *session,
                                     char bufp[], size_t len)
{
    if (session->gpsdata.subframe.is_almanac) {
        (void)snprintf(bufp, len,
            "$GPALM,1,1,%02d,%04d,%02x,%04x,%02x,%04x,%04x,%05x,"
            "%06x,%06x,%06x,%03x,%03x",
            (int)session->gpsdata.subframe.sub5.almanac.sv,
            (int)session->context->gps_week % 1024,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.svh,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.e,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.toa,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.deltai,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.Omegad,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.sqrtA,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.omega,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.Omega0,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.M0,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.af0,
            (unsigned int)session->gpsdata.subframe.sub5.almanac.af1);
        nmea_add_checksum(bufp);
    }
}

#ifdef AIVDM_ENABLE

#define GETLEFT(a) (((a%6) == 0) ? 0 : (6 - (a%6)))

static void gpsd_binary_ais_dump(struct gps_device_t *session,
                                 char bufp[], size_t len)
{
    char type[8] = "!AIVDM";
    unsigned char data[256];
    unsigned int msg1, msg2;
    char numc[4];
    char channel;
    unsigned int left;
    unsigned int datalen;
    unsigned int offset;

    channel = 'A';
    if ('B' == session->driver.aivdm.ais_channel) {
        channel = 'B';
    }

    memset(data, 0, sizeof(data));
    datalen = ais_binary_encode(&session->gpsdata.ais, &data[0], 0);
    if ((6 * 60) < datalen) {
        static int number1 = 0;

        msg1 = datalen / (6 * 60);
        if (0 != (datalen % (6 * 60))) {
            msg1 += 1;
        }
        numc[0] = '0' + (char)(number1 & 0x0f);
        numc[1] = '\0';
        number1 += 1;
        if (9 < number1) {
            number1 = 0;
        }
        offset = 0;
        for (msg2 = 1; msg2 <= msg1; msg2++) {
            unsigned char old;

            old = '\0';
            if (60 < strlen((char *)&data[(msg2 - 1) * 60])) {
                old = data[(msg2 - 0) * 60];
                data[(msg2 - 0) * 60] = '\0';
            }
            if ((6 * 60) <= datalen) {
                left = 0;
                datalen -= 6 * 60;
            } else {
                left = GETLEFT(datalen);
            }
            (void)snprintf(&bufp[offset], len-offset,
                           "%s,%u,%u,%s,%c,%s,%u",
                           type,
                           msg1,
                           msg2,
                           numc,
                           channel,
                           (char *)&data[(msg2-1)*60],
                           left);

            nmea_add_checksum(&bufp[offset]);
            if ((unsigned char)'\0' != old) {
                data[(msg2 - 0) * 60] = old;
            }
            offset = (unsigned int)strlen(bufp);
        }
    } else if (0 < datalen) {
        msg1 = 1;
        msg2 = 1;
        numc[0] = '\0';
        left = GETLEFT(datalen);
        (void)snprintf(bufp, len,
                       "%s,%u,%u,%s,%c,%s,%u",
                       type,
                       msg1,
                       msg2,
                       numc,
                       channel,
                       (char *)data,
                       left);

        nmea_add_checksum(bufp);
    }

    if (24 == session->gpsdata.ais.type) {
        msg1 = 1;
        msg2 = 1;
        numc[0] = '\0';

        memset(data, 0, sizeof(data));
        datalen = ais_binary_encode(&session->gpsdata.ais, &data[0], 1);
        if (0 < datalen) {
            left = GETLEFT(datalen);
            offset = (unsigned int)strlen(bufp);
            (void)snprintf(&bufp[offset], len-offset,
                           "%s,%u,%u,%s,%c,%s,%u",
                           type,
                           msg1,
                           msg2,
                           numc,
                           channel,
                           (char *)data,
                           left);
        nmea_add_checksum(bufp+offset);
        }
    }
}
#endif  // AIVDM_ENABLE

/* nmea_tpv_dump()
 * dump current fix as NMEA
 *
 * Return: void
 */
void nmea_tpv_dump(struct gps_device_t *session,
                   char bufp[], size_t len)
{
    ssize_t blen = 0;
    bufp[0] = '\0';

    // maybe all we need is REPORT_IS?
    if (0 != (session->gpsdata.set & (TIME_SET | REPORT_IS))) {
        blen = gpsd_binary_time_dump(session, bufp, len);
    }
    if (0 != (session->gpsdata.set & (LATLON_SET | MODE_SET | REPORT_IS))) {
        gpsd_position_fix_dump(session, bufp + blen, len - blen);
        gpsd_transit_fix_dump(session, bufp + strlen(bufp),
                              len - strlen(bufp));
    }
    if (0 != (session->gpsdata.set &
              (MODE_SET | DOP_SET | USED_IS | HERR_SET | REPORT_IS))) {
        gpsd_binary_quality_dump(session, bufp + strlen(bufp),
                                 len - strlen(bufp));
    }
}

void nmea_sky_dump(struct gps_device_t *session,
                   char bufp[], size_t len)
{
    bufp[0] = '\0';
    if (0 != (session->gpsdata.set & SATELLITE_SET)) {
        gpsd_binary_satellite_dump(session, bufp + strlen(bufp),
                                   len - strlen(bufp));
    }
}

// FIXME: this shim should go away...
void nmea_subframe_dump(struct gps_device_t *session,
                        char bufp[], size_t len)
{
    bufp[0] = '\0';
    if (0 != (session->gpsdata.set & SUBFRAME_SET)) {
        size_t buflen = strnlen(bufp, len);
        gpsd_binary_almanac_dump(session, bufp + buflen, len - buflen);
    }
}

#ifdef AIVDM_ENABLE
void nmea_ais_dump(struct gps_device_t *session,
                   char bufp[], size_t len)
{
    bufp[0] = '\0';
    if (0 != (session->gpsdata.set & AIS_SET)) {
        gpsd_binary_ais_dump(session, bufp + strlen(bufp), len - strlen(bufp));
    }
}
#endif  // AIVDM_ENABLE

// vim: set expandtab shiftwidth=4
