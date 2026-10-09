#include "update_platform.hpp"
#include <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace seed::editor {
namespace fs = std::filesystem;
namespace {
constexpr NSTimeInterval idle_timeout = 30;       // Seconds without data before a request fails.
constexpr NSTimeInterval download_timeout = 3600; // A whole download.

NSString* text(const std::string& value) {
    return [NSString stringWithUTF8String:value.c_str()];
}

std::string describe(NSError* error) {
    return error.localizedDescription ? std::string(error.localizedDescription.UTF8String) : "unknown error";
}

// A session that neither caches nor keeps cookies, so every check sees the current feed.
NSURLSession* fresh_session() {
    NSURLSessionConfiguration* configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.URLCache = nil;
    configuration.timeoutIntervalForRequest = idle_timeout;
    configuration.timeoutIntervalForResource = download_timeout;
    return [NSURLSession sessionWithConfiguration:configuration];
}

NSURL* parse_url(const std::string& url) {
    NSURL* parsed = [NSURL URLWithString:text(url)];
    if (!parsed || !parsed.scheme) throw std::runtime_error("Not a URL: " + url);
    return parsed;
}

// Waits for a task, cancelling it when `cancel` becomes true.
void await_task(NSURLSessionTask* task, dispatch_semaphore_t done, const std::atomic<bool>& cancel) {
    [task resume];
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0)
        if (cancel) {
            [task cancel];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            throw std::runtime_error("Cancelled");
        }
}

// HTTP must answer 200; file URLs (used by tests and local feeds) have no status.
void check_response(NSURLResponse* response, NSError* error, const std::string& url) {
    if (error) throw std::runtime_error("Could not fetch " + url + ": " + describe(error));
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
        const auto status = static_cast<NSHTTPURLResponse*>(response).statusCode;
        if (status != 200)
            throw std::runtime_error("Could not fetch " + url + ": the server answered " + std::to_string(status));
    }
}
} // namespace

bool updates_supported() {
    return true;
}

fs::path running_bundle() {
    @autoreleasepool {
        NSBundle* bundle = NSBundle.mainBundle;
        if (![bundle.bundleURL.pathExtension isEqualToString:@"app"]) return {};
        return fs::path(bundle.bundleURL.fileSystemRepresentation);
    }
}

BundleInfo bundle_info(const fs::path& app) {
    @autoreleasepool {
        const auto plist = app / "Contents" / "Info.plist";
        NSDictionary* info = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:text(plist.string())]
                                                                 error:nil];
        id identifier = info[@"CFBundleIdentifier"], version = info[@"CFBundleShortVersionString"];
        if (![identifier isKindOfClass:[NSString class]] || ![version isKindOfClass:[NSString class]])
            throw std::runtime_error("Cannot read the bundle identifier and version in " + plist.string());
        return {static_cast<NSString*>(identifier).UTF8String, static_cast<NSString*>(version).UTF8String};
    }
}

std::string fetch_text(const std::string& url, std::size_t limit, const std::atomic<bool>& cancel) {
    @autoreleasepool {
        NSURLSession* session = fresh_session();
        __block NSData* body = nil;
        __block NSURLResponse* answer = nil;
        __block NSError* failure = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task =
            [session dataTaskWithURL:parse_url(url)
                   completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                     body = data;
                     answer = response;
                     failure = error;
                     dispatch_semaphore_signal(done);
                   }];
        try {
            await_task(task, done, cancel);
        } catch (...) {
            [session invalidateAndCancel];
            throw;
        }
        [session finishTasksAndInvalidate];
        check_response(answer, failure, url);
        if (body.length > limit)
            throw std::runtime_error(url + " is over " + std::to_string(limit / 1024) + " KB");
        return std::string(static_cast<const char*>(body.bytes), body.length);
    }
}

void fetch_file(const std::string& url, const fs::path& to, std::uint64_t limit, const std::atomic<bool>& cancel) {
    @autoreleasepool {
        NSURLSession* session = fresh_session();
        __block NSURLResponse* answer = nil;
        __block NSError* failure = nil;
        __block std::string moved; // Why the downloaded file could not be kept, if it could not.
        const std::string target = to.string();
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        // The session deletes its temporary file when the handler returns, so it is moved here.
        NSURLSessionDownloadTask* task = [session
            downloadTaskWithURL:parse_url(url)
              completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error) {
                answer = response;
                failure = error;
                if (location && !error) {
                    NSError* move_error = nil;
                    NSURL* destination = [NSURL fileURLWithPath:text(target)];
                    [NSFileManager.defaultManager removeItemAtURL:destination error:nil];
                    if (![NSFileManager.defaultManager moveItemAtURL:location toURL:destination error:&move_error])
                        moved = describe(move_error);
                }
                dispatch_semaphore_signal(done);
              }];
        try {
            await_task(task, done, cancel);
        } catch (...) {
            [session invalidateAndCancel];
            throw;
        }
        [session finishTasksAndInvalidate];
        check_response(answer, failure, url);
        if (!moved.empty()) throw std::runtime_error("Could not keep the download: " + moved);
        if (fs::file_size(to) > limit) throw std::runtime_error(url + " is larger than expected");
    }
}

bool verify_signature(std::span<const std::uint8_t> public_key, std::string_view message,
                      std::span<const std::uint8_t> signature) {
    @autoreleasepool {
        if (public_key.size() != 65 || public_key[0] != 0x04) return false;
        NSDictionary* attributes = @{
            (id)kSecAttrKeyType : (id)kSecAttrKeyTypeECSECPrimeRandom,
            (id)kSecAttrKeyClass : (id)kSecAttrKeyClassPublic,
            (id)kSecAttrKeySizeInBits : @256,
        };
        NSData* key_data = [NSData dataWithBytes:public_key.data() length:public_key.size()];
        SecKeyRef key = SecKeyCreateWithData((__bridge CFDataRef)key_data, (__bridge CFDictionaryRef)attributes, nil);
        if (!key) return false;
        NSData* message_data = [NSData dataWithBytes:message.data() length:message.size()];
        NSData* signature_data = [NSData dataWithBytes:signature.data() length:signature.size()];
        const bool valid = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                                                 (__bridge CFDataRef)message_data,
                                                 (__bridge CFDataRef)signature_data, nil);
        CFRelease(key);
        return valid;
    }
}

std::string sha256_file(const fs::path& file) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> in(std::fopen(file.c_str(), "rb"), std::fclose);
    if (!in) throw std::runtime_error("Cannot read " + file.string());
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<unsigned char> buffer(1 << 20);
    while (const auto read = std::fread(buffer.data(), 1, buffer.size(), in.get()))
        CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(read));
    if (std::ferror(in.get())) throw std::runtime_error("Cannot read " + file.string());
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}
} // namespace seed::editor
