package schema

import (
	"context"

	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/auth"
	server_config "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/configs/server/config"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/dataplane/snapshot/storage/schema"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/monitoring/metrics"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/persistence"
	tasks_storage "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/storage"
)

/////////////////////////////////////////////////////////////////////////////////

func initDataplane(
	ctx context.Context,
	config *server_config.ServerConfig,
	creds auth.Credentials,
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

	snapshotConfig := config.GetDataplaneConfig().GetSnapshotConfig()

	snapshotDB, err := persistence.NewYDBClient(
		ctx,
		snapshotConfig.GetPersistenceConfig(),
		metrics.NewEmptyRegistry(),
		persistence.WithCredentials(creds),
	)
	if err != nil {
		return err
	}
	defer snapshotDB.Close(ctx)

	s3Config := snapshotConfig.GetPersistenceConfig().GetS3Config()
	var s3 *persistence.S3Client
	// TODO: remove when s3 will always be initialized.
	if s3Config != nil {
		s3, err = persistence.NewS3ClientFromConfig(s3Config)
		if err != nil {
			return err
		}
	}

	return schema.Create(ctx, snapshotConfig, snapshotDB, s3, dropUnusedColumns)
}
