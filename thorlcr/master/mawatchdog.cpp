/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

//Master Watchdog Server/Monitor

#include "platform.h"
#include <stdio.h>
#include "jlib.hpp"
#include "jmisc.hpp"
#include "thormisc.hpp"


#include "thgraphmanager.hpp"
#include "thwatchdog.hpp"
#include "mawatchdog.hpp"
#include "thcompressutil.hpp"
#include "thmastermain.hpp"
#include "thexception.hpp"
#include "thdemonserver.hpp"
#include "thgraphmaster.hpp"
#include "thorport.hpp"

#define DEFAULT_SLAVEDOWNTIMEOUT (60*5)
class CMachineStatus
{
public:
    SocketEndpoint ep;
    bool alive;
    bool markdead;
    CMachineStatus(const SocketEndpoint &_ep)
        : ep(_ep)
    {
        alive = true;
        markdead = false;
    }
    void update(HeartBeatPacketHeader &packet)
    {
        alive = true;
        if (markdead)
        {
            markdead = false;
            StringBuffer epstr;
            ep.getEndpointHostText(epstr);
            LOG(MCdebugProgress, thorJob, "Watchdog : Marking Machine as Up! [%s]", epstr.str());
        }
    }   
};


CMasterWatchdogBase::CMasterWatchdogBase() : threaded("CMasterWatchdogBase")
{
    stopped = true;
    watchdogMachineTimeout = globals->getPropInt("@slaveDownTimeout", DEFAULT_SLAVEDOWNTIMEOUT);
    if (watchdogMachineTimeout <= HEARTBEAT_INTERVAL*10)
        watchdogMachineTimeout = HEARTBEAT_INTERVAL*10;
    watchdogMachineTimeout *= 1000;
}

CMasterWatchdogBase::~CMasterWatchdogBase()
{
    stop();
    ForEachItemInRev(i, state)
    {
        CMachineStatus *mstate=(CMachineStatus *)state.item(i);
        delete mstate;
    }
}

void CMasterWatchdogBase::start()
{
    if (stopped)
    {
        PROGLOG("Starting watchdog");
        stopped = false;
        threaded.init(this);
#ifdef _WIN32
        threaded.adjustPriority(+1); // it is critical that watchdog packets get through.
#endif
    }
}

void CMasterWatchdogBase::addSlave(const SocketEndpoint &slave)
{
    synchronized block(mutex);
    CMachineStatus *mstate=new CMachineStatus(slave);
    state.append(mstate);
}

void CMasterWatchdogBase::removeSlave(const SocketEndpoint &slave)
{
    synchronized block(mutex);
    CMachineStatus *ms = findSlave(slave);
    if (ms) {
        state.zap(ms);
        delete ms;
    }
}

CMachineStatus *CMasterWatchdogBase::findSlave(const SocketEndpoint &ep)
{
    ForEachItemInRev(i, state)
    {
        CMachineStatus *mstate=(CMachineStatus *)state.item(i);
        if (mstate->ep.equals(ep))
            return mstate;
    }
    return NULL;
}


void CMasterWatchdogBase::stop()
{
    {
        synchronized block(mutex);
        if (stopped)
            return;
        stopped = true;
    }

    LOG(MCdebugProgress, thorJob, "Stopping watchdog");
#ifdef _WIN32
    threaded.adjustPriority(0); // restore to normal before stopping
#endif
    stopReading();
    threaded.join();
    LOG(MCdebugProgress, thorJob, "Stopped watchdog");
}

void CMasterWatchdogBase::checkMachineStatus()
{
    synchronized block(mutex);
    ForEachItemInRev(i, state)
    {
        CMachineStatus *mstate=(CMachineStatus *)state.item(i);
        if (!mstate->alive)
        {
            StringBuffer epstr;
            mstate->ep.getEndpointHostText(epstr);
            if (mstate->markdead)
                abortThor(MakeThorOperatorException(TE_AbortException, "Watchdog has lost contact with Thor slave: %s (Process terminated or node down?)", epstr.str()), TEC_Watchdog);
            else
            {
                mstate->markdead = true;
                LOG(MCdebugProgress, thorJob, "Watchdog : Marking Machine as Down! [%s]", epstr.str());
                //removeSlave(mstate->ep); // more TBD
            }
        }
        else {
            mstate->alive = false;
        }
    }
}

unsigned CMasterWatchdogBase::readPacket(HeartBeatPacketHeader &hb, MemoryBuffer &mb)
{
    mb.clear();
    unsigned read = readData(mb);
    if (read)
    {
        if (read < sizeof(HeartBeatPacketHeader))
        {
            IWARNLOG("Receive Monitor Packet: wrong size, got %d, less than HeartBeatPacketHeader size", read);
            return 0;
        }

        hb.deserialize(mb);
        if (read != hb.packetSize)  // check for corrupt packets
        {
            IWARNLOG("Receive Monitor Packet: wrong size, expected %d, got %d", hb.packetSize, read);
            return 0;
        }
        mb.setLength(hb.packetSize);
        return hb.packetSize;
    }
    else
        mb.clear();
    return 0;
}

