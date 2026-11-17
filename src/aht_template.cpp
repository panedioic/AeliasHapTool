#include "aht_template.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "cJSON.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace aht {

namespace {

// 1x1 透明 PNG：占位图标，避免模板依赖二进制资产
const char* kPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

bool base64Decode(const std::string& in, std::string& out)
{
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0;
    int bits = 0;

    for (char c : in) {
        if (c == '=') {
            break;
        }

        size_t idx = alphabet.find(c);

        if (idx == std::string::npos) {
            return false;
        }

        val = (val << 6) | static_cast<int>(idx);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((val >> bits) & 0xFF);
        }
    }

    return true;
}

bool makeDirs(const std::string& path)
{
    struct stat st;

    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    std::string acc;

    for (size_t i = 0; i < path.size(); ++i) {
        acc += path[i];

        if (path[i] == '/' || i + 1 == path.size()) {
            if (acc == "/" || acc.empty()) {
                continue;
            }
            mkdir(acc.c_str(), 0755);
        }
    }

    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string dirName(const std::string& path)
{
    size_t slash = path.find_last_of('/');

    if (slash == std::string::npos) {
        return path.empty() ? "." : path;
    }

    return path.substr(0, slash);
}

bool isDirEmpty(const std::string& path)
{
    DIR* dir = opendir(path.c_str());

    if (!dir) {
        return true;
    }

    bool empty = true;

    while (struct dirent* ent = readdir(dir)) {
        std::string name = ent->d_name;

        if (name != "." && name != "..") {
            empty = false;
            break;
        }
    }

    closedir(dir);
    return empty;
}

bool writeFile(const std::string& path, const std::string& data)
{
    if (!makeDirs(dirName(path))) {
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    if (!out) {
        return false;
    }

    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

// 纯数字 API → "platformVersion(api)"；表里没有就保留原样（旧 SDK 认整数）
const char* legacyPlatformVersion(int api)
{
    switch (api) {
        case 9:  return "3.2.0";
        case 10: return "4.0.0";
        case 11: return "4.1.0";
        case 12: return "5.0.0";
        case 13: return "5.0.1";
        case 14: return "5.0.2";
        case 15: return "5.0.3";
        case 16: return "5.0.4";
        case 17: return "5.0.5";
        case 18: return "5.1.0";
        case 19: return "5.1.1";
        case 20: return "6.0.0";
        case 21: return "6.0.1";
        case 22: return "6.0.2";
        case 23: return "6.1.0";
        case 24: return "6.1.1";
        default: return nullptr;
    }
}

bool readSdkMeta(const std::string& root, int& api, std::string& platform)
{
    std::ifstream in(root + "/ets/oh-uni-package.json");

    if (!in) {
        return false;
    }

    std::ostringstream buf;
    buf << in.rdbuf();

    cJSON* doc = cJSON_Parse(buf.str().c_str());

    if (!doc) {
        return false;
    }

    const cJSON* apiItem = cJSON_GetObjectItem(doc, "apiVersion");
    const cJSON* platItem = cJSON_GetObjectItem(doc, "platformVersion");

    if (apiItem && cJSON_IsString(apiItem) && apiItem->valuestring) {
        api = std::atoi(apiItem->valuestring);
    }

    if (platItem && cJSON_IsString(platItem) && platItem->valuestring) {
        platform = platItem->valuestring;
    }

    cJSON_Delete(doc);
    return api > 0 && !platform.empty();
}

} // namespace

// 归一化成 build-profile.json5 认的形态
std::string normalizeApiVersion(const std::string& given)
{
    // 1) 显式给了完整串（含括号）→ 原样
    if (!given.empty() && given.find('(') != std::string::npos) {
        return given;
    }

    // 2) SDK 元数据（OHOS_SDK / DevEco 默认布局）
    int sdkApi = 0;
    std::string sdkPlatform;
    std::vector<std::string> roots;

    if (const char* env = std::getenv("OHOS_SDK")) {
        roots.push_back(env);
    }
    if (const char* tool = std::getenv("TOOL_HOME")) {
        roots.push_back(std::string(tool) + "/sdk/default/openharmony");
    }
    roots.push_back("/apps/harmony/sdk/default/openharmony");

    for (const auto& root : roots) {
        if (readSdkMeta(root, sdkApi, sdkPlatform)) {
            break;
        }
    }

    (void)sdkApi;
    (void)sdkPlatform;

    // 3) 没给 → 稳定默认（Huawei 6.1.1 / API 24；本仓实测 hvigor 可构建）
    if (given.empty()) {
        return "6.1.1(24)";
    }

    int api = std::atoi(given.c_str());

    if (api <= 0) {
        return given;
    }

    if (const char* platform = legacyPlatformVersion(api)) {
        return std::string(platform) + "(" + std::to_string(api) + ")";
    }

    // 表外数字：原样写入（老 SDK 认整数），调用方给出提示
    return given;
}

namespace {

// json5 里纯数字的 apiVersion 不加引号
std::string jsonValue(const std::string& v)
{
    if (v.empty()) {
        return "\"\"";
    }

    bool numeric = true;

    for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            numeric = false;
            break;
        }
    }

    return numeric ? v : "\"" + v + "\"";
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    size_t pos = 0;

    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }

    return s;
}

