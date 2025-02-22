#include "volume_ut.h"

#include <cloud/blockstore/libs/storage/api/volume_proxy.h>
#include <cloud/blockstore/libs/storage/partition_common/events_private.h>

#include <util/system/hostname.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;

using namespace NKikimr;

using namespace NCloud::NBlockStore::NStorage::NPartition;

using namespace NCloud::NStorage;

using namespace NTestVolume;

using namespace NTestVolumeHelpers;

////////////////////////////////////////////////////////////////////////////////

namespace NTestVolumeHelpers {

TBlockRange64 GetBlockRangeById(ui32 blockIndex)
{
    return TBlockRange64::WithLength(1024 * blockIndex, 1024);
}

}   // namespace NTestVolumeHelpers

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TVolumeTest)
{
    Y_UNIT_TEST(ShouldUpdateVolumeConfig)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
    }

    Y_UNIT_TEST(ShouldLazilyStartPartitions)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        const auto partitionTabletId = NKikimr::MakeTabletID(0, HiveId, 2);

        auto actorId = runtime->AllocateEdgeActor(0);
        runtime->SendToPipe(
            partitionTabletId,
            actorId,
            new TEvService::TEvReadBlocksRequest(),
            0,
            GetPipeConfigWithRetries());

        TAutoPtr<IEventHandle> handle;
        runtime->GrabEdgeEventRethrow<TEvService::TEvReadBlocksResponse>(
            handle,
            TDuration::Seconds(1)
        );

        // after UpdateVolumeConfig partitions should be available
        UNIT_ASSERT(handle);

        volume.RebootTablet();

        actorId = runtime->AllocateEdgeActor(0);
        runtime->SendToPipe(
            partitionTabletId,
            actorId,
            new TEvService::TEvReadBlocksRequest(),
            0,
            GetPipeConfigWithRetries());

        handle.Reset();
        runtime->GrabEdgeEventRethrow<TEvService::TEvReadBlocksResponse>(
            handle,
            TDuration::Seconds(1)
        );

        // but after volume tablet reboot partitions should be offline
        UNIT_ASSERT(!handle);

        // StatVolume should start partitions and return OK
        volume.SendStatVolumeRequest();
        auto statVolumeResponse = volume.RecvStatVolumeResponse();
        UNIT_ASSERT_VALUES_EQUAL(S_OK, statVolumeResponse->GetStatus());

        actorId = runtime->AllocateEdgeActor(0);
        runtime->SendToPipe(
            partitionTabletId,
            actorId,
            new TEvService::TEvReadBlocksRequest(),
            0,
            GetPipeConfigWithRetries());

        handle.Reset();
        runtime->GrabEdgeEventRethrow<TEvService::TEvReadBlocksResponse>(
            handle,
            TDuration::Seconds(1)
        );

        // after StatVolume partitions should be available again
        UNIT_ASSERT(handle);

        volume.RebootTablet();

        // DescribeBlocks should start partitions and return OK
        volume.SendDescribeBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            TString());
        auto describeBlockResponse = volume.RecvDescribeBlocksResponse();
        UNIT_ASSERT_VALUES_EQUAL(S_OK, describeBlockResponse->GetStatus());
    }

    Y_UNIT_TEST(ShouldCorrectlyRestartPartition)
    {
        constexpr size_t MAX_ERROR_COUNT = 7;
        constexpr TDuration INCREMENT_TIME = TDuration::MilliSeconds(500);
        constexpr TDuration MAX_TIME = TDuration::MilliSeconds(2'000);

        NProto::TStorageServiceConfig config;
        config.SetTabletRebootCoolDownIncrement(INCREMENT_TIME.MilliSeconds());
        config.SetTabletRebootCoolDownMax(MAX_TIME.MilliSeconds());
        config.SetThrottlingEnabled(false);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        volume.WaitReady();

        bool suppressFailure = false;
        bool shouldStart = false;
        size_t errorCount = 0;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvBlobStorage::EvPutResult: {
                        if (suppressFailure || errorCount >= MAX_ERROR_COUNT) {
                            break;
                        }
                        auto msg = event->Get<TEvBlobStorage::TEvPutResult>();
                        msg->Status = NKikimrProto::ERROR;
                        ++errorCount;
                        shouldStart = true;
                        break;
                    }
                    case TEvPartition::EvWaitReadyResponse: {
                        shouldStart = false;
                        break;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        const auto range = TBlockRange64::WithLength(0, 1024);
        const auto suppressFailureFunction = [&](const auto& func) {
            suppressFailure = true;
            func();
            suppressFailure = false;
        };
        const auto writeBlockRangeFunction = [&](ui32 status) {
            volume.SendWriteBlocksRequest(range, clientInfo.GetClientId());
            auto response = volume.RecvWriteBlocksResponse(TDuration::Zero());
            UNIT_ASSERT(response);
            UNIT_ASSERT_VALUES_EQUAL(status, response->GetStatus());
        };

        TDuration prev = TDuration::Zero();
        TDuration curr = INCREMENT_TIME;

        // Increment RetryTimeout until it reaches MaxRetryTimeout.
        while (prev < curr && errorCount < MAX_ERROR_COUNT) {
            // Partition shut down.
            writeBlockRangeFunction(E_REJECTED);

            UNIT_ASSERT(shouldStart);

            suppressFailureFunction([&]() {
                // Partition has not been started yet.
                writeBlockRangeFunction(E_REJECTED);
            });

            suppressFailureFunction([&]() {
                runtime->DispatchEvents({}, prev);
            });

            // Not enough time skipped, need one more cycle.
            UNIT_ASSERT(shouldStart);
            suppressFailureFunction([&]() {
                writeBlockRangeFunction(E_REJECTED);
            });

            suppressFailureFunction([&]() {
                runtime->DispatchEvents({}, INCREMENT_TIME);
            });

            UNIT_ASSERT(!shouldStart);

            suppressFailureFunction([&]() {
                // Partition is up.
                writeBlockRangeFunction(S_OK);
            });

            prev = curr;
            curr = Min(curr + INCREMENT_TIME, MAX_TIME);
        }

        // Work with MaxRetryTimeout.
        while (errorCount < MAX_ERROR_COUNT) {
            // Partition shut down.
            writeBlockRangeFunction(E_REJECTED);

            UNIT_ASSERT(shouldStart);

            suppressFailureFunction([&]() {
                // Partition has not been started yet.
                writeBlockRangeFunction(E_REJECTED);
            });

            suppressFailureFunction([&]() {
                runtime->DispatchEvents({}, curr - INCREMENT_TIME);
            });

            // Not enough time skipped, need one more cycle.
            UNIT_ASSERT(shouldStart);
            suppressFailureFunction([&]() {
                writeBlockRangeFunction(E_REJECTED);
            });

            suppressFailureFunction([&]() {
                runtime->DispatchEvents({}, INCREMENT_TIME);
            });

            UNIT_ASSERT(!shouldStart);

            suppressFailureFunction([&]() {
                // Partition is up.
                writeBlockRangeFunction(S_OK);
            });
        }

        // Partition won't crash on new request, because all errors have passed.
        writeBlockRangeFunction(S_OK);

        // Move time further to get ahead of deadline => when tablet fails it
        // will reset timeout and deadline, because it has been working for
        // a long time without failures.
        runtime->AdvanceCurrentTime(curr);

        prev = TDuration::Zero();
        curr = INCREMENT_TIME;
        errorCount = 0;

        // Partition crash.
        writeBlockRangeFunction(E_REJECTED);

        // We should skip INCREMENT_TIME, because first start failed. And the
        // second one performed immediately. Cannot check first start, because
        // TTestActorRuntime is not flexible enough.
        suppressFailureFunction([&]() {
            runtime->DispatchEvents({}, INCREMENT_TIME);
        });

        suppressFailureFunction([&]() {
            // Partition is up.
            writeBlockRangeFunction(S_OK);
        });
    }

    Y_UNIT_TEST(ShouldAllocateDiskDuringUpdateVolumeConfig)
    {
        auto runtime = PrepareTestActorRuntime();

        const auto expectedBlockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            expectedDeviceCount * expectedBlockCount);

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(
            expectedBlockCount,
            devices[0].GetBlockCount()
        );
        UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(
            expectedBlockCount,
            devices[1].GetBlockCount()
        );
        UNIT_ASSERT_VALUES_EQUAL("transport2", devices[2].GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(
            expectedBlockCount,
            devices[2].GetBlockCount()
        );
    }

    Y_UNIT_TEST(ShouldStatNonreplicatedVolume)
    {
        auto runtime = PrepareTestActorRuntime();

        const auto blockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            blockCount);

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& v = stat->Record.GetVolume();
        UNIT_ASSERT_VALUES_EQUAL("vol0", v.GetDiskId());
        UNIT_ASSERT_VALUES_EQUAL("folder", v.GetFolderId());
        UNIT_ASSERT_VALUES_EQUAL("cloud", v.GetCloudId());
        UNIT_ASSERT_VALUES_EQUAL(1, v.GetConfigVersion());
        UNIT_ASSERT_VALUES_EQUAL(DefaultBlockSize, v.GetBlockSize());
        UNIT_ASSERT_VALUES_EQUAL(blockCount, v.GetBlocksCount());
        const auto& devices = v.GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(
            blockCount,
            devices[0].GetBlockCount()
        );
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartition)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        TVolumeClient volumeClient1(*runtime);
        TVolumeClient volumeClient2(*runtime);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());

        const auto& disk = state->Disks.at("vol0");
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL("", disk.PoolName);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volumeClient1.AddClient(clientInfo);
            const auto& volume = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, volume.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                volume.GetDevices(0).GetTransportId()
            );
        }
        UNIT_ASSERT_VALUES_EQUAL(clientInfo.GetClientId(), disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);
        volumeClient2.AddClient(clientInfo2);

        UNIT_ASSERT_VALUES_EQUAL(clientInfo.GetClientId(), disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(1, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL(clientInfo2.GetClientId(), disk.ReaderClientIds[0]);

        volumeClient1.WriteBlocks(TBlockRange64::MakeOneBlock(0), clientInfo.GetClientId(), 1);
        auto resp = volumeClient1.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);

        volume.RemoveClient(clientInfo.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(1, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL(clientInfo2.GetClientId(), disk.ReaderClientIds[0]);

        volume.RemoveClient(clientInfo2.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartitionDuringMigration)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        TVolumeClient client1(*runtime);
        TVolumeClient client2(*runtime);

        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            const auto& migrations = stat->Record.GetVolume().GetMigrations();
            UNIT_ASSERT_VALUES_EQUAL(3, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport2", devices[2].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(0, migrations.size());
        }

        const auto& disk = state->Disks.at("vol0");
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL("", disk.PoolName);

        // registering a writer
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = client1.AddClient(clientInfo);
            const auto& volume = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(3, volume.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                volume.GetDevices(0).GetTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport1",
                volume.GetDevices(1).GetTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport2",
                volume.GetDevices(2).GetTransportId()
            );
        }
        UNIT_ASSERT_VALUES_EQUAL(clientInfo.GetClientId(), disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());

        // writing some data whose migration we will test
        const auto range1 = TBlockRange64::MakeOneBlock(5);
        const auto range2 = TBlockRange64::MakeOneBlock(5 + blocksPerDevice);
        const auto range3 = TBlockRange64::MakeOneBlock(5 + 2 * blocksPerDevice);
        client1.WriteBlocksLocal(
            range1,
            clientInfo.GetClientId(),
            GetBlockContent(1)
        );
        client1.WriteBlocksLocal(
            range2,
            clientInfo.GetClientId(),
            GetBlockContent(2)
        );
        client1.WriteBlocksLocal(
            range3,
            clientInfo.GetClientId(),
            GetBlockContent(3)
        );

        state->MigrationMode = EMigrationMode::InProgress;

        TAutoPtr<IEventHandle> evRangeMigrated;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;

                if (event->GetTypeRewrite() == migratedEvent) {
                    auto range = event->Get<TMigratedEvent>()->Range;
                    if (range.Start > 1024) {
                        evRangeMigrated = event.Release();
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        client1.ReconnectPipe();
        client1.AddClient(clientInfo);
        volume.WaitReady();

        // checking that our volume sees the requested migrations
        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            const auto& migrations = stat->Record.GetVolume().GetMigrations();
            UNIT_ASSERT_VALUES_EQUAL(3, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport2", devices[2].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(2, migrations.size());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                migrations[0].GetSourceTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0_migration",
                migrations[0].GetTargetDevice().GetTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport2",
                migrations[1].GetSourceTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport2_migration",
                migrations[1].GetTargetDevice().GetTransportId()
            );
        }

        // adding a reader
        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);
        client2.AddClient(clientInfo2);

        UNIT_ASSERT_VALUES_EQUAL(clientInfo.GetClientId(), disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(1, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL(clientInfo2.GetClientId(), disk.ReaderClientIds[0]);

        UNIT_ASSERT(evRangeMigrated);

        // capturing w/z requests sent while our migration is in progress
        TVector<NProto::TWriteDeviceBlocksRequest> writeBlocksRequests;
        TVector<NProto::TZeroDeviceBlocksRequest> zeroBlocksRequests;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TEvDiskAgent::EvWriteDeviceBlocksRequest) {
                    auto* msg = event->Get<TEvDiskAgent::TEvWriteDeviceBlocksRequest>();
                    if (msg->Record.GetBlocks().BuffersSize() != 1024) {
                        writeBlocksRequests.push_back(msg->Record);
                    }
                } else if (event->GetTypeRewrite() == TEvDiskAgent::EvZeroDeviceBlocksRequest) {
                    auto* msg = event->Get<TEvDiskAgent::TEvZeroDeviceBlocksRequest>();
                    zeroBlocksRequests.push_back(msg->Record);
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // sending some write/zero requests
        // TODO: send write/zero requests to all devices
        client1.WriteBlocksLocal(
            TBlockRange64::MakeClosedInterval(1, 2),
            clientInfo.GetClientId(),
            GetBlockContent(4)
        );
        client1.ZeroBlocks(
            TBlockRange64::MakeClosedInterval(2, 2),
            clientInfo.GetClientId());

        TVector<TString> blocks;
        auto sglist = ResizeBlocks(
            blocks,
            2,
            TString::TUninitialized(DefaultBlockSize)
        );
        auto resp = client1.ReadBlocksLocal(
            TBlockRange64::MakeClosedInterval(1, 2),
            TGuardedSgList(std::move(sglist)),
            clientInfo.GetClientId()
        );
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(4), blocks[0]);
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(0), blocks[1]);

        // checking that these write/zero requests were mirrored
        UNIT_ASSERT_VALUES_EQUAL(2, writeBlocksRequests.size());
        Sort(
            writeBlocksRequests.begin(),
            writeBlocksRequests.end(),
            [] (const auto& l, const auto& r) {
                return l.GetDeviceUUID() < r.GetDeviceUUID();
            }
        );
        UNIT_ASSERT_VALUES_EQUAL(
            "uuid0",
            writeBlocksRequests[0].GetDeviceUUID()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            clientInfo.GetClientId(),
            writeBlocksRequests[0].GetHeaders().GetClientId()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            DefaultBlockSize,
            writeBlocksRequests[0].GetBlockSize()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            writeBlocksRequests[0].GetStartIndex()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            GetBlockContent(4),
            writeBlocksRequests[0].GetBlocks().GetBuffers(0)
        );
        UNIT_ASSERT_VALUES_EQUAL(
            GetBlockContent(4),
            writeBlocksRequests[0].GetBlocks().GetBuffers(1)
        );
        UNIT_ASSERT_VALUES_EQUAL(
            "uuid0_migration",
            writeBlocksRequests[1].GetDeviceUUID()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            clientInfo.GetClientId(),
            writeBlocksRequests[1].GetHeaders().GetClientId()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            DefaultBlockSize,
            writeBlocksRequests[1].GetBlockSize()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            writeBlocksRequests[1].GetStartIndex()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            GetBlockContent(4),
            writeBlocksRequests[1].GetBlocks().GetBuffers(0)
        );
        UNIT_ASSERT_VALUES_EQUAL(
            GetBlockContent(4),
            writeBlocksRequests[1].GetBlocks().GetBuffers(1)
        );

        UNIT_ASSERT_VALUES_EQUAL(2, zeroBlocksRequests.size());
        Sort(
            zeroBlocksRequests.begin(),
            zeroBlocksRequests.end(),
            [] (const auto& l, const auto& r) {
                return l.GetDeviceUUID() < r.GetDeviceUUID();
            }
        );
        UNIT_ASSERT_VALUES_EQUAL(
            "uuid0",
            zeroBlocksRequests[0].GetDeviceUUID()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            clientInfo.GetClientId(),
            zeroBlocksRequests[0].GetHeaders().GetClientId()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            DefaultBlockSize,
            zeroBlocksRequests[0].GetBlockSize()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            2,
            zeroBlocksRequests[0].GetStartIndex()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            zeroBlocksRequests[0].GetBlocksCount()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            "uuid0_migration",
            zeroBlocksRequests[1].GetDeviceUUID()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            clientInfo.GetClientId(),
            zeroBlocksRequests[1].GetHeaders().GetClientId()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            DefaultBlockSize,
            zeroBlocksRequests[1].GetBlockSize()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            2,
            zeroBlocksRequests[1].GetStartIndex()
        );
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            zeroBlocksRequests[1].GetBlocksCount()
        );

        runtime->Send(evRangeMigrated.Release());
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // checking that DR was notified about a finished migration
        UNIT_ASSERT_VALUES_EQUAL(1, state->FinishMigrationRequests);

        state->MigrationMode = EMigrationMode::Finish;

        // reallocating disk
        volume.ReallocateDisk();
        client2.ReconnectPipe();
        client2.AddClient(clientInfo2);
        volume.WaitReady();

        // checking that our volume sees new device list
        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            const auto& migrations = stat->Record.GetVolume().GetMigrations();
            UNIT_ASSERT_VALUES_EQUAL(3, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0_migration", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport2_migration", devices[2].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(0, migrations.size());
        }

        // checking that our data has been in fact migrated
        {
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range1.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = client2.ReadBlocksLocal(
                range1,
                TGuardedSgList(std::move(sglist)),
                clientInfo2.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), blocks[0]);
        }

        {
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range2.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = client2.ReadBlocksLocal(
                range2,
                TGuardedSgList(std::move(sglist)),
                clientInfo2.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(2), blocks[0]);
        }

        {
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range3.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = client2.ReadBlocksLocal(
                range3,
                TGuardedSgList(std::move(sglist)),
                clientInfo2.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(3), blocks[0]);
        }

        client1.ReconnectPipe(); // reconnect since pipe was closed when client2 started read/write
        client1.RemoveClient(clientInfo.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(1, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL(clientInfo2.GetClientId(), disk.ReaderClientIds[0]);

        client2.RemoveClient(clientInfo2.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
    }

    Y_UNIT_TEST(ShouldProperlyHandleBadMigrationConfiguration)
    {
        google::protobuf::RepeatedPtrField<NProto::TDeviceConfig> devices;

        *devices.Add() = MakeDevice("uuid0", "dev0", "transport0");
        *devices.Add() = MakeDevice(
            "uuid0_migration",
            "dev0_migration",
            "transport0_migration"
        );
        // misconfiguring source-target pair
        devices.begin()->SetBlocksCount(2 * DefaultDeviceBlockCount);

        auto diskRegistryState = MakeIntrusive<TDiskRegistryState>();
        diskRegistryState->Devices = TVector<NProto::TDeviceConfig>(
            devices.begin(),
            devices.end()
        );

        diskRegistryState->MigrationDevices["uuid0"] = devices[1];

        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto runtime = PrepareTestActorRuntime(config, diskRegistryState);

        NMonitoring::TDynamicCountersPtr counters = new NMonitoring::TDynamicCounters();
        InitCriticalEventsCounter(counters);
        auto badMigrationConfigCounter =
            counters->GetCounter("AppCriticalEvents/BadMigrationConfig", true);

        TVolumeClient volume(*runtime);

        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            blocksPerDevice
        );

        volume.WaitReady();

        UNIT_ASSERT_VALUES_EQUAL(0, badMigrationConfigCounter->Val());

        diskRegistryState->MigrationMode = EMigrationMode::InProgress;
        ui32 migratedRanges = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent
                    = TEvNonreplPartitionPrivate::EvRangeMigrated;
                if (event->GetTypeRewrite() == migratedEvent)
                {
                    ++migratedRanges;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();

        // checking that our volume sees the requested migrations
        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            const auto& migrations = stat->Record.GetVolume().GetMigrations();
            UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(1, migrations.size());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                migrations[0].GetSourceTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0_migration",
                migrations[0].GetTargetDevice().GetTransportId()
            );
        }

        UNIT_ASSERT_VALUES_EQUAL(1, badMigrationConfigCounter->Val());
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(0, migratedRanges);
    }

    Y_UNIT_TEST(ShouldEnsureMutualExclusionOfIntersectingWritesAndMigrationRequests)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        TVolumeClient client1(*runtime);

        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        // registering a writer
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        client1.AddClient(clientInfo);

        state->MigrationMode = EMigrationMode::InProgress;
        TAutoPtr<IEventHandle> evRangeMigrated;
        TBlockRange64 migratedRange;
        bool interceptMigration = true;

        TAutoPtr<IEventHandle> evWriteCompleted;
        bool interceptWrite = false;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto completionEvent =
                    TEvNonreplPartitionPrivate::EvWriteOrZeroCompleted;
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;

                if (interceptMigration
                        && event->GetTypeRewrite() == migratedEvent)
                {
                    migratedRange = event->Get<TMigratedEvent>()->Range;
                    evRangeMigrated = event.Release();
                    return TTestActorRuntime::EEventAction::DROP;
                } else if (interceptWrite
                        && event->GetTypeRewrite() == completionEvent)
                {
                    evWriteCompleted = event.Release();
                    return TTestActorRuntime::EEventAction::DROP;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        client1.ReconnectPipe();
        client1.AddClient(clientInfo);
        volume.WaitReady();

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(evRangeMigrated);

        client1.SendWriteBlocksRequest(
            TBlockRange64::MakeOneBlock(migratedRange.Start),
            clientInfo.GetClientId(),
            'a'
        );
        {
            auto response = client1.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
        }

        client1.SendWriteBlocksRequest(
            TBlockRange64::MakeOneBlock(migratedRange.End),
            clientInfo.GetClientId(),
            'a'
        );
        {
            auto response = client1.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
        }

        client1.SendZeroBlocksRequest(
            TBlockRange64::MakeOneBlock(migratedRange.Start),
            clientInfo.GetClientId()
        );
        {
            auto response = client1.RecvZeroBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
        }

        client1.SendZeroBlocksRequest(
            TBlockRange64::MakeOneBlock(migratedRange.End),
            clientInfo.GetClientId()
        );
        {
            auto response = client1.RecvZeroBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
        }

        interceptWrite = true;
        client1.SendWriteBlocksRequest(
            TBlockRange64::MakeOneBlock(migratedRange.End + 1),
            clientInfo.GetClientId(),
            'a'
        );

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(evWriteCompleted);

        runtime->Send(evRangeMigrated.Release());

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(!evRangeMigrated);

        runtime->AdvanceCurrentTime(TDuration::Minutes(1));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT(!evRangeMigrated);

        auto lastMigratedRange = migratedRange;

        interceptWrite = false;
        runtime->Send(evWriteCompleted.Release());

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(evRangeMigrated);

        UNIT_ASSERT_VALUES_EQUAL(
            lastMigratedRange.End + 1,
            migratedRange.Start
        );

        interceptMigration = false;
        runtime->Send(evRangeMigrated.Release());

        runtime->DispatchEvents({}, TDuration::Seconds(1));

        auto resp = client1.ReadBlocks(
            TBlockRange64::MakeOneBlock(lastMigratedRange.End + 1),
            clientInfo.GetClientId()
        );
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent('a'), bufs[0]);
    }

    Y_UNIT_TEST(ShouldRunMigrationForVolumesWithoutClients)
    {
        NProto::TStorageServiceConfig config;
        config.SetMaxMigrationBandwidth(999'999'999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        state->MigrationMode = EMigrationMode::InProgress;
        TBlockRange64 migratedRange;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;
                if (event->GetTypeRewrite() == migratedEvent)
                {
                    migratedRange = event->Get<TMigratedEvent>()->Range;
                    return TTestActorRuntime::EEventAction::DROP;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(migratedRange.Size() == 1024);
        UNIT_ASSERT_VALUES_EQUAL(0, state->FinishMigrationRequests);

        // rebooting volume to stop partition actors
        volume.RebootTablet();

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, state->FinishMigrationRequests);
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartitionDuringMigrationWithRetriableDeviceErrors)
    {
        // TODO
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartitionDuringMigrationWithFatalDeviceErrors)
    {
        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        NMonitoring::TDynamicCountersPtr counters = new NMonitoring::TDynamicCounters();
        InitCriticalEventsCounter(counters);
        auto migrationFailedCounter =
            counters->GetCounter("AppCriticalEvents/MigrationFailed", true);

        TVolumeClient volume(*runtime);

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        // registering a writer
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        state->MigrationMode = EMigrationMode::InProgress;
        ui32 rangeMigratedCount = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                if (event->GetTypeRewrite() == migratedEvent) {
                    ++rangeMigratedCount;
                } else if (event->GetTypeRewrite() == TEvDiskAgent::EvReadDeviceBlocksResponse) {
                    auto* msg = event->Get<TEvDiskAgent::TEvReadDeviceBlocksResponse>();
                    *msg->Record.MutableError() = MakeError(E_FAIL, "failure");
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, rangeMigratedCount);
        UNIT_ASSERT_VALUES_EQUAL(0, state->FinishMigrationRequests);
        UNIT_ASSERT_VALUES_EQUAL(1, migrationFailedCounter->Val());
    }

    Y_UNIT_TEST(ShouldRestoreMigrationIndexAfterReboot)
    {
        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;
        const auto indexCachingInterval = blocksPerDevice / 10;
        UNIT_ASSERT(indexCachingInterval > 0);

        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        config.SetMigrationIndexCachingInterval(indexCachingInterval);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        // registering a writer
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        // writing some data whose migration we will test
        const auto range1 = TBlockRange64::MakeOneBlock(5);
        const auto range2 = TBlockRange64::MakeOneBlock(5 + blocksPerDevice);
        const auto range3 =
            TBlockRange64::MakeOneBlock(5 + 2 * blocksPerDevice);
        volume.WriteBlocks(range1, clientInfo.GetClientId(), 1);
        volume.WriteBlocks(range2, clientInfo.GetClientId(), 2);
        volume.WriteBlocks(range3, clientInfo.GetClientId(), 3);

        state->MigrationMode = EMigrationMode::InProgress;

        const auto midIndex = range2.Start;
        ui32 migrationStartedCounter = 0;
        ui32 migrationProgressCounter = 0;

        bool drop = false;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;
                if (event->GetTypeRewrite() == migratedEvent) {
                    auto* msg = event->Get<TMigratedEvent>();
                    if (msg->Range.Start > midIndex) {
                        drop = true;
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                } else if (event->Recipient == MakeStorageStatsServiceId()
                        && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
                {
                    auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();

                    migrationStartedCounter =
                        msg->VolumeSelfCounters->Simple.MigrationStarted.Value;
                    migrationProgressCounter =
                        msg->VolumeSelfCounters->Simple.MigrationProgress.Value;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);
        volume.WaitReady();

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT(drop);
        UNIT_ASSERT_VALUES_EQUAL(0, state->FinishMigrationRequests);

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, migrationStartedCounter);
        UNIT_ASSERT_VALUES_EQUAL(80, migrationProgressCounter);

        volume.RebootTablet();
        volume.AddClient(clientInfo);

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;
                if (event->GetTypeRewrite() == migratedEvent) {
                    auto* msg = event->Get<TMigratedEvent>();
                    UNIT_ASSERT_C(
                        msg->Range.Start > midIndex - indexCachingInterval,
                        TStringBuilder() << "lagging migration index: " << msg->Range.Start
                    );
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.WaitReady();

        // checking that DR was notified about a finished migration
        UNIT_ASSERT_VALUES_EQUAL(1, state->FinishMigrationRequests);

        state->MigrationMode = EMigrationMode::Finish;

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);
        volume.WaitReady();

        // checking that our volume sees new device list
        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            const auto& migrations = stat->Record.GetVolume().GetMigrations();
            UNIT_ASSERT_VALUES_EQUAL(3, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0_migration", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport2_migration", devices[2].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(0, migrations.size());
        }

        // checking that our data has been in fact migrated
        {
            auto resp = volume.ReadBlocks(range1, clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);
        }

        {
            auto resp = volume.ReadBlocks(range2, clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(2), bufs[0]);
        }

        {
            auto resp = volume.ReadBlocks(range3, clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(3), bufs[0]);
        }
    }

    Y_UNIT_TEST(ShouldThrottleMigration)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(4); // 1 request per second
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2.5 * blocksPerDevice
        );

        volume.WaitReady();

        // registering a writer
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        state->MigrationMode = EMigrationMode::InProgress;
        TBlockRange64 lastMigratedRange;
        ui32 migratedRanges = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                using TMigratedEvent =
                    TEvNonreplPartitionPrivate::TEvRangeMigrated;

                const auto migratedEvent =
                    TEvNonreplPartitionPrivate::EvRangeMigrated;

                if (event->GetTypeRewrite() == migratedEvent) {
                    lastMigratedRange = event->Get<TMigratedEvent>()->Range;
                    ++migratedRanges;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        // reallocating disk
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();

        runtime->AdvanceCurrentTime(TDuration::Seconds(1));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, migratedRanges);
        UNIT_ASSERT_VALUES_EQUAL(0, lastMigratedRange.Start);

        runtime->AdvanceCurrentTime(TDuration::Seconds(1));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT_VALUES_EQUAL(2, migratedRanges);
        UNIT_ASSERT_VALUES_EQUAL(1024, lastMigratedRange.Start);

        runtime->AdvanceCurrentTime(TDuration::Seconds(1));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT_VALUES_EQUAL(3, migratedRanges);
        UNIT_ASSERT_VALUES_EQUAL(2048, lastMigratedRange.Start);
    }

    Y_UNIT_TEST(ShouldRegularlyReacquireNonreplicatedDisks)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetClientRemountPeriod(2000);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        TVolumeClient writerClient(*runtime);
        TVolumeClient readerClient1(*runtime);
        TVolumeClient readerClient2(*runtime);

        volume.WaitReady();

        ui32 writerAcquireRequests = 0;
        ui32 readerAcquireRequests = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TEvDiskRegistry::EvAcquireDiskRequest) {
                    auto* msg = event->Get<TEvDiskRegistry::TEvAcquireDiskRequest>();
                    if (msg->Record.GetAccessMode()
                            == NProto::VOLUME_ACCESS_READ_ONLY)
                    {
                        ++readerAcquireRequests;
                    } else {
                        ++writerAcquireRequests;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        runtime->AdvanceCurrentTime(TDuration::Seconds(2));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 0);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 0);

        auto writer = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        writerClient.AddClient(writer);

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 1);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 0);

        auto reader1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);
        readerClient1.AddClient(reader1);

        auto reader2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);
        readerClient2.AddClient(reader2);

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 1);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 2);

        runtime->AdvanceCurrentTime(TDuration::Seconds(2));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 2);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 4);

        readerClient1.RemoveClient(reader1.GetClientId());

        runtime->AdvanceCurrentTime(TDuration::Seconds(2));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 3);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 5);

        writerClient.RemoveClient(writer.GetClientId());
        readerClient2.RemoveClient(reader2.GetClientId());

        runtime->AdvanceCurrentTime(TDuration::Seconds(2));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));

        UNIT_ASSERT_VALUES_EQUAL(writerAcquireRequests, 3);
        UNIT_ASSERT_VALUES_EQUAL(readerAcquireRequests, 5);
    }

    Y_UNIT_TEST(ShouldPassAllParamsInAcquireDiskRequests)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetClientRemountPeriod(999999999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        NProto::EVolumeAccessMode accessMode = NProto::VOLUME_ACCESS_READ_ONLY;
        ui64 mountSeqNumber = 0;
        ui32 volumeGeneration = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskRegistry::EvAcquireDiskRequest: {
                        auto* msg = event->Get<TEvDiskRegistry::TEvAcquireDiskRequest>();

                        accessMode = msg->Record.GetAccessMode();
                        mountSeqNumber = msg->Record.GetMountSeqNumber();
                        volumeGeneration = msg->Record.GetVolumeGeneration();

                        break;
                    }

                    case TEvDiskRegistry::EvReleaseDiskRequest: {
                        auto* msg = event->Get<TEvDiskRegistry::TEvReleaseDiskRequest>();

                        volumeGeneration = msg->Record.GetVolumeGeneration();

                        break;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        auto writer = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            1);
        volume.AddClient(writer);

        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<int>(NProto::VOLUME_ACCESS_READ_WRITE),
            static_cast<int>(accessMode)
        );
        UNIT_ASSERT_VALUES_EQUAL(1, mountSeqNumber);
        UNIT_ASSERT_VALUES_EQUAL(2, volumeGeneration);

        auto reader = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            2);
        volume.AddClient(reader);

        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<int>(NProto::VOLUME_ACCESS_READ_ONLY),
            static_cast<int>(accessMode)
        );
        UNIT_ASSERT_VALUES_EQUAL(2, mountSeqNumber);
        UNIT_ASSERT_VALUES_EQUAL(2, volumeGeneration);

        volumeGeneration = 0;

        volume.RemoveClient(writer.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL(2, volumeGeneration);
    }

    Y_UNIT_TEST(ShouldReacquireNonreplicatedDisksUponInvalidSessionErrorFromAgent)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetClientRemountPeriod(999999999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        ui32 acquireRequests = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskAgent::EvWriteDeviceBlocksRequest: {
                        auto response = std::make_unique<TEvDiskAgent::TEvWriteDeviceBlocksResponse>(
                            MakeError(E_BS_INVALID_SESSION, "invalid session")
                        );

                        runtime->Send(new IEventHandle(
                            event->Sender,
                            event->Recipient,
                            response.release(),
                            0, // flags
                            event->Cookie
                        ), 0);

                        return TTestActorRuntime::EEventAction::DROP;
                    }

                    case TEvDiskRegistry::EvAcquireDiskRequest: {
                        ++acquireRequests;
                        break;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        runtime->AdvanceCurrentTime(TDuration::Seconds(2));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));

        UNIT_ASSERT_VALUES_EQUAL(0, acquireRequests);

        auto writer = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(writer);

        UNIT_ASSERT_VALUES_EQUAL(1, acquireRequests);

        volume.SendWriteBlocksRequest(
            TBlockRange64::MakeOneBlock(0),
            writer.GetClientId(),
            1);

        {
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
        }

        UNIT_ASSERT_VALUES_EQUAL(2, acquireRequests);
    }

    Y_UNIT_TEST(ShouldReleaseWriterForNonreplicatedDisksUponInvalidSessionErrorFromAgentDuringMigration)
    {
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->MigrationMode = EMigrationMode::InProgress;
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetClientRemountPeriod(999999999);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto runtime = PrepareTestActorRuntime(config, state);

        bool intercept = true;
        TString releaseClientId;
        TAutoPtr<IEventHandle> writeDeviceBlocks;

        auto replyError = [&] () {
            auto response = std::make_unique<TEvDiskAgent::TEvWriteDeviceBlocksResponse>(
                MakeError(E_BS_INVALID_SESSION, "invalid session")
            );

            runtime->Send(new IEventHandle(
                writeDeviceBlocks->Sender,
                writeDeviceBlocks->Recipient,
                response.release(),
                0, // flags
                writeDeviceBlocks->Cookie
            ), 0);
        };

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskAgent::EvWriteDeviceBlocksRequest: {
                        if (intercept) {
                            writeDeviceBlocks = event.Release();

                            return TTestActorRuntime::EEventAction::DROP;
                        }

                        break;
                    }

                    case TEvDiskRegistry::EvReleaseDiskRequest: {
                        auto* msg =
                            event->Get<TEvDiskRegistry::TEvReleaseDiskRequest>();
                        UNIT_ASSERT(!releaseClientId);

                        releaseClientId = msg->Record.GetHeaders().GetClientId();

                        break;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT(writeDeviceBlocks);
        replyError();

        // timeout has not passed yet
        UNIT_ASSERT_VALUES_EQUAL("", releaseClientId);

        runtime->AdvanceCurrentTime(TDuration::Seconds(5));
        runtime->DispatchEvents({}, TDuration::MilliSeconds(1));
        UNIT_ASSERT(writeDeviceBlocks);
        replyError();

        // timeout has passed, ReleaseDisk should've been sent
        UNIT_ASSERT_VALUES_EQUAL(AnyWriterClientId, releaseClientId);

        intercept = false;
        UNIT_ASSERT(writeDeviceBlocks);
        runtime->Send(writeDeviceBlocks.Release());
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartitionNoAcquire)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(false);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());

        const auto& disk = state->Disks.at("vol0");
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
        UNIT_ASSERT_VALUES_EQUAL("", disk.PoolName);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& volume = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, volume.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                volume.GetDevices(0).GetTransportId()
            );
        }
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        auto resp = volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);

        volume.RemoveClient(clientInfo.GetClientId());
        UNIT_ASSERT_VALUES_EQUAL("", disk.WriterClientId);
        UNIT_ASSERT_VALUES_EQUAL(0, disk.ReaderClientIds.size());
    }

    Y_UNIT_TEST(ShouldForwardRequestsToNonreplicatedPartitionAfterResizeNoAcquire)
    {
        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        const auto blocks =
            DefaultDeviceBlockSize * DefaultDeviceBlockCount / DefaultBlockSize;
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            blocks
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
        }

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            2,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            blocks * 2
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(2, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& volume = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(2, volume.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                volume.GetDevices(0).GetTransportId()
            );
            UNIT_ASSERT_VALUES_EQUAL(
                "transport1",
                volume.GetDevices(1).GetTransportId()
            );
        }

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);
        }

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(blocks * 2 - 1),
            clientInfo.GetClientId(),
            2);
        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeOneBlock(blocks * 2 - 1),
                clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(2), bufs[0]);
        }

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldProperlyProcessDiskAllocationRetriableError)
    {
        NMonitoring::TDynamicCountersPtr counters = new NMonitoring::TDynamicCounters();
        InitCriticalEventsCounter(counters);
        auto diskAllocationFailureCounter =
            counters->GetCounter("AppCriticalEvents/DiskAllocationFailure", true);

        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->CurrentErrorCode = E_REJECTED;
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.SendUpdateVolumeConfigRequest(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize,
            "fail"
        );

        {
            auto resp = volume.RecvUpdateVolumeConfigResponse();
            // schemeshard should get OK status since it's unable to abort this
            // disk creation tx and will simply kill itself with SIGABRT
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NKikimrBlockStore::OK),
                static_cast<ui32>(resp->Record.GetStatus())
            );
            UNIT_ASSERT_VALUES_EQUAL(TestTabletId, resp->Record.GetOrigin());
            UNIT_ASSERT_VALUES_EQUAL(123, resp->Record.GetTxId());
        }

        volume.SendStatVolumeRequest();
        TAutoPtr<IEventHandle> handle;
        runtime->GrabEdgeEventRethrow<TEvService::TEvStatVolumeResponse>(
            handle,
            WaitTimeout
        );

        // partition should be offline due to disk allocation error, request postponed
        UNIT_ASSERT(!handle);

        // client addition should still work
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        // but volume ops should produce meaningful errors
        {
            volume.SendReadBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId()
            );
            auto resp = volume.RecvReadBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, resp->GetStatus());
        }

        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1
            );
            auto resp = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, resp->GetStatus());
        }

        {
            volume.SendZeroBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId()
            );
            auto resp = volume.RecvZeroBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, resp->GetStatus());
        }

        state->CurrentErrorCode = S_OK;
        // waiting for background reallocation
        runtime->AdvanceCurrentTime(TDuration::Seconds(1));
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // after disk reallocation volume resets clients pipes
        // so we need to reestablish pipe again
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        volume.WaitReady();
        auto stat = volume.RecvStatVolumeResponse();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());

        // now requests should work
        {
            volume.SendReadBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId()
            );
            auto resp = volume.RecvReadBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                S_OK,
                resp->GetStatus(),
                resp->GetErrorReason()
            );
        }

        UNIT_ASSERT_VALUES_EQUAL(0, diskAllocationFailureCounter->Val());
    }

    Y_UNIT_TEST(ShouldTryToReallocateDiskAfterReboot)
    {
        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->CurrentErrorCode = E_BS_OUT_OF_SPACE;
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize,
            "fail"
        );

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        {
            volume.SendReadBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId()
            );
            auto resp = volume.RecvReadBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, resp->GetStatus());
        }

        volume.RebootTablet();

        state->CurrentErrorCode = S_OK;
        // waiting for background reallocation
        runtime->AdvanceCurrentTime(TDuration::Seconds(1));
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        volume.WaitReady();
        volume.AddClient(clientInfo);
        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());

        // now requests should work
        {
            volume.SendReadBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId()
            );
            auto resp = volume.RecvReadBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                S_OK,
                resp->GetStatus(),
                resp->GetErrorReason()
            );
        }
    }

    Y_UNIT_TEST(ShouldProperlyProcessFatalDiskAllocationError)
    {
        NMonitoring::TDynamicCountersPtr counters = new NMonitoring::TDynamicCounters();
        InitCriticalEventsCounter(counters);
        auto diskAllocationFailureCounter =
            counters->GetCounter("AppCriticalEvents/DiskAllocationFailure", true);

        int criticalEvents = 0;

        for (const auto code: {E_BS_RESOURCE_EXHAUSTED, E_ARGUMENT, E_BS_DISK_ALLOCATION_FAILED}) {
            NProto::TStorageServiceConfig config;
            auto state = MakeIntrusive<TDiskRegistryState>();
            state->CurrentErrorCode = code;
            auto runtime = PrepareTestActorRuntime(config, state);

            TVolumeClient volume(*runtime);
            volume.UpdateVolumeConfig(
                0,
                0,
                0,
                0,
                false,
                1,
                NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
                DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize,
                "fail"
            );

            volume.SendWaitReadyRequest();
            {
                auto response = volume.RecvWaitReadyResponse();
                UNIT_ASSERT_VALUES_EQUAL(code, response->GetStatus());
            }

            if (code != E_BS_RESOURCE_EXHAUSTED) {
                ++criticalEvents;
            }
            UNIT_ASSERT_VALUES_EQUAL(criticalEvents, diskAllocationFailureCounter->Val());
        }
    }

    Y_UNIT_TEST(ShouldStatVolumeWithoutPartitionReadiness)
    {
        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->CurrentErrorCode = E_BS_RESOURCE_EXHAUSTED;
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize,
            "fail"
        );

        {
            NProto::TVolumeClientInfo info;
            info.SetClientId("c");
            info.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_WRITE);
            info.SetVolumeMountMode(NProto::VOLUME_MOUNT_LOCAL);
            volume.AddClient(info);
        }

        volume.SendStatVolumeRequest(
            TString(),          // clientId
            TVector<TString>(), // storageConfigFields
            true                // noPartition
        );

        {
            auto response = volume.RecvStatVolumeResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
            const auto& clients = response->Record.GetClients();
            UNIT_ASSERT_VALUES_EQUAL(1, clients.size());
            UNIT_ASSERT_VALUES_EQUAL("c", clients[0].GetClientId());
        }
    }

    Y_UNIT_TEST(ShouldForwardRequestsToMirroredPartition)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 2;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR3,
            1024
        );

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        const auto& replicas = stat->Record.GetVolume().GetReplicas();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());

        UNIT_ASSERT_VALUES_EQUAL(2, replicas.size());
        UNIT_ASSERT_VALUES_EQUAL(1, replicas[0].DevicesSize());
        UNIT_ASSERT_VALUES_EQUAL(
            "transport1",
            replicas[0].GetDevices(0).GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(1, replicas[1].DevicesSize());
        UNIT_ASSERT_VALUES_EQUAL(
            "transport2",
            replicas[1].GetDevices(0).GetTransportId());

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& v = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, v.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                v.GetDevices(0).GetTransportId()
            );

            UNIT_ASSERT_VALUES_EQUAL(2, v.ReplicasSize());
            UNIT_ASSERT_VALUES_EQUAL(1, v.GetReplicas(0).DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport1",
                v.GetReplicas(0).GetDevices(0).GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(1, v.GetReplicas(1).DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport2",
                v.GetReplicas(1).GetDevices(0).GetTransportId());
        }

        ui64 writeRequests = 0;

        auto obs = [&] (TAutoPtr<IEventHandle>& event) {
            if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumePartCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumePartCounters>();

                writeRequests +=
                    msg->DiskCounters->RequestCounters.WriteBlocks.Count;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        runtime->SetObserverFunc(obs);

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(3, writeRequests);

        auto resp = volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldNotReadFromFreshDevices)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->DeviceReplacementUUIDs = {"uuid1"};
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 1;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR2,
            1024
        );

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& v = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, v.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", v.GetDevices(0).GetDeviceUUID());

            UNIT_ASSERT_VALUES_EQUAL(1, v.ReplicasSize());
            UNIT_ASSERT_VALUES_EQUAL(1, v.GetReplicas(0).DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "uuid1",
                v.GetReplicas(0).GetDevices(0).GetDeviceUUID());
        }

        auto writeToAgent = [&] (char c, const TString& deviceId) {
            auto diskAgentActorId = MakeDiskAgentServiceId(runtime->GetNodeId());
            auto sender = runtime->AllocateEdgeActor();

            auto request =
                std::make_unique<TEvDiskAgent::TEvWriteDeviceBlocksRequest>();

            request->Record.SetStartIndex(0);
            *request->Record.MutableBlocks()->AddBuffers() = GetBlockContent(c);
            request->Record.SetBlockSize(4_KB);
            request->Record.SetDeviceUUID(deviceId);

            runtime->Send(new IEventHandle(
                diskAgentActorId,
                sender,
                request.release()));

            runtime->DispatchEvents(TDispatchOptions(), TDuration::Seconds(1));
        };

        writeToAgent('A', "uuid0");

