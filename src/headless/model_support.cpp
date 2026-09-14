#include "stdhdr.h"
#include "campwp.h"
#include "find.h"
#include "radardata.h"
#include "simfile.h"
#include "datafile.h"
#include "falcent.h"
#include "radar.h"
#include <stdexcept>
void recalculate_waypoint(WayPointClass*,int,int);
// Native model routine from ui/src/common/waypoint.cpp
void recalculate_waypoint_list(WayPointClass *wp, int minSpeed, int maxSpeed)
{
    // First, find out if we're time locked ourselves
    if (wp->GetWPFlags() bitand WPF_TIME_LOCKED)
    {
        // We're gonna have to do two recalculations, one before and one after this waypoint
        WayPointClass *pw, *nw;
        float dist;
        int time;
        pw = wp->GetPrevWP();
        nw = wp->GetNextWP();

        if (pw and not(pw->GetWPFlags() bitand WPF_TIME_LOCKED))
            recalculate_waypoint(pw, minSpeed, maxSpeed);
        else if (pw)
        {
            // Simply set Speed between pw and wp
            dist = wp->DistanceTo(pw);
            time = wp->GetWPArrivalTime() - pw->GetWPDepartureTime();

            if (time not_eq 0)
                wp->SetWPSpeed((dist * CampaignHours) / time);
            else
                wp->SetWPSpeed(0);
        }

        if (nw and not(nw->GetWPFlags() bitand WPF_TIME_LOCKED))
            recalculate_waypoint(nw, minSpeed, maxSpeed);
        else if (nw)
        {
            // Simply set Speed between nw and wp
            dist = wp->DistanceTo(nw);
            time = nw->GetWPArrivalTime() - wp->GetWPDepartureTime();

            if (time not_eq 0)
                nw->SetWPSpeed((dist * CampaignHours) / time);
            else
                nw->SetWPSpeed(0);
        }
    }
    else
        recalculate_waypoint(wp, minSpeed, maxSpeed);
}
// Native model routine from ui/src/common/waypoint.cpp
void recalculate_waypoint(WayPointClass *wp, int, int)
{
    GridIndex x, y, nx, ny;
    float speed, dist, d;
    WayPointClass *pw, *nw, *w;
    CampaignTime startTime = 0, endTime = 0, lockedTime = 0, now;

    if (not wp)
        return;

    ShiAssert(not(wp->GetWPFlags() bitand WPF_TIME_LOCKED));

    // KCK: This is annoyingly complex.
    // Basically, we're trying to either move times in/out from the changed waypoint
    // or smooth speeds between two time locked waypoints,

    // First, find out if we're between a time locked pair, or if we've got open ends
    pw = nw = wp;
    w = wp->GetPrevWP();

    while (w)
    {
        pw = w;

        if (w->GetWPFlags() bitand WPF_TIME_LOCKED)
            break;

        w = w->GetPrevWP();
    }

    w = wp->GetNextWP();

    while (w)
    {
        nw = w;

        if (w->GetWPFlags() bitand WPF_TIME_LOCKED)
            break;

        w = w->GetNextWP();
    }

    if ((pw->GetWPFlags() bitand WPF_TIME_LOCKED) and
        (nw->GetWPFlags() bitand WPF_TIME_LOCKED))
    {
        // We're between timelocked stuff. smooth the speeds
        startTime = pw->GetWPDepartureTime();
        endTime = nw->GetWPArrivalTime();
        // Now calculate the total adjustable distance and time
        dist = 0.0F;
        pw->GetWPLocation(&x, &y);
        w = pw->GetNextWP();

        while (w)
        {
            w->GetWPLocation(&nx, &ny);
            d = Distance(x, y, nx, ny);

            if (w->GetWPFlags() bitand WPF_SPEED_LOCKED)
                lockedTime +=
                    FloatToInt32((d * CampaignHours) /
                                 w->GetWPSpeed()); // This time is locked up
            else
                dist += d;

            x = nx;
            y = ny;

            if (w == nw)
                break;

            w = w->GetNextWP();
        }

        // Now calculate our new average speed and apply it
        if (startTime > endTime)
            speed = (dist * CampaignHours) /
                    (((startTime - endTime) * -1) - lockedTime);
        else
            speed =
                (dist * CampaignHours) / ((endTime - startTime) - lockedTime);

        pw->GetWPLocation(&x, &y);
        now = startTime;
        w = pw->GetNextWP();

        while (w)
        {
            w->GetWPLocation(&nx, &ny);
            d = Distance(x, y, nx, ny);

            if (not(w->GetWPFlags() bitand WPF_SPEED_LOCKED))
            {
                w->SetWPSpeed(speed);

                if (speed)
                    now += FloatToInt32((d * CampaignHours) / speed);
            }
            else if (w->GetWPSpeed())
                now += FloatToInt32((d * CampaignHours) / w->GetWPSpeed());

            if (w == nw)
                break;

            w->SetWPTimes(now);
            now += w->GetWPStationTime();
            x = nx;
            y = ny;
            w = w->GetNextWP();
        }
    }
    else
    {
        // We've got one or more open ends, adjust times while keeping speeds
        if (nw and (nw->GetWPFlags() bitand WPF_TIME_LOCKED))
        {
            // Push backwards from nw (keep speeds constant)
            now = nw->GetWPArrivalTime();
            nw->GetWPLocation(&x, &y);
            w = nw->GetPrevWP();

            while (w)
            {
                w->GetWPLocation(&nx, &ny);
                d = Distance(x, y, nx, ny);
                speed = nw->GetWPSpeed();

                if (speed)
                    now -= FloatToInt32((d * CampaignHours) / speed);

                now -= w->GetWPStationTime();
                w->SetWPTimes(now);

                if (w == pw)
                    break;

                x = nx;
                y = ny;
                nw = w;
                w = w->GetPrevWP();
            }
        }
        else if (pw)
        {
            // Push forward from pw
            now = pw->GetWPDepartureTime();
            pw->GetWPLocation(&x, &y);
            w = pw->GetNextWP();

            while (w)
            {
                w->GetWPLocation(&nx, &ny);
                d = Distance(x, y, nx, ny);
                speed = w->GetWPSpeed();

                if (speed)
                    now += FloatToInt32((d * CampaignHours) / speed);

                w->SetWPTimes(now);
                now = w->GetWPDepartureTime();

                if (w == nw)
                    break;

                x = nx;
                y = ny;
                w = w->GetNextWP();
            }
        }
        else
            ShiAssert(0);
    }
}
// Native model routine from ui/src/common/waypoint.cpp
float get_air_speed(float speed, int altitude)
{
    float rsigma, pa, mach, qc, qpasl1, vcas, oper, ttheta;

    if (altitude <= 36089)
    {
        ttheta = 1.0F - 0.000006875F * altitude;
        rsigma = static_cast<float>(pow(ttheta, 4.256F));
    }
    else
    {
        ttheta = 0.7519F;
        rsigma = static_cast<float>(
            0.2971F * pow(2.718F, 0.00004806F * (36089.0F - altitude)));
    }

    mach = static_cast<float>(speed / (sqrt(ttheta) * AASLK));
    pa = ttheta * rsigma * PASL;

    if (mach <= 1.0F)
    {
        qc = ((float)pow((1.0F + 0.2F * mach * mach), 3.5F) - 1.0F) * pa;
    }
    else
    {
        qc = static_cast<float>(
            ((166.9 * mach * mach) /
                 (float)(pow((7.0F - 1.0F / (mach * mach)), 2.5F)) -
             1.0F) *
            pa);
    }

    qpasl1 = qc / PASL + 1.0F;
    vcas = static_cast<float>(1479.12F * sqrt(pow(qpasl1, 0.285714F) - 1.0F));

    if (qc > 1889.64F)
    {
        oper = static_cast<float>(
            qpasl1 * pow((7.0F - AASLK * AASLK / (vcas * vcas)), 2.5F));

        // sfr: holy shit, is this correct?
        if (oper < 0.0F)
            oper = 0.1F;

        {
            vcas = static_cast<float>(51.1987F * sqrt(oper));
        }
    }

    return vcas;
}
// Native model routine from sim/simlib/simfiltr.cpp
#ifndef FF_DETAILED_ENGINE
int SimCompare(VuEntity* ent1, VuEntity* ent2)
{
    int retval = 0;

    if (ent1 and ent2 and ent1->Id() not_eq ent2->Id())
    {
        retval = (ent2->Id() > ent1->Id() ? 1 : -1);
    }

    return (retval);
}
// Native model routine from sim/simlib/geometry.cpp
float TargetAz(FalconEntity* af1, FalconEntity* af2)
{
    float xft, yft, rx, ry, az;
    mlTrig psiTrig;

    mlSinCos(&psiTrig, af1->Yaw());

    xft = af2->XPos() - af1->XPos();
    yft = af2->YPos() - af1->YPos();

    rx = psiTrig.cos * xft + psiTrig.sin * yft;
    ry = -psiTrig.sin * xft + psiTrig.cos * yft;

    az = (float)atan2(ry, rx);

    return (az);
}
// Native model routine from sim/simlib/geometry.cpp
float TargetEl(FalconEntity* af1, FalconEntity* af2)
{
    float rx, ry, rz, el;

    rx = af2->XPos() - af1->XPos();
    ry = af2->YPos() - af1->YPos();
    rz = af2->ZPos() - af1->ZPos();

    /* sqrt returns positive, so this is cool */
    el = (float)atan2(-rz, sqrt(rx * rx + ry * ry));

    return (el);
}

