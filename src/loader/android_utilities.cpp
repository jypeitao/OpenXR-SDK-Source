// Copyright (c) 2020-2024, The Khronos Group Inc.
// Copyright (c) 2020-2021, Collabora, Ltd.
//
// SPDX-License-Identifier:  Apache-2.0 OR MIT
//
// Initial Author: Rylie Pavlik <rylie.pavlik@collabora.com>

#include "android_utilities.h"

#ifdef __ANDROID__
#include <wrap/android.net.h>
#include <wrap/android.content.h>
#include <wrap/android.database.h>
#include <json/value.h>

#include <openxr/openxr.h>

#include <dlfcn.h>
#include <sstream>
#include <vector>
#include <android/log.h>

#ifdef XR_PRIORITIZE_LOAD_RUNTIME_FROM_LOADER_LOCATION
#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>
#endif

#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OpenXR-Loader", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "OpenXR-Loader", __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "OpenXR-Loader", __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "OpenXR-Loader", __VA_ARGS__)

namespace openxr_android {
using wrap::android::content::ContentUris;
using wrap::android::content::Context;
using wrap::android::database::Cursor;
using wrap::android::net::Uri;
using wrap::android::net::Uri_Builder;

// Code in here corresponds roughly to the Java "BrokerContract" class and subclasses.
namespace {
constexpr auto AUTHORITY = "org.khronos.openxr.runtime_broker";
constexpr auto SYSTEM_AUTHORITY = "org.khronos.openxr.system_runtime_broker";
constexpr auto BASE_PATH = "openxr";
constexpr auto ABI_PATH = "abi";
constexpr auto RUNTIMES_PATH = "runtimes";

constexpr const char *getBrokerAuthority(bool systemBroker) { return systemBroker ? SYSTEM_AUTHORITY : AUTHORITY; }

struct BaseColumns {
    /**
     * The unique ID for a row.
     */
    [[maybe_unused]] static constexpr auto ID = "_id";
};

/**
 * Contains details for the /openxr/[major_ver]/abi/[abi]/runtimes/active URI.
 * <p>
 * This URI represents a "table" containing at most one item, the currently active runtime. The
 * policy of which runtime is chosen to be active (if more than one is installed) is left to the
 * content provider.
 * <p>
 * No sort order is required to be honored by the content provider.
 */
namespace active_runtime {
/**
 * Final path component to this URI.
 */
static constexpr auto TABLE_PATH = "active";

/**
 * Create a content URI for querying the data on the active runtime for a
 * given major version of OpenXR.
 *
 * @param systemBroker If the system runtime broker (instead of the installable one) should be queried.
 * @param majorVer The major version of OpenXR.
 * @param abi The Android ABI name in use.
 * @return A content URI for a single item: the active runtime.
 */
static Uri makeContentUri(bool systemBroker, int majorVersion, const char *abi) {
    auto builder = Uri_Builder::construct();
    builder.scheme("content")
        .authority(getBrokerAuthority(systemBroker))
        .appendPath(BASE_PATH)
        .appendPath(std::to_string(majorVersion))
        .appendPath(ABI_PATH)
        .appendPath(abi)
        .appendPath(RUNTIMES_PATH)
        .appendPath(TABLE_PATH);
    return builder.build();
}

struct Columns : BaseColumns {
    /**
     * Constant for the PACKAGE_NAME column name
     */
    static constexpr auto PACKAGE_NAME = "package_name";

    /**
     * Constant for the NATIVE_LIB_DIR column name
     */
    static constexpr auto NATIVE_LIB_DIR = "native_lib_dir";

    /**
     * Constant for the SO_FILENAME column name
     */
    static constexpr auto SO_FILENAME = "so_filename";

