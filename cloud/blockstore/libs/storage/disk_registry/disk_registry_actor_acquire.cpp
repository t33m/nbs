#include "disk_registry_actor.h"

#include <cloud/blockstore/libs/storage/api/disk_agent.h>

#include <cloud/blockstore/libs/kikimr/events.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;

using namespace NKikimr::NTabletFlatExecutor;

namespace {

////////////////////////////////////////////////////////////////////////////////

class TAcquireDiskActor final
    : public TActorBootstrapped<TAcquireDiskActor>
{
private:
    const TActorId Owner;
    TRequestInfoPtr RequestInfo;
    const TString DiskId;
    const TString ClientId;
    const NProto::EVolumeAccessMode AccessMode;
    const ui64 MountSeqNumber;
    const ui32 VolumeGeneration;
    const TDuration RequestTimeout;

    TVector<NProto::TDeviceConfig> Devices;
    ui32 LogicalBlockSize = 0;

    NProto::TError AcquireError;
    int PendingRequests = 0;

public:
    TAcquireDiskActor(
        const TActorId& owner,
        TRequestInfoPtr requestInfo,
        TString diskId,
        TString clientId,
        NProto::EVolumeAccessMode accessMode,
        ui64 mountSeqNumber,
        ui32 volumeGeneration,
        TDuration requestTimeout);

    void Bootstrap(const TActorContext& ctx);

private:
    void PrepareRequest(NProto::TAcquireDevicesRequest& request);
    void PrepareRequest(NProto::TReleaseDevicesRequest& request);

    void StartAcquireDisk(const TActorContext& ctx);
    void FinishAcquireDisk(const TActorContext& ctx);

    void CancelAcquireOperation(const TActorContext& ctx, NProto::TError error);

    void ReplyAndDie(const TActorContext& ctx, NProto::TError error);

    void OnAcquireResponse(const TActorContext& ctx, NProto::TError error);
    void OnReleaseResponse(
        const TActorContext& ctx,
        ui64 cookie,
        NProto::TError error);

    template <typename R>
    void SendRequests(const TActorContext& ctx);

private:
    STFUNC(StateAcquire);
    STFUNC(StateRelease);
    STFUNC(StateFinish);

    void HandlePoisonPill(
        const TEvents::TEvPoisonPill::TPtr& ev,
        const TActorContext& ctx);

    void HandleAcquireDevicesResponse(
        const TEvDiskAgent::TEvAcquireDevicesResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleReleaseDevicesResponse(
        const TEvDiskAgent::TEvReleaseDevicesResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleStartAcquireDiskResponse(
        const TEvDiskRegistryPrivate::TEvStartAcquireDiskResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleFinishAcquireDiskResponse(
        const TEvDiskRegistryPrivate::TEvFinishAcquireDiskResponse::TPtr& ev,
        const TActorContext& ctx);

    void HandleAcquireDevicesUndelivery(
        const TEvDiskAgent::TEvAcquireDevicesRequest::TPtr& ev,
        const TActorContext& ctx);

    void HandleReleaseDevicesUndelivery(
        const TEvDiskAgent::TEvReleaseDevicesRequest::TPtr& ev,
        const TActorContext& ctx);

    void HandleWakeup(
        const TEvents::TEvWakeup::TPtr& ev,
        const TActorContext& ctx);

    TString LogTargets() const;
};

////////////////////////////////////////////////////////////////////////////////

TAcquireDiskActor::TAcquireDiskActor(
        const TActorId& owner,
        TRequestInfoPtr requestInfo,
        TString diskId,
        TString clientId,
        NProto::EVolumeAccessMode accessMode,
        ui64 mountSeqNumber,
        ui32 volumeGeneration,
        TDuration requestTimeout)
    : Owner(owner)
    , RequestInfo(std::move(requestInfo))
    , DiskId(std::move(diskId))
    , ClientId(std::move(clientId))
    , AccessMode(accessMode)
    , MountSeqNumber(mountSeqNumber)
    , VolumeGeneration(volumeGeneration)
    , RequestTimeout(requestTimeout)
{
    ActivityType = TBlockStoreActivities::DISK_REGISTRY_WORKER;
}

void TAcquireDiskActor::Bootstrap(const TActorContext& ctx)
{
    Become(&TThis::StateAcquire);
    StartAcquireDisk(ctx);
    ctx.Schedule(RequestTimeout, new TEvents::TEvWakeup());
}

void TAcquireDiskActor::StartAcquireDisk(const TActorContext& ctx)
{
    using TType = TEvDiskRegistryPrivate::TEvStartAcquireDiskRequest;
    NCloud::Send(ctx, Owner, std::make_unique<TType>(DiskId, ClientId));
}

void TAcquireDiskActor::FinishAcquireDisk(const TActorContext& ctx)
{
    Become(&TThis::StateFinish);

    using TType = TEvDiskRegistryPrivate::TEvFinishAcquireDiskRequest;
    NCloud::Send(ctx, Owner, std::make_unique<TType>(DiskId, ClientId));
}

void TAcquireDiskActor::PrepareRequest(NProto::TAcquireDevicesRequest& request)
{
    request.MutableHeaders()->SetClientId(ClientId);
    request.SetAccessMode(AccessMode);
    request.SetMountSeqNumber(MountSeqNumber);
    request.SetDiskId(DiskId);
    request.SetVolumeGeneration(VolumeGeneration);
}

void TAcquireDiskActor::PrepareRequest(NProto::TReleaseDevicesRequest& request)
{
    request.MutableHeaders()->SetClientId(ClientId);
}

template <typename R>
void TAcquireDiskActor::SendRequests(const TActorContext& ctx)
{
    auto it = Devices.begin();
    ui32 cookie = 0;
    while (it != Devices.end()) {
        auto request = std::make_unique<R>();
        PrepareRequest(request->Record);

        const ui32 nodeId = it->GetNodeId();

        for (; it != Devices.end() && it->GetNodeId() == nodeId; ++it) {
            *request->Record.AddDeviceUUIDs() = it->GetDeviceUUID();
        }

        ++PendingRequests;

        TAutoPtr<IEventHandle> event(
            new IEventHandle(
                MakeDiskAgentServiceId(nodeId),
                ctx.SelfID,
                request.get(),
                IEventHandle::FlagForwardOnNondelivery,
                cookie++,
                &ctx.SelfID // forwardOnNondelivery
            ));
        request.release();

        ctx.Send(event);
    }
}

void TAcquireDiskActor::CancelAcquireOperation(
    const TActorContext& ctx,
    NProto::TError error)
{
    Become(&TThis::StateRelease);

    AcquireError = std::move(error);

    LOG_DEBUG(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
        "[%s] Canceling acquire operation for disk %s, targets %s",
        ClientId.c_str(),
        DiskId.c_str(),
        LogTargets().c_str());

    if (Devices.empty()) {
        ReplyAndDie(ctx, AcquireError);
        return;
    }

    PendingRequests = 0;
    SendRequests<TEvDiskAgent::TEvReleaseDevicesRequest>(ctx);
}

void TAcquireDiskActor::ReplyAndDie(const TActorContext& ctx, NProto::TError error)
{
    auto response = std::make_unique<TEvDiskRegistry::TEvAcquireDiskResponse>(
        std::move(error));

    if (HasError(response->GetError())) {
        LOG_ERROR(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
            "[%s] AcquireDisk %s targets %s error: %s",
            ClientId.c_str(),
            DiskId.c_str(),
            LogTargets().c_str(),
            FormatError(response->GetError()).c_str());
    } else {
        response->Record.MutableDevices()->Reserve(Devices.size());

        for (auto& device: Devices) {
            ToLogicalBlocks(device, LogicalBlockSize);
            *response->Record.AddDevices() = std::move(device);
        }
    }
    NCloud::Reply(ctx, *RequestInfo, std::move(response));

    NCloud::Send(
        ctx,
        Owner,
        std::make_unique<TEvDiskRegistryPrivate::TEvOperationCompleted>());
    Die(ctx);
}

////////////////////////////////////////////////////////////////////////////////

void TAcquireDiskActor::HandlePoisonPill(
    const TEvents::TEvPoisonPill::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    ReplyAndDie(ctx, MakeError(E_REJECTED, "Tablet is dead"));
}

void TAcquireDiskActor::OnAcquireResponse(
    const TActorContext& ctx,
    NProto::TError error)
{
    if (HasError(error)) {
        LOG_ERROR(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
            "[%s] AcquireDevices %s error: %s",
            ClientId.c_str(),
            LogTargets().c_str(),
            FormatError(error).c_str());

        CancelAcquireOperation(ctx, std::move(error));
        FinishAcquireDisk(ctx);
        return;
    }

    Y_ABORT_UNLESS(PendingRequests > 0);

    if (--PendingRequests == 0) {
        FinishAcquireDisk(ctx);
    }
}

void TAcquireDiskActor::OnReleaseResponse(
    const TActorContext& ctx,
    ui64 cookie,
    NProto::TError error)
{
    if (HasError(error)) {
        LOG_ERROR(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
            "[%s] ReleaseDevices %s error: %s, %llu",
            ClientId.c_str(),
            LogTargets().c_str(),
            FormatError(error).c_str(),
            cookie);
    }

    Y_ABORT_UNLESS(PendingRequests > 0);

    if (--PendingRequests == 0) {
        ReplyAndDie(ctx, AcquireError);
    }
}

void TAcquireDiskActor::HandleAcquireDevicesResponse(
    const TEvDiskAgent::TEvAcquireDevicesResponse::TPtr& ev,
    const TActorContext& ctx)
{
    OnAcquireResponse(ctx, ev->Get()->GetError());
}

void TAcquireDiskActor::HandleReleaseDevicesResponse(
    const TEvDiskAgent::TEvReleaseDevicesResponse::TPtr& ev,
    const TActorContext& ctx)
{
    OnReleaseResponse(ctx, ev->Cookie, ev->Get()->GetError());
}

void TAcquireDiskActor::HandleAcquireDevicesUndelivery(
    const TEvDiskAgent::TEvAcquireDevicesRequest::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    OnAcquireResponse(ctx, MakeError(E_REJECTED, "not delivered"));
}

void TAcquireDiskActor::HandleReleaseDevicesUndelivery(
    const TEvDiskAgent::TEvReleaseDevicesRequest::TPtr& ev,
    const TActorContext& ctx)
{
    OnReleaseResponse(ctx, ev->Cookie, MakeError(E_REJECTED, "not delivered"));
}

void TAcquireDiskActor::HandleWakeup(
    const TEvents::TEvWakeup::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    const auto err = Sprintf(
        "[%s] TAcquireDiskActor timeout, targets %s, pending requests: %d",
        ClientId.c_str(),
        LogTargets().c_str(),
        PendingRequests);
    LOG_ERROR(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER, err);

    AcquireError = MakeError(E_REJECTED, err);

    FinishAcquireDisk(ctx);
}

void TAcquireDiskActor::HandleStartAcquireDiskResponse(
    const TEvDiskRegistryPrivate::TEvStartAcquireDiskResponse::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    if (msg->GetStatus() != S_OK) {
        ReplyAndDie(ctx, msg->GetError());
        return;
    }

    LogicalBlockSize = msg->LogicalBlockSize;
    Devices = msg->Devices;

    if (Devices.empty()) {
        FinishAcquireDisk(ctx);
        return;
    }

    SortBy(Devices, [](auto& d) {
        return d.GetNodeId();
    });

    // TODO: setup rate limits

    LOG_DEBUG(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
        "[%s] Sending acquire devices requests for disk %s, targets %s",
        ClientId.c_str(),
        DiskId.c_str(),
        LogTargets().c_str());

    SendRequests<TEvDiskAgent::TEvAcquireDevicesRequest>(ctx);
}

void TAcquireDiskActor::HandleFinishAcquireDiskResponse(
    const TEvDiskRegistryPrivate::TEvFinishAcquireDiskResponse::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();
    if (HasError(msg->GetError())) {
        LOG_ERROR(ctx, TBlockStoreComponents::DISK_REGISTRY_WORKER,
            "[%s] FinishAcquireDisk targets %s error: %s",
            ClientId.c_str(),
            LogTargets().c_str(),
            FormatError(msg->GetError()).c_str());

        CancelAcquireOperation(ctx, msg->GetError());
        return;
    }

    ReplyAndDie(ctx, AcquireError);
}

////////////////////////////////////////////////////////////////////////////////

TString TAcquireDiskActor::LogTargets() const
{
    return LogDevices(Devices);
}

////////////////////////////////////////////////////////////////////////////////

STFUNC(TAcquireDiskActor::StateAcquire)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        HFunc(TEvDiskAgent::TEvAcquireDevicesResponse,
            HandleAcquireDevicesResponse);
        HFunc(TEvDiskAgent::TEvAcquireDevicesRequest,
            HandleAcquireDevicesUndelivery);

        HFunc(TEvDiskRegistryPrivate::TEvStartAcquireDiskResponse,
            HandleStartAcquireDiskResponse);

        HFunc(TEvents::TEvWakeup, HandleWakeup);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::DISK_REGISTRY_WORKER);
            break;
    }
}

STFUNC(TAcquireDiskActor::StateRelease)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        HFunc(TEvDiskAgent::TEvReleaseDevicesResponse,
            HandleReleaseDevicesResponse);
        HFunc(TEvDiskAgent::TEvReleaseDevicesRequest,
            HandleReleaseDevicesUndelivery);

