package datasource

import (
	"context"

	"github.com/stretchr/testify/mock"
	"github.com/ydb-platform/nbs/contrib/ydb/library/yql/providers/generic/connector/app/server/paging"
	"github.com/ydb-platform/nbs/contrib/ydb/library/yql/providers/generic/connector/app/server/utils"
	api_service_protos "github.com/ydb-platform/nbs/contrib/ydb/library/yql/providers/generic/connector/libgo/service/protos"
	"github.com/ydb-platform/nbs/library/go/core/log"
)

var _ DataSource = (*DataSourceMock)(nil)

type DataSourceMock struct {
	mock.Mock
}

func (m *DataSourceMock) DescribeTable(
	ctx context.Context,
	logger log.Logger,
	request *api_service_protos.TDescribeTableRequest,
) (*api_service_protos.TDescribeTableResponse, error) {
	panic("not implemented") // TODO: Implement
}

func (m *DataSourceMock) ReadSplit(
	ctx context.Context,
	logger log.Logger,
	split *api_service_protos.TSplit,
	pagingWriter paging.Sink,
) {
	m.Called(split, pagingWriter)
}

func (m *DataSourceMock) TypeMapper() utils.TypeMapper {
	panic("not implemented") // TODO: Implement
}