#endif
RadarDataSet* radarDatFileTable = nullptr;
short NumRadarDatFileTable = 0;
static const char RADAR_DIR[] = "sim/radar";
static const char RADAR_DATASET[] = "radtypes.lst";


#define OFFSET(x) offsetof(RadarDataSet, x)
static const InputDataDesc radarDataDesc[] = {
    {"Indx", InputDataDesc::ID_INT, OFFSET(Indx), "0"},
    {"prf", InputDataDesc::ID_INT, OFFSET(prf), "0"},
    {"TimeToLock", InputDataDesc::ID_INT, OFFSET(TimeToLock), "0"},
    {"MaxTwstargets", InputDataDesc::ID_INT, OFFSET(MaxTwstargets), "0"},
    {"Timetosearch1", InputDataDesc::ID_INT, OFFSET(Timetosearch1), "0"},
    {"Timetosearch2", InputDataDesc::ID_INT, OFFSET(Timetosearch2), "0"},
    {"Timetosearch3", InputDataDesc::ID_INT, OFFSET(Timetosearch3), "0"},
    {"Timetoacuire", InputDataDesc::ID_INT, OFFSET(Timetoacuire), "0"},
    {"Timetoguide", InputDataDesc::ID_INT, OFFSET(Timetoguide), "0"},
    {"Timetocoast", InputDataDesc::ID_INT, OFFSET(Timetocoast), "0"},
    {"Rangetosearch1", InputDataDesc::ID_INT, OFFSET(Rangetosearch1), "0"},
    {"Rangetosearch2", InputDataDesc::ID_INT, OFFSET(Rangetosearch2), "0"},
    {"Rangetosearch3", InputDataDesc::ID_INT, OFFSET(Rangetosearch3), "0"},
    {"Rangetoacuire", InputDataDesc::ID_INT, OFFSET(Rangetoacuire), "0"},
    {"Rangetoguide", InputDataDesc::ID_INT, OFFSET(Rangetoguide), "0"},
    {"Sweeptimesearch1", InputDataDesc::ID_INT, OFFSET(Sweeptimesearch1), "0"},
    {"Sweeptimesearch2", InputDataDesc::ID_INT, OFFSET(Sweeptimesearch2), "0"},
    {"Sweeptimesearch3", InputDataDesc::ID_INT, OFFSET(Sweeptimesearch3), "0"},
    {"Sweeptimeacuire", InputDataDesc::ID_INT, OFFSET(Sweeptimeacuire), "0"},
    {"Sweeptimeguide", InputDataDesc::ID_INT, OFFSET(Sweeptimeguide), "0"},
    {"Sweeptimecoast", InputDataDesc::ID_INT, OFFSET(Sweeptimecoast), "0"},
    {"Timeskillfactor", InputDataDesc::ID_INT, OFFSET(Timeskillfactor), "0"},
    {"Rwrsoundsearch1", InputDataDesc::ID_INT, OFFSET(Rwrsoundsearch1), "0"},
    {"Rwrsoundsearch2", InputDataDesc::ID_INT, OFFSET(Rwrsoundsearch2), "0"},
    {"Rwrsoundsearch3", InputDataDesc::ID_INT, OFFSET(Rwrsoundsearch3), "0"},
    {"Rwrsoundacuire", InputDataDesc::ID_INT, OFFSET(Rwrsoundacuire), "0"},
    {"Rwrsoundguide", InputDataDesc::ID_INT, OFFSET(Rwrsoundguide), "0"},
    {"Rwrsymbolsearch1", InputDataDesc::ID_INT, OFFSET(Rwrsymbolsearch1), "0"},
    {"Rwrsymbolsearch2", InputDataDesc::ID_INT, OFFSET(Rwrsymbolsearch2), "0"},
    {"Rwrsymbolsearch3", InputDataDesc::ID_INT, OFFSET(Rwrsymbolsearch3), "0"},
    {"Rwrsymbolacuire", InputDataDesc::ID_INT, OFFSET(Rwrsymbolacuire), "0"},
    {"Rwrsymbolguide", InputDataDesc::ID_INT, OFFSET(Rwrsymbolguide), "0"},
    {"AirFireRate", InputDataDesc::ID_INT, OFFSET(AirFireRate), "0"},
    {"Maxmissilesintheair", InputDataDesc::ID_INT, OFFSET(Maxmissilesintheair),
     "0"},
    {"Elevationbumpamounta", InputDataDesc::ID_INT,
     OFFSET(Elevationbumpamounta), "0"},
    {"Elevationbumpamountb", InputDataDesc::ID_INT,
     OFFSET(Elevationbumpamountb), "0"},
    {"AverageSpeed", InputDataDesc::ID_INT, OFFSET(AverageSpeed), "0"},
    {"MaxAngleDiffTws", InputDataDesc::ID_FLOAT, OFFSET(MaxAngleDiffTws), "0"},
    {"MaxRangeDiffTws", InputDataDesc::ID_FLOAT, OFFSET(MaxRangeDiffTws), "0"},
    {"MaxAngleDiffSam", InputDataDesc::ID_FLOAT, OFFSET(MaxAngleDiffSam), "0"},
    {"MaxRangeDiffSam", InputDataDesc::ID_FLOAT, OFFSET(MaxRangeDiffSam), "0"},
    {"MaxNctrRange", InputDataDesc::ID_FLOAT, OFFSET(MaxNctrRange),
     "364572.66"},
    {"NctrDelta", InputDataDesc::ID_FLOAT, OFFSET(NctrDelta), "0.1"},
    {"MinEngagementAlt", InputDataDesc::ID_FLOAT, OFFSET(MinEngagementAlt),
     "300.0"},
    {"MinEngagementRange", InputDataDesc::ID_FLOAT, OFFSET(MinEngagementRange),
     "0"},
    {NULL} // must be final node
};
#undef OFFSET