        HFunc(TEvents::TEvWakeup, HandleWakeup);

        IgnoreFunc(TEvDiskAgent::TEvAcquireDevicesResponse);
        IgnoreFunc(TEvDiskAgent::TEvAcquireDevicesRequest);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::DISK_REGISTRY_WORKER);
            break;
    }
}

STFUNC(TAcquireDiskActor::StateFinish)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        HFunc(TEvDiskRegistryPrivate::TEvFinishAcquireDiskResponse,
            HandleFinishAcquireDiskResponse);

        IgnoreFunc(TEvDiskRegistryPrivate::TEvStartAcquireDiskResponse);

        IgnoreFunc(TEvents::TEvWakeup);
        IgnoreFunc(TEvDiskAgent::TEvAcquireDevicesResponse);
        IgnoreFunc(TEvDiskAgent::TEvAcquireDevicesRequest);
        IgnoreFunc(TEvDiskAgent::TEvReleaseDevicesResponse);
        IgnoreFunc(TEvDiskAgent::TEvReleaseDevicesRequest);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::DISK_REGISTRY_WORKER);
            break;
    }
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void TDiskRegistryActor::HandleAcquireDisk(
    const TEvDiskRegistry::TEvAcquireDiskRequest::TPtr& ev,
    const TActorContext& ctx)
{
    BLOCKSTORE_DISK_REGISTRY_COUNTER(AcquireDisk);

    const auto* msg = ev->Get();

    auto requestInfo = CreateRequestInfo(
        ev->Sender,
        ev->Cookie,
        msg->CallContext
    );

    auto clientId = msg->Record.GetHeaders().GetClientId();

    LOG_DEBUG(ctx, TBlockStoreComponents::DISK_REGISTRY,
        "[%lu] Received AcquireDisk request: "
        "DiskId=%s, ClientId=%s, AccessMode=%u, MountSeqNumber=%lu"
        ", VolumeGeneration=%u",
        TabletID(),
        msg->Record.GetDiskId().c_str(),
        clientId.c_str(),
        static_cast<ui32>(msg->Record.GetAccessMode()),
        msg->Record.GetMountSeqNumber(),
        msg->Record.GetVolumeGeneration());

    auto actor = NCloud::Register<TAcquireDiskActor>(
        ctx,
        ctx.SelfID,
        std::move(requestInfo),
        msg->Record.GetDiskId(),
        std::move(clientId),
        msg->Record.GetAccessMode(),
        msg->Record.GetMountSeqNumber(),
        msg->Record.GetVolumeGeneration(),
        Config->GetAgentRequestTimeout());
    Actors.insert(actor);
}

