#pragma once

#include "public.h"

#include "profile_log_events.h"
#include "tablet_database.h"
#include "tablet_private.h"

#include <cloud/filestore/libs/diagnostics/profile_log.h>
#include <cloud/filestore/libs/service/request.h>
#include <cloud/filestore/libs/storage/api/tablet.h>
#include <cloud/filestore/libs/storage/core/request_info.h>
#include <cloud/filestore/libs/storage/tablet/model/block.h>
#include <cloud/filestore/libs/storage/tablet/model/block_buffer.h>
#include <cloud/filestore/libs/storage/tablet/model/range.h>
#include <cloud/filestore/libs/storage/tablet/model/range_locks.h>
#include <cloud/filestore/libs/storage/tablet/protos/tablet.pb.h>

#include <cloud/storage/core/libs/common/error.h>

#include <util/folder/pathsplit.h>
#include <util/generic/hash_set.h>
#include <util/generic/intrlist.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>
#include <util/generic/utility.h>
#include <util/generic/vector.h>

namespace NCloud::NFileStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

#define FILESTORE_VALIDATE_TX_SESSION(event, args)                             \
    if (auto session =                                                         \
            FindSession(args.ClientId, args.SessionId, args.SessionSeqNo);     \
        !session)                                                              \
    {                                                                          \
        args.Error = ErrorInvalidSession(                                      \
            args.ClientId,                                                     \
            args.SessionId,                                                    \
            args.SessionSeqNo);                                                \
        return true;                                                           \
    }                                                                          \
// FILESTORE_VALIDATE_TX_SESSION

#define FILESTORE_VALIDATE_DUPTX_SESSION(event, args)                          \
    FILESTORE_VALIDATE_TX_SESSION(event, args)                                 \
    else if (auto entry = session->LookupDupEntry(args.RequestId)) {           \
        args.Error = ErrorDuplicate();                                         \
        return true;                                                           \
    }                                                                          \
// FILESTORE_VALIDATE_TX_SESSION

#define FILESTORE_VALIDATE_TX_ERROR(event, args)                               \
    if (FAILED(args.Error.GetCode())) {                                        \
        return;                                                                \
    }                                                                          \
// FILESTORE_VALIDATE_TX_ERROR

////////////////////////////////////////////////////////////////////////////////

#define FILESTORE_TABLET_TRANSACTIONS(xxx, ...)                                \
    xxx(InitSchema,                         __VA_ARGS__)                       \
    xxx(LoadState,                          __VA_ARGS__)                       \
    xxx(UpdateConfig,                       __VA_ARGS__)                       \
                                                                               \
    xxx(CreateSession,                      __VA_ARGS__)                       \
    xxx(ResetSession,                       __VA_ARGS__)                       \
    xxx(DestroySession,                     __VA_ARGS__)                       \
                                                                               \
    xxx(CreateCheckpoint,                   __VA_ARGS__)                       \
    xxx(DeleteCheckpoint,                   __VA_ARGS__)                       \
                                                                               \
    xxx(ResolvePath,                        __VA_ARGS__)                       \
    xxx(CreateNode,                         __VA_ARGS__)                       \
    xxx(UnlinkNode,                         __VA_ARGS__)                       \
    xxx(RenameNode,                         __VA_ARGS__)                       \
    xxx(AccessNode,                         __VA_ARGS__)                       \
    xxx(ListNodes,                          __VA_ARGS__)                       \
    xxx(ReadLink,                           __VA_ARGS__)                       \
                                                                               \
    xxx(SetNodeAttr,                        __VA_ARGS__)                       \
    xxx(GetNodeAttr,                        __VA_ARGS__)                       \
    xxx(SetNodeXAttr,                       __VA_ARGS__)                       \
    xxx(GetNodeXAttr,                       __VA_ARGS__)                       \
    xxx(ListNodeXAttr,                      __VA_ARGS__)                       \
    xxx(RemoveNodeXAttr,                    __VA_ARGS__)                       \
                                                                               \
    xxx(CreateHandle,                       __VA_ARGS__)                       \
    xxx(DestroyHandle,                      __VA_ARGS__)                       \
                                                                               \
    xxx(AcquireLock,                        __VA_ARGS__)                       \
    xxx(ReleaseLock,                        __VA_ARGS__)                       \
    xxx(TestLock,                           __VA_ARGS__)                       \
                                                                               \
    xxx(ReadData,                           __VA_ARGS__)                       \
    xxx(WriteData,                          __VA_ARGS__)                       \
    xxx(WriteBatch,                         __VA_ARGS__)                       \
    xxx(AllocateData,                       __VA_ARGS__)                       \
                                                                               \
    xxx(AddBlob,                            __VA_ARGS__)                       \
    xxx(Cleanup,                            __VA_ARGS__)                       \
    xxx(Compaction,                         __VA_ARGS__)                       \
    xxx(DeleteGarbage,                      __VA_ARGS__)                       \
    xxx(DumpCompactionRange,                __VA_ARGS__)                       \
    xxx(FlushBytes,                         __VA_ARGS__)                       \
    xxx(TrimBytes,                          __VA_ARGS__)                       \
    xxx(TruncateCompleted,                  __VA_ARGS__)                       \
    xxx(TruncateRange,                      __VA_ARGS__)                       \
    xxx(ZeroRange,                          __VA_ARGS__)                       \
                                                                               \
    xxx(FilterAliveNodes,                   __VA_ARGS__)                       \
    xxx(ChangeStorageConfig,                __VA_ARGS__)                       \