#define TEST_READ(c) {                                                         \
            auto resp = volume.ReadBlocks(                                     \
                TBlockRange64::MakeOneBlock(0),                                \
                clientInfo.GetClientId());                                     \
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();          \
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());                          \
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(c), bufs[0]);             \
        }                                                                      \
// TEST_READ

        TEST_READ('A');
        TEST_READ('A');

        state->DeviceReplacementUUIDs = {"uuid0"};
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();
        volume.AddClient(clientInfo);

        writeToAgent('B', "uuid1");

        TEST_READ('B');
        TEST_READ('B');

        volume.RebootTablet();
        volume.WaitReady();
        volume.AddClient(clientInfo);

        TEST_READ('B');
        TEST_READ('B');

        auto stat = volume.StatVolume();
        const auto& v = stat->Record.GetVolume();
        UNIT_ASSERT_VALUES_EQUAL(1, v.FreshDeviceIdsSize());
        UNIT_ASSERT_VALUES_EQUAL("uuid0", v.GetFreshDeviceIds(0));

        volume.RemoveClient(clientInfo.GetClientId());

#undef TEST_READ
    }

    Y_UNIT_TEST(ShouldFillRequestIdInDeviceBlocksRequest)
    {
        NProto::TStorageServiceConfig config;
        config.SetAssignIdToWriteAndZeroRequestsEnabled(true);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            DefaultDeviceBlockSize * DefaultDeviceBlockCount /
                DefaultBlockSize);

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        ui64 writeRequestId = 0;
        ui64 zeroRequestId = 0;
        auto checkDeviceRequest = [&](TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() ==
                TEvDiskAgent::EvWriteDeviceBlocksRequest)
            {
                auto* msg = event->Get<TEvDiskAgent::TEvWriteDeviceBlocksRequest>();
                UNIT_ASSERT_VALUES_EQUAL(0, writeRequestId);
                writeRequestId = msg->Record.GetVolumeRequestId();
            }
            if (event->GetTypeRewrite() ==
                TEvDiskAgent::EvZeroDeviceBlocksRequest)
            {
                auto* msg = event->Get<TEvDiskAgent::TEvZeroDeviceBlocksRequest>();
                UNIT_ASSERT_VALUES_EQUAL(0, zeroRequestId);
                zeroRequestId = msg->Record.GetVolumeRequestId();

            }
            return TTestActorRuntime::DefaultObserverFunc(event);
        };
        runtime->SetObserverFunc(checkDeviceRequest);

        volume.WriteBlocks(GetBlockRangeById(0), clientInfo.GetClientId(), 's');
        volume.ZeroBlocks(GetBlockRangeById(0), clientInfo.GetClientId());

        UNIT_ASSERT_VALUES_UNEQUAL(0, writeRequestId);
        UNIT_ASSERT_VALUES_UNEQUAL(0, zeroRequestId);
        UNIT_ASSERT_GT(zeroRequestId, writeRequestId);
    }

    Y_UNIT_TEST(ShouldReportMigrationProgressForReplicatingMirroredDisk)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->DeviceReplacementUUIDs = {"uuid1"};
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 1;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR2,
            1024
        );

        volume.WaitReady();

        ui32 migrationProgressCounter = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
                {
                    auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();
                    migrationProgressCounter =
                        msg->VolumeSelfCounters->Simple.MigrationProgress.Value;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.SendToPipe(
            std::make_unique<TEvVolume::TEvUpdateMigrationState>(512));
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(50, migrationProgressCounter);

        state->DeviceReplacementUUIDs = {};
        volume.ReallocateDisk();
        volume.ReconnectPipe();
        volume.WaitReady();

        volume.SendToPipe(
            std::make_unique<TEvVolume::TEvUpdateMigrationState>(512));
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(0, migrationProgressCounter);
    }

    Y_UNIT_TEST(ShouldWaitReady)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();
    }

    Y_UNIT_TEST(ShouldForwardRequests)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        volume.StatVolume();
    }

    Y_UNIT_TEST(ShouldRebootDeadPartitions)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);

        TActorId partActorId;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TEvPartition::EvWaitReadyResponse) {
                    UNIT_ASSERT(!partActorId);
                    partActorId = event->Sender;
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            });

        volume.UpdateVolumeConfig();
        volume.WaitReady();

        UNIT_ASSERT(partActorId);

        auto sender = runtime->AllocateEdgeActor();
        runtime->Send(
            new IEventHandle(partActorId, sender, new TEvents::TEvPoisonPill()));

        partActorId = {};

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvPartition::EvWaitReadyResponse);
        runtime->DispatchEvents(options);

        UNIT_ASSERT(partActorId);
        volume.StatVolume();
    }

    Y_UNIT_TEST(ShouldPersistAddedAndRemovedClients)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        TVolumeClient client1(*runtime);
        TVolumeClient client2(*runtime);
        TVolumeClient client3(*runtime);

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);

        auto clientInfo3 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            0);

        client1.AddClient(clientInfo1);
        client2.AddClient(clientInfo2);
        client3.AddClient(clientInfo3);

        client1.RemoveClient(clientInfo1.GetClientId());

        volume.RebootTablet();
        volume.WaitReady();

        client1.ReconnectPipe();
        client1.AddClient(clientInfo1);

        {
            client2.ReconnectPipe();
            auto response = client2.AddClient(clientInfo2);
            UNIT_ASSERT(!FAILED(response->GetStatus()));
        }

        {
            client3.ReconnectPipe();
            auto response = client3.AddClient(clientInfo3);
            UNIT_ASSERT(!FAILED(response->GetStatus()));
        }
    }

    Y_UNIT_TEST(ShouldRemoveInactiveClients)
    {
        auto unmountClientsTimeout = TDuration::Seconds(9);
        NProto::TStorageServiceConfig storageServiceConfig;
        storageServiceConfig.SetClientRemountPeriod(unmountClientsTimeout.MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(unmountClientsTimeout.MilliSeconds());
        auto runtime = PrepareTestActorRuntime(storageServiceConfig);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);
        volume.RebootTablet();

        volume.WaitReady();

        auto secondClientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        // Immediate add second client request should fail
        // as the first client hasn't timed out yet
        volume.SendAddClientRequest(secondClientInfo);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(FAILED(response->GetStatus()));
        }

        runtime->UpdateCurrentTime(runtime->GetCurrentTime() + unmountClientsTimeout);

        // Now should be able to add the second client as the first client timed
        // out
        volume.AddClient(secondClientInfo);
    }

    Y_UNIT_TEST(ShouldRejectReadWriteRequestsFromUnaccountedClients)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        storageServiceConfig.SetClientRemountPeriod(TDuration::Seconds(10).MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(TDuration::Seconds(10).MilliSeconds());

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto range = TBlockRange64::WithLength(0, 1);

        {
            auto request = volume.CreateReadBlocksRequest(
                range,
                clientInfo.GetClientId()
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvReadBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_BS_INVALID_SESSION);
        }

        {
            auto request = volume.CreateWriteBlocksRequest(
                range,
                clientInfo.GetClientId(),
                char(1)
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvWriteBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_BS_INVALID_SESSION);
        }

        {
            auto request = volume.CreateZeroBlocksRequest(
                range,
                clientInfo.GetClientId()
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvZeroBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_BS_INVALID_SESSION);
        }
    }

    Y_UNIT_TEST(ShouldRejectWriteRequestsFromClientsAddedWithReadOnlyAccess)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        storageServiceConfig.SetClientRemountPeriod(TDuration::Seconds(10).MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(TDuration::Seconds(10).MilliSeconds());

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        auto range = TBlockRange64::WithLength(0, 1);

        {
            auto request = volume.CreateReadBlocksRequest(
                range,
                clientInfo.GetClientId()
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvReadBlocksResponse>();
            UNIT_ASSERT(SUCCEEDED(response->GetStatus()));
        }

        {
            auto request = volume.CreateWriteBlocksRequest(
                range,
                clientInfo.GetClientId(),
                char(1)
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvWriteBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_ARGUMENT);
        }

        {
            auto request = volume.CreateZeroBlocksRequest(
                range,
                clientInfo.GetClientId()
            );
            request->Record.MutableHeaders()->SetClientId(clientInfo.GetClientId());

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvZeroBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_ARGUMENT);
        }
    }

    Y_UNIT_TEST(ShouldRejectReadWriteRequestsWhilePartitionsAreNotReady)
    {
        auto runtime = PrepareTestActorRuntime();

        runtime->SetObserverFunc([] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvWaitReadyResponse: {
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            });

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto range = TBlockRange64::WithLength(0, 1);

        {
            auto request = volume.CreateReadBlocksRequest(
                range,
                clientInfo.GetClientId()
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvReadBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_REJECTED);
            UNIT_ASSERT(response->GetErrorReason().Contains("not ready"));
        }

        {
            auto request = volume.CreateWriteBlocksRequest(
                range,
                clientInfo.GetClientId(),
                char(1)
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvWriteBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_REJECTED);
            UNIT_ASSERT(response->GetErrorReason().Contains("not ready"));
        }

        {
            auto request = volume.CreateZeroBlocksRequest(
                range,
                clientInfo.GetClientId()
            );
            request->Record.MutableHeaders()->SetClientId(clientInfo.GetClientId());

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvService::TEvZeroBlocksResponse>();
            UNIT_ASSERT(response->GetStatus() == E_REJECTED);
            UNIT_ASSERT(response->GetErrorReason().Contains("not ready"));
        }
    }

    Y_UNIT_TEST(ShouldntThrottleIfThrottlingEnabledFlagIsNotSet)
    {
#define DO_TEST(mainConfFlag, volumeConfFlag, isSsd) {                      \
        NProto::TStorageServiceConfig config;                               \
        if (isSsd) {                                                        \
            config.SetThrottlingEnabledSSD(mainConfFlag);                   \
            config.SetThrottlingEnabled(true);                              \
        } else {                                                            \
            config.SetThrottlingEnabledSSD(true);                           \
            config.SetThrottlingEnabled(mainConfFlag);                      \
        }                                                                   \
        auto runtime = PrepareTestActorRuntime(config);                     \
                                                                            \
        TVolumeClient volume(*runtime);                                     \
        volume.UpdateVolumeConfig(                                          \
            DefaultBlockSize,                                               \
            1,                                                              \
            200,                                                            \
            DefaultBlockSize * 3,                                           \
            volumeConfFlag,                                                 \
            1,                                                              \
            isSsd                                                           \
            ? NCloud::NProto::STORAGE_MEDIA_SSD                             \
            : NCloud::NProto::STORAGE_MEDIA_HYBRID                          \
        );                                                                  \
        volume.WaitReady();                                                 \
        auto clientInfo = CreateVolumeClientInfo(                           \
            NProto::VOLUME_ACCESS_READ_WRITE,                               \
            NProto::VOLUME_MOUNT_LOCAL,                                     \
            0);                                                             \
        volume.AddClient(clientInfo);                                       \
                                                                            \
        const auto tenBlocks = TBlockRange64::WithLength(0, 10);            \
        volume.ReadBlocks(tenBlocks, clientInfo.GetClientId());             \
        volume.WriteBlocks(tenBlocks, clientInfo.GetClientId());            \
        volume.DescribeBlocks(tenBlocks, clientInfo.GetClientId());         \
} // DO_TEST

        DO_TEST(false, false, false);
        DO_TEST(true, false, false);
        DO_TEST(false, true, false);
        DO_TEST(false, false, true);
        DO_TEST(true, false, true);
        DO_TEST(false, true, true);