std::string tpl(const char* text, const TemplateOptions& o)
{
    std::string s = text;
    s = replaceAll(s, "@BUNDLE@", o.bundle);
    s = replaceAll(s, "@MODULE@", o.moduleName);
    s = replaceAll(s, "@APPNAME@", o.appName);
    s = replaceAll(s, "@MODEL@", o.modelVersion);
    s = replaceAll(s, "@API@", jsonValue(o.apiVersion));
    return s;
}

// ── 模板内容 ────────────────────────────────────────────────────────────

const char* kAppJson5 = R"({
  "app": {
    "bundleName": "@BUNDLE@",
    "vendor": "example",
    "versionCode": 1000000,
    "versionName": "1.0.0",
    "icon": "$media:layered_image",
    "label": "$string:app_name"
  }
}
)";

const char* kAppStringJson = R"({
  "string": [
    {
      "name": "app_name",
      "value": "@APPNAME@"
    }
  ]
}
)";

const char* kLayeredImageJson = R"({
  "layered-image": {
    "background": "$media:background",
    "foreground": "$media:foreground"
  }
}
)";

const char* kRootBuildProfile = R"({
  "app": {
    "signingConfigs": [],
    "products": [
      {
        "name": "default",
        "targetSdkVersion": @API@,
        "compatibleSdkVersion": @API@,
        "runtimeOS": "HarmonyOS",
        "buildOption": {
          "strictMode": {
            "caseSensitiveCheck": true,
            "useNormalizedOHMUrl": true
          }
        }
      }
    ],
    "buildModeSet": [
      {
        "name": "debug"
      },
      {
        "name": "release"
      }
    ]
  },
  "modules": [
    {
      "name": "@MODULE@",
      "srcPath": "./@MODULE@",
      "targets": [
        {
          "name": "default",
          "applyToProducts": [
            "default"
          ]
        }
      ]
    }
  ]
}
)";

const char* kRootHvigorfile = R"(import { appTasks } from '@ohos/hvigor-ohos-plugin';

export default {
  system: appTasks, /* Built-in plugin of Hvigor. It cannot be modified. */
  plugins: []       /* Custom plugin to extend the functionality of Hvigor. */
}
)";

const char* kRootOhPackage = R"({
  "modelVersion": "@MODEL@",
  "description": "@APPNAME@",
  "dependencies": {},
  "devDependencies": {}
}
)";

const char* kModuleBuildProfile = R"({
  "apiType": "stageMode",
  "buildOption": {
    "externalNativeOptions": {
      "path": "./src/main/cpp/CMakeLists.txt",
      "arguments": "",
      "cppFlags": "",
      "abiFilters": [
        "arm64-v8a",
        "x86_64"
      ]
    }
  },
  "buildOptionSet": [
    {
      "name": "release",
      "arkOptions": {
        "obfuscation": {
          "ruleOptions": {
            "enable": false,
            "files": [
              "./obfuscation-rules.txt"
            ]
          }
        }
      }
    },
    {
      "name": "debug"
    }
  ],
  "targets": [
    {
      "name": "default"
    }
  ]
}
)";

const char* kModuleHvigorfile = R"(import { hapTasks } from '@ohos/hvigor-ohos-plugin';