// FILESTORE_TABLET_TRANSACTIONS

////////////////////////////////////////////////////////////////////////////////

struct TSessionAware
{
    const TString ClientId;
    const TString FileSystemId;
    const TString SessionId;
    const ui64 RequestId;
    const ui64 SessionSeqNo;

    NProto::TError Error;

    template<typename T>
    explicit TSessionAware(const T& request) noexcept
        : ClientId(GetClientId(request))
        , FileSystemId(GetFileSystemId(request))
        , SessionId(GetSessionId(request))
        , RequestId(GetRequestId(request))
        , SessionSeqNo(GetSessionSeqNo(request))
    {}
};

struct TProfileAware {
    NProto::TProfileLogRequestInfo ProfileLogRequest;

    explicit TProfileAware(EFileStoreSystemRequest requestType) noexcept
    {
        ProfileLogRequest.SetRequestType(static_cast<ui32>(requestType));
    }

protected:
    void Clear()
    {
        const ui32 requestType = ProfileLogRequest.GetRequestType();

        ProfileLogRequest.Clear();
        ProfileLogRequest.SetRequestType(requestType);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TWriteRequest
    : public TIntrusiveListItem<TWriteRequest>
    , public TSessionAware
{
    const TRequestInfoPtr RequestInfo;
    const ui64 Handle;
    const TByteRange ByteRange;
    const IBlockBufferPtr Buffer;

    ui64 NodeId = InvalidNodeId;

    TWriteRequest(
            TRequestInfoPtr requestInfo,
            const NProto::TWriteDataRequest& request,
            TByteRange byteRange,
            IBlockBufferPtr buffer)
        : TSessionAware(request)
        , RequestInfo(std::move(requestInfo))
        , Handle(request.GetHandle())
        , ByteRange(byteRange)
        , Buffer(std::move(buffer))
    {}
};

using TWriteRequestList = TIntrusiveListWithAutoDelete<TWriteRequest, TDelete>;

////////////////////////////////////////////////////////////////////////////////

struct TNodeOps
{
    template <typename T>
    static auto GetNodeId(const T& value)
    {
        return value;
    }

    static auto GetNodeId(const TIndexTabletDatabase::TNode& node)
    {
        return node.NodeId;
    }

    struct TNodeSetHash
    {
        template <typename T>
        size_t operator ()(const T& value) const noexcept
        {
            return IntHash(GetNodeId(value));
        }
    };

    struct TNodeSetEqual
    {
        template <typename T1, typename T2>
        bool operator ()(const T1& lhs, const T2& rhs) const noexcept
        {
            return GetNodeId(lhs) == GetNodeId(rhs);
        }
    };
};

using TNodeSet = THashSet<
    TIndexTabletDatabase::TNode,
    TNodeOps::TNodeSetHash,
    TNodeOps::TNodeSetEqual>;

////////////////////////////////////////////////////////////////////////////////

struct TTxIndexTablet
{
    //
    // InitSchema
    //

    struct TInitSchema
    {
        // actually unused, needed in tablet_tx.h to avoid sophisticated
        // template tricks
        const TRequestInfoPtr RequestInfo;

        void Clear()
        {
            // nothing to do
        }
    };

    //
    // LoadState
    //

    struct TLoadState
    {
        // actually unused, needed in tablet_tx.h to avoid sophisticated
        // template tricks
        const TRequestInfoPtr RequestInfo;

        NProto::TFileSystem FileSystem;
        NProto::TFileSystemStats FileSystemStats;
        NCloud::NProto::TTabletStorageInfo TabletStorageInfo;
        TMaybe<TIndexTabletDatabase::TNode> RootNode;
        TVector<NProto::TSession> Sessions;
        TVector<NProto::TSessionHandle> Handles;
        TVector<NProto::TSessionLock> Locks;
        TVector<TIndexTabletDatabase::TFreshBytesEntry> FreshBytes;
        TVector<TIndexTabletDatabase::TFreshBlock> FreshBlocks;
        TVector<TPartialBlobId> NewBlobs;
        TVector<TPartialBlobId> GarbageBlobs;
        TVector<NProto::TCheckpoint> Checkpoints;
        TVector<TCompactionRangeInfo> CompactionMap;
        TVector<NProto::TDupCacheEntry> DupCache;
        TVector<NProto::TTruncateEntry> TruncateQueue;
        TMaybe<NProto::TStorageConfig> StorageConfig;
        TVector<NProto::TSessionHistoryEntry> SessionsHistory;

        NProto::TError Error;

        void Clear()
        {
            FileSystem.Clear();
            FileSystemStats.Clear();
            TabletStorageInfo.Clear();
            RootNode.Clear();
            Sessions.clear();
            Handles.clear();
            Locks.clear();
            FreshBytes.clear();
            FreshBlocks.clear();
            NewBlobs.clear();
            GarbageBlobs.clear();
            Checkpoints.clear();
            CompactionMap.clear();
            DupCache.clear();
            TruncateQueue.clear();
            StorageConfig.Clear();
            SessionsHistory.clear();
        }
    };

    //
    // UpdateConfig
    //

    struct TUpdateConfig
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 TxId;
        const NProto::TFileSystem FileSystem;

        TUpdateConfig(
                TRequestInfoPtr requestInfo,
                ui64 txId,
                NProto::TFileSystem fileSystem)
            : RequestInfo(std::move(requestInfo))
            , TxId(txId)
            , FileSystem(std::move(fileSystem))
        {}

        void Clear()
        {
            // nothing to do
        }
    };

    //
    // CreateSession
    //

    struct TCreateSession
    {
        const TRequestInfoPtr RequestInfo;
        const NProtoPrivate::TCreateSessionRequest Request;

        NProto::TError Error;
        TString SessionId;

        TCreateSession(
                TRequestInfoPtr requestInfo,
                const NProtoPrivate::TCreateSessionRequest& request)
            : RequestInfo(std::move(requestInfo))
            , Request(request)
        {}

        void Clear()
        {
            SessionId.clear();
        }
    };

    //
    // ResetSession
    //

    struct TResetSession
    {
        const TRequestInfoPtr RequestInfo;
        const TString SessionId;
        const ui64 SessionSeqNo;
        const TString SessionState;

        TNodeSet Nodes;

        TResetSession(
                TRequestInfoPtr requestInfo,
                TString sessionId,
                ui64 sessionSeqNo,
                TString sessionState)
            : RequestInfo(std::move(requestInfo))
            , SessionId(std::move(sessionId))
            , SessionSeqNo(sessionSeqNo)
            , SessionState(std::move(sessionState))
        {}

        void Clear()
        {
            Nodes.clear();
        }
    };

    //
    // DestroySession
    //

    struct TDestroySession
    {
        const TRequestInfoPtr RequestInfo;
        const TString SessionId;
        const ui64 SessionSeqNo;

        TNodeSet Nodes;

        TDestroySession(
                TRequestInfoPtr requestInfo,
                TString sessionId,
                ui64 sessionSeqNo)
            : RequestInfo(std::move(requestInfo))
            , SessionId(std::move(sessionId))
            , SessionSeqNo(sessionSeqNo)
        {}

        void Clear()
        {
            Nodes.clear();
        }
    };

    //
    // CreateCheckpoint
    //

    struct TCreateCheckpoint
    {
        const TRequestInfoPtr RequestInfo;
        const TString CheckpointId;
        const ui64 NodeId;

        NProto::TError Error;
        ui64 CommitId = InvalidCommitId;

        TCreateCheckpoint(
                TRequestInfoPtr requestInfo,
                TString checkpointId,
                ui64 nodeId)
            : RequestInfo(std::move(requestInfo))
            , CheckpointId(std::move(checkpointId))
            , NodeId(nodeId)
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
        }
    };

