//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2006/03/23
// Author: Sriram Rao
//
// Copyright 2008-2012 Quantcast Corp.
// Copyright 2006-2008 Kosmix Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
//
//----------------------------------------------------------------------------

#include "ClientSM.h"

#include "ChunkManager.h"
#include "ChunkServer.h"
#include "utils.h"
#include "KfsOps.h"
#include "AtomicRecordAppender.h"
#include "DiskIo.h"

#include "common/MsgLogger.h"
#include "common/time.h"
#include "kfsio/Globals.h"
#include "kfsio/NetManager.h"
#include "qcdio/QCUtils.h"

#include <algorithm>
#include <string>
#include <sstream>

#define CLIENT_SM_LOG_STREAM_PREFIX \
    << "I" << mInstanceNum << "I " << GetPeerName() << " "
#define CLIENT_SM_LOG_STREAM(pri)  \
    KFS_LOG_STREAM(pri)  CLIENT_SM_LOG_STREAM_PREFIX
#define CLIENT_SM_LOG_STREAM_DEBUG \
    KFS_LOG_STREAM_DEBUG CLIENT_SM_LOG_STREAM_PREFIX
#define CLIENT_SM_LOG_STREAM_WARN  \
    KFS_LOG_STREAM_WARN  CLIENT_SM_LOG_STREAM_PREFIX
#define CLIENT_SM_LOG_STREAM_INFO  \
    KFS_LOG_STREAM_INFO  CLIENT_SM_LOG_STREAM_PREFIX
#define CLIENT_SM_LOG_STREAM_ERROR \
    KFS_LOG_STREAM_ERROR CLIENT_SM_LOG_STREAM_PREFIX
#define CLIENT_SM_LOG_STREAM_FATAL \
    KFS_LOG_STREAM_FATAL CLIENT_SM_LOG_STREAM_PREFIX

