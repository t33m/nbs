package images

import (
	"context"
	"math/rand"
	"time"

	"github.com/ydb-platform/nbs/cloud/disk_manager/api"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/common"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/services/images/config"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/services/images/protos"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/errors"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/types"
)

////////////////////////////////////////////////////////////////////////////////

type service struct {
	taskScheduler tasks.Scheduler
	config        *config.ImagesConfig
}

func (s *service) CreateImage(
	ctx context.Context,
	req *disk_manager.CreateImageRequest,
) (string, error) {

	rand.Seed(time.Now().UnixNano())
	useS3 := common.Find(s.config.GetUseS3ForFolder(), req.FolderId) ||
		rand.Uint32()%100 < s.config.GetUseS3Percentage()

	pools := make([]*types.DiskPool, 0)
	if req.Pooled {
		for _, c := range s.config.GetDefaultDiskPoolConfigs() {
			pools = append(pools, &types.DiskPool{
				ZoneId:   c.GetZoneId(),
				Capacity: c.GetCapacity(),
			})
		}
	}

	switch src := req.Src.(type) {
	case *disk_manager.CreateImageRequest_SrcSnapshotId:
		if len(src.SrcSnapshotId) == 0 || len(req.DstImageId) == 0 {
			return "", errors.NewInvalidArgumentError(
				"some of parameters are empty, req=%v",
				req,
			)
		}

		return s.taskScheduler.ScheduleTask(
			ctx,
			"images.CreateImageFromSnapshot",
			"",
			&protos.CreateImageFromSnapshotRequest{
				SrcSnapshotId:                       src.SrcSnapshotId,
				DstImageId:                          req.DstImageId,
				FolderId:                            req.FolderId,
				OperationCloudId:                    req.OperationCloudId,
				OperationFolderId:                   req.OperationFolderId,
				DiskPools:                           pools,
				UseDataplaneTasksForLegacySnapshots: true, // TODO: remove it.
				UseS3:                               useS3,
			},
			req.OperationCloudId,
			req.OperationFolderId,
		)
	case *disk_manager.CreateImageRequest_SrcImageId:
		if len(src.SrcImageId) == 0 || len(req.DstImageId) == 0 {
			return "", errors.NewInvalidArgumentError(
				"some of parameters are empty, req=%v",
				req,
			)
		}

		return s.taskScheduler.ScheduleTask(
			ctx,
			"images.CreateImageFromImage",
			"",
			&protos.CreateImageFromImageRequest{
				SrcImageId:                          src.SrcImageId,
				DstImageId:                          req.DstImageId,
				FolderId:                            req.FolderId,
				OperationCloudId:                    req.OperationCloudId,
				OperationFolderId:                   req.OperationFolderId,
				DiskPools:                           pools,
				UseDataplaneTasksForLegacySnapshots: true, // TODO: remove it.
				UseS3:                               useS3,
			},
			req.OperationCloudId,
			req.OperationFolderId,
		)
	case *disk_manager.CreateImageRequest_SrcUrl:
		if len(src.SrcUrl.Url) == 0 || len(req.DstImageId) == 0 {
			return "", errors.NewInvalidArgumentError(
				"some of parameters are empty, req=%v",
				req,
			)
		}

		return s.taskScheduler.ScheduleTask(
			ctx,
			"images.CreateImageFromURL",
			"",
			&protos.CreateImageFromURLRequest{
				SrcURL:                                src.SrcUrl.Url,
				DstImageId:                            req.DstImageId,
				FolderId:                              req.FolderId,
				DiskPools:                             pools,
				OperationCloudId:                      req.OperationCloudId,
				OperationFolderId:                     req.OperationFolderId,
				UseDataplaneTasks:                     true, // TODO: remove it.
				UseDataplaneTasksSupportedFormatsOnly: true, // TODO: remove it.
				UseS3:                                 useS3,
				UseDataplaneTasksForVMDK:              true, // TODO: remove it.,
			},
			req.OperationCloudId,
			req.OperationFolderId,
		)
	case *disk_manager.CreateImageRequest_SrcDiskId:
		if len(src.SrcDiskId.ZoneId) == 0 ||
			len(src.SrcDiskId.DiskId) == 0 ||
			len(req.DstImageId) == 0 {

			return "", errors.NewInvalidArgumentError(
				"some of parameters are empty, req=%v",
				req,
			)
		}

		return s.taskScheduler.ScheduleTask(
			ctx,
			"images.CreateImageFromDisk",
			"",
			&protos.CreateImageFromDiskRequest{
				SrcDisk: &types.Disk{
					ZoneId: src.SrcDiskId.ZoneId,
					DiskId: src.SrcDiskId.DiskId,
				},
				DstImageId:        req.DstImageId,
				FolderId:          req.FolderId,
				OperationCloudId:  req.OperationCloudId,
				OperationFolderId: req.OperationFolderId,
				DiskPools:         pools,
				UseDataplaneTasks: true, // TODO: remove it.
				UseS3:             useS3,
			},
			req.OperationCloudId,
			req.OperationFolderId,
		)
	default:
		return "", errors.NewInvalidArgumentError("unknown src %s", src)
	}
}

func (s *service) DeleteImage(
	ctx context.Context,
	req *disk_manager.DeleteImageRequest,
) (string, error) {

	if len(req.ImageId) == 0 {
		return "", errors.NewInvalidArgumentError(
			"some of parameters are empty, req=%v",
			req,
		)
	}

	return s.taskScheduler.ScheduleTask(
		ctx,
		"images.DeleteImage",
		"",
		&protos.DeleteImageRequest{
			ImageId:           req.ImageId,
			OperationCloudId:  req.OperationCloudId,
			OperationFolderId: req.OperationFolderId,
		},
		req.OperationCloudId,
		req.OperationFolderId,
	)
}

////////////////////////////////////////////////////////////////////////////////

func NewService(
	taskScheduler tasks.Scheduler,
	config *config.ImagesConfig,
) Service {

	return &service{
		taskScheduler: taskScheduler,
		config:        config,
	}
}
