#pragma once
#include <contrib/ydb/core/tx/tx_proxy/proxy.h>
#include <contrib/ydb/services/ext_index/metadata/object.h>
#include <contrib/ydb/services/metadata/ds_table/scheme_describe.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>
#include <contrib/ydb/services/ext_index/common/config.h>

namespace NKikimr::NCSIndex {

class IActivationExternalController {
public:
    using TPtr = std::shared_ptr<IActivationExternalController>;
    virtual ~IActivationExternalController() = default;
    virtual void OnActivationFailed(const TString& errorMessage, const TString& requestId) = 0;
    virtual void OnActivationSuccess(const TString& requestId) = 0;
};

class TActivation: public NMetadata::NProvider::ISchemeDescribeController,
    public NMetadata::NRequest::IExternalController<NMetadata::NRequest::TDialogYQLRequest>,
    public NMetadata::NInitializer::IModifierExternalController
{
private:
    mutable std::shared_ptr<TActivation> SelfContainer;
    NMetadata::NCSIndex::TObject Object;
    IActivationExternalController::TPtr ExternalController;
    const TString RequestId;
    const TConfig Config;

    NKikimr::NMetadata::NRequest::TDialogYQLRequest::TRequest BuildUpdateRequest() const;

protected:
    virtual void OnDescriptionFailed(const TString& errorMessage, const TString& requestId) override {
        ExternalController->OnActivationFailed(errorMessage, requestId);
        SelfContainer = nullptr;
    }
    virtual void OnDescriptionSuccess(NMetadata::NProvider::TTableInfo&& result, const TString& requestId) override;

    virtual void OnModificationFinished(const TString& modificationId) override;

    virtual void OnModificationFailed(const TString& errorMessage, const TString& modificationId) override;

    virtual void OnRequestResult(NMetadata::NRequest::TDialogYQLRequest::TResponse&& /*result*/) override {
        ExternalController->OnActivationSuccess(RequestId);
        SelfContainer = nullptr;
    }

    virtual void OnRequestFailed(const TString& errorMessage) override {
        ExternalController->OnActivationFailed(errorMessage, RequestId);
        SelfContainer = nullptr;
    }
public:
    void Start(std::shared_ptr<TActivation> selfContainer);

    TActivation(const NMetadata::NCSIndex::TObject& object,
        IActivationExternalController::TPtr externalController,
        const TString& requestId, const TConfig& config)
        : Object(object)
        , ExternalController(externalController)
        , RequestId(requestId)
        , Config(config)
    {

    }

};

}
