#pragma once

#include <util/generic/string.h>

#include <functional>
#include <memory>

namespace NCloud::NBlockStore::NNotify {

////////////////////////////////////////////////////////////////////////////////

using THttpsCallback = std::function<void(int code, const TString& data)>;

class THttpsClient
{
public:
    THttpsClient();
    ~THttpsClient();

public:
    void LoadCaCerts(const TString& path);

    void Post(
        const TString& endpoint,
        const TString& data,
        const TString& contentType,
        const THttpsCallback& callback);

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;
};

}   // namespace NCloud::NBlockStore::NNotify
