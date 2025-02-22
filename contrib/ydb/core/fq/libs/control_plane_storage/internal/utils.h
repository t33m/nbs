#pragma once

#include <tuple>

#include <contrib/ydb/public/sdk/cpp/client/ydb_value/value.h>

#include <contrib/ydb/library/yql/public/issue/yql_issue_message.h>

#include <contrib/ydb/core/fq/libs/config/protos/issue_id.pb.h>
#include <contrib/ydb/core/fq/libs/control_plane_storage/ydb_control_plane_storage_impl.h>
#include <contrib/ydb/core/fq/libs/exceptions/exceptions.h>

namespace NFq {

struct TTopicConsumerLess {
    bool operator()(const Fq::Private::TopicConsumer& c1, const Fq::Private::TopicConsumer& c2) const {
        // Cluster endpoint/use ssl are not in key
        return std::tie(c1.database_id(), c1.database(), c1.topic_path(), c1.consumer_name()) < std::tie(c2.database_id(), c2.database(), c2.topic_path(), c2.consumer_name());
    }
};

NYql::TIssues ValidateWriteResultData(const TString& resultId, const Ydb::ResultSet& resultSet, const TInstant& deadline, const TDuration& ttl);

NYql::TIssues ValidateGetTask(const TString& owner, const TString& hostName);

NYql::TIssues ValidatePingTask(const TString& scope, const TString& queryId, const TString& owner, const TInstant& deadline, const TDuration& ttl);

NYql::TIssues ValidateNodesHealthCheck(
    const TString& tenant,
    const TString& instanceId,
    const TString& hostName);

NYql::TIssues ValidateCreateOrDeleteRateLimiterResource(const TString& queryId, const TString& scope, const TString& tenant, const TString& owner);

std::vector<TString> GetMeteringRecords(const TString& statistics, bool billable, const TString& jobId, const TString& scope, const TString& sourceId);
TString GetPrettyStatistics(const TString& statistics);

};