    /**
     * Constant for the HAS_FUNCTIONS column name.
     * <p>
     * If this column contains true, you should check the /functions/ URI for that runtime.
     */
    static constexpr auto HAS_FUNCTIONS = "has_functions";
};
}  // namespace active_runtime

/**
 * Contains details for the /openxr/[major_ver]/abi/[abi]/runtimes/[package]/functions URI.
 * <p>
 * This URI is for package-specific function name remapping. Since this is an optional field in
 * the corresponding JSON manifests for OpenXR, it is optional here as well. If the active
 * runtime contains "true" in its "has_functions" column, then this table must exist and be
 * queryable.
 * <p>
 * No sort order is required to be honored by the content provider.
 */
namespace functions {
/**
 * Final path component to this URI.
 */
static constexpr auto TABLE_PATH = "functions";

/**
 * Create a content URI for querying all rows of the function remapping data for a given
 * runtime package and major version of OpenXR.
 *
 * @param systemBroker If the system runtime broker (instead of the installable one) should be queried.
 * @param majorVer    The major version of OpenXR.
 * @param packageName The package name of the runtime.
 * @param abi The Android ABI name in use.
 * @return A content URI for the entire table: the function remapping for that runtime.
 */
static Uri makeContentUri(bool systemBroker, int majorVersion, std::string const &packageName, const char *abi) {
    auto builder = Uri_Builder::construct();
    builder.scheme("content")
        .authority(getBrokerAuthority(systemBroker))
        .appendPath(BASE_PATH)
        .appendPath(std::to_string(majorVersion))
        .appendPath(ABI_PATH)
        .appendPath(abi)
        .appendPath(RUNTIMES_PATH)
        .appendPath(packageName)
        .appendPath(TABLE_PATH);
    return builder.build();
}

struct Columns : BaseColumns {
    /**
     * Constant for the FUNCTION_NAME column name
     */
    static constexpr auto FUNCTION_NAME = "function_name";

    /**
     * Constant for the SYMBOL_NAME column name
     */
    static constexpr auto SYMBOL_NAME = "symbol_name";
};
}  // namespace functions

}  // namespace

static inline jni::Array<std::string> makeArray(std::initializer_list<const char *> &&list) {
    auto ret = jni::Array<std::string>{(long)list.size()};
    long i = 0;
    for (auto &&elt : list) {
        ret.setElement(i, elt);
        ++i;
    }
    return ret;
}

#if defined(__arm__)
static constexpr auto ABI = "armeabi-v7l";
#elif defined(__aarch64__)
static constexpr auto ABI = "arm64-v8a";
#elif defined(__i386__)
static constexpr auto ABI = "x86";
#elif defined(__x86_64__)
static constexpr auto ABI = "x86_64";
#else
#error "Unknown ABI!"
#endif

/// Helper class to generate the jsoncpp object corresponding to a synthetic runtime manifest.
class JsonManifestBuilder {
   public:
    JsonManifestBuilder(const std::string &libraryPathParent, const std::string &libraryPath);
    JsonManifestBuilder &function(const std::string &functionName, const std::string &symbolName);

    Json::Value build() const { return root_node; }

   private:
    Json::Value root_node;
};

inline JsonManifestBuilder::JsonManifestBuilder(const std::string &libraryPathParent, const std::string &libraryPath)
    : root_node(Json::objectValue) {
    root_node["file_format_version"] = "1.0.0";
    root_node["instance_extensions"] = Json::Value(Json::arrayValue);
    root_node["functions"] = Json::Value(Json::objectValue);
    root_node[libraryPathParent] = Json::objectValue;
    root_node[libraryPathParent]["library_path"] = libraryPath;
}

inline JsonManifestBuilder &JsonManifestBuilder::function(const std::string &functionName, const std::string &symbolName) {
    root_node["functions"][functionName] = symbolName;
    return *this;
}

static constexpr const char *getBrokerTypeName(bool systemBroker) { return systemBroker ? "system" : "installable"; }

static int populateFunctions(wrap::android::content::Context const &context, bool systemBroker, const std::string &packageName,
                             JsonManifestBuilder &builder) {
    jni::Array<std::string> projection = makeArray({functions::Columns::FUNCTION_NAME, functions::Columns::SYMBOL_NAME});

    auto uri = functions::makeContentUri(systemBroker, XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), packageName, ABI);
    ALOGI("populateFunctions: Querying URI: %s", uri.toString().c_str());

    Cursor cursor = context.getContentResolver().query(uri, projection);

