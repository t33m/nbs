package app

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/spf13/cobra"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/accounting"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/auth"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/clients/nbs"
	server_config "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/configs/server/config"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/dataplane"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/health"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/logging"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/monitoring"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/monitoring/metrics"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/persistence"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/errors"
	tasks_storage "github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/tasks/storage"
	"github.com/ydb-platform/nbs/cloud/disk_manager/internal/pkg/util"
)

////////////////////////////////////////////////////////////////////////////////

func Run(
	appName string,
	defaultConfigFilePath string,
	newMetricsRegistry metrics.NewRegistryFunc,
) {

	var configFilePath string
	config := &server_config.ServerConfig{}

	var rootCmd = &cobra.Command{
		Use:   appName,
		Short: "Disk Manager Server",
		PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
			return util.ParseProto(configFilePath, config)
		},
		RunE: func(cmd *cobra.Command, args []string) error {
			return run(config, newMetricsRegistry)
		},
	}
	rootCmd.Flags().StringVar(
		&configFilePath,
		"config",
		defaultConfigFilePath,
		"Path to the config file",
	)

	if err := rootCmd.Execute(); err != nil {
		log.Fatalf("Error: %v", err)
	}
}

////////////////////////////////////////////////////////////////////////////////

func run(
	config *server_config.ServerConfig,
	newMetricsRegistry metrics.NewRegistryFunc,
) error {

	var err error

	ignoreSigpipe()

	// Use cancellable context.
	ctx := context.Background()

	var hostname string

	// TODO: move hostname from GrpcConfig.
	if config.GrpcConfig != nil {
		hostname = config.GetGrpcConfig().GetHostname()
	}

	logger := logging.NewLogger(config.LoggingConfig)
	ctx = logging.SetLogger(ctx, logger)

	if len(hostname) == 0 {
		hostname, err = os.Hostname()
		if err != nil {
			logging.Error(ctx, "Failed to get hostname from OS: %v", err)
			return errors.NewNonRetriableError(err)
		}
	}

	logging.Info(ctx, "Starting monitoring")
	mon := monitoring.NewMonitoring(config.MonitoringConfig, newMetricsRegistry)
	mon.Start(ctx)

	accounting.Init(mon.NewRegistry("accounting"))

	creds := auth.NewCredentials(ctx, config.GetAuthConfig())

	logging.Info(ctx, "Initializing YDB client")
	ydbClientRegistry := mon.NewRegistry("ydb_client")
	db, err := persistence.NewYDBClient(
		ctx,
		config.PersistenceConfig,
		ydbClientRegistry,
		persistence.WithRegistry(mon.NewRegistry("ydb")),
		persistence.WithCredentials(creds),
	)
	if err != nil {
		logging.Error(ctx, "Failed to initialize YDB client: %v", err)
		return err
	}
	defer db.Close(ctx)
	logging.Info(ctx, "Initialized YDB client")

	taskMetricsRegistry := mon.NewRegistry("tasks")
	taskStorage, err := tasks_storage.NewStorage(
		config.TasksConfig,
		taskMetricsRegistry,
		db,
	)
	if err != nil {
		logging.Error(ctx, "Failed to initialize task storage: %v", err)
		return err
	}

	logging.Info(ctx, "Creating task scheduler")
	taskRegistry := tasks.NewRegistry()
	taskScheduler, err := tasks.NewScheduler(
		ctx,
		taskRegistry,
		taskStorage,
		config.TasksConfig,
	)
	if err != nil {
		logging.Error(ctx, "Failed to create task scheduler: %v", err)
		return err
	}

	nbsClientMetricsRegistry := mon.NewRegistry("nbs_client")
	nbsSessionMetricsRegistry := mon.NewRegistry("nbs_session")
	nbsFactory, err := nbs.NewFactoryWithCreds(
		ctx,
		config.NbsConfig,
		creds,
		nbsClientMetricsRegistry,
		nbsSessionMetricsRegistry,
	)
	if err != nil {
		logging.Error(ctx, "Failed to create nbs factory: %v", err)
		return err
	}

	var s3 *persistence.S3Client
	var s3Bucket string

	if config.GetDataplaneConfig() == nil {
		logging.Info(ctx, "Registering dataplane tasks")
		err = dataplane.Register(ctx, taskRegistry)
		if err != nil {
			logging.Error(ctx, "Failed to register dataplane tasks: %v", err)
			return err
		}
	} else {
		logging.Info(ctx, "Initializing YDB client for snapshot database")
		snapshotConfig := config.GetDataplaneConfig().GetSnapshotConfig()
		snapshotDB, err := persistence.NewYDBClient(
			ctx,
			snapshotConfig.GetPersistenceConfig(),
			ydbClientRegistry,
			persistence.WithCredentials(creds),
		)
		if err != nil {
			logging.Error(ctx, "Failed to initialize YDB client for snapshot database: %v", err)
			return err
		}
		defer snapshotDB.Close(ctx)

		s3Config := snapshotConfig.GetPersistenceConfig().GetS3Config()
		s3Bucket = snapshotConfig.GetS3Bucket()
		// TODO: remove when s3 will always be initialized.
		if s3Config != nil {
			var err error
			s3, err = persistence.NewS3ClientFromConfig(s3Config)
			if err != nil {
				return err
			}
		}

		err = initDataplane(
			ctx,
			config,
			mon,
			snapshotDB,
			taskRegistry,
			taskScheduler,
			nbsFactory,
			s3,
		)
		if err != nil {
			logging.Error(ctx, "Failed to initialize dataplane: %v", err)
			return err
		}
	}

	var serve func() error

	if config.GetGrpcConfig() != nil {
		logging.Info(ctx, "Initializing GRPC services")
		serve, err = initControlplane(
			ctx,
			config,
			mon,
			creds,
			db,
			taskStorage,
			taskRegistry,
			taskScheduler,
			nbsFactory,
		)
		if err != nil {
			logging.Error(ctx, "Failed to initialize GRPC services: %v", err)
			return err
		}
	}

	runnerMetricsRegistry := mon.NewRegistry("runners")

	controller := tasks.NewController(
		ctx,
		taskStorage,
		taskRegistry,
		runnerMetricsRegistry,
		config.GetTasksConfig(),
		hostname,
	)

	err = controller.StartRunners()
	if err != nil {
		logging.Error(ctx, "Failed to start runners: %v", err)
		return err
	}

	health.MonitorHealth(
		ctx,
		mon.NewRegistry("health"),
		db,
		nbsFactory,
		s3,
		s3Bucket,
		controller.HealthChangedCallback,
	)

	if serve != nil {
		logging.Info(ctx, "Serving requests")
		return serve()
	}

	select {}
}

////////////////////////////////////////////////////////////////////////////////

func ignoreSigpipe() {
	c := make(chan os.Signal, 1)
	signal.Notify(c, syscall.SIGPIPE)
}