namespace KFS
{
using std::string;
using std::max;
using std::make_pair;
using std::list;
using KFS::libkfsio::globalNetManager;

// KFS client protocol state machine implementation.

const int kMaxCmdHeaderLength = 1 << 10;

bool     ClientSM::sTraceRequestResponseFlag         = false;
bool     ClientSM::sEnforceMaxWaitFlag               = true;
bool     ClientSM::sCloseWriteOnPendingOverQuotaFlag = false;
int      ClientSM::sMaxReqSizeDiscard                = 256 << 10;
uint64_t ClientSM::sInstanceNum                      = 10000;

inline string
ClientSM::GetPeerName()
{
    return (mNetConnection ?
        mNetConnection->GetPeerName() :
        string("not connected")
    );
}

inline /* static */ BufferManager&
ClientSM::GetBufferManager()
{
    return DiskIo::GetBufferManager();
}

inline /* static */ BufferManager*
ClientSM::FindDevBufferManager(KfsOp& op)
{
    const bool kFindFlag  = true;
    const bool kResetFlag = false;
    return op.GetDeviceBufferManager(kFindFlag, kResetFlag);
}

inline ClientSM::Client*
ClientSM::GetDevBufMgrClient(const BufferManager* bufMgr)
{
    if (! bufMgr) {
        return 0;
    }
    DevBufferManagerClient*& cli = mDevBufMgrClients[bufMgr];
    if (! cli) {
        cli = new (mDevCliMgrAllocator.allocate(1))
            DevBufferManagerClient(*this);
    }
    return cli;
}

inline void
ClientSM::PutAndResetDevBufferManager(KfsOp& op, ByteCount opBytes)
{
    const bool kFindFlag  = false;
    const bool kResetFlag = true;
    BufferManager* const devBufMgr =
        op.GetDeviceBufferManager(kFindFlag, kResetFlag);
    if (devBufMgr) {
        // Return everything back to the device buffer manager now, only count
        // pending response against global buffer manager.
        devBufMgr->Put(*GetDevBufMgrClient(devBufMgr), opBytes);
    }
}

inline void
ClientSM::SendResponse(KfsOp* op, ClientSM::ByteCount opBytes)
{
    ByteCount respBytes = mNetConnection->GetNumBytesToWrite();
    SendResponse(op);
    respBytes = max(ByteCount(0),
        mNetConnection->GetNumBytesToWrite() - respBytes);
    mPrevNumToWrite = mNetConnection->GetNumBytesToWrite();
    PutAndResetDevBufferManager(*op, opBytes);
    GetBufferManager().Put(*this, opBytes - respBytes);
}

inline static bool
IsDependingOpType(const KfsOp* op)
{
    const KfsOp_t type = op->op;
    return (
        (type == CMD_WRITE_PREPARE &&
            ! static_cast<const WritePrepareOp*>(op)->replyRequestedFlag) ||
        (type == CMD_WRITE_PREPARE_FWD &&
            ! static_cast<const WritePrepareFwdOp*>(
                op)->owner.replyRequestedFlag) ||
        type == CMD_WRITE
    );
}

/* static */ void
ClientSM::SetParameters(const Properties& prop)
{
    sTraceRequestResponseFlag = prop.getValue(
        "chunkServer.clientSM.traceRequestResponse",
        sTraceRequestResponseFlag ? 1 : 0) != 0;
    sEnforceMaxWaitFlag = prop.getValue(
        "chunkServer.clientSM.enforceMaxWait",
        sEnforceMaxWaitFlag ? 1 : 0) != 0;
    sCloseWriteOnPendingOverQuotaFlag = prop.getValue(
        "chunkServer.clientSM.closeWriteOnPendingOverQuota",
        sCloseWriteOnPendingOverQuotaFlag ? 1 : 0) != 0;
    sMaxReqSizeDiscard = prop.getValue(
        "chunkServer.clientSM.maxReqSizeDiscard",
        sMaxReqSizeDiscard);
}

ClientSM::ClientSM(NetConnectionPtr &conn)
    : KfsCallbackObj(),
      BufferManager::Client(),
      mNetConnection(conn),
      mCurOp(0),
      mOps(),
      mReservations(),
      mPendingOps(),
      mPendingSubmitQueue(),
      mRemoteSyncers(),
      mPrevNumToWrite(0),
      mRecursionCnt(0),
      mDiscardByteCnt(0),
      mInstanceNum(sInstanceNum++),
      mWOStream(),
      mDevBufMgrClients(),
      mDevBufMgr(0),
      mDevCliMgrAllocator()
{
    SET_HANDLER(this, &ClientSM::HandleRequest);
    mNetConnection->SetMaxReadAhead(kMaxCmdHeaderLength);
    mNetConnection->SetInactivityTimeout(gClientManager.GetIdleTimeoutSec());
}

ClientSM::~ClientSM()
{
    KfsOp* op;

    assert(mOps.empty() && mPendingOps.empty() && mPendingSubmitQueue.empty());
    while (!mOps.empty()) {
        op = mOps.front().first;
        mOps.pop_front();
        delete op;
    }
    while (!mPendingOps.empty()) {
        op = mPendingOps.front().dependentOp;
        mPendingOps.pop_front();
        delete op;
    }
    while (!mPendingSubmitQueue.empty()) {
        op = mPendingSubmitQueue.front().dependentOp;
        mPendingSubmitQueue.pop_front();
        delete op;
    }
    delete mCurOp;
    mCurOp = 0;
    for (DevBufferManagerClients::iterator it = mDevBufMgrClients.begin();
            it != mDevBufMgrClients.end();
            ++it) {
        assert(it->second);
        mDevCliMgrAllocator.destroy(it->second);
        mDevCliMgrAllocator.deallocate(it->second, 1);
    }
    gClientManager.Remove(this);
}

///
/// Send out the response to the client request.  The response is
/// generated by MetaRequest as per the protocol.
/// @param[in] op The request for which we finished execution.
///
void
ClientSM::SendResponse(KfsOp* op)
{
    assert(mNetConnection && op);

    const int64_t timespent = max(int64_t(0),
        globalNetManager().Now() * 1000000 - op->startTime);
    const bool    tooLong   = timespent > 5 * 1000000;
    CLIENT_SM_LOG_STREAM(
            (op->status >= 0 ||
                (op->op == CMD_SPC_RESERVE && op->status == -ENOSPC)) ?
            (tooLong ? MsgLogger::kLogLevelINFO : MsgLogger::kLogLevelDEBUG) :
            MsgLogger::kLogLevelERROR) <<
        "seq: "        << op->seq <<
        " status: "    << op->status <<
        " buffers: "   << GetByteCount() <<
        " " << op->Show() <<
        (op->statusMsg.empty() ? "" : " msg: ") << op->statusMsg <<
        (tooLong ? " RPC too long " : " took: ") <<
            timespent << " usec." <<
    KFS_LOG_EOM;

    op->Response(mWOStream.Set(mNetConnection->GetOutBuffer()));
    mWOStream.Reset();

    IOBuffer* iobuf = 0;
    int       len   = 0;
    op->ResponseContent(iobuf, len);
    mNetConnection->Write(iobuf, len);
    gClientManager.RequestDone(timespent, *op);
}

///
/// Generic event handler.  Decode the event that occurred and
/// appropriately extract out the data and deal with the event.
/// @param[in] code: The type of event that occurred
/// @param[in] data: Data being passed in relative to the event that
/// occurred.
/// @retval 0 to indicate successful event handling; -1 otherwise.
///
int
ClientSM::HandleRequest(int code, void* data)
{
    assert(mRecursionCnt >= 0 && mNetConnection);
    mRecursionCnt++;

    switch (code) {
    case EVENT_NET_READ: {
        if (IsWaiting() || mDevBufMgr) {
            CLIENT_SM_LOG_STREAM_DEBUG <<
                "spurious read:"
                " cur op: "  << KfsOp::ShowOp(mCurOp) <<
                " buffers: " << GetByteCount() <<
                " waiting for " << (mDevBufMgr ? "dev. " : "") <<
                "io buffers " <<
            KFS_LOG_EOM;
            mNetConnection->SetMaxReadAhead(0);
            break;
        }
        // We read something from the network.  Run the RPC that
        // came in.
        int       cmdLen = 0;
        bool      gotCmd = false;
        IOBuffer& iobuf  = mNetConnection->GetInBuffer();
        assert(&iobuf == data);
        while ((mCurOp || IsMsgAvail(&iobuf, &cmdLen)) &&
                (gotCmd = HandleClientCmd(&iobuf, cmdLen))) {
            cmdLen = 0;
            gotCmd = false;
        }
        if (! mCurOp) {
            int hdrsz;
            if (cmdLen > 0 && ! gotCmd) {
                CLIENT_SM_LOG_STREAM_ERROR <<
                    " failed to parse request, closing connection;"
                    " header size: "    << cmdLen <<
                    " read available: " << iobuf.BytesConsumable() <<
                KFS_LOG_EOM;
                gClientManager.BadRequest();
            } else if ((hdrsz = iobuf.BytesConsumable()) > MAX_RPC_HEADER_LEN) {
                CLIENT_SM_LOG_STREAM_ERROR <<
                    " exceeded max request header size: " << hdrsz <<
                    " limit: " << MAX_RPC_HEADER_LEN <<
                    ", closing connection" <<
                KFS_LOG_EOM;
                gClientManager.BadRequestHeader();
            } else {
                break;
            }
            iobuf.Clear();
            mNetConnection->Close();
        }
        break;
    }

    case EVENT_NET_WROTE: {
        const int rem = mNetConnection->GetNumBytesToWrite();
        GetBufferManager().Put(*this, mPrevNumToWrite - rem);
        mPrevNumToWrite = rem;
        break;
    }

    case EVENT_CMD_DONE: {
        // An op finished execution.  Send response back in FIFO
        if (! data) {
            die("invalid null op completion");
            return -1;
        }
        KfsOp* op = reinterpret_cast<KfsOp*>(data);
        gChunkServer.OpFinished();
        op->done = true;
        assert(! mOps.empty());
        if (sTraceRequestResponseFlag) {
            IOBuffer::OStream os;
            op->Response(os);
            IOBuffer::IStream is(os);
            string line;
            while (getline(is, line)) {
                CLIENT_SM_LOG_STREAM_DEBUG <<
                    "response: " << line <<
                KFS_LOG_EOM;
            }
        }
        while (! mOps.empty()) {
            KfsOp* const qop = mOps.front().first;
            if (! qop->done) {
                if (! op) {
                    break;
                }
                if (! IsDependingOpType(op)) {
                    OpsQueue::iterator i;
                    for (i = mOps.begin(); i != mOps.end() && op != i->first; ++i)
                        {}
                    assert(i != mOps.end() && op == i->first);
                    assert(mPendingOps.empty() || op != mPendingOps.front().op);
                    if (i != mOps.end()) {
                        SendResponse(op, i->second);
                    }
                    if (i != mOps.end()) {
                        mOps.erase(i);
                        OpFinished(op);
                    }
                    delete op;
                    op = 0;
                } else {
                    CLIENT_SM_LOG_STREAM_DEBUG <<
                        "previous op still pending: " <<
                        qop->Show() << "; deferring reply to: " <<
                        op->Show() <<
                    KFS_LOG_EOM;
                }
                break;
            }
            if (qop == op) {
                op = 0;
            }
            SendResponse(qop, mOps.front().second);
            mOps.pop_front();
            OpFinished(qop);
            delete qop;
        }
        if (op) {
            // Waiting for other op. Disk io done -- put device buffers.
            OpsQueue::iterator i;
            for (i = mOps.begin(); i != mOps.end() && op != i->first; ++i)
                {}
            if (i == mOps.end()) {
                die("deferred reply op is not in the queue");
            } else {
                PutAndResetDevBufferManager(*op, i->second);
            }
        }
        break;
    }

    case EVENT_INACTIVITY_TIMEOUT:
    case EVENT_NET_ERROR:
        CLIENT_SM_LOG_STREAM_DEBUG <<
            "closing connection"
            " due to " << (code == EVENT_INACTIVITY_TIMEOUT ?
                "inactivity timeout" : "network error") <<
            ", socket error: " <<
                QCUtils::SysError(mNetConnection->GetSocketError()) <<
            ", pending read: " << mNetConnection->GetNumBytesToRead() <<
            " write: " << mNetConnection->GetNumBytesToWrite() <<
        KFS_LOG_EOM;
        mNetConnection->Close();
        if (mCurOp) {
            if (mDevBufMgr) {
                GetDevBufMgrClient(mDevBufMgr)->CancelRequest();
            } else {
                PutAndResetDevBufferManager(*mCurOp, GetWaitingForByteCount());
                CancelRequest();
            }
            delete mCurOp;
            mCurOp = 0;
        }
        break;

    default:
        assert(!"Unknown event");
        break;
    }

    assert(mRecursionCnt > 0);
    if (mRecursionCnt == 1) {
        mNetConnection->StartFlush();
        if (mNetConnection->IsGood()) {
            // Enforce 5 min timeout if connection has pending read and write.
            mNetConnection->SetInactivityTimeout(
                (mNetConnection->HasPendingRead() ||
                    mNetConnection->IsWriteReady()) ?
                gClientManager.GetIoTimeoutSec() :
                gClientManager.GetIdleTimeoutSec());
        } else {
            list<RemoteSyncSMPtr> serversToRelease;

            mRemoteSyncers.swap(serversToRelease);
            // get rid of the connection to all the peers in daisy chain;
            // if there were any outstanding ops, they will all come back
            // to this method as EVENT_CMD_DONE and we clean them up above.
            ReleaseAllServers(serversToRelease);
            ReleaseChunkSpaceReservations();
            mRecursionCnt--;
            // if there are any disk ops, wait for the ops to finish
            SET_HANDLER(this, &ClientSM::HandleTerminate);
            HandleTerminate(EVENT_NET_ERROR, NULL);
            // this can be deleted, return now.
            return 0;
        }
    }
    mRecursionCnt--;
    return 0;
}

///
/// Termination handler.  For the client state machine, we could have
/// ops queued at the logger.  So, for cleanup wait for all the
/// outstanding ops to finish and then delete this.  In this state,
/// the only event that gets raised is that an op finished; anything
/// else is bad.
///
int
ClientSM::HandleTerminate(int code, void* data)
{
    switch (code) {

    case EVENT_CMD_DONE: {
        assert(data);
        KfsOp* op = reinterpret_cast<KfsOp*>(data);
        gChunkServer.OpFinished();
        // An op finished execution.  Send a response back
        op->done = true;
        if (op != mOps.front().first) {
            break;
        }
        while (!mOps.empty()) {
            op = mOps.front().first;
            if (!op->done) {
                break;
            }
            const ByteCount opBytes = mOps.front().second;
            PutAndResetDevBufferManager(*op, opBytes);
            GetBufferManager().Put(*this, opBytes);
            OpFinished(op);
            // we are done with the op
            mOps.pop_front();
            delete op;
        }
        break;
    }

    case EVENT_INACTIVITY_TIMEOUT:
    case EVENT_NET_ERROR:
        // clean things up
        break;

    default:
        assert(!"Unknown event");
        break;
    }

    if (mOps.empty()) {
        // all ops are done...so, now, we can nuke ourself.
        assert(mPendingOps.empty());
        if (mNetConnection) {
            mNetConnection->SetOwningKfsCallbackObj(0);
        }
        delete this;
        return 1;
    }
    return 0;
}

inline static BufferManager::ByteCount
IoRequestBytes(BufferManager::ByteCount numBytes, bool forwardFlag = false)
{
    BufferManager::ByteCount ret = IOBufferData::GetDefaultBufferSize();
    if (forwardFlag) {
        // ret += (numBytes + ret - 1) / ret * ret;
    }
    if (numBytes > 0) {
        ret += ((numBytes + CHECKSUM_BLOCKSIZE - 1) /
            CHECKSUM_BLOCKSIZE * CHECKSUM_BLOCKSIZE);
    }
    return ret;
}

bool
ClientSM::GetWriteOp(KfsOp* wop, int align, int numBytes,
    IOBuffer* iobuf, IOBuffer*& ioOpBuf, bool forwardFlag)
{
    const int nAvail = iobuf->BytesConsumable();
    if (! mCurOp || mDevBufMgr) {
        mDevBufMgr = mCurOp ? 0 : FindDevBufferManager(*wop);
        Client* const   mgrCli        = GetDevBufMgrClient(mDevBufMgr);
        const ByteCount bufferBytes   = IoRequestBytes(numBytes, forwardFlag);
        BufferManager&  bufMgr        = GetBufferManager();
        bool            overQuotaFlag = false;
        if (! mCurOp && (numBytes < 0 || numBytes > min(
                        mDevBufMgr ? mDevBufMgr->GetMaxClientQuota() :
                            (ByteCount(1) << 31),
                        min(bufMgr.GetMaxClientQuota(),
                            (ByteCount)gChunkManager.GetMaxIORequestSize())) ||
                (overQuotaFlag = sCloseWriteOnPendingOverQuotaFlag &&
                    (bufMgr.IsOverQuota(*this, bufferBytes) ||
                    (mDevBufMgr &&
                    mDevBufMgr->IsOverQuota(*mgrCli, bufferBytes)))))) {
            // Over quota can theoretically occur if the quota set unreasonably
            // low, or if client uses the same connection to do both read and
            // write simultaneously. Presently client never attempts to do
            // concurrent read and write using the same connection.
            CLIENT_SM_LOG_STREAM_ERROR <<
                "seq: " << wop->seq <<
                " invalid write request size: " << bufferBytes <<
                " buffers: " << GetByteCount() <<
                (overQuotaFlag ? " over quota" : "") <<
                ", closing connection" <<
            KFS_LOG_EOM;
            delete wop;
            return false;
        }
        if (! mCurOp && nAvail <= numBytes) {
            // Move write data to the start of the buffers, to make it
            // aligned. Normally only one buffer will be created.
            const int off(align % IOBufferData::GetDefaultBufferSize());
            if (off > 0) {
                IOBuffer buf;
                buf.ReplaceKeepBuffersFull(iobuf, off, nAvail);
                iobuf->Move(&buf);
                iobuf->Consume(off);
            } else {
                iobuf->MakeBuffersFull();
            }
        }
        mDiscardByteCnt = 0;
        mCurOp          = wop;
        if (mDevBufMgr && mDevBufMgr->GetForDiskIo(*mgrCli, bufferBytes)) {
            mDevBufMgr = 0;
        }
        if (mDevBufMgr || ! bufMgr.GetForDiskIo(*this, bufferBytes)) {
            const bool failFlag = numBytes <= sMaxReqSizeDiscard - nAvail &&
                FailIfExceedsWait(bufMgr, 0, *wop, bufferBytes);
            const BufferManager& mgr = mDevBufMgr ? *mDevBufMgr : bufMgr;
            CLIENT_SM_LOG_STREAM_DEBUG <<
                "seq: " << wop->seq <<
                " request for: " << bufferBytes << " bytes denied" <<
                (mDevBufMgr ? " by dev." : "") <<
                " cur: "   << GetByteCount() <<
                " total: " << mgr.GetTotalByteCount() <<
                " used: "  << mgr.GetUsedByteCount() <<
                " bufs: "  << mgr.GetFreeBufferCount() <<
                " op: " << wop->Show() <<
                (failFlag ? "exceeds max wait" : " waiting for buffers") <<
            KFS_LOG_EOM;
            if (failFlag) {
                mDiscardByteCnt = numBytes;
            } else {
                mNetConnection->SetMaxReadAhead(0);
                return false;
            }
        }
    }
    if (mDiscardByteCnt > 0) {
        mDiscardByteCnt -= iobuf->Consume(mDiscardByteCnt);
        if (mDiscardByteCnt > 0) {
            mNetConnection->SetMaxReadAhead(
                min(mDiscardByteCnt, 2 * kMaxCmdHeaderLength));
            return false;
        }
        if (wop->status >= 0) {
            wop->status = -ESERVERBUSY;
        }
        mDiscardByteCnt = 0;
        mCurOp          = 0;
        return true;
    }
    if (nAvail < numBytes) {
        mNetConnection->SetMaxReadAhead(numBytes - nAvail);
        // we couldn't process the command...so, wait
        return false;
    }
    if (ioOpBuf) {
        ioOpBuf->Clear();
    } else {
        ioOpBuf = new IOBuffer();
    }
    if (nAvail != numBytes) {
        assert(nAvail > numBytes);
        const int off(align % IOBufferData::GetDefaultBufferSize());
        ioOpBuf->ReplaceKeepBuffersFull(iobuf, off, numBytes);
        if (off > 0) {
            ioOpBuf->Consume(off);
        }
    } else {
        ioOpBuf->Move(iobuf);
    }
    mCurOp = 0;
    mNetConnection->SetMaxReadAhead(kMaxCmdHeaderLength);
    return true;
}

bool
ClientSM::FailIfExceedsWait(
    BufferManager&         bufMgr,
    BufferManager::Client* mgrCli,
    KfsOp&                 op,
    int64_t                bufferBytes)
{
    if (! sEnforceMaxWaitFlag || op.maxWaitMillisec <= 0) {
        return false;
    }
    const int64_t maxWait        = op.maxWaitMillisec * 1000;
    const bool    devMgrWaitFlag = mDevBufMgr != 0 && mgrCli != 0;
    const int64_t curWait        = bufMgr.GetWaitingAvgUsecs() +
        (devMgrWaitFlag ? mDevBufMgr->GetWaitingAvgUsecs() : int64_t(0));
    if (curWait <= maxWait ||
            microseconds() + curWait < op.startTime + maxWait) {
        return false;
    }
    CLIENT_SM_LOG_STREAM_DEBUG <<
        " exceeded wait:"
        " current: " << curWait <<
        " max: "     << maxWait <<
        " op: "      << op.Show() <<
    KFS_LOG_EOM;
    op.status         = -ESERVERBUSY;
    op.statusMsg      = "exceeds max wait";
    if(devMgrWaitFlag) {
        mgrCli->CancelRequest();
    } else {
        PutAndResetDevBufferManager(op, bufferBytes);
        CancelRequest();
    }
    gClientManager.WaitTimeExceeded();
    return true;
}

///
/// We have a command in a buffer.  It is possible that we don't have
/// everything we need to execute it (for example, for a write we may
/// not have received all the data the client promised).  So, parse
/// out the command and if we have everything execute it.
///
bool
ClientSM::HandleClientCmd(IOBuffer* iobuf, int cmdLen)
{
    KfsOp* op = mCurOp;

    assert(op ? cmdLen == 0 : cmdLen > 0);
    if (! op) {
        if (sTraceRequestResponseFlag) {
            IOBuffer::IStream is(*iobuf, cmdLen);
            string line;
            while (getline(is, line)) {
                CLIENT_SM_LOG_STREAM_DEBUG <<
                    "request: " << line <<
                KFS_LOG_EOM;
            }
        }
        if (ParseCommand(*iobuf, cmdLen, &op) != 0) {
            assert(! op);
            IOBuffer::IStream is(*iobuf, cmdLen);
            string line;
            int    maxLines = 64;
            while (--maxLines >= 0 && getline(is, line)) {
                CLIENT_SM_LOG_STREAM_ERROR <<
                    "invalid request: " << line <<
                KFS_LOG_EOM;
            }
            iobuf->Consume(cmdLen);
            // got a bogus command
            return false;
        }
    }

    iobuf->Consume(cmdLen);
    ByteCount bufferBytes = -1;
    if (op->op == CMD_WRITE_PREPARE) {
        WritePrepareOp* const wop = static_cast<WritePrepareOp*>(op);
        assert(! wop->dataBuf);
        const bool kForwardFlag = false; // The forward always share the buffers.
        if (! GetWriteOp(wop, wop->offset, (int)wop->numBytes,
                iobuf, wop->dataBuf, kForwardFlag)) {
            return false;
        }
        bufferBytes = op->status >= 0 ? IoRequestBytes(wop->numBytes) : 0;
    } else if (op->op == CMD_RECORD_APPEND) {
        RecordAppendOp* const waop = static_cast<RecordAppendOp*>(op);
        IOBuffer*  opBuf       = &waop->dataBuf;
        bool       forwardFlag = false;
        const int  align       = mCurOp ? 0 :
            gAtomicRecordAppendManager.GetAlignmentAndFwdFlag(
                waop->chunkId, forwardFlag);
        if (! GetWriteOp(
                waop,
                align,
                (int)waop->numBytes,
                iobuf,
                opBuf,
                forwardFlag
            )) {
            return false;
        }
        assert(opBuf == &waop->dataBuf);
        bufferBytes = op->status >= 0 ? IoRequestBytes(waop->numBytes) : 0;
    }
    CLIENT_SM_LOG_STREAM_DEBUG <<
        "got: seq: " << op->seq << " " << op->Show() <<
    KFS_LOG_EOM;

    bool         submitResponseFlag = op->status < 0;
    kfsChunkId_t chunkId  = 0;
    int64_t      reqBytes = 0;
    if (! submitResponseFlag &&
            bufferBytes < 0 && op->IsChunkReadOp(reqBytes, chunkId) &&
            reqBytes >= 0) {
        bufferBytes = reqBytes + IoRequestBytes(0); // 1 buffer for reply header
        if (! mCurOp || mDevBufMgr) {
            mDevBufMgr = mCurOp ? 0 : FindDevBufferManager(*op);
            Client* const  mgrCli = GetDevBufMgrClient(mDevBufMgr);
            BufferManager& bufMgr = GetBufferManager();
            if (! mCurOp && (bufMgr.IsOverQuota(*this, bufferBytes) ||
                    (mDevBufMgr &&
                    mDevBufMgr->IsOverQuota(*mgrCli, bufferBytes)))) {
                CLIENT_SM_LOG_STREAM_ERROR <<
                    " bad read request size: " << bufferBytes <<
                    " need: " << bufferBytes <<
                    " buffers: " << GetByteCount() <<
                    " over buffer quota" <<
                    " " << op->Show() <<
                KFS_LOG_EOM;
                op->status         = -EAGAIN;
                op->statusMsg      = "over io buffers quota";
                submitResponseFlag = true;
            } else {
                if (mDevBufMgr &&
                        mDevBufMgr->GetForDiskIo(*mgrCli, bufferBytes)) {
                    mDevBufMgr = 0;
                }
                if (mDevBufMgr || ! bufMgr.GetForDiskIo(*this, bufferBytes)) {
                    mCurOp = op;
                    submitResponseFlag =
                        FailIfExceedsWait(bufMgr, mgrCli, *op, bufferBytes);
                    const BufferManager& mgr =
                        mDevBufMgr ? *mDevBufMgr : bufMgr;
                    CLIENT_SM_LOG_STREAM_DEBUG <<
                        "request for: " << bufferBytes << " bytes denied" <<
                            (mDevBufMgr ? " by dev." : "") <<
                        " cur: "   << GetByteCount() <<
                        " total: " << mgr.GetTotalByteCount() <<
                        " used: "  << mgr.GetUsedByteCount() <<
                        " bufs: "  << mgr.GetFreeBufferCount() <<
                        " op: "    << op->Show() <<
                        (submitResponseFlag ?
                            "exceeds max wait" : " waiting for buffers") <<
                    KFS_LOG_EOM;
                    if (! submitResponseFlag) {
                        mNetConnection->SetMaxReadAhead(0);
                        return false;
                    }
                }
            }
            mNetConnection->SetMaxReadAhead(kMaxCmdHeaderLength);
        }
        mCurOp = 0;
        if (! gChunkManager.IsChunkReadable(chunkId)) {
            // Do not allow dirty reads.
            op->statusMsg = "chunk not readable";
            op->status    = -EAGAIN;
            submitResponseFlag = true;
            CLIENT_SM_LOG_STREAM_ERROR <<
                " read request for chunk: " << chunkId <<
                " denied: " << op->statusMsg <<
            KFS_LOG_EOM;
        }
    }

    if (bufferBytes < 0) {
        assert(
            op->op != CMD_WRITE_PREPARE &&
            op->op != CMD_RECORD_APPEND &&
            op->op != CMD_READ
        );
        // This is needed to account for large number of small responses to
        // prevent out of buffers in the case where the client queues requests
        // but doesn't read replies.
        // To speedup append status recovery give record append status inquiry a
        // "free pass", if there are no ops pending and connection input and
        // output buffers are empty. This should be the normal case as clients
        // create new connection to do status inquiry. There is virtually
        // no danger of running out of buffers: the reply size is small enough
        // to fit into the socket buffer, and free up the io buffer immediately.
        // Since the op is synchronous and doesn't involve disk io or forwarding
        // the same io buffer that was just freed by IOBuffer::Consume() the
        // the above should be re-used for send, and freed immediately as the
        // kernel's socket buffer is expected to have at least around 1K
        // available.
        bufferBytes = (op->op == CMD_GET_RECORD_APPEND_STATUS &&
                ! mCurOp &&
                mOps.empty() &&
                GetByteCount() <= 0 &&
                ! IsWaiting() &&
                mNetConnection->GetOutBuffer().IsEmpty() &&
                mNetConnection->GetInBuffer().IsEmpty()
            ) ? ByteCount(0) : IoRequestBytes(0);
        if (! mCurOp) {
            BufferManager& bufMgr = GetBufferManager();
            if (! bufMgr.Get(*this, bufferBytes)) {
                mCurOp = op;
                submitResponseFlag =
                    FailIfExceedsWait(bufMgr, 0, *op, bufferBytes);
                CLIENT_SM_LOG_STREAM_DEBUG <<
                    "request for: " << bufferBytes << " bytes denied" <<
                    " cur: "   << GetByteCount() <<
                    " total: " << bufMgr.GetTotalByteCount() <<
                    " used: "  << bufMgr.GetUsedByteCount() <<
                    " bufs: "  << bufMgr.GetFreeBufferCount() <<
                    " op: "    << op->Show() <<
                    (submitResponseFlag ?
                        "exceeds max wait" : " waiting for buffers") <<
                KFS_LOG_EOM;
                if (! submitResponseFlag) {
                    mNetConnection->SetMaxReadAhead(0);
                    return false;
                }
            }
        }
        mNetConnection->SetMaxReadAhead(kMaxCmdHeaderLength);
        mCurOp = 0;
    }

    op->clientSMFlag = true;
    if (op->op == CMD_WRITE_SYNC) {
        // make the write sync depend on a previous write
        KfsOp* w = 0;
        for (OpsQueue::iterator i = mOps.begin(); i != mOps.end(); i++) {
            if (IsDependingOpType(i->first)) {
                w = i->first;
            }
        }
        if (w) {
            op->clnt = this;
            mPendingOps.push_back(OpPair(w, op));
            CLIENT_SM_LOG_STREAM_DEBUG <<
                "keeping write-sync (" << op->seq <<
                ") pending and depends on " << w->seq <<
            KFS_LOG_EOM;
            return true;
        } else {
            CLIENT_SM_LOG_STREAM_DEBUG <<
                "write-sync is being pushed down; no writes left, "
                << mOps.size() << " ops left" <<
            KFS_LOG_EOM;
        }
    }

    mOps.push_back(make_pair(op, bufferBytes));
    op->clnt = this;
    gChunkServer.OpInserted();
    if (submitResponseFlag) {
        HandleRequest(EVENT_CMD_DONE, op);
    } else {
        SubmitOp(op);
    }
    return true;
}

void
ClientSM::OpFinished(KfsOp* doneOp)
{
    // Multiple ops could be waiting for a single op to finish.
    //
    // Do not run pending submit queue here, if it is not empty.
    // If pending submit is not empty here, then this is recursive call. Just
    // add the op to the pending submit queue and let the caller run the queue.
    // This is need to send responses in the request order, and to limit the
    // recursion depth.
    const bool runPendingSubmitQueueFlag = mPendingSubmitQueue.empty();
    while (! mPendingOps.empty()) {
        OpPair& p = mPendingOps.front();
        if (p.op != doneOp) {
            break;
        }
        CLIENT_SM_LOG_STREAM_DEBUG <<
            "submitting write-sync (" << p.dependentOp->seq <<
            ") since " << p.op->seq << " finished" <<
        KFS_LOG_EOM;
        mPendingSubmitQueue.splice(mPendingSubmitQueue.end(),
            mPendingOps, mPendingOps.begin());
    }
    if (! runPendingSubmitQueueFlag) {
        return;
    }
    while (! mPendingSubmitQueue.empty()) {
        KfsOp* const op = mPendingSubmitQueue.front().dependentOp;
        mPendingSubmitQueue.pop_front();
        gChunkServer.OpInserted();
        mOps.push_back(make_pair(op, 0));
        SubmitOp(op);
    }
}

void
ClientSM::ReleaseChunkSpaceReservations()
{
    for (ChunkSpaceResMap::iterator iter = mReservations.begin();
         iter != mReservations.end(); iter++) {
        gAtomicRecordAppendManager.ChunkSpaceRelease(
            iter->first.chunkId, iter->first.transactionId, iter->second);
    }
}

RemoteSyncSMPtr
ClientSM::FindServer(const ServerLocation &loc, bool connect)
{
    return KFS::FindServer(mRemoteSyncers, loc, connect);
}

void
ClientSM::GrantedSelf(ClientSM::ByteCount byteCount, bool devBufManagerFlag)
{
    CLIENT_SM_LOG_STREAM_DEBUG <<
        "granted: " << (devBufManagerFlag ? "by dev. " : "") <<
        byteCount << " op: " << KfsOp::ShowOp(mCurOp) <<
        " dev. mgr: " << (const void*)mDevBufMgr <<
    KFS_LOG_EOM;
    assert(devBufManagerFlag == (mDevBufMgr != 0));
    if (! mNetConnection) {
        return;
    }
    if (mCurOp) {
        ClientSM::HandleClientCmd(&(mNetConnection->GetInBuffer()), 0);
    } else {
        mNetConnection->SetMaxReadAhead(kMaxCmdHeaderLength);
    }
}
}
