package pools

import (
	"context"
	"fmt"

	"github.com/golang/protobuf/proto"
	"github.com/golang/protobuf/ptypes/empty"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/clients/nbs"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/headers"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/services/pools/protos"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/services/pools/storage"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/errors"
)

////////////////////////////////////////////////////////////////////////////////

type retireBaseDiskTask struct {
	scheduler  tasks.Scheduler
	storage    storage.Storage
	nbsFactory nbs.Factory
	request    *protos.RetireBaseDiskRequest
	state      *protos.RetireBaseDiskTaskState
}

func (t *retireBaseDiskTask) Save() ([]byte, error) {
	return proto.Marshal(t.state)
}

func (t *retireBaseDiskTask) Load(request, state []byte) error {
	t.request = &protos.RetireBaseDiskRequest{}
	err := proto.Unmarshal(request, t.request)
	if err != nil {
		return err
	}

	t.state = &protos.RetireBaseDiskTaskState{}
	return proto.Unmarshal(state, t.state)
}

func (t *retireBaseDiskTask) Run(
	ctx context.Context,
	execCtx tasks.ExecutionContext,
) error {

	baseDiskID := t.request.BaseDiskId
	selfTaskID := execCtx.GetTaskID()

	if t.request.SrcDisk != nil {
		client, err := t.nbsFactory.GetClient(ctx, t.request.SrcDisk.ZoneId)
		if err != nil {
			return err
		}

		err = client.GetCheckpointSize(
			ctx,
			func(blockIndex uint64, checkpointSize uint64) error {
				t.state.SrcDiskMilestoneBlockIndex = blockIndex
				t.state.SrcDiskMilestoneCheckpointSize = checkpointSize
				return execCtx.SaveState(ctx)
			},
			t.request.SrcDisk.DiskId,
			t.request.SrcDiskCheckpointId,
			t.state.SrcDiskMilestoneBlockIndex,
			t.state.SrcDiskMilestoneCheckpointSize,
		)
		if err != nil {
			if nbs.IsDiskNotFoundError(err) {
				// Should be idempotent.
				return nil
			}

			return err
		}
	}

	rebaseInfos, err := t.storage.RetireBaseDisk(
		ctx,
		baseDiskID,
		t.request.SrcDisk,
		t.request.SrcDiskCheckpointId,
		t.state.SrcDiskMilestoneCheckpointSize,
	)
	if err != nil {
		return err
	}

	rebaseTasks := make([]string, 0)

	for _, info := range rebaseInfos {
		idempotencyKey := selfTaskID
		idempotencyKey += "_" + info.OverlayDisk.DiskId
		idempotencyKey += "_" + info.TargetBaseDiskID

		taskID, err := t.scheduler.ScheduleTask(
			headers.SetIncomingIdempotencyKey(ctx, idempotencyKey),
			"pools.RebaseOverlayDisk",
			fmt.Sprintf(
				"Rebase overlay disk %v from %v to %v",
				info.OverlayDisk.DiskId,
				info.BaseDiskID,
				info.TargetBaseDiskID,
			),
			&protos.RebaseOverlayDiskRequest{
				OverlayDisk:      info.OverlayDisk,
				BaseDiskId:       info.BaseDiskID,
				TargetBaseDiskId: info.TargetBaseDiskID,
				SlotGeneration:   info.SlotGeneration,
			},
			"",
			"",
		)
		if err != nil {
			return err
		}

		rebaseTasks = append(rebaseTasks, taskID)
	}

	for _, taskID := range rebaseTasks {
		err := t.scheduler.WaitTaskEnded(ctx, taskID)
		if err != nil {
			return err
		}
	}

	retired, err := t.storage.IsBaseDiskRetired(ctx, baseDiskID)
	if err != nil {
		return err
	}

	if !retired {
		// NBS-3316: loop until base disk is retired.
		return errors.NewInterruptExecutionError()
	}

	return nil
}

func (t *retireBaseDiskTask) Cancel(
	ctx context.Context,
	execCtx tasks.ExecutionContext,
) error {

	return nil
}

func (t *retireBaseDiskTask) GetMetadata(
	ctx context.Context,
	taskID string,
) (proto.Message, error) {

	return &empty.Empty{}, nil
}

func (t *retireBaseDiskTask) GetResponse() proto.Message {
	return &empty.Empty{}
}
