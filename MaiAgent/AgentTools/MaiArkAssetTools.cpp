#include "MaiArkAssetTools.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

using Json = nlohmann::json;

std::string field(const Json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) return {};
    return object[key].get<std::string>();
}

bool validId(const std::string& id, const char* prefix) {
    return id.rfind(prefix, 0) == 0 && id.size() <= 128 &&
           std::all_of(id.begin(), id.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
           });
}

MaiToolResult invalid(const char* message) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
}

class MaiArkAssetTool final : public MaiTool {
public:
    explicit MaiArkAssetTool(MaiArkAssetProvider provider) : mProvider(std::move(provider)) {}

    std::string name() const override {
        return "ark_assets";
    }

    std::string description() const override {
        return "Manage Seedance private virtual avatar assets. Use list_groups, create_group, "
               "create_asset, upload_image, get_asset, or list_assets. create_asset submits an "
               "existing Ark-accessible HTTPS image URL directly. For local JPEG/PNG files, "
               "upload_image optionally stages the source in private object storage first. Ark "
               "processes uploads asynchronously: call get_asset until Status is Active or Failed. "
               "Only an Active asset ID can be passed to seedance_video as "
               "virtual_avatar_asset_id. Ark may reject a submission after upload; return its "
               "actual status and reason. The asset project must match the video API key project. "
               "The service requires Ark Assets AK/SK; only local-file staging needs an upload "
               "route.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["discover","list_groups","create_group","create_asset","upload_image","get_asset","list_assets"]},"name":{"type":"string"},"description":{"type":"string"},"group_id":{"type":"string"},"url":{"type":"string"},"image_path":{"type":"string"},"asset_id":{"type":"string"},"next_token":{"type":"string"}},"required":["action"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const std::string action = field(args, "action");
        return action != "discover" && action != "list_groups" && action != "list_assets" &&
               action != "get_asset";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return invalid("action is required");
        const std::string action = field(args, "action");
        if (action == "discover")
            return MaiToolResult::success(
                Json{{"configured", static_cast<bool>(mProvider)},
                     {"actions",
                      {"list_groups", "create_group", "create_asset", "upload_image", "get_asset",
                       "list_assets"}},
                     {"asset_type", "AIGC"},
                     {"usage", "Use asset://<ID> only after get_asset reports Active"}}
                    .dump());
        if (!mProvider)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "Ark Assets service is not configured on this device");
        Json request = {{"action", action}};
        if (action == "create_group") {
            const std::string name = field(args, "name");
            if (name.empty() || name.size() > 64)
                return invalid("name is required (up to 64 bytes)");
            request["name"] = name;
            request["description"] = field(args, "description");
        } else if (action == "create_asset") {
            const std::string groupId = field(args, "group_id");
            const std::string url = field(args, "url");
            if (!validId(groupId, "group-")) return invalid("valid group_id is required");
            if (url.rfind("https://", 0) != 0 || url.size() > 3000)
                return invalid("url must be an Ark-accessible HTTPS image URL");
            request["group_id"] = groupId;
            request["url"] = url;
            request["name"] = field(args, "name");
        } else if (action == "upload_image") {
            const std::string groupId = field(args, "group_id");
            const std::string source = context.resolvePath(field(args, "image_path"));
            if (!validId(groupId, "group-")) return invalid("valid group_id is required");
            const MaiFilePath sourcePath = MaiFilePath::fromUtf8(source);
            std::uint64_t size = 0;
            if (source.empty() || MaiFileSystem::isSymbolicLink(sourcePath) ||
                !MaiFileSystem::fileSize(sourcePath, size) || size == 0 || size > 30'000'000)
                return invalid("image_path must be an accessible regular file");
            const std::string base = sourcePath.baseName().toUtf8();
            const std::size_t dot = base.find_last_of('.');
            std::string extension = dot == std::string::npos ? std::string{} : base.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".jpg" && extension != ".jpeg" && extension != ".png")
                return invalid("image_path must be JPEG or PNG");
            request["group_id"] = groupId;
            request["image_path"] = source;
            request["name"] = field(args, "name");
        } else if (action == "get_asset") {
            const std::string assetId = field(args, "asset_id");
            if (!validId(assetId, "asset-")) return invalid("valid asset_id is required");
            request["asset_id"] = assetId;
        } else if (action == "list_assets" || action == "list_groups") {
            const std::string groupId = field(args, "group_id");
            if (!groupId.empty() && !validId(groupId, "group-"))
                return invalid("group_id is invalid");
            if (!groupId.empty()) request["group_id"] = groupId;
            const std::string nextToken = field(args, "next_token");
            if (nextToken.size() > 2048) return invalid("next_token is too long");
            if (!nextToken.empty()) request["next_token"] = nextToken;
        } else {
            return invalid("unknown Ark Assets action");
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "Ark Assets request was canceled");
        const MaiResult<std::string> response = mProvider(request.dump(), context);
        if (!response)
            return MaiToolResult::failure(response.error().code(), response.error().message());
        const Json result = Json::parse(response.value(), nullptr, false);
        if (!result.is_object())
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "Ark Assets returned invalid JSON");
        Json output = result;
        if (action == "get_asset" && result.value("Status", Json{}) == "Active" &&
            result.value("Id", Json{}).is_string())
            output["asset_uri"] = "asset://" + result["Id"].get<std::string>();
        return MaiToolResult::success(output.dump());
    }

private:
    MaiArkAssetProvider mProvider;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiArkAssetTool(MaiArkAssetProvider provider) {
    return std::make_unique<MaiArkAssetTool>(std::move(provider));
}
