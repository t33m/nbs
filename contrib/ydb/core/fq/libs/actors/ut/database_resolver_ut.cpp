#include <contrib/ydb/core/fq/libs/actors/database_resolver.h>
#include <contrib/ydb/core/fq/libs/events/events.h>
#include <contrib/ydb/core/fq/libs/db_id_async_resolver_impl/mdb_endpoint_generator.h>

#include <contrib/ydb/core/testlib/actors/test_runtime.h>
#include <contrib/ydb/core/testlib/basics/helpers.h>

#include <contrib/ydb/library/yql/providers/common/token_accessor/client/factory.h>
#include <library/cpp/testing/unittest/registar.h>
#include <contrib/ydb/library/actors/http/http_proxy.h>

namespace {

using namespace NKikimr;
using namespace NFq;

struct TTestBootstrap : public TTestActorRuntime {
    NConfig::TCheckpointCoordinatorConfig Settings;
    NActors::TActorId DatabaseResolver;
    NActors::TActorId HttpProxy;
    NActors::TActorId AsyncResolver;
    THashMap<TActorId, ui64> ActorToTask;

    explicit TTestBootstrap()
        : TTestActorRuntime(true)
    {
        TAutoPtr<TAppPrepare> app = new TAppPrepare();
        
        Initialize(app->Unwrap());
        HttpProxy = AllocateEdgeActor();
        AsyncResolver = AllocateEdgeActor();

        SetLogPriority(NKikimrServices::STREAMS_CHECKPOINT_COORDINATOR, NLog::PRI_DEBUG);
        auto credentialsFactory = NYql::CreateSecuredServiceAccountCredentialsOverTokenAccessorFactory("", true, "");

        DatabaseResolver = Register(CreateDatabaseResolver(
            HttpProxy,
            credentialsFactory
        ));
    }

    void WaitForBootstrap() {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(NActors::TEvents::TSystem::Bootstrap, 1);
        UNIT_ASSERT(DispatchEvents(options));
    }

    void CheckEqual(
        const NHttp::TEvHttpProxy::TEvHttpOutgoingRequest& lhs,
        const NHttp::TEvHttpProxy::TEvHttpOutgoingRequest& rhs) {
        UNIT_ASSERT_EQUAL(lhs.Request->URL, rhs.Request->URL);
    }

    void CheckEqual(
        const NFq::TEvents::TEvEndpointResponse& lhs,
        const NFq::TEvents::TEvEndpointResponse& rhs) {
        UNIT_ASSERT_EQUAL(lhs.DbResolverResponse.Success, rhs.DbResolverResponse.Success);
        UNIT_ASSERT_EQUAL(lhs.DbResolverResponse.DatabaseDescriptionMap.size(), rhs.DbResolverResponse.DatabaseDescriptionMap.size());
        for (auto it = lhs.DbResolverResponse.DatabaseDescriptionMap.begin(); it != lhs.DbResolverResponse.DatabaseDescriptionMap.end(); ++it) {
            auto key = it->first;
            UNIT_ASSERT(rhs.DbResolverResponse.DatabaseDescriptionMap.contains(key));
            const NYql::TDatabaseResolverResponse::TDatabaseDescription& lhsDesc = it->second;
            const NYql::TDatabaseResolverResponse::TDatabaseDescription& rhsDesc = rhs.DbResolverResponse.DatabaseDescriptionMap.find(key)->second;
            UNIT_ASSERT_EQUAL(lhsDesc.Endpoint, rhsDesc.Endpoint);
            UNIT_ASSERT_EQUAL(lhsDesc.Host, rhsDesc.Host);
            UNIT_ASSERT_EQUAL(lhsDesc.Port, rhsDesc.Port);
            UNIT_ASSERT_EQUAL(lhsDesc.Database, rhsDesc.Database);
            UNIT_ASSERT_EQUAL(lhsDesc.Secure, rhsDesc.Secure);
        }
    }