void CMasterWatchdogBase::threadmain()
{
    LOG(MCdebugProgress, thorJob, "Started watchdog");
    unsigned lastbeat=msTick();
    unsigned lastcheck=lastbeat;

    retrycount = 0;
    while (!stopped)
    {
        try
        {
            HeartBeatPacketHeader hb;
            MemoryBuffer progressData;
            unsigned sz = readPacket(hb, progressData);
            if (stopped)
                break;
            else if (sz)
            {
                synchronized block(mutex);
                CMachineStatus *ms = findSlave(hb.sender);
                if (ms)
                {
                    ms->update(hb);
                    if (progressData.remaining())
                    {
                        Owned<IJobManager> jobManager = getJobManager();
                        if (jobManager)
                            jobManager->queryDeMonServer()->takeHeartBeat(progressData);
                    }
                }
                else
                {
                    StringBuffer epstr;
                    hb.sender.getEndpointHostText(epstr);
                    LOG(MCdebugProgress, thorJob, "Watchdog : Unknown Machine! [%s]", epstr.str()); //TBD
                }
            }
            unsigned now=msTick();
            if (now-lastcheck>watchdogMachineTimeout)
            {
                checkMachineStatus();
                lastcheck = msTick();
            }
            if (now-lastbeat>THORBEAT_INTERVAL)
            {
                if (retrycount<=0) retrycount=THORBEAT_RETRY_INTERVAL; else retrycount -= THORBEAT_INTERVAL;
                lastbeat = msTick();
            }
        }
        catch (IMP_Exception *e)
        {
            if (MPERR_link_closed != e->errorCode())
            {
                FLLOG(MCexception(e), thorJob, e,"Watchdog Server Exception");
                e->Release();
            }
            else
            {
                const SocketEndpoint &ep = e->queryEndpoint();
                StringBuffer epStr;
                ep.getEndpointHostText(epStr);
                abortThor(MakeThorOperatorException(TE_AbortException, "Watchdog has lost connectivity with Thor slave: %s (Process terminated or node down?)", epStr.str()), TEC_Watchdog);
            }
        }
        catch (IException *e)
        {
            FLLOG(MCexception(e), thorJob, e,"Watchdog Server Exception");
            e->Release();
            // NB: it is important to continue with master watchdog, to continue to consume packets from workers
        }
    }
}


class CMasterWatchdogUDP : public CMasterWatchdogBase
{
    ISocket *sock;
public:
    CMasterWatchdogUDP(bool startNow)
    {
        sock = ISocket::udp_create(getFixedPort(TPORT_watchdog));
        if (startNow)
            start();
    }
    ~CMasterWatchdogUDP()
    {
        ::Release(sock);
    }
    virtual unsigned readData(MemoryBuffer &mb)
    {
        size32_t read;
        try
        {
            sock->readtms(mb.reserveTruncate(UDP_DATA_MAX), sizeof(HeartBeatPacketHeader), UDP_DATA_MAX, read, watchdogMachineTimeout);
        }
        catch (IJSOCK_Exception *e)
        {
            if ((e->errorCode()!=JSOCKERR_timeout_expired)&&(e->errorCode()!=JSOCKERR_broken_pipe)&&(e->errorCode()!=JSOCKERR_graceful_close))
                throw;
            e->Release();
            return 0; // will retry
        }
        return read;
    }
    virtual void stopReading()
    {
        if (sock)
        {
            SocketEndpoint masterEp(getMasterPortBase());
            StringBuffer ipStr;
            masterEp.getHostText(ipStr);
            Owned<ISocket> sock = ISocket::udp_connect(getFixedPort(masterEp.port, TPORT_watchdog), ipStr.str());
            // send empty packet, stopped set, will cease reading
            HeartBeatPacketHeader hb;
            hb.packetSize = sizeof(HeartBeatPacketHeader);
            sock->write(&hb, sizeof(HeartBeatPacketHeader));
            sock->close();
        }
    }
};

/////////////////////

class CMasterWatchdogMP : public CMasterWatchdogBase
{
public:
    CMasterWatchdogMP(bool startNow)
    {
        if (startNow)
            start();
    }
    virtual unsigned readData(MemoryBuffer &mb)
    {
        CMessageBuffer msg;
        if (!queryNodeComm().recv(msg, RANK_ALL, MPTAG_THORWATCHDOG, NULL, watchdogMachineTimeout))
            return 0;
        mb.swapWith(msg);
        return mb.length();
    }
    virtual void stopReading()
    {
        queryNodeComm().cancel(0, MPTAG_THORWATCHDOG);
    }
};

/////////////////////

CMasterWatchdogBase *createMasterWatchdog(bool udp, bool startNow)
{
    if (udp)
        return new CMasterWatchdogUDP(startNow);
    else
        return new CMasterWatchdogMP(startNow);
}
