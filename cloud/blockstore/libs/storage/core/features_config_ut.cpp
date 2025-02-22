#include "features_config.h"

#include <cloud/blockstore/config/features.pb.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/random/random.h>

namespace NCloud::NBlockStore::NStorage {

namespace {

////////////////////////////////////////////////////////////////////////////////

TVector<TString> RandomStrings(ui32 n)
{
    TVector<TString> r;
    for (ui32 i = 0; i < n; ++i) {
        TString s(20, 0);
        for (ui32 j = 0; j < s.Size(); ++j) {
            s[j] = RandomNumber<ui8>();
        }
        r.push_back(s);
    }
    return r;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TFeaturesConfigTest)
{
    Y_UNIT_TEST(ShouldMatchCloudsByProbability)
    {
        NProto::TFeaturesConfig fc;
        auto* f = fc.AddFeatures();
        f->SetName("some_feature");
        f->SetCloudProbability(0.2);
        *f->MutableWhitelist()->AddCloudIds() = "whitelisted_cloud";
        TFeaturesConfig config(fc);

        auto clouds = RandomStrings(1000);
        ui32 matches = 0;
        for (const auto& cloud: clouds) {
            matches += config.IsFeatureEnabled(cloud, {}, f->GetName());
        }

        UNIT_ASSERT_C(150 < matches && matches < 250, TStringBuilder()
            << "match count: " << matches);

        UNIT_ASSERT(config.IsFeatureEnabled(
            "whitelisted_cloud",
            {},
            f->GetName()));
    }

    Y_UNIT_TEST(ShouldMatchFoldersByProbability)
    {
        NProto::TFeaturesConfig fc;
        auto* f = fc.AddFeatures();
        f->SetName("some_feature");
        f->SetFolderProbability(0.3);
        *f->MutableWhitelist()->AddFolderIds() = "whitelisted_folder";
        TFeaturesConfig config(fc);

        auto folders = RandomStrings(1000);
        ui32 matches = 0;
        for (const auto& folder: folders) {
            matches += config.IsFeatureEnabled({}, folder, f->GetName());
        }

        UNIT_ASSERT_C(250 < matches && matches < 350, TStringBuilder()
            << "match count: " << matches);

        UNIT_ASSERT(config.IsFeatureEnabled(
            {},
            "whitelisted_folder",
            f->GetName()));
    }
}

}   // namespace NCloud::NBlockStore::NStorage