    template <typename TEvent>
    typename TEvent::TPtr ExpectEvent(NActors::TActorId actorId, const TEvent& expectedEventValue, NActors::TActorId* outSenderActorId = nullptr) {
        typename TEvent::TPtr eventHolder = GrabEdgeEvent<TEvent>(actorId, TDuration::Seconds(10));
        UNIT_ASSERT(eventHolder.Get() != nullptr);
        TEvent* actual = eventHolder.Get()->Get();
        CheckEqual(expectedEventValue, *actual);
        if (outSenderActorId) {
            *outSenderActorId = eventHolder->Sender;
        }
        return eventHolder;
    }
   
};
} // namespace

namespace NFq {

Y_UNIT_TEST_SUITE(TDatabaseResolverTests) {

    void Test(
        NYql::EDatabaseType databaseType,
        NYql::NConnector::NApi::EProtocol protocol,
        const TString& getUrl,
        const TString& responseBody,
        const NYql::TDatabaseResolverResponse::TDatabaseDescription& description)
    {
        TTestBootstrap bootstrap;

        NYql::TDatabaseAuth databaseAuth;
        databaseAuth.UseTls = true;
        databaseAuth.Protocol = protocol;

        TString databaseId{"etn021us5r9rhld1vgbh"};
        auto requestIdAnddatabaseType = std::make_pair(databaseId, databaseType);

        bootstrap.Send(new IEventHandle(
            bootstrap.DatabaseResolver,
            bootstrap.AsyncResolver,
            new NFq::TEvents::TEvEndpointRequest(
                NYql::IDatabaseAsyncResolver::TDatabaseAuthMap(
                    {std::make_pair(requestIdAnddatabaseType, databaseAuth)}),
                TString("https://ydbc.ydb.cloud.yandex.net:8789/ydbc/cloud-prod"),
                TString("mdbGateway"),
                TString("traceId"),
                NFq::MakeMdbEndpointGeneratorGeneric(true))));

        NActors::TActorId processorActorId;
        auto httpRequest = NHttp::THttpOutgoingRequest::CreateRequestGet(getUrl);
        auto httpOutgoingRequestHolder = bootstrap.ExpectEvent<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>(bootstrap.HttpProxy, NHttp::TEvHttpProxy::TEvHttpOutgoingRequest(
             httpRequest), &processorActorId);

        NHttp::TEvHttpProxy::TEvHttpOutgoingRequest* httpOutgoingRequest = httpOutgoingRequestHolder.Get()->Get();

        bootstrap.WaitForBootstrap();

        auto response = std::make_unique<NHttp::THttpIncomingResponse>(nullptr);
        response->Status = "200";
        response->Body = responseBody;

        bootstrap.Send(new IEventHandle(
            processorActorId,
            bootstrap.HttpProxy,
            new NHttp::TEvHttpProxy::TEvHttpIncomingResponse(httpOutgoingRequest->Request, response.release(), "")));

        NYql::TDatabaseResolverResponse::TDatabaseDescriptionMap result;
        result[requestIdAnddatabaseType] = description;
        bootstrap.ExpectEvent<TEvents::TEvEndpointResponse>(bootstrap.AsyncResolver, 
            NFq::TEvents::TEvEndpointResponse(
                NYql::TDatabaseResolverResponse(std::move(result), true, NYql::TIssues{})));
    }
    
    Y_UNIT_TEST(Ydb_Serverless) {
        Test(
            NYql::EDatabaseType::Ydb,
            NYql::NConnector::NApi::EProtocol::PROTOCOL_UNSPECIFIED,
            "https://ydbc.ydb.cloud.yandex.net:8789/ydbc/cloud-prod/database?databaseId=etn021us5r9rhld1vgbh",
            R"(
                {
                    "endpoint":"grpcs://ydb.serverless.yandexcloud.net:2135/?database=/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh"
                })",           
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{"ydb.serverless.yandexcloud.net:2135"},
                TString{""},
                0,
                TString("/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh"),
                true
                }
            );
    }

    Y_UNIT_TEST(DataStreams_Serverless) {
        Test(
            NYql::EDatabaseType::DataStreams,
            NYql::NConnector::NApi::EProtocol::PROTOCOL_UNSPECIFIED,
            "https://ydbc.ydb.cloud.yandex.net:8789/ydbc/cloud-prod/database?databaseId=etn021us5r9rhld1vgbh",
            R"(
                {
                    "endpoint":"grpcs://ydb.serverless.yandexcloud.net:2135/?database=/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh"
                })",
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{"yds.serverless.yandexcloud.net:2135"},
                TString{""},
                0,
                TString("/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh"),
                true
                }
            );
    }

    Y_UNIT_TEST(DataStreams_Dedicated) {
        Test(
            NYql::EDatabaseType::DataStreams,
            NYql::NConnector::NApi::EProtocol::PROTOCOL_UNSPECIFIED,
            "https://ydbc.ydb.cloud.yandex.net:8789/ydbc/cloud-prod/database?databaseId=etn021us5r9rhld1vgbh",
            R"(
                {
                    "endpoint":"grpcs://lb.etn021us5r9rhld1vgbh.ydb.mdb.yandexcloud.net:2135/?database=/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh",
                    "storageConfig":{"storageSizeLimit":107374182400}
                })",
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{"u-lb.etn021us5r9rhld1vgbh.ydb.mdb.yandexcloud.net:2135"},
                TString{""},
                0,
                TString("/ru-central1/b1g7jdjqd07qg43c4fmp/etn021us5r9rhld1vgbh"),
                true
                }
            );
    }

    Y_UNIT_TEST(ClickhouseNative) {
        Test(
            NYql::EDatabaseType::ClickHouse,
            NYql::NConnector::NApi::EProtocol::NATIVE,
            "https://mdb.api.cloud.yandex.net:443/managed-clickhouse/v1/clusters/etn021us5r9rhld1vgbh/hosts",
            R"({
                "hosts": [
                {
                "services": [
                    {
                    "type": "CLICKHOUSE",
                    "health": "ALIVE"
                    }
                ],
                "name": "rc1a-d6dv17lv47v5mcop.mdb.yandexcloud.net",
                "clusterId": "c9ql09h4firghvrv49jt",
                "zoneId": "ru-central1-a",
                "type": "CLICKHOUSE",
                "health": "ALIVE"
                }
                ]
                })",
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{""},
                TString{"rc1a-d6dv17lv47v5mcop.db.yandex.net"},
                9440,
                TString(""),
                true
                }
            );
    }

    Y_UNIT_TEST(ClickhouseHttp) {
        Test(
            NYql::EDatabaseType::ClickHouse,
            NYql::NConnector::NApi::EProtocol::HTTP,
            "https://mdb.api.cloud.yandex.net:443/managed-clickhouse/v1/clusters/etn021us5r9rhld1vgbh/hosts",
            R"({
                "hosts": [
                {
                "services": [
                    {
                    "type": "CLICKHOUSE",
                    "health": "ALIVE"
                    }
                ],
                "name": "rc1a-d6dv17lv47v5mcop.mdb.yandexcloud.net",
                "clusterId": "c9ql09h4firghvrv49jt",
                "zoneId": "ru-central1-a",
                "type": "CLICKHOUSE",
                "health": "ALIVE"
                }
                ]
                })",
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{""},
                TString{"rc1a-d6dv17lv47v5mcop.db.yandex.net"},
                8443,
                TString(""),
                true
                }
            );
    }

    Y_UNIT_TEST(Postgres) {
        Test(
            NYql::EDatabaseType::PostgreSQL,
            NYql::NConnector::NApi::EProtocol::NATIVE,
            "https://mdb.api.cloud.yandex.net:443/managed-postgresql/v1/clusters/etn021us5r9rhld1vgbh/hosts",
            R"({
                "hosts": [
                {
                "services": [
                    {
                    "type": "POOLER",
                    "health": "ALIVE"
                    },
                    {
                    "type": "POSTGRESQL",
                    "health": "ALIVE"
                    }
                ],
                "name": "rc1b-eyt6dtobu96rwydq.mdb.yandexcloud.net",
                "clusterId": "c9qb2bjghs8onbncpamk",
                "zoneId": "ru-central1-b",
                "role": "MASTER",
                "health": "ALIVE"
                }
                ]
                })",
            NYql::TDatabaseResolverResponse::TDatabaseDescription{
                TString{""},
                TString{"rc1b-eyt6dtobu96rwydq.db.yandex.net"},
                6432,
                TString(""),
                true
                }
            );
    }
}

} // namespace NFq
