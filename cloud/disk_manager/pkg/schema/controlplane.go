package schema

import (
	"context"

	server_config "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/configs/server/config"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/persistence"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/resources"
	pools_storage "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/services/pools/storage"
	tasks_storage "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/storage"
)

////////////////////////////////////////////////////////////////////////////////

func initControlplane(
	ctx context.Context,
	config *server_config.ServerConfig,
	db *persistence.YDBClient,
	dropUnusedColumns bool,
) error {

	err := tasks_storage.CreateYDBTables(
		ctx,
		config.GetTasksConfig(),
		db,
		dropUnusedColumns,
	)
	if err != nil {
		return err
	}

	err = pools_storage.CreateYDBTables(
		ctx,
		config.GetPoolsConfig(),
		db,
		dropUnusedColumns,
	)
	if err != nil {
		return err
	}

	filesystemStorageFolder := ""
	if config.GetFilesystemConfig() != nil {
		filesystemStorageFolder = config.GetFilesystemConfig().GetStorageFolder()
	}

	return resources.CreateYDBTables(
		ctx,
		config.GetDisksConfig().GetStorageFolder(),
		config.GetImagesConfig().GetStorageFolder(),
		config.GetSnapshotsConfig().GetStorageFolder(),
		filesystemStorageFolder,
		config.GetPlacementGroupConfig().GetStorageFolder(),
		db,
		dropUnusedColumns,
	)
}