    //
    // DeleteCheckpoint
    //

    struct TDeleteCheckpoint
    {
        const TRequestInfoPtr RequestInfo;
        const TString CheckpointId;
        const EDeleteCheckpointMode Mode;
        const ui64 CollectBarrier;

        NProto::TError Error;
        ui64 CommitId = InvalidCommitId;

        TVector<ui64> NodeIds;
        TVector<TIndexTabletDatabase::TNode> Nodes;
        TVector<TIndexTabletDatabase::TNodeAttr> NodeAttrs;
        TVector<TIndexTabletDatabase::TNodeRef> NodeRefs;

        TVector<TIndexTabletDatabase::TCheckpointBlob> Blobs;
        TVector<TIndexTabletDatabase::TMixedBlob> MixedBlobs;

        // NOTE: should persist state across tx restarts
        TSet<ui32> MixedBlocksRanges;

        TDeleteCheckpoint(
                TRequestInfoPtr requestInfo,
                TString checkpointId,
                EDeleteCheckpointMode mode,
                ui64 collectBarrier)
            : RequestInfo(std::move(requestInfo))
            , CheckpointId(std::move(checkpointId))
            , Mode(mode)
            , CollectBarrier(collectBarrier)
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;

            NodeIds.clear();
            Nodes.clear();
            NodeAttrs.clear();
            NodeRefs.clear();