////////////////////////////////////////////////////////////////////////////////

void TDiskRegistryActor::HandleStartAcquireDisk(
    const TEvDiskRegistryPrivate::TEvStartAcquireDiskRequest::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    TDiskInfo diskInfo;

    auto error = State->StartAcquireDisk(msg->DiskId, diskInfo);
    State->FilterDevicesAtUnavailableAgents(diskInfo);

    auto devices = std::move(diskInfo.Devices);
    for (auto& migration: diskInfo.Migrations) {
        devices.push_back(std::move(*migration.MutableTargetDevice()));
    }
    for (auto& replica: diskInfo.Replicas) {
        for (auto& device: replica) {
            devices.push_back(std::move(device));
        }
    }

    auto response = std::make_unique<TEvDiskRegistryPrivate::TEvStartAcquireDiskResponse>(
        std::move(error), std::move(devices), diskInfo.LogicalBlockSize);
    NCloud::Reply(ctx, *ev, std::move(response));
}

void TDiskRegistryActor::HandleFinishAcquireDisk(
    const TEvDiskRegistryPrivate::TEvFinishAcquireDiskRequest::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    State->FinishAcquireDisk(msg->DiskId);
    auto response =
        std::make_unique<TEvDiskRegistryPrivate::TEvFinishAcquireDiskResponse>();

    NCloud::Reply(ctx, *ev, std::move(response));
}

}   // namespace NCloud::NBlockStore::NStorage