#undef DO_TEST
    }

    struct TThrottledVolumeTestEnv
    {
        std::unique_ptr<TTestActorRuntime> Runtime;
        std::unique_ptr<TVolumeClient> Volume;

        TThrottledVolumeTestEnv(
            ui32 postponedWeightMultiplier,
            NCloud::NProto::EStorageMediaKind mediaKind
                = NCloud::NProto::STORAGE_MEDIA_HYBRID,
            TDuration maxThrottlerDelay = TDuration::Seconds(25),
            bool diskSpaceScoreThrottlingEnabled = false)
        {
            NProto::TStorageServiceConfig config;
            config.SetThrottlingEnabled(true);
            config.SetThrottlingEnabledSSD(true);
            config.SetMaxThrottlerDelay(maxThrottlerDelay.MilliSeconds());
            config.SetDiskSpaceScoreThrottlingEnabled(
                diskSpaceScoreThrottlingEnabled);
            Runtime = PrepareTestActorRuntime(config);

            Volume.reset(new TVolumeClient(*Runtime));
            // we need to multiply all rates to take iops-bandwidth dependency
            // into account
            Volume->UpdateVolumeConfig(
                2 * DefaultBlockSize,
                2,
                100,
                DefaultBlockSize * postponedWeightMultiplier,
                true,
                1,
                mediaKind
            );
            Volume->WaitReady();
        }

        TThrottledVolumeTestEnv(
            ui32 throttlerStateWriteIntervalMilliseconds,
            ui32 boostTimeMilliseconds,
            ui32 boostPercentage,
            NCloud::NProto::EStorageMediaKind mediaKind)
        {
            NProto::TStorageServiceConfig config;
            config.SetThrottlingEnabled(true);
            config.SetThrottlerStateWriteInterval(throttlerStateWriteIntervalMilliseconds);
            config.SetMaxThrottlerDelay(TDuration::Seconds(25).MilliSeconds());
            Runtime = PrepareTestActorRuntime(config);

            Volume = std::make_unique<TVolumeClient>(*Runtime);
            auto request = Volume->CreateUpdateVolumeConfigRequest(
                DefaultBlockSize,
                1,
                100,
                DefaultBlockSize,
                true,
                1,
                mediaKind
            );
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostTime(boostTimeMilliseconds);
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostPercentage(boostPercentage);
            Volume->SendToPipe(std::move(request));
            auto response = Volume->RecvUpdateVolumeConfigResponse();
            UNIT_ASSERT_C(
                response->Record.GetStatus() == NKikimrBlockStore::OK,
                "Unexpected status: " <<
                NKikimrBlockStore::EStatus_Name(response->Record.GetStatus()));
            Volume->WaitReady();
        }
    };

    void DoTestShouldThrottleSomeOps(
        NCloud::NProto::EStorageMediaKind mediaKind,
        bool diskSpaceScoreThrottlingEnabled)
    {
        TThrottledVolumeTestEnv env(
            5,
            mediaKind,
            TDuration::Seconds(25),
            diskSpaceScoreThrottlingEnabled);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        // due to the dependency between iops and bandwidth, one block is
        // exactly 2 times cheaper than three blocks and exactly 3 times
        // cheaper than five blocks
        const auto oneBlock = TBlockRange64::MakeOneBlock(0);
        const auto twoBlocks = TBlockRange64::WithLength(0, 2);
        const auto threeBlocks = TBlockRange64::WithLength(0, 3);
        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        const auto describeBlocksCode =
            mediaKind == NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED
            ? E_NOT_IMPLEMENTED : S_OK;

        // 0. testing that at 1rps nothing is throttled
        for (size_t i = 0; i < 10; ++i) {
            TICK(runtime);
            volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
            TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK);

            TICK(runtime);
            volume.SendWriteBlocksRequest(oneBlock, clientInfo.GetClientId());
            TEST_QUICK_RESPONSE(runtime, WriteBlocks, S_OK);

            TICK(runtime);
            // here and later Describe requests are doubled since their
            // throttling ignores request size => oneBlock Describe requests
            // are 2 times more 'light' than Read/Write requests
            volume.SendDescribeBlocksRequest(twoBlocks, clientInfo.GetClientId(), 1);
            TEST_QUICK_RESPONSE_VOLUME_EVENT(
                runtime,
                DescribeBlocks,
                describeBlocksCode
            );
            volume.SendDescribeBlocksRequest(twoBlocks, clientInfo.GetClientId(), 1);
            TEST_QUICK_RESPONSE_VOLUME_EVENT(
                runtime,
                DescribeBlocks,
                describeBlocksCode
            );
        }

        // 1. testing that excess requests are postponed
        for (ui32 i = 0; i < 19; ++i) { // 1 non-write request occupies 1KiB in queue
            volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
            TEST_NO_RESPONSE(runtime, ReadBlocks);
        }

        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 1);
        TEST_NO_RESPONSE_VOLUME_EVENT(runtime, DescribeBlocks);

        // testing that DescribeBlocks requests with zero BlocksCountToRead are
        // not affected by limits
        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 0);
        TEST_QUICK_RESPONSE_VOLUME_EVENT(
            runtime,
            DescribeBlocks,
            describeBlocksCode
        );

        // 2. testing that we start rejecting requests after our postponed limit saturates
        volume.SendReadBlocksRequest(twoBlocks, clientInfo.GetClientId());
        TEST_QUICK_RESPONSE(runtime, ReadBlocks, E_REJECTED);

        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 1);
        TEST_QUICK_RESPONSE_VOLUME_EVENT(runtime, DescribeBlocks, E_REJECTED);
        // testing that DescribeBlocks requests with zero BlocksCountToRead are
        // not affected by limits
        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 0);
        TEST_QUICK_RESPONSE_VOLUME_EVENT(
            runtime,
            DescribeBlocks,
            describeBlocksCode
        );

        // 3. testing that after some time passes our postponed requests are successfully processed
        // test actor runtime will automatically advance the timer for us
        for (ui32 i = 0; i < 19; ++i) {
            TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);
        }

        TEST_RESPONSE_VOLUME_EVENT(
            volume,
            DescribeBlocks,
            describeBlocksCode,
            WaitTimeout
        );

        // 4. testing that bursts actually work
        TICK(runtime);
        TICK(runtime);
        volume.SendReadBlocksRequest(threeBlocks, clientInfo.GetClientId());
        TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK);
        volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);

        // 5. requests of any size should work, but not immediately (TODO: test precise timings)
        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);

        // testing backpressure effects
        TICK(runtime);
        TICK(runtime);
        volume.SendReadBlocksRequest(threeBlocks, clientInfo.GetClientId()); // spending current budget
        TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK);
        volume.SendToPipe(volume.CreateBackpressureReport({3, 0, 0, 0}));
        TICK(runtime);
        TICK(runtime);
        volume.SendReadBlocksRequest(threeBlocks, clientInfo.GetClientId());
        TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK); // reads should not be affected by backpressure
        TICK(runtime);
        TICK(runtime);
        volume.SendWriteBlocksRequest(oneBlock, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, WriteBlocks); // but writes should
        TEST_RESPONSE(volume, WriteBlocks, S_OK, WaitTimeout);

        volume.SendToPipe(volume.CreateBackpressureReport({0, 0, 3, 0}));
        TICK(runtime);
        TICK(runtime);
        volume.SendWriteBlocksRequest(oneBlock, clientInfo.GetClientId());
        if (diskSpaceScoreThrottlingEnabled) {
            TEST_NO_RESPONSE(runtime, WriteBlocks);
            TEST_RESPONSE(volume, WriteBlocks, S_OK, WaitTimeout);
        } else {
            TEST_QUICK_RESPONSE(runtime, WriteBlocks, S_OK);
        }
    }

    void DoThrottlerTestWhenTabletRestarts(bool sysTabletRestart)
    {
        TThrottledVolumeTestEnv env(5);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        if (sysTabletRestart) {
            volume.RebootSysTablet();
        } else {
            volume.RebootTablet();
        }
        TEST_QUICK_RESPONSE(runtime, ReadBlocks, E_REJECTED);
    }

    Y_UNIT_TEST(ShouldThrottleSomeOpsSSD)
    {
        DoTestShouldThrottleSomeOps(NCloud::NProto::STORAGE_MEDIA_SSD, false);
    }

    Y_UNIT_TEST(ShouldThrottleSomeOpsSSDWithDiskSpaceScore)
    {
        DoTestShouldThrottleSomeOps(NCloud::NProto::STORAGE_MEDIA_SSD, true);
    }

    Y_UNIT_TEST(ShouldThrottleSomeOpsHybrid)
    {
        DoTestShouldThrottleSomeOps(NCloud::NProto::STORAGE_MEDIA_HYBRID, false);
    }

    Y_UNIT_TEST(ShouldThrottleSomeOpsHDD)
    {
        DoTestShouldThrottleSomeOps(NCloud::NProto::STORAGE_MEDIA_HDD, false);
    }

    Y_UNIT_TEST(ShouldThrottleSomeOpsSSDNonreplicated)
    {
        DoTestShouldThrottleSomeOps(NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED, false);
    }

    void DoTestShouldNotSaveThrottlerState(const NProto::EStorageMediaKind mediaKind)
    {
        TThrottledVolumeTestEnv env(
            30'000,   // throttlerStateWriteIntervalMilliseconds
            10'000,   // boostTimeMilliseconds
            200,      // boostPercentage
            mediaKind);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);
        UNIT_ASSERT_VALUES_EQUAL(10'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 9'000

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(30'000));

        const auto thirtyThreeBlocks = TBlockRange64::WithLength(0, 33);

        volume.SendReadBlocksRequest(thirtyThreeBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 7'250

        volume.RebootTablet();
        volume.WaitReady();
        UNIT_ASSERT_VALUES_EQUAL(10'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());
    }

    Y_UNIT_TEST(ShouldNotSaveThrottlerStateOnSSD)
    {
        DoTestShouldNotSaveThrottlerState(NProto::EStorageMediaKind::STORAGE_MEDIA_SSD);
    }

    Y_UNIT_TEST(ShouldNotSaveThrottlerStateOnSSDNonreplicated)
    {
        DoTestShouldNotSaveThrottlerState(NProto::EStorageMediaKind::STORAGE_MEDIA_SSD_NONREPLICATED);
    }

    void DoTestShouldSaveThrottlerState(const NProto::EStorageMediaKind mediaKind)
    {
        TThrottledVolumeTestEnv env(
            30'000,   // throttlerStateWriteIntervalMilliseconds
            10'000,   // boostTimeMilliseconds
            200,      // boostPercentage
            mediaKind);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);
        UNIT_ASSERT_VALUES_EQUAL(10'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 9'000

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(30'000));

        const auto thirtyThreeBlocks = TBlockRange64::WithLength(0, 33);

        volume.SendReadBlocksRequest(thirtyThreeBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 7'250

        volume.RebootTablet();
        volume.WaitReady();
        UNIT_ASSERT_VALUES_EQUAL(7'250, volume.StatVolume()->Record.GetStats().GetBoostBudget());
    }

    Y_UNIT_TEST(ShouldSaveThrottlerStateOnHybrid)
    {
        DoTestShouldSaveThrottlerState(NProto::EStorageMediaKind::STORAGE_MEDIA_HYBRID);
    }

    Y_UNIT_TEST(ShouldSaveThrottlerStateOnHDD)
    {
        DoTestShouldSaveThrottlerState(NProto::EStorageMediaKind::STORAGE_MEDIA_HDD);
    }

    Y_UNIT_TEST(ShouldNotSaveThrottlerStateBeforeTimeout)
    {
        TThrottledVolumeTestEnv env(
            30'000,   // throttlerStateWriteIntervalMilliseconds
            10'000,   // boostTimeMilliseconds
            200,      // boostPercentage
            NCloud::NProto::EStorageMediaKind::STORAGE_MEDIA_HDD);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);
        UNIT_ASSERT_VALUES_EQUAL(10'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 9'000

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(30'000));

        const auto thirtyThreeBlocks = TBlockRange64::WithLength(0, 33);

        volume.SendReadBlocksRequest(thirtyThreeBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 7'250

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(25'000));

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 8'000

        volume.RebootTablet();
        volume.WaitReady();
        UNIT_ASSERT_VALUES_EQUAL(7'250, volume.StatVolume()->Record.GetStats().GetBoostBudget());
    }

    Y_UNIT_TEST(ShouldNotRefillThrottlerStateUponUpdateConfigRequest)
    {
        NProto::TStorageServiceConfig config;
        config.SetThrottlingEnabled(true);
        config.SetThrottlerStateWriteInterval(TDuration::Seconds(1).MilliSeconds());
        config.SetMaxThrottlerDelay(TDuration::Seconds(25).MilliSeconds());
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        {
            auto request = volume.CreateUpdateVolumeConfigRequest(
                DefaultBlockSize,
                1,
                100,
                DefaultBlockSize,
                true,
                1,
                NCloud::NProto::STORAGE_MEDIA_HDD
            );
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostTime(10'000);
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostPercentage(200);
            volume.SendToPipe(std::move(request));
            auto response = volume.RecvUpdateVolumeConfigResponse();
            UNIT_ASSERT_C(
                response->Record.GetStatus() == NKikimrBlockStore::OK,
                "Unexpected status: " <<
                NKikimrBlockStore::EStatus_Name(response->Record.GetStatus()));
            volume.WaitReady();
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);
        UNIT_ASSERT_VALUES_EQUAL(10'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);   // boost = 9'000

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(config.GetThrottlerStateWriteInterval()));

        {
            auto request = volume.CreateUpdateVolumeConfigRequest(
                DefaultBlockSize,
                1,
                100,
                DefaultBlockSize,
                true,
                1,
                NCloud::NProto::STORAGE_MEDIA_HDD
            );
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostTime(10'000);
            request->Record.MutableVolumeConfig()->SetPerformanceProfileBoostPercentage(200);
            volume.SendToPipe(std::move(request));
            auto response = volume.RecvUpdateVolumeConfigResponse();
            UNIT_ASSERT_C(
                response->Record.GetStatus() == NKikimrBlockStore::OK,
                "Unexpected status: " <<
                NKikimrBlockStore::EStatus_Name(response->Record.GetStatus()));
            volume.WaitReady();
        }

        volume.RebootTablet();
        volume.WaitReady();
        UNIT_ASSERT_VALUES_EQUAL(9'000, volume.StatVolume()->Record.GetStats().GetBoostBudget());
    }

    Y_UNIT_TEST(ShouldMaintainRequestOrderWhenThrottling)
    {
        TThrottledVolumeTestEnv env(6);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto oneBlock = TBlockRange64::MakeOneBlock(0);
        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        // small request shouldn't be able to bypass the large one that was sent earlier
        volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        TICK(runtime);
        TEST_NO_RESPONSE(runtime, ReadBlocks);
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);
    }

    Y_UNIT_TEST(ShouldProperlyProcessOldPostponedRequestsAfterConfigUpdate)
    {
        TThrottledVolumeTestEnv env(5);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto fiveBlocks = TBlockRange64::WithLength(0, 5);

        volume.SendReadBlocksRequest(fiveBlocks, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        volume.UpdateVolumeConfig(0, 0, 0, 0, false, 2);

        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);
    }

    Y_UNIT_TEST(ShouldRespondToThrottledRequestsUponTabletDeath)
    {
        DoThrottlerTestWhenTabletRestarts(false);
    }

    Y_UNIT_TEST(ShouldRespondToThrottledRequestsUponSysTabletDeath)
    {
        DoThrottlerTestWhenTabletRestarts(true);
    }

    Y_UNIT_TEST(ShouldntThrottleIfThrottlingIsDisabledInMountOptions)
    {
        const auto theRange = TBlockRange64::MakeClosedInterval(0, 10);

        auto throttledClient = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        ui32 mountFlags = 0;
        SetProtoFlag(mountFlags, NProto::MF_THROTTLING_DISABLED);

        auto specialClient = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_REMOTE,
            mountFlags);

        {
            TThrottledVolumeTestEnv env(theRange.Size());
            auto& runtime = env.Runtime;
            //auto& volume = *env.Volume;
            TVolumeClient throttledVolumeClient(*runtime);
            TVolumeClient specialVolumeClient(*runtime);

            throttledVolumeClient.AddClient(throttledClient);
            specialVolumeClient.AddClient(specialClient);

            {
                auto request = throttledVolumeClient.CreateReadBlocksRequest(
                    theRange,
                    throttledClient.GetClientId()
                );

                throttledVolumeClient.SendToPipe(std::move(request));
                TEST_NO_RESPONSE(runtime, ReadBlocks);
            }

            {
                auto request = specialVolumeClient.CreateReadBlocksRequest(
                    theRange,
                    specialClient.GetClientId()
                );

                specialVolumeClient.SendToPipe(std::move(request));

                TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK);
            }
        }

        {
            TThrottledVolumeTestEnv env(theRange.Size());
            auto& runtime = env.Runtime;
            //auto& volume = *env.Volume;
            TVolumeClient throttledVolumeClient(*runtime);
            TVolumeClient specialVolumeClient(*runtime);

            throttledVolumeClient.AddClient(throttledClient);
            specialVolumeClient.AddClient(specialClient);

            {
                auto request = specialVolumeClient.CreateReadBlocksRequest(
                    theRange,
                    specialClient.GetClientId()
                );

                specialVolumeClient.SendToPipe(std::move(request));
                TEST_QUICK_RESPONSE(runtime, ReadBlocks, S_OK);
            }

            {
                auto request = throttledVolumeClient.CreateReadBlocksRequest(
                    theRange,
                    throttledClient.GetClientId()
                );

                throttledVolumeClient.SendToPipe(std::move(request));
                TEST_NO_RESPONSE(runtime, ReadBlocks);
            }
        }
    }

    Y_UNIT_TEST(ShouldRejectRequestsThrottledForTooLong)
    {
        TThrottledVolumeTestEnv env(
            5,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            TDuration::Seconds(1));
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto threeBlocks =
            TBlockRange64::WithLength(0, 3);   // delay = 2s, budget = 1s,
        // the resulting delay should be exactly 1s
        volume.SendReadBlocksRequest(threeBlocks, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        // should stay in queue for at least 1s
        const auto oneBlock = TBlockRange64::MakeOneBlock(0);
        // => more than limit
        volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
        TEST_NO_RESPONSE(runtime, ReadBlocks);

        // TODO test that the second request gets rejected, not just any one of
        // the two requests
        TEST_RESPONSE(volume, ReadBlocks, E_REJECTED, TDuration::Seconds(1));
        TEST_RESPONSE(volume, ReadBlocks, S_OK, WaitTimeout);
    }

    Y_UNIT_TEST(ShouldAcceptAddClientRequestWithLargerRequestGeneration)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto addClientRequest = volume.CreateAddClientRequest(clientInfo);
        SetRequestGeneration(1, *addClientRequest);
        volume.SendToPipe(std::move(addClientRequest));
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(SUCCEEDED(response->GetStatus()));
        }

        addClientRequest = volume.CreateAddClientRequest(clientInfo);
        SetRequestGeneration(2, *addClientRequest);
        volume.SendToPipe(std::move(addClientRequest));
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(SUCCEEDED(response->GetStatus()));
        }
    }

    Y_UNIT_TEST(ShouldHandleDescribeBlocksRequest)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        const auto range = TBlockRange64::WithLength(0, 1);
        {
            auto request = volume.CreateDescribeBlocksRequest(
                range,
                clientInfo.GetClientId()
            );

            volume.SendToPipe(std::move(request));
            auto response = volume.RecvResponse<TEvVolume::TEvDescribeBlocksResponse>();
            UNIT_ASSERT(SUCCEEDED(response->GetStatus()));
        }
    }

    Y_UNIT_TEST(ShouldHandleGetUsedBlocksRequest)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();
        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.SendToPipe(
            std::make_unique<TEvVolume::TEvGetUsedBlocksRequest>()
        );
        auto response = volume.RecvResponse<TEvVolume::TEvGetUsedBlocksResponse>();
        UNIT_ASSERT(SUCCEEDED(response->GetStatus()));
    }

    Y_UNIT_TEST(ShouldProperlySetDisconnectTimeAtStartup)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        auto unmountTime = TDuration::Seconds(10);
        storageServiceConfig.SetClientRemountPeriod(TDuration::Seconds(10).MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(TDuration::Seconds(10).MilliSeconds());

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo1);

        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }

        volume.RebootTablet();

        runtime->AdvanceCurrentTime(unmountTime / 2);
        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }

        volume.RebootTablet();
        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }
        runtime->AdvanceCurrentTime(unmountTime / 2);

        volume.AddClient(clientInfo2);
    }

    Y_UNIT_TEST(ShouldClearPersistedDisconnectTimeUponReconnect)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        auto unmountTime = TDuration::Seconds(10);
        storageServiceConfig.SetClientRemountPeriod(unmountTime.MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(unmountTime.MilliSeconds());
        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo1);

        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }

        volume.RebootTablet();

        runtime->AdvanceCurrentTime(unmountTime / 2);
        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }
        volume.AddClient(clientInfo1);

        volume.RebootTablet();
        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }
        runtime->AdvanceCurrentTime(unmountTime / 2);

        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }
    }

    Y_UNIT_TEST(ShouldRejectAddClientIfMountSeqNumberIsNotGreaterThanCurrent)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        auto unmountTime = TDuration::Seconds(10);
        storageServiceConfig.SetClientRemountPeriod(unmountTime.MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(unmountTime.MilliSeconds());

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            0);

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            0);

        volume.AddClient(clientInfo1);

        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }

        clientInfo1.SetMountSeqNumber(1);
        volume.AddClient(clientInfo1);

        clientInfo2.SetMountSeqNumber(1);
        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT(response->GetStatus() == E_BS_MOUNT_CONFLICT);
        }
    }

    Y_UNIT_TEST(ShouldResetMountSeqNumberWhenClientIsRemoved)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        auto unmountTime = TDuration::Seconds(10);
        storageServiceConfig.SetClientRemountPeriod(unmountTime.MilliSeconds());
        storageServiceConfig.SetInactiveClientsTimeout(unmountTime.MilliSeconds());

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            1);

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            0);

        volume.AddClient(clientInfo1);

        volume.SendAddClientRequest(clientInfo2);
        {
            auto response = volume.RecvAddClientResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_BS_MOUNT_CONFLICT, response->GetStatus());
        }
        volume.RebootTablet();
        runtime->AdvanceCurrentTime(unmountTime);
        volume.AddClient(clientInfo2);
    }

    Y_UNIT_TEST(ShouldWriteToCorrectDeviceRanges)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            2 * DefaultDeviceBlockSize * DefaultDeviceBlockCount / DefaultBlockSize
        );

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(2, devices.size());

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto range = TBlockRange64::MakeClosedInterval(
            devices[0].GetBlockCount() - 1,
            devices[0].GetBlockCount() + 98);

        volume.WriteBlocks(range, clientInfo.GetClientId(), 'X');
        auto resp = volume.ReadBlocks(range, clientInfo.GetClientId());
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(100, bufs.size());
        for (const auto& buf: bufs) {
            for (auto c: buf) {
                UNIT_ASSERT_VALUES_EQUAL(c, 'X');
            }
        }

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldForwardRequestsToMultipartitionVolume)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(v.GetBlocksCount(), 21 * 1024);
            UNIT_ASSERT_VALUES_EQUAL(v.GetPartitionsCount(), 3);
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& volumeProto = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(
                3 * 7 * 1024,
                volumeProto.GetBlocksCount()
            );
        }

        for (ui32 i = 0; i < 21; ++i) {
            volume.WriteBlocks(
                TBlockRange64::WithLength(1024 * i, 1024),
                clientInfo.GetClientId(),
                1 + i
            );
        }

        for (ui32 i = 0; i < 21; ++i) {
            auto resp = volume.ReadBlocks(
                TBlockRange64::WithLength(1024 * i, 1024),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1024, bufs.size());
            for (ui32 j = 0; j < 1024; ++j) {
                UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1 + i), bufs[j]);
            }
        }

        volume.WriteBlocks(
            TBlockRange64::MakeClosedInterval(500, 501),
            clientInfo.GetClientId(),
            30
        );
        volume.WriteBlocks(
            TBlockRange64::MakeClosedInterval(502, 503),
            clientInfo.GetClientId(),
            40
        );
        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(504),
            clientInfo.GetClientId(),
            50
        );

        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(500, 501),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(2, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[1]);
        }

        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(500, 504),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(5, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[1]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(40), bufs[2]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(40), bufs[3]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(50), bufs[4]);
        }

        volume.ZeroBlocks(
            TBlockRange64::MakeClosedInterval(503, 1526),
            clientInfo.GetClientId()
        );
        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(500, 504),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(5, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), bufs[1]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(40), bufs[2]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[3]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[4]);
        }

        volume.ZeroBlocks(
            TBlockRange64::MakeOneBlock(500),
            clientInfo.GetClientId()
        );
        volume.ZeroBlocks(
            TBlockRange64::MakeOneBlock(501),
            clientInfo.GetClientId()
        );
        volume.ZeroBlocks(
            TBlockRange64::MakeOneBlock(502),
            clientInfo.GetClientId()
        );
        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(500, 504),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(5, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[1]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[2]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[3]);
            UNIT_ASSERT_VALUES_EQUAL(TString(), bufs[4]);
        }

        // testing single-partition requests
        volume.WriteBlocks(
            TBlockRange64::MakeClosedInterval(500, 501),
            clientInfo.GetClientId(),
            50
        );
        volume.WriteBlocks(
            TBlockRange64::MakeClosedInterval(502, 503),
            clientInfo.GetClientId(),
            60
        );

        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(500, 501),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(2, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(50), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(50), bufs[1]);
        }

        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(502, 503),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(2, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(60), bufs[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(60), bufs[1]);
        }
    }

    void DoTestShouldForwardLocalRequestsToMultipartitionVolume(TString tags)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2,
            std::move(tags)
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(v.GetBlocksCount(), 21 * 1024);
            UNIT_ASSERT_VALUES_EQUAL(v.GetPartitionsCount(), 3);
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        for (ui32 i = 0; i < 21; ++i) {
            volume.WriteBlocksLocal(
                TBlockRange64::WithLength(1024 * i, 1024),
                clientInfo.GetClientId(),
                GetBlockContent(1 + i)
            );
        }

        for (ui32 i = 0; i < 21; ++i) {
            auto range = TBlockRange64::WithLength(1024 * i, 1024);
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = volume.ReadBlocksLocal(
                range,
                TGuardedSgList(std::move(sglist)),
                clientInfo.GetClientId()
            );
            for (ui32 j = 0; j < 1024; ++j) {
                UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1 + i), blocks[j]);
            }
        }

        volume.WriteBlocksLocal(
            TBlockRange64::MakeClosedInterval(500, 501),
            clientInfo.GetClientId(),
            GetBlockContent(30)
        );
        volume.WriteBlocksLocal(
            TBlockRange64::MakeClosedInterval(502, 503),
            clientInfo.GetClientId(),
            GetBlockContent(40)
        );
        volume.WriteBlocksLocal(
            TBlockRange64::MakeOneBlock(504),
            clientInfo.GetClientId(),
            GetBlockContent(50)
        );

        {
            const auto range = TBlockRange64::MakeClosedInterval(500, 501);
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = volume.ReadBlocksLocal(
                range,
                TGuardedSgList(std::move(sglist)),
                clientInfo.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), blocks[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), blocks[1]);
        }

        {
            const auto range = TBlockRange64::MakeClosedInterval(499, 504);
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = volume.ReadBlocksLocal(
                range,
                TGuardedSgList(std::move(sglist)),
                clientInfo.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), blocks[0]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), blocks[1]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(30), blocks[2]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(40), blocks[3]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(40), blocks[4]);
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(50), blocks[5]);
        }
    }

    Y_UNIT_TEST(ShouldForwardLocalRequestsToMultipartitionVolume)
    {
        DoTestShouldForwardLocalRequestsToMultipartitionVolume("");
    }

    Y_UNIT_TEST(ShouldForwardLocalRequestsToMultipartitionVolumeWithTrackUsed)
    {
        DoTestShouldForwardLocalRequestsToMultipartitionVolume("track-used");
    }

    Y_UNIT_TEST(ShouldForwardProtoWritesWithBigBuffersToMultipartitionVolume)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto write = [&] (ui32 startIndex, ui32 blocks, char fill) {
            auto request = std::make_unique<TEvService::TEvWriteBlocksRequest>();
            request->Record.SetStartIndex(startIndex);
            request->Record.MutableHeaders()->SetClientId(
                clientInfo.GetClientId());

            auto& buffers = *request->Record.MutableBlocks()->MutableBuffers();
            *buffers.Add() = TString(DefaultBlockSize * blocks, fill);

            volume.SendToPipe(std::move(request));

            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        };

        write(0, 20, 1);
        write(15, 10, 2);

        {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeClosedInterval(0, 24),
                clientInfo.GetClientId()
            );
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(25, bufs.size());
            for (ui32 i = 0; i < 15; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[i]);
            }
            for (ui32 i = 15; i < 25; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(2), bufs[i]);
            }
        }
    }

    Y_UNIT_TEST(ShouldProperlyHandleDestroyedSglistInMultipartitionVolume)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();

        auto range = TBlockRange64::WithLength(0, 1024);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        {
            auto blockContent = GetBlockContent(1);

            TSgList sglist;
            sglist.resize(range.Size(), {blockContent.Data(), blockContent.Size()});
            TGuardedSgList glist(std::move(sglist));

            auto request = std::make_unique<TEvService::TEvWriteBlocksLocalRequest>();
            request->Record.SetStartIndex(range.Start);
            request->Record.MutableHeaders()->SetClientId(clientInfo.GetClientId());
            request->Record.Sglist = glist;
            request->Record.BlocksCount = range.Size();
            request->Record.BlockSize = DefaultBlockSize;

            glist.Close();

            volume.SendToPipe(std::move(request));

            auto response = volume.RecvWriteBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                E_REJECTED,
                response->GetStatus(),
                response->GetErrorReason()
            );
        }

        {
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                range.Size(),
                TString::TUninitialized(DefaultBlockSize)
            );
            TGuardedSgList glist(std::move(sglist));
            glist.Close();

            volume.SendReadBlocksLocalRequest(
                range,
                glist,
                clientInfo.GetClientId()
            );

            auto response = volume.RecvReadBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                E_REJECTED,
                response->GetStatus(),
                response->GetErrorReason()
            );
        }
    }

    Y_UNIT_TEST(ShouldReallocateDisk)
    {
        auto runtime = PrepareTestActorRuntime();

        const auto expectedBlockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            expectedBlockCount);

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount());

        }

        {
            auto sender = runtime->AllocateEdgeActor();

            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskRequest>();
            request->Record.SetDiskId("vol0");
            request->Record.SetBlockSize(DefaultBlockSize);
            request->Record.SetBlocksCount(expectedDeviceCount * expectedBlockCount);

            runtime->Send(new IEventHandle(
                MakeDiskRegistryProxyServiceId(),
                sender,
                request.release()));
        }

        volume.ReallocateDisk();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("transport1", devices[1].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("transport2", devices[2].GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }
    }

    Y_UNIT_TEST(ShouldRejectAllocateDiskResponseWithInvalidDeviceSizes)
    {
        auto runtime = PrepareTestActorRuntime();

        const auto expectedBlockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            expectedDeviceCount * expectedBlockCount);

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }

        {
            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskResponse>();
            auto* device0 = request->Record.AddDevices();
            device0->SetDeviceUUID("uuid0");
            device0->SetBlocksCount(expectedBlockCount);
            device0->SetBlockSize(DefaultBlockSize);
            auto* device1 = request->Record.AddDevices();
            device1->SetDeviceUUID("uuid1_bad");
            device1->SetBlocksCount(expectedBlockCount + 1);
            device1->SetBlockSize(DefaultBlockSize);
            auto* device2 = request->Record.AddDevices();
            device2->SetDeviceUUID("uuid2");
            device2->SetBlocksCount(expectedBlockCount);
            device2->SetBlockSize(DefaultBlockSize);

            volume.SendToPipe(std::move(request));
        }

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }

        {
            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskResponse>();
            auto* device0 = request->Record.AddDevices();
            device0->SetDeviceUUID("uuid0");
            device0->SetBlocksCount(expectedBlockCount);
            device0->SetBlockSize(DefaultBlockSize);
            auto* device1 = request->Record.AddDevices();
            device1->SetDeviceUUID("uuid1_bad");
            device1->SetBlocksCount(expectedBlockCount);
            device1->SetBlockSize(2 * DefaultBlockSize);
            auto* device2 = request->Record.AddDevices();
            device2->SetDeviceUUID("uuid2");
            device2->SetBlocksCount(expectedBlockCount);
            device2->SetBlockSize(DefaultBlockSize);

            volume.SendToPipe(std::move(request));
        }

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }

        {
            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskResponse>();
            auto* device0 = request->Record.AddDevices();
            device0->SetDeviceUUID("uuid0");
            device0->SetBlocksCount(expectedBlockCount);
            device0->SetBlockSize(DefaultBlockSize);
            auto* device1 = request->Record.AddDevices();
            device1->SetDeviceUUID("uuid1_good");
            device1->SetBlocksCount(expectedBlockCount);
            device1->SetBlockSize(DefaultBlockSize);
            auto* device2 = request->Record.AddDevices();
            device2->SetDeviceUUID("uuid2");
            device2->SetBlocksCount(expectedBlockCount);
            device2->SetBlockSize(DefaultBlockSize);

            volume.SendToPipe(std::move(request));
        }

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1_good", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }

        {
            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskResponse>();
            auto* device0 = request->Record.AddDevices();
            device0->SetDeviceUUID("uuid0");
            device0->SetBlocksCount(expectedBlockCount);
            device0->SetBlockSize(DefaultBlockSize);
            auto* device1 = request->Record.AddDevices();
            device1->SetDeviceUUID("uuid1_bad");
            device1->SetBlocksCount(expectedBlockCount + 1);
            device1->SetBlockSize(DefaultBlockSize);
            auto* device2 = request->Record.AddDevices();
            device2->SetDeviceUUID("uuid2");
            device2->SetBlocksCount(expectedBlockCount);
            device2->SetBlockSize(DefaultBlockSize);
            auto* device3 = request->Record.AddDevices();
            device3->SetDeviceUUID("uuid3");
            device3->SetBlocksCount(expectedBlockCount);
            device3->SetBlockSize(DefaultBlockSize);

            volume.SendToPipe(std::move(request));
        }

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1_good", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
        }

        {
            auto request = std::make_unique<TEvDiskRegistry::TEvAllocateDiskResponse>();
            auto* device0 = request->Record.AddDevices();
            device0->SetDeviceUUID("uuid0");
            device0->SetBlocksCount(expectedBlockCount);
            device0->SetBlockSize(DefaultBlockSize);
            auto* device1 = request->Record.AddDevices();
            device1->SetDeviceUUID("uuid1_better");
            device1->SetBlocksCount(expectedBlockCount);
            device1->SetBlockSize(DefaultBlockSize);
            auto* device2 = request->Record.AddDevices();
            device2->SetDeviceUUID("uuid2");
            device2->SetBlocksCount(expectedBlockCount);
            device2->SetBlockSize(DefaultBlockSize);
            auto* device3 = request->Record.AddDevices();
            device3->SetDeviceUUID("uuid3");
            device3->SetBlocksCount(expectedBlockCount);
            device3->SetBlockSize(DefaultBlockSize);

            volume.SendToPipe(std::move(request));
        }

        {
            auto stat = volume.StatVolume();
            const auto& devices = stat->Record.GetVolume().GetDevices();
            UNIT_ASSERT_VALUES_EQUAL(expectedDeviceCount + 1, devices.size());
            UNIT_ASSERT_VALUES_EQUAL("uuid0", devices[0].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[0].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid1_better", devices[1].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[1].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid2", devices[2].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[2].GetBlockCount()
            );
            UNIT_ASSERT_VALUES_EQUAL("uuid3", devices[3].GetDeviceUUID());
            UNIT_ASSERT_VALUES_EQUAL(
                expectedBlockCount,
                devices[3].GetBlockCount()
            );
        }
    }

    Y_UNIT_TEST(ShouldSupportReadOnlyMode)
    {
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(
            NProto::TStorageServiceConfig(), state);

        const auto expectedBlockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;

        TVolumeClient volume(*runtime);

        auto updateConfig = [&, version = 1] () mutable {
            volume.UpdateVolumeConfig(
                0,
                0,
                0,
                0,
                false,
                version++,
                NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
                expectedBlockCount);
        };

        updateConfig();

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& volume = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, volume.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                volume.GetDevices(0).GetTransportId());
        }

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());

        auto& disk = state->Disks.at("vol0");
        disk.IOMode = NProto::VOLUME_IO_ERROR_READ_ONLY;
        volume.ReallocateDisk();

        // reallocate disk will trigger pipes reset, so reestablish connection
        volume.ReconnectPipe();

        updateConfig();
        volume.AddClient(clientInfo);

        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_IO, response->GetStatus());

            volume.ReadBlocks(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId());
        }

        disk.IOMode = NProto::VOLUME_IO_OK;
        volume.ReallocateDisk();
        // reallocate disk will trigger pipes reset, so reestablish connection
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldAggregateAndCachePartitionStats)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            1024,       // block count per partition
            "vol0",
            "cloud",
            "folder",
            2,          // partition count
            2
        );

        volume.WaitReady();

        ui64 bytesCount = 0;
        ui64 usedBytesCount = 0;
        ui32 partStatsSaved = 0;
        ui64 channelHistorySize = 0;
        ui32 partitionCount = 0;
        ui64 loadTime = 0;
        ui64 startTime = 0;

        auto obs = [&] (TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvVolumePrivate::EvPartStatsSaved) {
                ++partStatsSaved;
            } else if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumePartCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumePartCounters>();

                bytesCount = msg->DiskCounters->Simple.BytesCount.Value;
                usedBytesCount = msg->DiskCounters->Simple.UsedBytesCount.Value;
                channelHistorySize = msg->DiskCounters->Simple.ChannelHistorySize.Value;
            } else if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();

                partitionCount =
                    msg->VolumeSelfCounters->Simple.PartitionCount.Value;
                loadTime = msg->VolumeSelfCounters->Simple.LastVolumeLoadTime.Value;
                startTime = msg->VolumeSelfCounters->Simple.LastVolumeStartTime.Value;
            }


            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        runtime->SetObserverFunc(obs);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        volume.WriteBlocks(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1);
        volume.WriteBlocks(
            TBlockRange64::WithLength(1024, 512),
            clientInfo.GetClientId(),
            2);

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT(partStatsSaved >= 2);
        UNIT_ASSERT_VALUES_EQUAL(8_MB, bytesCount);
        UNIT_ASSERT_VALUES_EQUAL(6_MB, usedBytesCount);
        UNIT_ASSERT_VALUES_UNEQUAL(0, channelHistorySize);
        UNIT_ASSERT_VALUES_UNEQUAL(0, loadTime);
        UNIT_ASSERT_VALUES_UNEQUAL(0, startTime);
        UNIT_ASSERT_VALUES_EQUAL(2, partitionCount);

        volume.RebootTablet();
        bytesCount = 0;
        usedBytesCount = 0;
        channelHistorySize = 0;
        loadTime = 0;
        startTime = 0;
        partitionCount = 0;

        runtime->SetObserverFunc(obs);
        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(8_MB, bytesCount);
        UNIT_ASSERT_VALUES_EQUAL(6_MB, usedBytesCount);
        UNIT_ASSERT_VALUES_UNEQUAL(0, channelHistorySize);
        UNIT_ASSERT_VALUES_UNEQUAL(0, loadTime);
        UNIT_ASSERT_VALUES_EQUAL(0, startTime);
        UNIT_ASSERT_VALUES_EQUAL(2, partitionCount);

        // partition stats should be sent not just once
        bytesCount = 0;
        usedBytesCount = 0;
        channelHistorySize = 0;
        loadTime = 0;
        startTime = 0;
        partitionCount = 0;

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(8_MB, bytesCount);
        UNIT_ASSERT_VALUES_EQUAL(6_MB, usedBytesCount);
        UNIT_ASSERT_VALUES_UNEQUAL(0, channelHistorySize);
        UNIT_ASSERT_VALUES_UNEQUAL(0, loadTime);
        UNIT_ASSERT_VALUES_EQUAL(0, startTime);
        UNIT_ASSERT_VALUES_EQUAL(2, partitionCount);
    }

    Y_UNIT_TEST(ShouldSupportCompactRangesForMultipartitionVolume)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(v.GetBlocksCount(), 21 * 1024);
            UNIT_ASSERT_VALUES_EQUAL(v.GetPartitionsCount(), 3);
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        for (ui32 i = 0; i < 21; ++i) {
            volume.WriteBlocks(
                TBlockRange64::WithLength(1024 * i, 1024),
                clientInfo.GetClientId(),
                1 + i
            );
        }

        auto compactResponse = volume.CompactRange(
            TBlockRange64::MakeClosedInterval(0, 21 * 1024),
            "op1");
        UNIT_ASSERT_VALUES_UNEQUAL(true, compactResponse->Record.GetOperationId().empty());

        auto response = volume.GetCompactionStatus("op1");
        UNIT_ASSERT_VALUES_UNEQUAL(0, response->Record.GetTotal());
    }

    Y_UNIT_TEST(ShouldSendPartitionStatsForNonreplicatedVolume)
    {
        auto state = MakeIntrusive<TDiskRegistryState>();
        state->MigrationMode = EMigrationMode::InProgress;
        NProto::TStorageServiceConfig config;
        // XXX
        // disabling migration index caching - migration preempts metrics-related
        // code in test actor runtime
        config.SetMigrationIndexCachingInterval(999999);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto runtime = PrepareTestActorRuntime(config, state);

        const auto expectedBlockCount =
            DefaultDeviceBlockSize * DefaultDeviceBlockCount / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            expectedDeviceCount * expectedBlockCount,
            "vol0");

        volume.WaitReady();

        ui64 bytesCount = 0;
        ui32 partStatsSaved = 0;

        auto obs = [&] (TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvVolumePrivate::EvPartStatsSaved) {
                ++partStatsSaved;
            } else if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumePartCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumePartCounters>();

                bytesCount = msg->DiskCounters->Simple.BytesCount.Value;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        runtime->SetObserverFunc(obs);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        volume.WriteBlocks(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1);
        volume.WriteBlocks(
            TBlockRange64::WithLength(1024, 512),
            clientInfo.GetClientId(),
            2);

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT(partStatsSaved >= 2);
        UNIT_ASSERT_VALUES_EQUAL(
            expectedDeviceCount * expectedBlockCount * DefaultBlockSize,
            bytesCount);

        volume.RebootTablet();
        bytesCount = 0;

        runtime->SetObserverFunc(obs);
        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(
            expectedDeviceCount * expectedBlockCount * DefaultBlockSize,
            bytesCount);
    }

    Y_UNIT_TEST(ShouldSendCachedValuesWhenPartitionIsOffline)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            1024,       // block count per partition
            "vol0",
            "cloud",
            "folder",
            1,          // partition count
            2
        );

        CheckVolumeSendsStatsEvenIfPartitionsAreDead(
            std::move(runtime),
            volume,
            4_MB,
            true);
    }

    Y_UNIT_TEST(ShouldSendCachedValuesWhenNonReplPartitionIsOffline)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        // XXX
        // disabling migration index caching - migration preempts metrics-related
        // code in test actor runtime
        config.SetMigrationIndexCachingInterval(999999);

        auto state = MakeIntrusive<TDiskRegistryState>();
        state->MigrationMode = EMigrationMode::InProgress;
        auto runtime = PrepareTestActorRuntime(config, state);

        const auto expectedBlockCount =
            DefaultDeviceBlockSize * DefaultDeviceBlockCount / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            expectedDeviceCount * expectedBlockCount,
            "vol0");

        CheckVolumeSendsStatsEvenIfPartitionsAreDead(
            std::move(runtime),
            volume,
            expectedBlockCount * DefaultBlockSize * expectedDeviceCount,
            false);
    }

    Y_UNIT_TEST(ShouldMuteIOErrorsViaTag)
    {
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = TTestRuntimeBuilder()
            .With(state)
            .Build();

        TVolumeClient volume(*runtime);

        auto updateVolumeConfig = volume.TagUpdater(
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            DefaultDeviceBlockSize * DefaultDeviceBlockCount / DefaultBlockSize);

        updateVolumeConfig("");

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());

        auto& disk = state->Disks.at("vol0");
        disk.IOMode = NProto::VOLUME_IO_ERROR_READ_ONLY;
        volume.ReallocateDisk();

        // reallocate disk will trigger pipes reset, so reestablish connection
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);
        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_IO, response->GetStatus());
        }

        updateVolumeConfig("mute-io-errors");
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        volume.ReallocateDisk();

        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_IO_SILENT, response->GetStatus());
            UNIT_ASSERT(HasProtoFlag(
                response->GetError().GetFlags(),
                NProto::EF_SILENT));
        }
    }

    Y_UNIT_TEST(ShouldAutoMuteIOErrors)
    {
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = TTestRuntimeBuilder()
            .With(state)
            .Build();

        TVolumeClient volume(*runtime);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED);

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto shoot = [&]
        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volume.RecvWriteBlocksResponse();
            return response->GetError();
        };

        UNIT_ASSERT_VALUES_EQUAL(S_OK, shoot().GetCode());

        auto& disk = state->Disks.at("vol0");
        disk.IOMode = NProto::VOLUME_IO_ERROR_READ_ONLY;
        disk.IOModeTs = runtime->GetCurrentTime();
        disk.MuteIOErrors = true;

        volume.ReallocateDisk();
        // reallocate disk will trigger pipes reset, so reestablish connection
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        auto error = shoot();
        UNIT_ASSERT_VALUES_EQUAL(E_IO_SILENT, error.GetCode());
        UNIT_ASSERT(HasProtoFlag(error.GetFlags(), NProto::EF_SILENT));
    }

    Y_UNIT_TEST(ShouldCollectTracesForMultipartitionVolumesUponRequest)
    {
        NProto::TStorageServiceConfig config;
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();
        volume.StatVolume();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        auto response = volume.RecvWriteBlocksResponse();

        CheckForkJoin(response->Record.GetTrace().GetLWTrace().GetTrace(), true);
    }

    Y_UNIT_TEST(ShouldCorrectlyCollectTracesForMultipartitionVolumesIfPartitionReturnError)
    {
        NProto::TStorageServiceConfig config;
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();
        volume.StatVolume();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::MakeClosedInterval(1024 * 0, 1024 * 24),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        auto response = volume.RecvWriteBlocksResponse();

        UNIT_ASSERT(FAILED(response->GetStatus()));

        const auto& trace = response->Record.GetTrace().GetLWTrace().GetTrace().GetEvents();

        UNIT_ASSERT_C(
            std::find_if(
                trace.begin(),
                trace.end(),
                [] (const auto& e) {
                    return e.GetName() == "Join";
                }) != trace.end(),
            "No Join found");
    }

    Y_UNIT_TEST(ShouldCollectTracesForSinglePartitionVolumesUponRequest)
    {
        NProto::TStorageServiceConfig storageServiceConfig;

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        auto response = volume.RecvWriteBlocksResponse();

        CheckForkJoin(response->Record.GetTrace().GetLWTrace().GetTrace(), true);
    }

    Y_UNIT_TEST(ShouldCollectTracesForSinglePartitionVolumesIfRequestWasRejectedByVolume)
    {
        auto runtime = PrepareTestActorRuntime({});

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();

        volume.RebootTablet();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        auto response = volume.RecvWriteBlocksResponse();

        UNIT_ASSERT_VALUES_UNEQUAL(
            0,
            response->Record.GetTrace().GetLWTrace().GetTrace().GetEvents().size());
    }

    Y_UNIT_TEST(ShouldCollectTracesForSinglePartitionVolumesIfRequestWasPostponedByVolume)
    {
        TThrottledVolumeTestEnv env(5, NCloud::NProto::STORAGE_MEDIA_SSD);
        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto oneBlock = TBlockRange64::MakeOneBlock(0);

        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 1);
        TEST_QUICK_RESPONSE_VOLUME_EVENT(
            runtime,
            DescribeBlocks,
            S_OK
        );

        for (ui32 i = 0; i < 19; ++i) {
            auto request = volume.CreateReadBlocksRequest(
                oneBlock,
                clientInfo.GetClientId()
            );
            request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

            volume.SendToPipe(std::move(request));

            TEST_NO_RESPONSE(runtime, ReadBlocks);
        }

        for (ui32 i = 0; i < 19; ++i) {
            auto response =
                volume.TryRecvResponse<TEvService::TEvReadBlocksResponse>(WaitTimeout);
            UNIT_ASSERT(response);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());

            UNIT_ASSERT_VALUES_UNEQUAL(
                0,
                response->Record.GetTrace().GetLWTrace().GetTrace().GetEvents().size());
        }
    }

    void DoTestThatTracedRequestsAreRejectedWhenVolumesIsKilled(bool sysTabletRestart)
    {
        auto runtime = PrepareTestActorRuntime({});

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        ui32 cnt = 0;

        auto obs = [&] (TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvService::EvWriteBlocksResponse &&
                !cnt++)
            {
                if (sysTabletRestart) {
                    volume.RebootSysTablet();
                } else {
                    volume.RebootTablet();
                }
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        runtime->SetObserverFunc(obs);

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        auto response = volume.RecvWriteBlocksResponse();

        UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
    }

    Y_UNIT_TEST(ShouldRejectTracedRequestsIfVolumeUserActorIsKilled)
    {
        DoTestThatTracedRequestsAreRejectedWhenVolumesIsKilled(false);
    }

    Y_UNIT_TEST(ShouldRejectTracedRequestsIfVolumeSysActorIsKilled)
    {
        DoTestThatTracedRequestsAreRejectedWhenVolumesIsKilled(true);
    }

    Y_UNIT_TEST(ShouldFillThrottlerDelayFieldForDelayedRequests)
    {
        TThrottledVolumeTestEnv env(5, NProto::STORAGE_MEDIA_SSD);

        auto& runtime = env.Runtime;
        auto& volume = *env.Volume;

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        const auto oneBlock = TBlockRange64::MakeOneBlock(0);

        volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 1);
        TEST_QUICK_RESPONSE_VOLUME_EVENT(
            runtime,
            DescribeBlocks,
            S_OK
        );

        {
            volume.SendReadBlocksRequest(oneBlock, clientInfo.GetClientId());
            TEST_NO_RESPONSE(runtime, ReadBlocks);
        }

        {
            volume.SendWriteBlocksRequest(oneBlock, clientInfo.GetClientId());
            TEST_NO_RESPONSE(runtime, ReadBlocks);
        }

        {
            volume.SendDescribeBlocksRequest(oneBlock, clientInfo.GetClientId(), 1);
            TEST_NO_RESPONSE(runtime, ReadBlocks);
        }

        {
            TICK(runtime);
            auto response =
                volume.TryRecvResponse<TEvService::TEvReadBlocksResponse>(WaitTimeout);
            UNIT_ASSERT(response);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());

            UNIT_ASSERT_VALUES_UNEQUAL(0, response->Record.GetThrottlerDelay());
        }

        {
            TICK(runtime);
            auto response =
                volume.TryRecvResponse<TEvService::TEvWriteBlocksResponse>(WaitTimeout);
            UNIT_ASSERT(response);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());

            UNIT_ASSERT_VALUES_UNEQUAL(0, response->Record.GetThrottlerDelay());
        }

        {
            TICK(runtime);
            auto response =
                volume.TryRecvResponse<TEvVolume::TEvDescribeBlocksResponse>(WaitTimeout);
            UNIT_ASSERT(response);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());

            UNIT_ASSERT_VALUES_UNEQUAL(0, response->Record.GetThrottlerDelay());
        }
    }

    Y_UNIT_TEST(ShouldRejectIntersectingWriteAndZeroRequests)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        TAutoPtr<IEventHandle> evPut;
        bool evPutSeen = false;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event)
            {
                if (event->GetTypeRewrite() == TEvBlobStorage::EvPutResult && !evPutSeen) {
                    evPut = event.Release();
                    evPutSeen = true;
                    return TTestActorRuntime::EEventAction::DROP;
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        const auto range = TBlockRange64::MakeClosedInterval(4, 8);
        volume.SendWriteBlocksRequest(
            range,
            clientInfo.GetClientId(),
            GetBlockContent('a'));

        runtime->DispatchEvents(TDispatchOptions(), TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(true, evPutSeen);

        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(4),
                clientInfo.GetClientId(),
                GetBlockContent('b'));

            TEST_NO_RESPONSE(runtime, WriteBlocks);
        }

        {
            volume.SendZeroBlocksRequest(
                TBlockRange64::MakeClosedInterval(5, 7),
                clientInfo.GetClientId());

            TEST_NO_RESPONSE(runtime, ZeroBlocks);
        }

        {
            volume.SendWriteBlocksLocalRequest(
                range,
                clientInfo.GetClientId(),
                GetBlockContent('c'));

            TEST_NO_RESPONSE(runtime, WriteBlocksLocal);
        }

        {
            volume.SendWriteBlocksRequest(
                TBlockRange64::MakeClosedInterval(6, 10),
                clientInfo.GetClientId(),
                GetBlockContent('d'));

            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetError().GetCode());
        }

        {
            volume.SendWriteBlocksLocalRequest(
                TBlockRange64::MakeClosedInterval(6, 10),
                clientInfo.GetClientId(),
                GetBlockContent('e'));

            auto response = volume.RecvWriteBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetError().GetCode());
        }

        UNIT_ASSERT(evPut);
        runtime->Send(evPut.Release());
        runtime->DispatchEvents(TDispatchOptions(), TDuration::Seconds(1));

        {
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        {
            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        {
            auto response = volume.RecvZeroBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        {
            auto response = volume.RecvWriteBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        {
            auto response = volume.ReadBlocks(range, clientInfo.GetClientId());
            const auto& bufs = response->Record.GetBlocks().GetBuffers();
            for (ui32 i = 0; i < range.Size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL(GetBlockContent('a'), bufs[i]);
            }
        }
    }

    Y_UNIT_TEST(ShouldRejectPendingStatVolumeRequestsIfTabletReboots)
    {
        auto runtime = PrepareTestActorRuntime();

        auto observer = [] (TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvPartition::EvWaitReadyResponse) {
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        runtime->SetObserverFunc(observer);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();

        volume.SendStatVolumeRequest();

        TAutoPtr<IEventHandle> handle;
        runtime->GrabEdgeEventRethrow<TEvService::TEvStatVolumeResponse>(
            handle,
            TDuration::Seconds(1)
        );

        // no response since partition is not up
        UNIT_ASSERT(!handle);

        volume.RebootTablet();

        auto response = volume.RecvStatVolumeResponse();
        UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
    }

    Y_UNIT_TEST(ShouldRejectWritesIfReadOnlyTagIsSet)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        auto updateVolumeConfig = volume.TagUpdater(
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024);

        updateVolumeConfig("read-only");

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        volume.SendWriteBlocksRequest(
            TBlockRange64::MakeOneBlock(0), clientInfo.GetClientId(), 0);

        auto response = volume.RecvWriteBlocksResponse();
        UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());
    }

    void DoTestShouldTrackUsedBlocksIfTrackUsedTagIsSet(
        NProto::EStorageMediaKind mediaKind)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        auto updateVolumeConfig = volume.TagUpdater(mediaKind, 1024);

        updateVolumeConfig("");

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        auto writeBlocks = [&] (TBlockRange64 range, char fill) {
            volume.WriteBlocks(range, clientInfo.GetClientId(), fill);
        };

        auto writeBlocksLocal = [&] (TBlockRange64 range, char fill) {
            auto blockContent = GetBlockContent(fill);

            TSgList sglist;
            sglist.resize(
                range.Size(),
                {blockContent.Data(), blockContent.Size()}
            );
            TGuardedSgList glist(std::move(sglist));

            auto request =
                std::make_unique<TEvService::TEvWriteBlocksLocalRequest>();
            request->Record.SetStartIndex(range.Start);
            request->Record.MutableHeaders()->SetClientId(
                clientInfo.GetClientId());
            request->Record.Sglist = glist;
            request->Record.BlocksCount = range.Size();
            request->Record.BlockSize = DefaultBlockSize;

            volume.SendToPipe(std::move(request));

            auto response = volume.RecvWriteBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                S_OK,
                response->GetStatus(),
                response->GetErrorReason()
            );
        };

        auto writeBlocksBigBuffer = [&] (TBlockRange64 range, char fill) {
            auto request = std::make_unique<TEvService::TEvWriteBlocksRequest>();
            request->Record.SetStartIndex(range.Start);
            request->Record.MutableHeaders()->SetClientId(
                clientInfo.GetClientId());

            auto& buffers = *request->Record.MutableBlocks()->MutableBuffers();
            *buffers.Add() = TString(DefaultBlockSize * range.Size(), fill);

            volume.SendToPipe(std::move(request));

            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        };

        writeBlocks(TBlockRange64::WithLength(10, 20), 1);
        writeBlocks(TBlockRange64::WithLength(20, 30), 2);
        writeBlocksLocal(TBlockRange64::MakeClosedInterval(60, 69), 3);

        {
            const auto stats = volume.StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(0, stats.GetVolumeUsedBlocksCount());
        }

        TVector<TBlockRange64> ranges;
        ranges.push_back(TBlockRange64::WithLength(0, 1));

        volume.SendUpdateUsedBlocksRequest(ranges, true);

        {
            auto response = volume.RecvUpdateUsedBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_FALSE, response->GetStatus());
        }

        updateVolumeConfig("track-used");
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        writeBlocks(TBlockRange64::MakeClosedInterval(20, 39), 4);
        writeBlocks(TBlockRange64::MakeClosedInterval(30, 59), 5);
        writeBlocksLocal(TBlockRange64::MakeClosedInterval(65, 69), 6);
        writeBlocksBigBuffer(TBlockRange64::MakeClosedInterval(300, 309), 7);

        ranges.clear();
        ranges.push_back(TBlockRange64::WithLength(100, 30));
        ranges.push_back(TBlockRange64::WithLength(200, 20));
        volume.UpdateUsedBlocks(ranges, true);

        ranges.clear();
        ranges.push_back(TBlockRange64::WithLength(110, 15));
        ranges.push_back(TBlockRange64::WithLength(210, 10));
        volume.UpdateUsedBlocks(ranges, false);

        {
            const auto stats = volume.StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(80, stats.GetVolumeUsedBlocksCount());
        }

        volume.RebootTablet();
        volume.AddClient(clientInfo);

        {
            const auto stats = volume.StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(80, stats.GetVolumeUsedBlocksCount());
        }

        auto readBlocks = [&] (ui64 blockIndex, char fill) {
            auto resp = volume.ReadBlocks(
                TBlockRange64::MakeOneBlock(blockIndex),
                clientInfo.GetClientId());
            const auto& bufs = resp->Record.GetBlocks().GetBuffers();
            UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(fill), bufs[0]);
        };

        auto readBlocksLocal = [&] (ui64 blockIndex, char fill) {
            TVector<TString> blocks;
            auto sglist = ResizeBlocks(
                blocks,
                1,
                TString::TUninitialized(DefaultBlockSize)
            );
            auto resp = volume.ReadBlocksLocal(
                TBlockRange64::MakeOneBlock(blockIndex),
                TGuardedSgList(std::move(sglist)),
                clientInfo.GetClientId()
            );
            UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(fill), blocks[0]);
        };

        readBlocks(10, 1);
        readBlocks(20, 4);
        readBlocks(60, 3);
        readBlocks(65, 6);
        readBlocks(300, 7);
        readBlocks(309, 7);

        updateVolumeConfig("mask-unused");
        volume.ReconnectPipe();
        volume.AddClient(clientInfo);

        readBlocks(10, 0);
        readBlocksLocal(10, 0);
        readBlocks(20, 4);
        readBlocksLocal(20, 4);
        readBlocks(60, 0);
        readBlocksLocal(60, 0);
        readBlocks(65, 6);
        readBlocksLocal(65, 6);
        readBlocks(300, 7);
        readBlocksLocal(300, 7);
        readBlocks(309, 7);
        readBlocksLocal(309, 7);
    }

    Y_UNIT_TEST(ShouldTrackUsedBlocksIfTrackUsedTagIsSetSSD)
    {
        DoTestShouldTrackUsedBlocksIfTrackUsedTagIsSet(
            NCloud::NProto::STORAGE_MEDIA_SSD);
    }

    Y_UNIT_TEST(ShouldTrackUsedBlocksIfTrackUsedTagIsSetHybrid)
    {
        DoTestShouldTrackUsedBlocksIfTrackUsedTagIsSet(
            NCloud::NProto::STORAGE_MEDIA_HYBRID);
    }

    Y_UNIT_TEST(ShouldTrackUsedBlocksIfTrackUsedTagIsSetHDD)
    {
        DoTestShouldTrackUsedBlocksIfTrackUsedTagIsSet(
            NCloud::NProto::STORAGE_MEDIA_HDD);
    }

    Y_UNIT_TEST(ShouldTrackUsedBlocksIfTrackUsedTagIsSetSSDNonreplicated)
    {
        DoTestShouldTrackUsedBlocksIfTrackUsedTagIsSet(
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED);
    }

    void DoTestShouldFailRequestIfUpdateUsedBlocksRequestFailed(
        NProto::EStorageMediaKind mediaKind)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        auto updateVolumeConfig = volume.TagUpdater(mediaKind, 1024);

        updateVolumeConfig("track-used");

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvVolume::EvUpdateUsedBlocksRequest: {
                        auto response = std::make_unique<TEvVolume::TEvUpdateUsedBlocksResponse>(
                            MakeError(E_REJECTED, "some error")
                        );

                        runtime->Send(new IEventHandle(
                            event->Sender,
                            event->Recipient,
                            response.release(),
                            0, // flags
                            event->Cookie
                        ), 0);

                        return TTestActorRuntime::EEventAction::DROP;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0
        );
        volume.AddClient(clientInfo);

        auto writeBlocks = [&] (TBlockRange64 range, char fill) {
            volume.SendWriteBlocksRequest(range, clientInfo.GetClientId(), fill);

            auto response = volume.RecvWriteBlocksResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                E_REJECTED,
                response->GetStatus(),
                response->GetErrorReason()
            );
        };

        auto writeBlocksLocal = [&] (TBlockRange64 range, char fill) {
            auto blockContent = GetBlockContent(fill);

            TSgList sglist;
            sglist.resize(
                range.Size(),
                {blockContent.Data(), blockContent.Size()}
            );
            TGuardedSgList glist(std::move(sglist));

            auto request =
                std::make_unique<TEvService::TEvWriteBlocksLocalRequest>();
            request->Record.SetStartIndex(range.Start);
            request->Record.MutableHeaders()->SetClientId(
                clientInfo.GetClientId());
            request->Record.Sglist = glist;
            request->Record.BlocksCount = range.Size();
            request->Record.BlockSize = DefaultBlockSize;

            volume.SendToPipe(std::move(request));

            auto response = volume.RecvWriteBlocksLocalResponse();
            UNIT_ASSERT_VALUES_EQUAL_C(
                E_REJECTED,
                response->GetStatus(),
                response->GetErrorReason()
            );
        };

        writeBlocks(TBlockRange64::MakeOneBlock(1), 1);
        writeBlocksLocal(TBlockRange64::MakeOneBlock(1), 2);
    }

    Y_UNIT_TEST(ShouldFailRequestIfUpdateUsedBlocksRequestFailedSSD)
    {
        DoTestShouldFailRequestIfUpdateUsedBlocksRequestFailed(
            NCloud::NProto::STORAGE_MEDIA_SSD);
    }

    Y_UNIT_TEST(ShouldFailRequestIfUpdateUsedBlocksRequestFailedHybrid)
    {
        DoTestShouldFailRequestIfUpdateUsedBlocksRequestFailed(
            NCloud::NProto::STORAGE_MEDIA_HYBRID);
    }

    Y_UNIT_TEST(ShouldFailRequestIfUpdateUsedBlocksRequestFailedHDD)
    {
        DoTestShouldFailRequestIfUpdateUsedBlocksRequestFailed(
            NCloud::NProto::STORAGE_MEDIA_HDD);
    }

    Y_UNIT_TEST(ShouldFailRequestIfUpdateUsedBlocksRequestFailedSSDNonreplicated)
    {
        DoTestShouldFailRequestIfUpdateUsedBlocksRequestFailed(
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED);
    }

    Y_UNIT_TEST(ShouldSupportMultipleClientsWithSameClientId)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        TVolumeClient volumeClient1(*runtime);
        TVolumeClient volumeClient2(*runtime);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volumeClient1.AddClient(clientInfo);
        volumeClient2.AddClient(clientInfo);

        volumeClient1.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);
        volumeClient2.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);

        {
            volumeClient1.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volumeClient1.RecvWriteBlocksResponse();

            UNIT_ASSERT_VALUES_EQUAL(response->GetStatus(), E_BS_INVALID_SESSION);
        }

        {
            volumeClient1.SendAddClientRequest(clientInfo);
            auto response = volumeClient1.RecvAddClientResponse();

            UNIT_ASSERT_VALUES_EQUAL(response->GetStatus(), E_BS_INVALID_SESSION);
        }

        volumeClient1.RemoveClient(clientInfo.GetClientId());

        volumeClient2.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);

        volumeClient1.AddClient(clientInfo);
        volumeClient1.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);

        {
            volumeClient2.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(),
                1);
            auto response = volumeClient1.RecvWriteBlocksResponse();

            UNIT_ASSERT_VALUES_EQUAL(response->GetStatus(), E_BS_INVALID_SESSION);
        }
    }

    Y_UNIT_TEST(CheckBlobLoadedMetrics)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        NBlobMetrics::TBlobLoadMetrics volumeMetrics;
        auto obs = [&volumeMetrics] (TAutoPtr<IEventHandle>& ev)
        {
            if (ev->GetTypeRewrite() == TEvStatsService::EvVolumePartCounters) {
                TEvStatsService::TVolumePartCounters* msg =
                    ev->Get<TEvStatsService::TEvVolumePartCounters>();
                volumeMetrics += msg->BlobLoadMetrics;
            }
            return TTestActorRuntime::DefaultObserverFunc(ev);
        };

        runtime->SetObserverFunc(obs);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        volume.WriteBlocks(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1);
        volume.ReadBlocks(
            TBlockRange64::WithLength(0, 512),
            clientInfo.GetClientId());

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(volumeMetrics.PoolKind2TabletOps.size(), 1);

        NBlobMetrics::TBlobLoadMetrics::TTabletMetric commonMetrics;
        for (const auto& metric: volumeMetrics.PoolKind2TabletOps.begin()->second) {
            commonMetrics += metric.second;
        }

        UNIT_ASSERT_VALUES_EQUAL(commonMetrics.ReadOperations.ByteCount, 512 * 4096);
        UNIT_ASSERT_VALUES_EQUAL(commonMetrics.ReadOperations.Iops, 1);
        UNIT_ASSERT_GT(commonMetrics.WriteOperations.ByteCount, 1024 * 4096);
        UNIT_ASSERT_GT(commonMetrics.WriteOperations.Iops, 1);
    }

    Y_UNIT_TEST(ShouldSupportMetadataRebuildForSinglePartitionVolumes)
    {
        CheckRebuildMetadata(1, 0);
    }

    Y_UNIT_TEST(ShouldSupportMetadataRebuildForMultiPartitionVolumes)
    {
        CheckRebuildMetadata(3, 2);
    }

    Y_UNIT_TEST(ShouldHandlePartitionRestartsForMetadataRebuild)
    {
        NProto::TStorageServiceConfig config;
        config.SetMinChannelCount(4);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NProto::STORAGE_MEDIA_HYBRID,
            7 * 1024,   // block count per partition
            "vol0",
            "cloud",
            "folder",
            3,          // partition count
            2
        );

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(v.GetBlocksCount(), 3 * 7 * 1024);
            UNIT_ASSERT_VALUES_EQUAL(v.GetPartitionsCount(), 3);
        }

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);
        volume.AddClient(clientInfo);

        volume.WriteBlocksLocal(
            TBlockRange64::WithLength(0, 1024 * 3 * 7),
            clientInfo.GetClientId(),
            GetBlockContent(1)
        );

        ui32 cnt = 0;
        bool allowPoison = true;

        auto obs = [&] (TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvVolume::EvRebuildMetadataResponse &&
                ++cnt == 1 &&
                allowPoison)
            {
                runtime->Send(
                    new IEventHandle(ev->Sender, ev->Sender, new TEvents::TEvPoisonPill()));
            }

            return TTestActorRuntime::DefaultObserverFunc(ev);
        };

        runtime->SetObserverFunc(obs);

        {
            auto response = volume.RebuildMetadata(NProto::ERebuildMetadataType::BLOCK_COUNT, 10);

            volume.SendGetRebuildMetadataStatusRequest();
            auto progress = volume.RecvGetRebuildMetadataStatusResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_NOT_FOUND, progress->Record.GetError().GetCode());
        }

        {
            allowPoison = false;

            // run metadata rebuild again
            auto response = volume.RebuildMetadata(NProto::ERebuildMetadataType::BLOCK_COUNT, 10);

            auto progress = volume.GetRebuildMetadataStatus();
            UNIT_ASSERT_VALUES_EQUAL(21, progress->Record.GetProgress().GetProcessed());
            UNIT_ASSERT_VALUES_EQUAL(21, progress->Record.GetProgress().GetTotal());
            UNIT_ASSERT_VALUES_EQUAL(true, progress->Record.GetProgress().GetIsCompleted());
        }
    }

    Y_UNIT_TEST(ShouldSetClientIdWhenAddingNewClient)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        TVolumeClient volumeClient(*runtime);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_ONLY,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volumeClient.AddClient(clientInfo);

        {
            volumeClient.SendWriteBlocksRequest(
                TBlockRange64::MakeOneBlock(0),
                clientInfo.GetClientId(), 1);

            auto response = volumeClient.RecvWriteBlocksResponse();
            const auto& error = response->GetError();
            UNIT_ASSERT_C(FAILED(error.GetCode()), "No Error returned");
            UNIT_ASSERT_C(
                error.GetMessage().find(clientInfo.GetClientId()) != std::string::npos,
                "No client id in error");
        }
    }

    Y_UNIT_TEST(ShouldCleanupWriteAndZeroRequestsInFlightUponUndelivery)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);

        bool undeliver = true;
        TActorId volumeActorId;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event)
            {
                if (event->GetTypeRewrite() == TEvVolume::EvWaitReadyResponse) {
                    volumeActorId = event->Sender;
                } else if (event->Sender == volumeActorId &&
                    event->GetTypeRewrite() == TEvService::EvWriteBlocksRequest &&
                    undeliver)
                {
                    auto sendTo = event->Sender;
                    runtime->Send(
                        new IEventHandle(
                            sendTo,
                            sendTo,
                            event->ReleaseBase().Release(),
                            0,
                            event->Cookie,
                            nullptr),
                        0);
                    undeliver = false;
                    return TTestActorRuntime::EEventAction::DROP;
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        volume.SendWriteBlocksRequest(
            TBlockRange64::MakeClosedInterval(4, 8),
            clientInfo.GetClientId());

        auto response = volume.RecvWriteBlocksResponse();
        UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());

        volume.WriteBlocks(
            TBlockRange64::MakeClosedInterval(1, 4),
            clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldWaitRequestsInFlightUponReallocate)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024
        );

        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        std::unique_ptr<IEventHandle> writeDeviceBlocksRes;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event)
            {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskAgent::EvWriteDeviceBlocksResponse: {
                        writeDeviceBlocksRes.reset(event.Release());
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.SendWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            2
        );

        {
            TDispatchOptions options;
            options.CustomFinalCondition = [&] {
                return writeDeviceBlocksRes != nullptr;
            };
            runtime->DispatchEvents(options, TDuration::Seconds(10));
        }

        UNIT_ASSERT(writeDeviceBlocksRes);

        volume.SendReallocateDiskRequest();
        volume.SendReallocateDiskRequest();

        TEST_NO_RESPONSE_VOLUME_EVENT(runtime, ReallocateDisk);

        runtime->Send(writeDeviceBlocksRes.release());

        {
            TDispatchOptions options;
            options.FinalEvents = {
                TDispatchOptions::TFinalEventCondition(
                    TEvVolume::EvReallocateDiskResponse, 2)
            };
            runtime->DispatchEvents(options, TDuration::Seconds(10));
        }
    }

    Y_UNIT_TEST(ShouldAllocateLocalSSD)
    {
        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_LOCAL);

        volume.WaitReady();

        UNIT_ASSERT(state->Disks.contains("vol0"));

        const auto& disk = state->Disks["vol0"];
        UNIT_ASSERT_VALUES_EQUAL("", disk.PoolName);
        UNIT_ASSERT_EQUAL(
            NCloud::NProto::STORAGE_MEDIA_SSD_LOCAL,
            disk.MediaKind);
    }

    Y_UNIT_TEST(ShouldIncrementExternaBootTimeoutWhenBootFails)
    {
        auto minTimeout = TDuration::Seconds(1);
        auto timeoutIncrement = TDuration::MilliSeconds(500);
        NProto::TStorageServiceConfig config;
        config.SetMinExternalBootRequestTimeout(minTimeout.MilliSeconds());
        config.SetExternalBootRequestTimeoutIncrement(timeoutIncrement.MilliSeconds());
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);

        TActorId volumeActorId;

        bool firstAttempt = true;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event)
            {
                if (event->GetTypeRewrite() == TEvHiveProxy::EvBootExternalRequest) {
                    const auto* msg =
                        event->Get<TEvHiveProxy::TEvBootExternalRequest>();
                    if (firstAttempt) {
                        UNIT_ASSERT_VALUES_EQUAL(minTimeout, msg->RequestTimeout);

                        auto sendTo = event->Sender;
                        auto response =
                            std::make_unique<TEvHiveProxy::TEvBootExternalResponse>(
                                MakeKikimrError(
                                    NKikimrProto::EReplyStatus::TRYLATER,
                                    "Timeout"));
                        runtime->Send(
                            new IEventHandle(
                                sendTo,
                                sendTo,
                                response.release(),
                                0,
                                event->Cookie,
                                nullptr),
                            0);
                        firstAttempt = false;
                        return TTestActorRuntime::EEventAction::DROP;
                    } else {
                        UNIT_ASSERT_VALUES_EQUAL(
                            minTimeout + timeoutIncrement,
                            msg->RequestTimeout);
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.UpdateVolumeConfig();
        volume.WaitReady();
    }

    Y_UNIT_TEST(ShouldStartPartitionIfBootSuggestIsOutdated)
    {
        NProto::TStorageServiceConfig config;

        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);
        TActorId volumeActorId;
        TActorId partActorId;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event)
            {
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvWaitReadyResponse: {
                        partActorId = event->Sender;
                        break;
                    }
                    case TEvHiveProxy::EvBootExternalResponse: {
                        auto* msg = event->Get<TEvHiveProxy::TEvBootExternalResponse>();
                        auto* suggestedGeneration =
                            const_cast<ui32*>(&msg->SuggestedGeneration);
                        *suggestedGeneration = 1;
                        break;
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.UpdateVolumeConfig();
        volume.WaitReady();
        UNIT_ASSERT(partActorId);

        auto sender = runtime->AllocateEdgeActor();
        runtime->Send(
            new IEventHandle(partActorId, sender, new TEvents::TEvPoisonPill()));
        volume.WaitReady();
    }

    Y_UNIT_TEST(ShouldCorrectlyCalculateDiskRegistryPartitionParameters)
    {
        const auto expectedBlockCount = DefaultDeviceBlockSize * DefaultDeviceBlockCount
            / DefaultBlockSize;
        const auto expectedDeviceCount = 3;

        std::unique_ptr<TTestActorRuntime> runtime;
        std::unique_ptr<TVolumeClient> client;

        NProto::TStorageServiceConfig storageServiceConfig;
        NProto::TFeaturesConfig featuresConfig;

        struct TVolumeParamValue
        {
            TString value;
            uint64_t ttlMs;
        };

        const auto initVolume = [&] (NProto::EStorageMediaKind mediaKind,
                                     TString tags = "",
                                     TMaybe<TVolumeParamValue> timeoutOverride = {})
        {
            runtime = PrepareTestActorRuntime(
                storageServiceConfig,
                {},
                featuresConfig);

            client = std::make_unique<TVolumeClient>(*runtime);

            client->UpdateVolumeConfig(
                0,
                0,
                0,
                0,
                false,
                1,
                mediaKind,
                expectedDeviceCount * expectedBlockCount,
                "vol0",
                "cloud",
                "folder",
                1,
                0,
                std::move(tags));

            if (timeoutOverride) {
                NProto::TUpdateVolumeParamsMapValue protoParam;
                protoParam.SetValue(timeoutOverride->value);
                protoParam.SetTtlMs(timeoutOverride->ttlMs);

                THashMap<TString, NProto::TUpdateVolumeParamsMapValue> volumeParams {
                    {"max-timed-out-device-state-duration", protoParam}
                };

                client->UpdateVolumeParams("vol0", volumeParams);
            }

            client->WaitReady();
        };

        {

            initVolume(NProto::STORAGE_MEDIA_SSD_NONREPLICATED);

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                0,
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        const auto reliableMediaKinds = {
            NProto::STORAGE_MEDIA_SSD_MIRROR2,
            NProto::STORAGE_MEDIA_SSD_MIRROR3
        };

        for (auto mediaKind: reliableMediaKinds) {
            initVolume(mediaKind);

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                TDuration::Max().MilliSeconds(),
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        auto* feature = featuresConfig.AddFeatures();
        feature->SetName("MaxTimedOutDeviceStateDuration");
        feature->SetValue("1m");
        auto* wl = feature->MutableWhitelist();
        wl->AddFolderIds("folder");

        // default value
        {
            initVolume(NProto::STORAGE_MEDIA_SSD_NONREPLICATED);

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                60'000,
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        for (auto mediaKind: reliableMediaKinds) {
            initVolume(mediaKind);

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                TDuration::Max().MilliSeconds(),
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        // invalid tag value
        {
            initVolume(
                NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
                "max-timed-out-device-state-duration=broken-duration");

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                60'000,
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        // valid tag value
        {
            initVolume(
                NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
                "max-timed-out-device-state-duration=30s");

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                30'000,
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        for (auto mediaKind: reliableMediaKinds) {
            initVolume(mediaKind, "max-timed-out-device-state-duration=30s");

            const auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                30'000,
                stats.GetMaxTimedOutDeviceStateDuration());
        }

        // override
        {
            initVolume(
                NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
                "max-timed-out-device-state-duration=30s",
                TVolumeParamValue{"10s",3*UpdateCountersInterval.MilliSeconds()});

            auto stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                10'000,
                stats.GetMaxTimedOutDeviceStateDuration());

            // override persists after reboot

            // FIXME: scheduled events stop working after some time after reboot
            /*
            client->RebootTablet();
            client->WaitReady();
            */

            runtime->AdvanceCurrentTime(UpdateCountersInterval);
            runtime->DispatchEvents({}, TDuration::Seconds(1));
            client->WaitReady();

            stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                10'000,
                stats.GetMaxTimedOutDeviceStateDuration());

            // override expiration

            runtime->AdvanceCurrentTime(2*UpdateCountersInterval);
            runtime->DispatchEvents({}, TDuration::Seconds(1));
            client->WaitReady();

            stats = client->StatVolume()->Record.GetStats();
            UNIT_ASSERT_VALUES_EQUAL(
                30'000,
                stats.GetMaxTimedOutDeviceStateDuration());
        }
    }

    Y_UNIT_TEST(ShouldStoreResyncIndex)
    {
        const auto blocksPerDevice =
            DefaultDeviceBlockCount * DefaultDeviceBlockSize / DefaultBlockSize;

        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetMaxMigrationBandwidth(999'999'999);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        // creating a nonreplicated disk
        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            blocksPerDevice
        );

        volume.WaitReady();

        ui32 resyncProgressCounter = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
                {
                    auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();
                    resyncProgressCounter =
                        msg->VolumeSelfCounters->Simple.ResyncProgress.Value;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        volume.SendToPipe(
            std::make_unique<TEvVolume::TEvUpdateResyncState>(blocksPerDevice / 2));
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>()
        );
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(50, resyncProgressCounter);
    }

    void DoTestShouldForwardRequestsToMirroredPartitionDuringResync(
        std::shared_ptr<TRdmaClientTest> rdmaClient,
        TString tags)
    {
        NProto::TStorageServiceConfig config;
        config.SetAcquireNonReplicatedDevices(true);
        config.SetUseMirrorResync(true);
        config.SetForceMirrorResync(true);
        if (rdmaClient) {
            config.SetUseRdma(true);
            config.SetUseNonreplicatedRdmaActor(true);
        }
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(
            config,
            state,
            {}, // featuresConfig
            rdmaClient);

        ui64 writeRequests = 0;
        TAutoPtr<IEventHandle> evResyncFinished;

        auto obs = [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
            if (event->GetTypeRewrite() == TEvVolume::EvResyncFinished) {
                // making sure that resync mode doesn't get disabled
                evResyncFinished = event.Release();
                return true;
            }

            if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumePartCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumePartCounters>();

                writeRequests +=
                    msg->DiskCounters->RequestCounters.WriteBlocks.Count;
            }

            return false;
        };

        runtime->SetEventFilter(obs);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 2;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR3,
            1024,
            "vol0",
            "cloud",
            "folder",
            1,  // partitionCount
            0,  // blocksPerStripe
            std::move(tags));

        volume.WaitReady();

        auto stat = volume.StatVolume();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        const auto& replicas = stat->Record.GetVolume().GetReplicas();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());

        UNIT_ASSERT_VALUES_EQUAL(2, replicas.size());
        UNIT_ASSERT_VALUES_EQUAL(1, replicas[0].DevicesSize());
        UNIT_ASSERT_VALUES_EQUAL(
            "transport1",
            replicas[0].GetDevices(0).GetTransportId());
        UNIT_ASSERT_VALUES_EQUAL(1, replicas[1].DevicesSize());
        UNIT_ASSERT_VALUES_EQUAL(
            "transport2",
            replicas[1].GetDevices(0).GetTransportId());

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        {
            auto response = volume.AddClient(clientInfo);
            const auto& v = response->Record.GetVolume();
            UNIT_ASSERT_VALUES_EQUAL(1, v.DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport0",
                v.GetDevices(0).GetTransportId()
            );

            UNIT_ASSERT_VALUES_EQUAL(2, v.ReplicasSize());
            UNIT_ASSERT_VALUES_EQUAL(1, v.GetReplicas(0).DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport1",
                v.GetReplicas(0).GetDevices(0).GetTransportId());
            UNIT_ASSERT_VALUES_EQUAL(1, v.GetReplicas(1).DevicesSize());
            UNIT_ASSERT_VALUES_EQUAL(
                "transport2",
                v.GetReplicas(1).GetDevices(0).GetTransportId());
        }

        if (rdmaClient) {
            UNIT_ASSERT_VALUES_EQUAL(1, rdmaClient->InitAllEndpoints());
        }

        volume.WriteBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId(),
            1);

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(3, writeRequests);

        auto resp = volume.ReadBlocks(
            TBlockRange64::MakeOneBlock(0),
            clientInfo.GetClientId());
        const auto& bufs = resp->Record.GetBlocks().GetBuffers();
        UNIT_ASSERT_VALUES_EQUAL(1, bufs.size());
        UNIT_ASSERT_VALUES_EQUAL(GetBlockContent(1), bufs[0]);

        while (!evResyncFinished) {
            runtime->DispatchEvents({}, TDuration::Seconds(1));
        }

        runtime->Send(evResyncFinished.Release());

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldForwardRequestsToMirroredPartitionDuringResync)
    {
        DoTestShouldForwardRequestsToMirroredPartitionDuringResync(nullptr, "");
    }

    Y_UNIT_TEST(ShouldForwardRequestsToMirroredPartitionDuringResyncRdma)
    {
        auto rdmaClient = std::make_shared<TRdmaClientTest>();
        TString tags;
        // NBS-3786#63baeaf302dcf2032262746b
        // tags = "use-rdma";
        DoTestShouldForwardRequestsToMirroredPartitionDuringResync(
            rdmaClient,
            tags);
    }

    Y_UNIT_TEST(ShouldCreateResyncActorIfClientIdChanges)
    {
        NProto::TStorageServiceConfig config;
        config.SetUseMirrorResync(true);
        config.SetForceMirrorResync(false);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 2;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR3,
            1024
        );

        volume.WaitReady();

        ui32 resyncStartedCounter = 0;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                if (event->Recipient == MakeStorageStatsServiceId()
                        && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
                {
                    auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();

                    resyncStartedCounter =
                        msg->VolumeSelfCounters->Simple.ResyncStarted.Value;
                }

                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        NProto::TVolumeClientInfo clientInfo;
        clientInfo.SetClientId("client-vasya");
        clientInfo.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_WRITE);
        clientInfo.SetMountSeqNumber(0);
        clientInfo.SetVolumeMountMode(NProto::VOLUME_MOUNT_LOCAL);

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>());
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        UNIT_ASSERT_VALUES_EQUAL(0, resyncStartedCounter);

        volume.AddClient(clientInfo);
        volume.ReconnectPipe();
        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should start if client id changes from "" to str
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>());
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, resyncStartedCounter);

        clientInfo.SetClientId("client-petya");
        clientInfo.SetMountSeqNumber(1);

        volume.AddClient(clientInfo);
        volume.ReconnectPipe();
        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should start if client id changes from str to another str
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>());
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, resyncStartedCounter);

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldCreateResyncActorAfterVolumeReboot)
    {
        NProto::TStorageServiceConfig config;
        config.SetUseMirrorResync(true);
        config.SetForceMirrorResync(false);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 2;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR3,
            1024
        );

        volume.WaitReady();

        NProto::TVolumeClientInfo clientInfo;
        clientInfo.SetClientId("client-vasya");
        clientInfo.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_WRITE);
        clientInfo.SetMountSeqNumber(0);
        clientInfo.SetVolumeMountMode(NProto::VOLUME_MOUNT_LOCAL);

        auto obs = [] (TAutoPtr<IEventHandle>& event)
        {
            if (event->GetTypeRewrite() == TEvVolume::EvResyncFinished) {
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };
        runtime->SetObserverFunc(obs);

        volume.AddClient(clientInfo);
        volume.ReconnectPipe();
        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should start if client id changes from "" to str
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        volume.RebootTablet();
        volume.AddClient(clientInfo);
        volume.WaitReady();

        /*
        // TODO: learn how to set observer func after tablet reboot and before
        // any events are processed by tablet actor
        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should be active after reboot
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        volume.SendToPipe(std::make_unique<TEvVolume::TEvResyncFinished>());
        */

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should be inactive after EvResyncFinished
            UNIT_ASSERT(!v.GetResyncInProgress());
        }

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldCreateResyncActorIfClientIsAbsentForSomeTime)
    {
        NProto::TStorageServiceConfig config;
        config.SetUseMirrorResync(true);
        config.SetForceMirrorResync(false);
        config.SetResyncAfterClientInactivityInterval(60'000);
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        state->ReplicaCount = 2;

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_MIRROR3,
            1024
        );

        volume.WaitReady();

        NProto::TVolumeClientInfo clientInfo;
        clientInfo.SetClientId("client-vasya");
        clientInfo.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_WRITE);
        clientInfo.SetMountSeqNumber(0);
        clientInfo.SetVolumeMountMode(NProto::VOLUME_MOUNT_LOCAL);

        volume.AddClient(clientInfo);
        volume.ReconnectPipe();
        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should start if client id changes from "" to str
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvVolume::EvResyncFinished);
        runtime->DispatchEvents(options);

        runtime->AdvanceCurrentTime(UpdateCountersInterval);
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        runtime->AdvanceCurrentTime(TDuration::MilliSeconds(
            config.GetResyncAfterClientInactivityInterval()));

        auto obs = [] (TAutoPtr<IEventHandle>& event)
        {
            if (event->GetTypeRewrite() == TEvVolume::EvResyncFinished) {
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };
        runtime->SetObserverFunc(obs);

        volume.AddClient(clientInfo);
        volume.ReconnectPipe();
        volume.WaitReady();
        {
            auto stat = volume.StatVolume();
            const auto& v = stat->Record.GetVolume();
            // resync should start after client inactivity interval
            UNIT_ASSERT(v.GetResyncInProgress());
        }

        volume.RemoveClient(clientInfo.GetClientId());
    }

    Y_UNIT_TEST(ShouldVolumeInfo)
    {
        NProto::TStorageServiceConfig config;
        auto state = MakeIntrusive<TDiskRegistryState>();
        auto runtime = PrepareTestActorRuntime(config, state);

        TVolumeClient volume(*runtime);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED);

        volume.WaitReady();

        auto stat = volume.GetVolumeInfo();
        const auto& devices = stat->Record.GetVolume().GetDevices();
        UNIT_ASSERT_VALUES_EQUAL(1, devices.size());
        UNIT_ASSERT_VALUES_EQUAL("transport0", devices[0].GetTransportId());
    }

    Y_UNIT_TEST(ShouldStartPartitionsOnce)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        bool externalBootHappened = false;
        bool garbageCollectorCompleted = false;
        bool partitionsStopped = false;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvWaitReadyResponse: {
                        externalBootHappened = true;
                        break;
                    }
                    case TEvPartition::EvGarbageCollectorCompleted: {
                        garbageCollectorCompleted = true;
                        break;
                    }
                    case TEvBootstrapper::EvStop: {
                        partitionsStopped = true;
                        break;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();

        UNIT_ASSERT(externalBootHappened);
        UNIT_ASSERT(garbageCollectorCompleted);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);

        // Flag StartPartitionsNeeded becomes true
        volume.AddClient(clientInfo);
        volume.RemoveClient(clientInfo.GetClientId());

        // First reboot => partitions should start

        externalBootHappened = false;
        garbageCollectorCompleted = false;

        volume.RebootTablet();

        // Partitions had to start in RebootTablet
        UNIT_ASSERT(externalBootHappened);
        // Garbage collector should send success report
        UNIT_ASSERT(garbageCollectorCompleted);
        // Partitions should be stopped after gc
        UNIT_ASSERT(partitionsStopped);

        // Second reboot => partitions shouldn't start

        externalBootHappened = false;
        garbageCollectorCompleted = false;

        volume.RebootTablet();

        // Partitions hadn't start in RebootTablet
        UNIT_ASSERT(!externalBootHappened);
        UNIT_ASSERT(!garbageCollectorCompleted);

    }

    Y_UNIT_TEST(PartitionsShouldntStopAfterGcCompletedMsg)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        bool partitionsStopped = false;
        bool garbageCollectorCompleted = false;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvBootstrapper::EvStop: {
                        partitionsStopped = true;
                        break;
                    }
                    case TEvPartition::EvGarbageCollectorCompleted: {
                        garbageCollectorCompleted = true;
                        break;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();
        volume.RebootTablet();

        partitionsStopped = false;
        garbageCollectorCompleted = false;

        volume.WaitReady();

        // gc completed msg received
        UNIT_ASSERT(garbageCollectorCompleted);
        // partitions not stopped, as were started by WaitReady()
        UNIT_ASSERT(!partitionsStopped);
    }

    Y_UNIT_TEST(PartitionsShouldntStartAgainIfAlreadyStartedForGC)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        bool externalBootHappenedRequested = false;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvWaitReadyRequest: {
                        externalBootHappenedRequested = true;
                        break;
                    }
                    case TEvPartition::EvGarbageCollectorCompleted: {
                        return true;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);

        // Flag StartPartitionsNeeded becomes true
        volume.AddClient(clientInfo);
        volume.RemoveClient(clientInfo.GetClientId());

        volume.RebootTablet();

        externalBootHappenedRequested = false;

        // Start partitions
        volume.WaitReady();

        // We filtered gc completed msg
        // So partitions should not stop after gc
        UNIT_ASSERT(!externalBootHappenedRequested);
    }

    Y_UNIT_TEST(PartitionsStartBeforeStopAfterGC)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        bool partitionsStopped = false;
        bool bootRequested = false;

        bool stoppedStatus = false;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvBootstrapper::EvStatus: {
                        auto* msg = event->Get<TEvBootstrapper::TEvStatus>();
                        if (msg->Status == TEvBootstrapper::STOPPED) {
                                stoppedStatus = true;
                                return true;
                            }
                    }
                    case TEvBootstrapper::EvStop: {
                        partitionsStopped = true;
                        break;
                    }
                    case TEvHiveProxy::EvBootExternalRequest: {
                        bootRequested = true;
                        break;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);

        // Flag StartPartitionsNeeded becomes true
        volume.AddClient(clientInfo);
        volume.RemoveClient(clientInfo.GetClientId());

        volume.RebootTablet();

        UNIT_ASSERT(partitionsStopped);
        UNIT_ASSERT(stoppedStatus);

        bootRequested = false;

        // Should start partitions
        volume.SendStatVolumeRequest();
        auto response = volume.RecvStatVolumeResponse();
        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());

        // Partitions stopped, so should start
        UNIT_ASSERT(bootRequested);
    }

    Y_UNIT_TEST(ShouldGetStorageConfigValues)
    {
        NProto::TStorageServiceConfig config;
        config.SetCompactionRangeCountPerRun(10);
        auto runtime = PrepareTestActorRuntime(config);
        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        ui32 hasStorageConfigPatchCounter = 0;

        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
            if (event->Recipient == MakeStorageStatsServiceId()
                    && event->GetTypeRewrite() == TEvStatsService::EvVolumeSelfCounters)
            {
                auto* msg = event->Get<TEvStatsService::TEvVolumeSelfCounters>();

                hasStorageConfigPatchCounter =
                    msg->VolumeSelfCounters->Simple.HasStorageConfigPatch.Value;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        });

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>());
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(0, hasStorageConfigPatchCounter);

        NProto::TStorageServiceConfig patch;
        patch.SetCompactionRangeCountPerRun(11);
        volume.ChangeStorageConfig(std::move(patch));

        volume.SendToPipe(
            std::make_unique<TEvVolumePrivate::TEvUpdateCounters>());
        runtime->DispatchEvents({}, TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(1, hasStorageConfigPatchCounter);

        TVector<TString> requestedFields = {
            "CompactionRangeCountPerRun", "MaxSSDGroupWriteIops", "Unknown"};

        volume.SendStatVolumeRequest("", requestedFields);
        auto response = volume.RecvStatVolumeResponse();
        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        auto mapping = response->Record.GetStorageConfigFieldsToValues();

        UNIT_ASSERT_VALUES_EQUAL(mapping.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(mapping["CompactionRangeCountPerRun"], "11");
        UNIT_ASSERT_VALUES_EQUAL(mapping["MaxSSDGroupWriteIops"], "Default");
        UNIT_ASSERT_VALUES_EQUAL(mapping["Unknown"], "Not found");
    }

    Y_UNIT_TEST(ShouldDisableByFlag)
    {
        NProto::TStorageServiceConfig config;
        config.SetDisableStartPartitionsForGc(true);
        auto runtime = PrepareTestActorRuntime(config);

        TVolumeClient volume(*runtime);

        bool externalBootHappened = false;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvWaitReadyResponse: {
                        externalBootHappened = true;
                        break;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();

        UNIT_ASSERT(externalBootHappened);

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);

        // Flag StartPartitionsNeeded becomes true
        volume.AddClient(clientInfo);
        volume.RemoveClient(clientInfo.GetClientId());

        // First reboot => partitions should start, but it's off by flag

        externalBootHappened = false;

        volume.RebootTablet();

        // Partitions had not to start in RebootTablet
        UNIT_ASSERT(!externalBootHappened);
    }

    Y_UNIT_TEST(ShouldWaitForAllGCReportsBeforeStopPartitions)
    {
        auto runtime = PrepareTestActorRuntime();
        TVolumeClient volume(*runtime);

        bool gcReceived = false;
        bool partitionsStopped = false;
        bool externalBootHappened = false;

        bool StartPartitionsNeededSet = false;

        TAutoPtr<IEventHandle> evGarbageCollectorCompleted;
        ui64 oldTabletId = 0;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                if (!StartPartitionsNeededSet) {
                    return false;
                }
                switch (event->GetTypeRewrite()) {
                    case TEvPartition::EvGarbageCollectorCompleted: {
                        auto* msg = event->Get<TEvPartition::TEvGarbageCollectorCompleted>();
                        if (!gcReceived) {
                            gcReceived = true;
                            evGarbageCollectorCompleted = event.Release();
                            return true;
                        }
                        oldTabletId = msg->TabletId;
                        break;
                    }
                    case TEvBootstrapper::EvStop: {
                        partitionsStopped = true;
                        break;
                    }
                    case TEvHiveProxy::EvBootExternalRequest: {
                        externalBootHappened = true;
                        break;
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig(
            // default arguments
            0,
            0,
            0,
            0,
            false,
            1,
            NCloud::NProto::EStorageMediaKind::STORAGE_MEDIA_HYBRID,
            1024,
            "vol0",
            "cloud",
            "folder",
            2 // partitions count
        );
        volume.RebootTablet();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            false);

        // Flag StartPartitionsNeeded becomes true
        volume.AddClient(clientInfo);
        volume.RemoveClient(clientInfo.GetClientId());

        StartPartitionsNeededSet = true;
        volume.RebootTablet();
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // Partitions had to start in RebootTablet
        UNIT_ASSERT(externalBootHappened);
        // Partitions shouldn't be stopped, as not all partitions sent gc report
        UNIT_ASSERT(!partitionsStopped);

        runtime->Send(new IEventHandle(
            evGarbageCollectorCompleted->Recipient,
            evGarbageCollectorCompleted->Sender,
            new TEvPartition::TEvGarbageCollectorCompleted(oldTabletId)));

        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // Not all partitions sent gc report => partitions shouldn't stop
        UNIT_ASSERT(!partitionsStopped);

        runtime->Send(evGarbageCollectorCompleted.Release());
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // All partitions sent gc report => partitions should stop
        UNIT_ASSERT(partitionsStopped);
    }

    Y_UNIT_TEST(ShouldReturnClientsAndHostnameInStatVolumeResponse)
    {
        auto runtime = PrepareTestActorRuntime();

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();

        {
            NProto::TVolumeClientInfo info;
            info.SetClientId("c1");
            info.SetInstanceId("i1");
            info.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_WRITE);
            info.SetVolumeMountMode(NProto::VOLUME_MOUNT_LOCAL);
            volume.AddClient(info);
        }

        {
            NProto::TVolumeClientInfo info;
            info.SetClientId("c2");
            info.SetVolumeAccessMode(NProto::VOLUME_ACCESS_READ_ONLY);
            info.SetVolumeMountMode(NProto::VOLUME_MOUNT_REMOTE);
            volume.AddClient(info);
        }

        volume.WaitReady();

        {
            auto stat = volume.StatVolume();
            const auto& clients = stat->Record.GetClients();
            UNIT_ASSERT_VALUES_EQUAL(2, clients.size());
            UNIT_ASSERT_VALUES_EQUAL("c1", clients[0].GetClientId());
            UNIT_ASSERT_VALUES_EQUAL("i1", clients[0].GetInstanceId());
            UNIT_ASSERT_VALUES_EQUAL(0, clients[0].GetDisconnectTimestamp());
            UNIT_ASSERT_VALUES_EQUAL("c2", clients[1].GetClientId());
            UNIT_ASSERT_VALUES_EQUAL("", clients[1].GetInstanceId());
            UNIT_ASSERT_VALUES_EQUAL(0, clients[1].GetDisconnectTimestamp());
            UNIT_ASSERT_VALUES_EQUAL(FQDNHostName(), stat->Record.GetTabletHost());
            UNIT_ASSERT_VALUES_EQUAL(2, stat->Record.GetVolumeGeneration());
        }

        auto now = runtime->GetCurrentTime();

        // rebooting to set DisconnectTimestamp and to increment VolumeGeneration
        volume.RebootTablet();

        {
            auto stat = volume.StatVolume();
            const auto& clients = stat->Record.GetClients();
            UNIT_ASSERT_VALUES_EQUAL(2, clients.size());
            UNIT_ASSERT_VALUES_EQUAL("c1", clients[0].GetClientId());
            UNIT_ASSERT_VALUES_EQUAL("i1", clients[0].GetInstanceId());
            UNIT_ASSERT_C(
                clients[0].GetDisconnectTimestamp() > now.MicroSeconds(),
                TStringBuilder()
                    << "DisconnectTimestamp should be greater than reboot ts "
                    << clients[0].GetDisconnectTimestamp()
                    << ", " << now.MicroSeconds());
            UNIT_ASSERT_VALUES_EQUAL("c2", clients[1].GetClientId());
            UNIT_ASSERT_VALUES_EQUAL("", clients[1].GetInstanceId());
            UNIT_ASSERT_C(
                clients[1].GetDisconnectTimestamp() > now.MicroSeconds(),
                TStringBuilder()
                    << "DisconnectTimestamp should be greater than reboot ts "
                    << clients[1].GetDisconnectTimestamp()
                    << ", " << now.MicroSeconds());
            UNIT_ASSERT_VALUES_EQUAL(FQDNHostName(), stat->Record.GetTabletHost());
            UNIT_ASSERT_VALUES_EQUAL(3, stat->Record.GetVolumeGeneration());
        }
    }

    Y_UNIT_TEST(ShouldReportTracesForNestedRequests)
    {
        NProto::TStorageServiceConfig storageServiceConfig;

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);

        bool gotVolumeActorId = false;
        TActorId volumeActor;
        TAutoPtr<IEventHandle> delayedRequest;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) {
                switch (ev->GetTypeRewrite()) {
                    case TEvBlockStore::EvUpdateVolumeConfigResponse: {
                        if (!gotVolumeActorId) {
                            volumeActor = ev->Sender;
                            gotVolumeActorId = true;
                        }
                        break;
                    }
                    case TEvService::EvWriteBlocksRequest: {
                        if (!gotVolumeActorId ||
                            ev->Sender != volumeActor)
                        {
                            break;
                        }
                        if (!delayedRequest) {
                            delayedRequest = ev.Release();
                            return true;
                        }
                    }
                }
                return false;
            }
        );

        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);

        volume.AddClient(clientInfo);

        {
            auto request = volume.CreateWriteBlocksRequest(
                TBlockRange64::WithLength(0, 1024),
                clientInfo.GetClientId(),
                1
            );
            request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

            volume.SendToPipe(std::move(request));
        }

        auto request = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1
        );
        request->Record.MutableHeaders()->MutableInternal()->MutableTrace()->SetIsTraced(true);

        volume.SendToPipe(std::move(request));

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        runtime->Send(delayedRequest.Release());

        auto duplicateResponse = volume.RecvWriteBlocksResponse();
        auto response = volume.RecvWriteBlocksResponse();

        UNIT_ASSERT(
            HasProbe(
                duplicateResponse->Record.GetTrace().GetLWTrace().GetTrace(),
                "DuplicatedRequestReceived_Volume"));
    }

    Y_UNIT_TEST(ShouldDescribeFromBaseDisk)
    {
        NProto::TStorageServiceConfig storageServiceConfig;

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));
        runtime->RegisterService(
            MakeVolumeProxyServiceId(), runtime->AllocateEdgeActor(), 0);


        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,  // maxBandwidth
            0,  // maxIops
            0,  // burstPercentage
            0,  // maxPostponedWeight
            false,  // throttlingEnabled
            1,  // version
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024,   // block count per partition
            "vol0",  // diskId
            "cloud",  // cloudId
            "folder",  // folderId
            1,  // partition count
            2,  // blocksPerStripe
            "",  // tags
            "disk1",  // baseDiskId
            "ch"  // baseDiskCheckpointId
        );
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        bool firstWriteRequest = true;
        bool describeRequest = false;
        runtime->SetObserverFunc([&] (TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvService::TEvWriteBlocksRequest::EventType:
                        if (firstWriteRequest) {
                            firstWriteRequest = false;
                            break;
                        }
                        runtime->Send(new IEventHandle(
                            event->Sender,
                            event->Sender,
                            new TEvService::TEvWriteBlocksResponse(
                                MakeError(E_REJECTED)),
                            0,
                            event->Cookie,
                            nullptr));
                        return TTestActorRuntime::EEventAction::DROP;
                    case TEvVolume::TEvDescribeBlocksRequest::EventType:
                        describeRequest = true;
                        runtime->Send(new IEventHandle(
                            event->Sender,
                            event->Sender,
                            new TEvVolume::TEvDescribeBlocksResponse(
                                MakeError(E_REJECTED))));
                        return TTestActorRuntime::EEventAction::DROP;
                }
                return TTestActorRuntime::DefaultObserverFunc(event);
            }
        );

        auto writeRequest = volume.CreateWriteBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId(),
            1);

        volume.SendToPipe(std::move(writeRequest));
        auto response = volume.RecvWriteBlocksResponse();
        UNIT_ASSERT(response);
        UNIT_ASSERT_VALUES_EQUAL(E_REJECTED, response->GetStatus());

        runtime->DispatchEvents({}, TDuration::Seconds(1));
        auto readRequest = volume.CreateReadBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId());
        volume.SendToPipe(std::move(readRequest));
        volume.RecvReadBlocksResponse();

        UNIT_ASSERT(describeRequest);
    }

    Y_UNIT_TEST(ShouldReportLongRunningForBaseDisk)
    {
        NProto::TStorageServiceConfig storageServiceConfig;

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));
        runtime->RegisterService(
            MakeVolumeProxyServiceId(), runtime->AllocateEdgeActor(), 0);

        // Enable Schedule for all actors!!!
        runtime->SetRegistrationObserverFunc(
            [](auto& runtime, const auto& parentId, const auto& actorId)
            {
                Y_UNUSED(parentId);
                runtime.EnableScheduleForActor(actorId);
            });

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig(
            0,  // maxBandwidth
            0,  // maxIops
            0,  // burstPercentage
            0,  // maxPostponedWeight
            false,  // throttlingEnabled
            1,  // version
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED,
            1024,   // block count per partition
            "vol0",  // diskId
            "cloud",  // cloudId
            "folder",  // folderId
            1,  // partition count
            2,  // blocksPerStripe
            "",  // tags
            "disk1",  // baseDiskId
            "ch"  // baseDiskCheckpointId
        );
        volume.WaitReady();

        auto clientInfo = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0);
        volume.AddClient(clientInfo);

        auto makeDescribeResponse = []()
        {
            NKikimrProto::TLogoBlobID protoLogoBlobID1;
            protoLogoBlobID1.SetRawX1(142);
            protoLogoBlobID1.SetRawX2(143);
            protoLogoBlobID1.SetRawX3(0x8000);   // blob size 2028

            NKikimr::TLogoBlobID logoBlobID1(
                protoLogoBlobID1.GetRawX1(),
                protoLogoBlobID1.GetRawX2(),
                protoLogoBlobID1.GetRawX3());

            NProto::TRangeInBlob RangeInBlob1;
            RangeInBlob1.SetBlobOffset(0);
            RangeInBlob1.SetBlockIndex(0);
            RangeInBlob1.SetBlocksCount(1);

            NProto::TBlobPiece TBlobPiece1;
            TBlobPiece1.MutableBlobId()->CopyFrom(protoLogoBlobID1);
            TBlobPiece1.SetBSGroupId(2181038123);
            TBlobPiece1.MutableRanges()->Add(std::move(RangeInBlob1));

            auto result =
                std::make_unique<TEvVolume::TEvDescribeBlocksResponse>();
            result->Record.MutableBlobPieces()->Add(std::move(TBlobPiece1));
            return result;
        };
        // Make handler for stealing nested messages
        std::vector<std::unique_ptr<IEventHandle>> stolenRequests;
        auto requestThief = [&](TAutoPtr<IEventHandle>& event)
        {
            switch (event->GetTypeRewrite()) {
                case TEvVolume::TEvDescribeBlocksRequest::EventType:
                    runtime->Send(new IEventHandle(
                        event->Sender,
                        event->Sender,
                        makeDescribeResponse().release()
                        ));
                    return TTestActorRuntime::EEventAction::DROP;
                case TEvBlobStorage::EvGet: {
                    stolenRequests.push_back(
                        std::unique_ptr<IEventHandle>{event.Release()});
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        // Make handler for taking counters from TEvVolumeSelfCounters message.
        int longRunningReads = 0;
        TSimpleCounter writeCounter;
        auto takeCounters = [&](TAutoPtr<IEventHandle>& event)
        {
            if (event->Recipient == MakeStorageStatsServiceId() &&
                event->GetTypeRewrite() ==
                    TEvStatsService::EvVolumeSelfCounters)
            {
                auto* msg =
                    event->Get<TEvStatsService::TEvVolumeSelfCounters>();
                longRunningReads +=
                    msg->VolumeSelfCounters->Simple.LongRunningReadBlob.Value;
            }

            return TTestActorRuntime::DefaultObserverFunc(event);
        };

        // Ready to postpone request.
        runtime->SetObserverFunc(requestThief);

        // Starting the execution of the request. It won't be finished since we
        // stole it EvGet message.
        auto readRequest = volume.CreateReadBlocksRequest(
            TBlockRange64::WithLength(0, 1024),
            clientInfo.GetClientId());
        volume.SendToPipe(std::move(readRequest));

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvBlobStorage::EvGet);
        runtime->DispatchEvents(options, TDuration());
        UNIT_ASSERT_VALUES_UNEQUAL(0, stolenRequests.size());

        // Wait for EvLongRunningOperation arrived.
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(
                TEvPartitionCommonPrivate::EvLongRunningOperation);
            runtime->AdvanceCurrentTime(TDuration::Seconds(60));
            runtime->DispatchEvents(options, TDuration::Seconds(1));
        }

        // Wait for EvDiskRegistryBasedPartitionCounters arrived.
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(
                TEvVolume::TEvDiskRegistryBasedPartitionCounters::EventType);
            runtime->DispatchEvents(options);
        }

        // Wait for counters.
        {
            runtime->SetObserverFunc(takeCounters);
            TDispatchOptions options;
            options.FinalEvents.emplace_back(
                TEvVolumePrivate::TEvPartStatsSaved::EventType);
            runtime->DispatchEvents(options);

            UNIT_ASSERT_VALUES_EQUAL(1, longRunningReads);
        }
    }

    Y_UNIT_TEST(ShouldStartPartitionsAfterStop)
    {
        NProto::TStorageServiceConfig storageServiceConfig;

        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        THashMap<TString, NProto::TUpdateVolumeParamsMapValue> volumeParams;

        bool externalBootHappened = false;

        TAutoPtr<IEventHandle> handle;

        TActorId stoppedBootstrapperActorId;
        TActorId restoredBootstrapperActorId;

        ui32 generation;

        runtime->SetEventFilter(
            [&] (TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvBootstrapper::EvStop: {
                        stoppedBootstrapperActorId = event->Recipient;
                        handle = event.Release();
                        return true;
                    }
                    case TEvHiveProxy::EvBootExternalRequest: {
                        restoredBootstrapperActorId = event->Recipient;
                        externalBootHappened = true;
                        return false;
                    }
                    case TEvTablet::EvRestored: {
                        auto* msg = event->Get<TEvTablet::TEvRestored>();
                        generation = msg->Generation;
                        return false;
                    }
                }
                return false;
            }
        );

        externalBootHappened = false;

        volume.UpdateVolumeParams("vol0", volumeParams);

        // Partitions should be started even if stopping haven't finished
        UNIT_ASSERT(externalBootHappened);
        UNIT_ASSERT(handle);
        // Generation should be increased
        UNIT_ASSERT_EQUAL(generation, 2);

        runtime->Send(handle.Release());
        runtime->DispatchEvents({}, TDuration::Seconds(1));

        // Check that restored bootstrapper is new
        UNIT_ASSERT(stoppedBootstrapperActorId != restoredBootstrapperActorId);
    }

    Y_UNIT_TEST(ShouldResetAllPipesUponUpdateConfig)
    {
        NProto::TStorageServiceConfig storageServiceConfig;
        auto runtime = PrepareTestActorRuntime(std::move(storageServiceConfig));

        TVolumeClient volume(*runtime);
        volume.UpdateVolumeConfig();
        volume.WaitReady();

        auto clientInfo1 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            1);

        auto clientId = clientInfo1.GetClientId();

        volume.AddClient(clientInfo1);

        volume.UpdateVolumeConfig(
            0,
            0,
            0,
            0,
            false,
            2);
        volume.ReconnectPipe();

        auto clientInfo2 = CreateVolumeClientInfo(
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_LOCAL,
            0,
            1);

        clientInfo2.SetClientId(clientId);

        volume.AddClient(clientInfo2);

        volume.RemoveClient(clientId);

        auto stat = volume.StatVolume();
        const auto& clients = stat->Record.GetClients();
        UNIT_ASSERT_VALUES_EQUAL(clients.size(), 0);
    }
}

}   // namespace NCloud::NBlockStore::NStorage