            Blobs.clear();
            MixedBlobs.clear();
        }
    };

    //
    // ResolvePath
    //

    struct TResolvePath : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TResolvePathRequest Request;
        const TString Path;

        ui64 CommitId = InvalidCommitId;

        TResolvePath(
                TRequestInfoPtr requestInfo,
                const NProto::TResolvePathRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , Path(request.GetPath())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
        }
    };

    //
    // CreateNode
    //

    struct TCreateNode : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 ParentNodeId;
        const ui64 TargetNodeId;
        const TString Name;
        const NProto::TNode Attrs;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> ParentNode;
        ui64 ChildNodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> ChildNode;

        NProto::TCreateNodeResponse Response;

        TCreateNode(
                TRequestInfoPtr requestInfo,
                const NProto::TCreateNodeRequest request,
                ui64 parentNodeId,
                ui64 targetNodeId,
                const NProto::TNode& attrs)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , ParentNodeId(parentNodeId)
            , TargetNodeId(targetNodeId)
            , Name(request.GetName())
            , Attrs(attrs)
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            ParentNode.Clear();
            ChildNodeId = InvalidNodeId;
            ChildNode.Clear();

            Response.Clear();
        }
    };

    //
    // UnlinkNode
    //

    struct TUnlinkNode : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TUnlinkNodeRequest Request;
        const ui64 ParentNodeId;
        const TString Name;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> ParentNode;
        TMaybe<TIndexTabletDatabase::TNode> ChildNode;
        TMaybe<TIndexTabletDatabase::TNodeRef> ChildRef;

        NProto::TUnlinkNodeResponse Response;

        TUnlinkNode(
                TRequestInfoPtr requestInfo,
                const NProto::TUnlinkNodeRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , ParentNodeId(request.GetNodeId())
            , Name(request.GetName())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            ParentNode.Clear();
            ChildNode.Clear();
            ChildRef.Clear();
            Response.Clear();
        }
    };

    //
    // RenameNode
    //

    struct TRenameNode : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 ParentNodeId;
        const TString Name;
        const ui64 NewParentNodeId;
        const TString NewName;
        const ui32 Flags;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> ParentNode;
        TMaybe<TIndexTabletDatabase::TNode> ChildNode;
        TMaybe<TIndexTabletDatabase::TNodeRef> ChildRef;

        TMaybe<TIndexTabletDatabase::TNode> NewParentNode;
        TMaybe<TIndexTabletDatabase::TNode> NewChildNode;
        TMaybe<TIndexTabletDatabase::TNodeRef> NewChildRef;

        NProto::TRenameNodeResponse Response;

        TRenameNode(
                TRequestInfoPtr requestInfo,
                NProto::TRenameNodeRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , ParentNodeId(request.GetNodeId())
            , Name(std::move(*request.MutableName()))
            , NewParentNodeId(request.GetNewParentId())
            , NewName(std::move(*request.MutableNewName()))
            , Flags(request.GetFlags())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            ParentNode.Clear();
            ChildNode.Clear();
            ChildRef.Clear();

            NewParentNode.Clear();
            NewChildNode.Clear();
            NewChildRef.Clear();

            Response.Clear();
        }
    };

    //
    // AccessNode
    //

    struct TAccessNode : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TAccessNodeRequest Request;
        const ui64 NodeId;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;

        TAccessNode(
                TRequestInfoPtr requestInfo,
                const NProto::TAccessNodeRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
        }
    };

    //
    // ReadLink
    //

    struct TReadLink : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TReadLinkRequest Request;
        const ui64 NodeId;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;

        TReadLink(
                TRequestInfoPtr requestInfo,
                const NProto::TReadLinkRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , NodeId(request.GetNodeId())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
        }
    };

    //
    // ListNodes
    //

    struct TListNodes : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TListNodesRequest Request;
        const ui64 NodeId;
        const TString Cookie;
        const ui32 MaxBytes;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TVector<TIndexTabletDatabase::TNodeRef> ChildRefs;
        TVector<TIndexTabletDatabase::TNode> ChildNodes;
        TString Next;

        ui32 BytesToPrecharge = 0;

        TListNodes(
                TRequestInfoPtr requestInfo,
                const NProto::TListNodesRequest& request,
                ui32 maxBytes)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
            , Cookie(request.GetCookie())
            , MaxBytes(maxBytes)
            , BytesToPrecharge(MaxBytes)
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
            ChildRefs.clear();
            ChildNodes.clear();
            Next.clear();

            BytesToPrecharge =
                ClampVal(2 * BytesToPrecharge, MaxBytes, 10 * MaxBytes);
        }
    };

    //
    // SetNodeAttr
    //

    struct TSetNodeAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TSetNodeAttrRequest Request;
        const ui64 NodeId;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;

        TSetNodeAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TSetNodeAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
        }
    };

    //
    // GetNodeAttr
    //

    struct TGetNodeAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TGetNodeAttrRequest Request;
        const ui64 NodeId;
        const TString Name;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> ParentNode;
        ui64 TargetNodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> TargetNode;

        TGetNodeAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TGetNodeAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
            , Name(request.GetName())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            ParentNode.Clear();
            TargetNodeId = InvalidNodeId;
            TargetNode.Clear();
        }
    };

    //
    // SetNodeXAttr
    //

    struct TSetNodeXAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TSetNodeXAttrRequest Request;
        const ui64 NodeId;
        const TString Name;
        const TString Value;

        ui64 Version = 0;
        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TMaybe<TIndexTabletDatabase::TNodeAttr> Attr;

        TSetNodeXAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TSetNodeXAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
            , Name(request.GetName())
            , Value(request.GetValue())
        {}

        void Clear()
        {
            Version = 0;
            CommitId = InvalidCommitId;
            Node.Clear();
            Attr.Clear();
        }
    };

    //
    // GetNodeXAttr
    //

    struct TGetNodeXAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TGetNodeXAttrRequest Request;
        const ui64 NodeId;
        const TString Name;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TMaybe<TIndexTabletDatabase::TNodeAttr> Attr;

        TGetNodeXAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TGetNodeXAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
            , Name(request.GetName())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
            Attr.Clear();
        }
    };

    //
    // ListNodeXAttr
    //

    struct TListNodeXAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TListNodeXAttrRequest Request;
        const ui64 NodeId;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TVector<TIndexTabletDatabase::TNodeAttr> Attrs;

        TListNodeXAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TListNodeXAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
            Attrs.clear();
        }
    };

    //
    // RemoveNodeXAttr
    //

    struct TRemoveNodeXAttr : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TRemoveNodeXAttrRequest Request;
        const ui64 NodeId;
        const TString Name;

        ui64 CommitId = InvalidCommitId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TMaybe<TIndexTabletDatabase::TNodeAttr> Attr;

        TRemoveNodeXAttr(
                TRequestInfoPtr requestInfo,
                const NProto::TRemoveNodeXAttrRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
            , NodeId(request.GetNodeId())
            , Name(request.GetName())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            Node.Clear();
            Attr.Clear();
        }
    };

    //
    // CreateHandle
    //

    struct TCreateHandle : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 NodeId;
        const TString Name;
        const ui32 Flags;
        const ui32 Mode;
        const ui32 Uid;
        const ui32 Gid;

        ui64 ReadCommitId = InvalidCommitId;
        ui64 WriteCommitId = InvalidCommitId;
        ui64 TargetNodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> TargetNode;
        TMaybe<TIndexTabletDatabase::TNode> ParentNode;

        NProto::TCreateHandleResponse Response;

        TCreateHandle(
                TRequestInfoPtr requestInfo,
                const NProto::TCreateHandleRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , NodeId(request.GetNodeId())
            , Name(request.GetName())
            , Flags(request.GetFlags())
            , Mode(request.GetMode())
            , Uid(request.GetUid())
            , Gid(request.GetGid())
        {}

        void Clear()
        {
            ReadCommitId = InvalidCommitId;
            WriteCommitId = InvalidCommitId;
            TargetNodeId = InvalidNodeId;
            TargetNode.Clear();
            ParentNode.Clear();

            Response.Clear();
        }
    };

    //
    // DestroyHandle
    //

    struct TDestroyHandle : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TDestroyHandleRequest Request;

        TMaybe<TIndexTabletDatabase::TNode> Node;

        TDestroyHandle(
                TRequestInfoPtr requestInfo,
                const NProto::TDestroyHandleRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
        {}

        void Clear()
        {
            Node.Clear();
        }
    };

    //
    // AcquireLock
    //

    struct TAcquireLock : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TAcquireLockRequest Request;

        TAcquireLock(
                TRequestInfoPtr requestInfo,
                const NProto::TAcquireLockRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
        {}

        void Clear()
        {
            // nothing to do
        }
    };

    //
    // ReleaseLock
    //

    struct TReleaseLock : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TReleaseLockRequest Request;

        std::optional<NProto::ELockOrigin> IncompatibleLockOrigin;

        TReleaseLock(
                TRequestInfoPtr requestInfo,
                const NProto::TReleaseLockRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
        {}

        void Clear()
        {
            IncompatibleLockOrigin.reset();
        }
    };

    //
    // TestLock
    //

    struct TTestLock : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TTestLockRequest Request;

        std::optional<TLockIncompatibleInfo> Incompatible;

        TTestLock(
                TRequestInfoPtr requestInfo,
                const NProto::TTestLockRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Request(request)
        {}

        void Clear()
        {
            Incompatible.reset();
        }
    };

    //
    // ReadData
    //

    struct TReadData : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 Handle;
        const TByteRange OriginByteRange;
        const TByteRange AlignedByteRange;
        /*const*/ IBlockBufferPtr Buffer;

        ui64 CommitId = InvalidCommitId;
        ui64 NodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> Node;
        TVector<TBlockDataRef> Blocks;
        TVector<TBlockBytes> Bytes;

        // NOTE: should persist state across tx restarts
        TSet<ui32> MixedBlocksRanges;

        TReadData(
                TRequestInfoPtr requestInfo,
                const NProto::TReadDataRequest& request,
                TByteRange originByteRange,
                TByteRange alignedByteRange,
                IBlockBufferPtr buffer)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Handle(request.GetHandle())
            , OriginByteRange(originByteRange)
            , AlignedByteRange(alignedByteRange)
            , Buffer(std::move(buffer))
            , Blocks(AlignedByteRange.BlockCount())
            , Bytes(AlignedByteRange.BlockCount())
        {
            Y_DEBUG_ABORT_UNLESS(AlignedByteRange.IsAligned());
        }

        void Clear()
        {
            CommitId = InvalidCommitId;
            NodeId = InvalidNodeId;
            Node.Clear();

            std::fill(Blocks.begin(), Blocks.end(), TBlockDataRef());
            std::fill(Bytes.begin(), Bytes.end(), TBlockBytes());
        }
    };

    //
    // WriteData
    //

    struct TWriteData : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui32 WriteBlobThreshold;
        const ui64 Handle;
        const TByteRange ByteRange;
        /*const*/ IBlockBufferPtr Buffer;

        ui64 CommitId = InvalidCommitId;
        ui64 NodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> Node;

        TWriteData(
                TRequestInfoPtr requestInfo,
                const ui32 writeBlobThreshold,
                const NProto::TWriteDataRequest& request,
                TByteRange byteRange,
                IBlockBufferPtr buffer)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , WriteBlobThreshold(writeBlobThreshold)
            , Handle(request.GetHandle())
            , ByteRange(byteRange)
            , Buffer(std::move(buffer))
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            NodeId = InvalidNodeId;
            Node.Clear();
        }

        bool ShouldWriteBlob() const
        {
            // skip fresh completely for large aligned writes
            return ByteRange.IsAligned()
                && ByteRange.Length >= WriteBlobThreshold;
        }
    };

    //
    // WriteBatch
    //

    struct TWriteBatch
    {
        const TRequestInfoPtr RequestInfo;
        const bool SkipFresh;
        /*const*/ TWriteRequestList WriteBatch;

        ui64 CommitId = InvalidCommitId;
        TMap<ui64, ui64> WriteRanges;
        TNodeSet Nodes;

        NProto::TError Error;

        TWriteBatch(
                TRequestInfoPtr requestInfo,
                bool skipFresh,
                TWriteRequestList writeBatch)
            : RequestInfo(std::move(requestInfo))
            , SkipFresh(skipFresh)
            , WriteBatch(std::move(writeBatch))
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            WriteRanges.clear();
            Nodes.clear();
        }
    };

    //
    // AllocateData
    //

    struct TAllocateData : TSessionAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 Handle;
        const ui64 Offset;
        const ui64 Length;
        const ui32 Flags;

        ui64 CommitId = InvalidCommitId;
        ui64 NodeId = InvalidNodeId;
        TMaybe<TIndexTabletDatabase::TNode> Node;

        TAllocateData(
                TRequestInfoPtr requestInfo,
                const NProto::TAllocateDataRequest& request)
            : TSessionAware(request)
            , RequestInfo(std::move(requestInfo))
            , Handle(request.GetHandle())
            , Offset(request.GetOffset())
            , Length(request.GetLength())
            , Flags(request.GetFlags())
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            NodeId = InvalidNodeId;
            Node.Clear();
        }
    };

    //
    // AddBlob
    //

    struct TAddBlob : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const EAddBlobMode Mode;
        /*const*/ TVector<TMixedBlobMeta> SrcBlobs;
        /*const*/ TVector<TBlock> SrcBlocks;
        /*const*/ TVector<TMixedBlobMeta> MixedBlobs;
        /*const*/ TVector<TMergedBlobMeta> MergedBlobs;
        const TVector<TWriteRange> WriteRanges;

        ui64 CommitId = InvalidCommitId;
        TNodeSet Nodes;

        TAddBlob(
                TRequestInfoPtr requestInfo,
                EAddBlobMode mode,
                TVector<TMixedBlobMeta> srcBlobs,
                TVector<TBlock> srcBlocks,
                TVector<TMixedBlobMeta> mixedBlobs,
                TVector<TMergedBlobMeta> mergedBlobs,
                TVector<TWriteRange> writeRanges)
            : TProfileAware(EFileStoreSystemRequest::AddBlob)
            , RequestInfo(std::move(requestInfo))
            , Mode(mode)
            , SrcBlobs(std::move(srcBlobs))
            , SrcBlocks(std::move(srcBlocks))
            , MixedBlobs(std::move(mixedBlobs))
            , MergedBlobs(std::move(mergedBlobs))
            , WriteRanges(std::move(writeRanges))
        {}

        void Clear()
        {
            TProfileAware::Clear();

            CommitId = InvalidCommitId;
            Nodes.clear();
        }
    };

    //
    // FlushBytes
    //

    struct TFlushBytes : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 ReadCommitId;
        const ui64 ChunkId;
        const TVector<TBytes> Bytes;

        ui64 CollectCommitId = InvalidCommitId;

        // NOTE: should persist state across tx restarts
        TSet<ui32> MixedBlocksRanges;

        TFlushBytes(
                TRequestInfoPtr requestInfo,
                ui64 readCommitId,
                ui64 chunkId,
                TVector<TBytes> bytes)
            : TProfileAware(EFileStoreSystemRequest::FlushBytes)
            , RequestInfo(std::move(requestInfo))
            , ReadCommitId(readCommitId)
            , ChunkId(chunkId)
            , Bytes(std::move(bytes))
        {}

        void Clear()
        {
            TProfileAware::Clear();

            CollectCommitId = InvalidCommitId;
        }
    };

    //
    // TrimBytes
    //

    struct TTrimBytes : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 ChunkId;

        TTrimBytes(TRequestInfoPtr requestInfo, ui64 chunkId)
            : TProfileAware(EFileStoreSystemRequest::TrimBytes)
            , RequestInfo(std::move(requestInfo))
            , ChunkId(chunkId)
        {}

        void Clear()
        {
            TProfileAware::Clear();
        }
    };

    //
    // Compaction
    //

    struct TCompaction : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui32 RangeId;
        const bool FilterNodes;

        // should persist across tx restarts
        bool RangeLoaded = false;

        TSet<ui64> Nodes;
        TVector<TMixedBlobMeta> CompactionBlobs;
        ui64 CommitId = InvalidCommitId;

        TCompaction(TRequestInfoPtr requestInfo, ui32 rangeId, bool filterNodes)
            : TProfileAware(EFileStoreSystemRequest::Compaction)
            , RequestInfo(std::move(requestInfo))
            , RangeId(rangeId)
            , FilterNodes(filterNodes)
        {}

        void Clear()
        {
            TProfileAware::Clear();

            Nodes.clear();
            CompactionBlobs.clear();
            CommitId = InvalidCommitId;
        }
    };

    //
    // Cleanup
    //

    struct TCleanup : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui32 RangeId;
        const ui64 CollectBarrier;

        ui64 CommitId = InvalidCommitId;

        TCleanup(TRequestInfoPtr requestInfo, ui32 rangeId, ui64 collectBarrier)
            : TProfileAware(EFileStoreSystemRequest::Cleanup)
            , RequestInfo(std::move(requestInfo))
            , RangeId(rangeId)
            , CollectBarrier(collectBarrier)
        {}

        void Clear()
        {
            TProfileAware::Clear();

            CommitId = InvalidCommitId;
        }
    };

    //
    // DeleteGarbage
    //

    struct TDeleteGarbage : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 CollectCommitId;

        TVector<TPartialBlobId> NewBlobs;
        TVector<TPartialBlobId> GarbageBlobs;
        TVector<TPartialBlobId> RemainingNewBlobs;
        TVector<TPartialBlobId> RemainingGarbageBlobs;

        TDeleteGarbage(
                TRequestInfoPtr requestInfo,
                ui64 collectCommitId,
                TVector<TPartialBlobId> newBlobs,
                TVector<TPartialBlobId> garbageBlobs)
            : TProfileAware(EFileStoreSystemRequest::DeleteGarbage)
            , RequestInfo(std::move(requestInfo))
            , CollectCommitId(collectCommitId)
            , NewBlobs(std::move(newBlobs))
            , GarbageBlobs(std::move(garbageBlobs))
        {}

        void Clear()
        {
            TProfileAware::Clear();
        }
    };

    //
    // TruncateRange
    //

    struct TTruncateRange : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 NodeId;
        const TByteRange Range;

        TTruncateRange(
                TRequestInfoPtr requestInfo,
                ui64 nodeId,
                TByteRange range)
            : TProfileAware(EFileStoreSystemRequest::TruncateRange)
            , RequestInfo(std::move(requestInfo))
            , NodeId(nodeId)
            , Range(range)
        {}

        void Clear()
        {
            TProfileAware::Clear();
        }
    };

    //
    // TruncateCompleted
    //

    struct TTruncateCompleted
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 NodeId;

        TTruncateCompleted(
                TRequestInfoPtr requestInfo,
                ui64 nodeId)
            : RequestInfo(std::move(requestInfo))
            , NodeId(nodeId)
        {}

        void Clear()
        {
            // nothing to do
        }
    };

    //
    // ZeroRange
    //

    struct TZeroRange : TProfileAware
    {
        const TRequestInfoPtr RequestInfo;
        const ui64 NodeId;
        const TByteRange Range;

        TZeroRange(
                TRequestInfoPtr requestInfo,
                ui64 nodeId,
                TByteRange range)
            : TProfileAware(EFileStoreSystemRequest::ZeroRange)
            , RequestInfo(std::move(requestInfo))
            , NodeId(nodeId)
            , Range(range)
        {}

        void Clear()
        {
            TProfileAware::Clear();
        }
    };

    //
    // FilterAliveNodes
    //

    struct TFilterAliveNodes
    {
        const TRequestInfoPtr RequestInfo;
        const TStackVec<ui64, 16> Nodes;

        ui64 CommitId = InvalidNodeId;
        TSet<ui64> AliveNodes;

        TFilterAliveNodes(
                TRequestInfoPtr requestInfo,
                TStackVec<ui64, 16> nodes)
            : RequestInfo(std::move(requestInfo))
            , Nodes(std::move(nodes))
        {}

        void Clear()
        {
            CommitId = InvalidCommitId;
            AliveNodes.clear();
        }
    };

    //
    // DumpCompactionRange
    //

    struct TDumpCompactionRange
    {
        const TRequestInfoPtr RequestInfo;

        const ui32 RangeId = 0;
        TVector<TMixedBlobMeta> Blobs;

        TDumpCompactionRange(
                TRequestInfoPtr requestInfo,
                ui32 rangeId)
            : RequestInfo(std::move(requestInfo))
            , RangeId(rangeId)
        {}

        void Clear()
        {
            Blobs.clear();
        }
    };

    //
    // ChangeStorageConfig
    //

    struct TChangeStorageConfig
    {
        const TRequestInfoPtr RequestInfo;
        const NProto::TStorageConfig StorageConfigNew;
        const bool MergeWithStorageConfigFromTabletDB;

        TMaybe<NProto::TStorageConfig> StorageConfigFromDB;
        NProto::TStorageConfig ResultStorageConfig;

        TChangeStorageConfig(
                TRequestInfoPtr requestInfo,
                NProto::TStorageConfig storageConfig,
                bool mergeWithStorageConfigFromTabletDB)
            : RequestInfo(std::move(requestInfo))
            , StorageConfigNew(std::move(storageConfig))
            , MergeWithStorageConfigFromTabletDB(
                mergeWithStorageConfigFromTabletDB)
        {}

        void Clear()
        {
            StorageConfigFromDB.Clear();
            ResultStorageConfig.Clear();
        }
    };
};

}   // namespace NCloud::NFileStore::NStorage