static void ReadDataArray(void* dataPtr, SimlibFileClass* inputFile,
                          const InputDataDesc* desc)
{
    SimlibFileName buffer;

    while (inputFile->ReadLine(buffer, sizeof buffer) == SIMLIB_OK and
           buffer[0] not_eq 0)
    {
        ParseField(dataPtr, buffer, desc);
        buffer[0] = 0;
    }
}

void ReadHeadlessRadarData(void)
{
    int i;
    SimlibFileClass* rclist;
    SimlibFileClass* inputFile;
    SimlibFileName buffer;
    SimlibFileName fileName;
    SimlibFileName fName;

    sprintf(fileName, "%s/%s", RADAR_DIR, RADAR_DATASET);
    rclist = SimlibFileClass::Open(fileName, SIMLIB_READ);

    if (rclist == NULL)
        return;

    NumRadarDatFileTable = atoi(rclist->GetNext());

    radarDatFileTable = new RadarDataSet[NumRadarDatFileTable];

    for (i = 0; i < NumRadarDatFileTable; i++)
    {
        if (rclist->ReadLine(buffer, 80) != SIMLIB_OK) {
            if (NumRadarDatFileTable == 170 && i == 169) {
                // FF6's shipped list has 169 names despite the 170 header.
                // Match the legacy reader's last-buffer reuse without reading
                // stale input. Other truncation is a hard load error.
                radarDatFileTable[i] = radarDatFileTable[i - 1];
                continue;
            }
            throw std::runtime_error("Truncated radar dataset list");
        }

        /*-----------------*/
        /* open input file */
        /*-----------------*/
        sprintf(fName, "%s/%s.dat", RADAR_DIR, buffer);
        inputFile = SimlibFileClass::Open(fName, SIMLIB_READ);
        if (!inputFile) throw std::runtime_error("Missing radar dataset");

        F4Assert(inputFile);
        ReadDataArray(&radarDatFileTable[i], inputFile, radarDataDesc);
        inputFile->Close();
        delete inputFile;
        inputFile = NULL;
    }

    rclist->Close();
    delete rclist;
}