    if (cursor.isNull()) {
        ALOGE("Null cursor when querying content resolver for functions.");
        return -1;
    }
    if (cursor.getCount() < 1) {
        ALOGE("Non-null but empty cursor when querying content resolver for functions.");
        cursor.close();
        return -1;
    }
    auto functionIndex = cursor.getColumnIndex(functions::Columns::FUNCTION_NAME);
    auto symbolIndex = cursor.getColumnIndex(functions::Columns::SYMBOL_NAME);
    while (cursor.moveToNext()) {
        builder.function(cursor.getString(functionIndex), cursor.getString(symbolIndex));
    }

    cursor.close();
    return 0;
}

// The current file relies on android-jni-wrappers and jnipp, which may throw on failure.
// This is problematic when the loader is compiled with exception handling disabled - the consumers can reasonably
// expect that the compilation with -fno-exceptions will succeed, but the compiler will not accept the code that
// uses `try` & `catch` keywords. We cannot use the `exception_handling.hpp` here since we're not at an ABI boundary,
// so we define helper macros here. This is fine for now since the only occurrence of exception-handling code is in this file.
#ifdef XRLOADER_DISABLE_EXCEPTION_HANDLING

#define ANDROID_UTILITIES_TRY
#define ANDROID_UTILITIES_CATCH_FALLBACK(...)

#else

#define ANDROID_UTILITIES_TRY try
#define ANDROID_UTILITIES_CATCH_FALLBACK(...) \
    catch (const std::exception &e) {         \
        __VA_ARGS__                           \
    }

#endif  // XRLOADER_DISABLE_EXCEPTION_HANDLING

/// Get cursor for active runtime, parameterized by whether or not we use the system broker
static bool getActiveRuntimeCursor(wrap::android::content::Context const &context, jni::Array<std::string> const &projection,
                                   bool systemBroker, Cursor &cursor) {
    auto uri = active_runtime::makeContentUri(systemBroker, XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), ABI);
    ALOGI("getActiveRuntimeCursor: Querying URI: %s", uri.toString().c_str());

    ANDROID_UTILITIES_TRY { cursor = context.getContentResolver().query(uri, projection); }
    ANDROID_UTILITIES_CATCH_FALLBACK({
        ALOGW("Exception when querying %s content resolver: %s", getBrokerTypeName(systemBroker), e.what());
        cursor = {};
        return false;
    })

    if (cursor.isNull()) {
        ALOGW("Null cursor when querying %s content resolver.", getBrokerTypeName(systemBroker));
        cursor = {};
        return false;
    }
    if (cursor.getCount() < 1) {
        ALOGW("Non-null but empty cursor when querying %s content resolver.", getBrokerTypeName(systemBroker));
        cursor.close();
        cursor = {};
        return false;
    }
    return true;
}

#ifdef XR_PRIORITIZE_LOAD_RUNTIME_FROM_LOADER_LOCATION

#include <sys/system_properties.h>

    struct android_read_arg {
        char *chars;
        size_t char_count;
    };

    static void
    android_on_property_read(void *cookie, const char *name, const char *value, uint32_t serial) {
        struct android_read_arg *a = (struct android_read_arg *) cookie;

        snprintf(a->chars, a->char_count, "%s", value);
    }

    static const char *get_option_raw(char *chars, size_t char_count, const char *name) {
        const struct prop_info *pi = __system_property_find(name);
        if (pi == NULL) {
            return NULL;
        }

        struct android_read_arg a = {.chars = chars, .char_count = char_count};
        __system_property_read_callback(pi, &android_on_property_read, &a);

        return chars;
    }

    static long debug_string_to_num(const char *string, long _default) {

        if (string == NULL) {
            return _default;
        }

        char *endptr;
        long ret = strtol(string, &endptr, 0);

        // Restore the default value when no digits were found.
        if (string == endptr) {
            return _default;
        }

        return ret;
    }

    static long debug_get_num_option(const char *name, long _default) {
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

        char chars[1024];
        const char *raw = get_option_raw(chars, ARRAY_SIZE(chars), name);

        long ret = debug_string_to_num(raw, _default);

        return ret;
    }

    static bool isForceUseBroker() {
        static auto force = [] {
            return debug_get_num_option("debug.force.use.broker", 0) == 1;
        }();

        return force;
    }

    static std::string getSelfPath() {
        Dl_info info{};
        std::string path;
        if (dladdr((void *) &getSelfPath, &info)) {
            path = info.dli_fname;
        }
        return dirname(path.c_str());
    }

    static bool isNativeLibExist(std::string &libPath) {
        if (access(libPath.c_str(), F_OK) == 0) {
            return true;
        }

        // for uncompressed native libs /data/app/../../base.apk!/lib/ABI/libopenxr_monado.so
        void *handle = dlopen(libPath.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle != NULL) {
            dlclose(handle);
            return true;
        }
        return false;
    }

    // no functions
    static int getActiveRuntimeVirtualManifestFromSelf(Json::Value &virtualManifest) {
        // TODO Keep the same name as runtime
#ifdef XR_RUNTIME_FILE_NAME
        constexpr auto runtimeFileName = XR_RUNTIME_FILE_NAME;
#else
        constexpr auto runtimeFileName = "libopenxr_monado.so";
#endif

        auto libDir = getSelfPath();
        auto lib_path = libDir + "/" + runtimeFileName;

        if (isForceUseBroker()) {
            ALOGI("force use broker");
            return -1;
        }

        ALOGI("check path:%s", lib_path.c_str());
        if (!isNativeLibExist(lib_path)) {
            return -1;
        }


        JsonManifestBuilder builder{"runtime", lib_path};
        virtualManifest = builder.build();

        return 0;
    }