export default {
  system: hapTasks, /* Built-in plugin of Hvigor. It cannot be modified. */
  plugins: []       /* Custom plugin to extend the functionality of Hvigor. */
}
)";

const char* kModuleOhPackage = R"({
  "name": "@MODULE@",
  "version": "1.0.0",
  "description": "@APPNAME@ entry module",
  "main": "",
  "author": "",
  "license": "",
  "dependencies": {}
}
)";

const char* kModuleJson5 = R"({
  "module": {
    "name": "@MODULE@",
    "type": "entry",
    "description": "$string:module_desc",
    "mainElement": "EntryAbility",
    "deviceTypes": [
      "phone",
      "tablet",
      "2in1"
    ],
    "deliveryWithInstall": true,
    "installationFree": false,
    "pages": "$profile:main_pages",
    "abilities": [
      {
        "name": "EntryAbility",
        "srcEntry": "./ets/entryability/EntryAbility.ets",
        "description": "$string:EntryAbility_desc",
        "icon": "$media:layered_image",
        "label": "$string:EntryAbility_label",
        "startWindowIcon": "$media:startIcon",
        "startWindowBackground": "$color:start_window_background",
        "exported": true,
        "skills": [
          {
            "entities": [
              "entity.system.home"
            ],
            "actions": [
              "ohos.want.action.home"
            ]
          }
        ]
      }
    ]
  }
}
)";

const char* kEntryAbility = R"(import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN = 0x0000;
const TAG = 'AhtTemplate';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load content: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'Content loaded');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'Ability onBackground');
  }
}
)";

const char* kIndexEts = R"(import { hilog } from '@kit.PerformanceAnalysisKit';
import nativeEntry from 'lib@MODULE@.so';

@Entry
@Component
struct Index {
  @State message: string = 'Hello HAP';
  @State sum: string = '-';

  aboutToAppear(): void {
    try {
      // 验证 NAPI 链路：native add(2, 3) === 5
      this.sum = String(nativeEntry.add(2, 3));
    } catch (e) {
      hilog.error(0x0000, 'AhtTemplate', 'native call failed: %{public}s', JSON.stringify(e));
      this.sum = 'native call failed';
    }
  }

  build() {
    Column({ space: 12 }) {
      Text(this.message)
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
      Text('native add(2, 3) = ' + this.sum)
        .fontSize(18)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }
}
)";

const char* kCmakeLists = R"(# native 库：ArkTS 通过 libentry.so 调用这里的 NAPI 模块
cmake_minimum_required(VERSION 3.5.0)
project(@MODULE@)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(@MODULE@ SHARED
    napi_init.cpp
)

target_link_libraries(@MODULE@ PUBLIC libace_napi.z.so)
)";

const char* kNapiInit = R"(// 最小 NAPI 模块：这里加自己的导出函数
#include <cstdint>
#include <string>

#include "napi/native_api.h"

namespace {

napi_value Add(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};

    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    double a = 0;
    double b = 0;

    napi_get_value_double(env, args[0], &a);
    napi_get_value_double(env, args[1], &b);

    napi_value result = nullptr;
    napi_create_double(env, a + b, &result);
    return result;
}

} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"add", nullptr, Add, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "@MODULE@",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&demoModule);
}
)";

const char* kTypesIndexDts = R"(export const add: (a: number, b: number) => number;
)";

const char* kTypesOhPackage = R"({
  "name": "lib@MODULE@.so",
  "types": "./index.d.ts",
  "version": "1.0.0",
  "description": "Native types for @MODULE@"
}
)";

const char* kColorJson = R"({
  "color": [
    {
      "name": "start_window_background",
      "value": "#FFFFFF"
    }
  ]
}
)";

const char* kModuleStringJson = R"({
  "string": [
    {
      "name": "module_desc",
      "value": "@APPNAME@ module"
    },
    {
      "name": "EntryAbility_desc",
      "value": "entry ability"
    },
    {
      "name": "EntryAbility_label",
      "value": "@APPNAME@"
    }
  ]
}
)";

const char* kMainPagesJson = R"({
  "src": [
    "pages/Index"
  ]
}
)";

const char* kHvigorConfig = R"({
  "modelVersion": "@MODEL@",
  "dependencies": {
  },
  "execution": {
  },
  "logging": {
  },
  "debugging": {
  },
  "nodeOptions": {
  }
}
)";