#endif

int getActiveRuntimeVirtualManifest(wrap::android::content::Context const &context, Json::Value &virtualManifest) {

#ifdef XR_PRIORITIZE_LOAD_RUNTIME_FROM_LOADER_LOCATION
    if (getActiveRuntimeVirtualManifestFromSelf(virtualManifest) == 0) {
        return 0;
    }
#endif

    jni::Array<std::string> projection = makeArray({active_runtime::Columns::PACKAGE_NAME, active_runtime::Columns::NATIVE_LIB_DIR,
                                                    active_runtime::Columns::SO_FILENAME, active_runtime::Columns::HAS_FUNCTIONS});

    // First, try getting the installable broker's provider
    bool systemBroker = false;
    Cursor cursor;
    if (!getActiveRuntimeCursor(context, projection, systemBroker, cursor)) {
        // OK, try the system broker as a fallback.
        systemBroker = true;
        getActiveRuntimeCursor(context, projection, systemBroker, cursor);
    }

    if (cursor.isNull()) {
        // Couldn't find either broker
        ALOGE("Could access neither the installable nor system runtime broker.");
        return -1;
    }

    cursor.moveToFirst();

    do {
        auto filename = cursor.getString(cursor.getColumnIndex(active_runtime::Columns::SO_FILENAME));
        auto libDir = cursor.getString(cursor.getColumnIndex(active_runtime::Columns::NATIVE_LIB_DIR));
        auto packageName = cursor.getString(cursor.getColumnIndex(active_runtime::Columns::PACKAGE_NAME));

        auto hasFunctions = cursor.getInt(cursor.getColumnIndex(active_runtime::Columns::HAS_FUNCTIONS)) == 1;
        ALOGI("Got runtime: package: %s, so filename: %s, native lib dir: %s, has functions: %s", packageName.c_str(),
              filename.c_str(), libDir.c_str(), (hasFunctions ? "yes" : "no"));

        auto lib_path = libDir + "/" + filename;
        auto *lib = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            // we found a runtime that we can dlopen, use it.
            dlclose(lib);

            JsonManifestBuilder builder{"runtime", lib_path};
            if (hasFunctions) {
                int result = populateFunctions(context, systemBroker, packageName, builder);
                if (result != 0) {
                    ALOGW("Unable to populate functions from runtime: %s, checking for more records...", lib_path.c_str());
                    continue;
                }
            }
            virtualManifest = builder.build();
            cursor.close();
            return 0;
        }
        // this runtime was not accessible, see if the broker has more runtimes on
        // offer.
        ALOGV("Unable to open broker provided runtime at %s, checking for more records...", lib_path.c_str());
    } while (cursor.moveToNext());

    ALOGE("Unable to open any of the broker provided runtimes.");
    cursor.close();
    return -1;
}
}  // namespace openxr_android

#endif  // __ANDROID__