const char* kGitignore = R"(/node_modules
/oh_modules
/.preview
/build
/.cxx
/.hvigor
/local.properties
/.idea
*.iml
/entry/build
/entry/.preview
/entry/oh_modules
)";

const char* kRawfileReadme = R"(把随包分发的文件放这里（运行时通过 getRawFileContent 读取）。
)";

} // namespace

bool createProject(const TemplateOptions& opts, int& fileCount, std::string& error)
{
    if (opts.path.empty()) {
        error = "目标目录为空";
        return false;
    }

    struct stat st;

    if (stat(opts.path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            error = opts.path + " 已存在且不是目录";
            return false;
        }

        if (!isDirEmpty(opts.path) && !opts.force) {
            error = opts.path + " 已存在且非空（加 --force 覆盖写入）";
            return false;
        }
    }

    if (!makeDirs(opts.path)) {
        error = "无法创建目录 " + opts.path;
        return false;
    }

    const std::string root = opts.path;
    const std::string mod = opts.moduleName;
    TemplateOptions norm = opts;
    norm.apiVersion = normalizeApiVersion(opts.apiVersion);
    fileCount = 0;

    struct File {
        const char* rel;
        const char* content;
    };

    const File files[] = {
        {"AppScope/app.json5", kAppJson5},
        {"AppScope/resources/base/element/string.json", kAppStringJson},
        {"AppScope/resources/base/media/layered_image.json", kLayeredImageJson},
        {"build-profile.json5", kRootBuildProfile},
        {"hvigorfile.ts", kRootHvigorfile},
        {"oh-package.json5", kRootOhPackage},
        {"hvigor/hvigor-config.json5", kHvigorConfig},
        {".gitignore", kGitignore},
    };

    for (const auto& f : files) {
        if (!writeFile(root + "/" + f.rel, tpl(f.content, norm))) {
            error = std::string("写入失败：") + f.rel;
            return false;
        }
        ++fileCount;
    }

    // 模块级文件（路径带模块名）
    const std::string m = root + "/" + mod + "/";
    const File moduleFiles[] = {
        {"build-profile.json5", kModuleBuildProfile},
        {"hvigorfile.ts", kModuleHvigorfile},
        {"oh-package.json5", kModuleOhPackage},
        {"src/main/module.json5", kModuleJson5},
        {"src/main/ets/entryability/EntryAbility.ets", kEntryAbility},
        {"src/main/ets/pages/Index.ets", kIndexEts},
        {"src/main/cpp/CMakeLists.txt", kCmakeLists},
        {"src/main/cpp/napi_init.cpp", kNapiInit},
        {"src/main/cpp/types/lib@MODULE@/index.d.ts", kTypesIndexDts},
        {"src/main/cpp/types/lib@MODULE@/oh-package.json5", kTypesOhPackage},
        {"src/main/resources/base/element/color.json", kColorJson},
        {"src/main/resources/base/element/string.json", kModuleStringJson},
        {"src/main/resources/base/profile/main_pages.json", kMainPagesJson},
        {"src/main/resources/base/media/layered_image.json", kLayeredImageJson},
        {"src/main/resources/rawfile/README.txt", kRawfileReadme},
    };

    for (const auto& f : moduleFiles) {
        if (!writeFile(m + tpl(f.rel, norm), tpl(f.content, norm))) {
            error = std::string("写入失败：") + f.rel;
            return false;
        }
        ++fileCount;
    }

    // 图标：1x1 透明 PNG，三处名字不同（AppScope 背景/前景、模块启动图标）
    std::string png;

    if (!base64Decode(kPngBase64, png)) {
        error = "内置图标解码失败";
        return false;
    }

    const std::string icons[] = {
        "AppScope/resources/base/media/background.png",
        "AppScope/resources/base/media/foreground.png",
        "AppScope/resources/base/media/app_icon.png",
        mod + "/src/main/resources/base/media/background.png",
        mod + "/src/main/resources/base/media/foreground.png",
        mod + "/src/main/resources/base/media/startIcon.png",
    };

    for (const auto& rel : icons) {
        if (!writeFile(root + "/" + rel, png)) {
            error = "写入失败：" + rel;
            return false;
        }
        ++fileCount;
    }

    return true;
}

} // namespace aht
