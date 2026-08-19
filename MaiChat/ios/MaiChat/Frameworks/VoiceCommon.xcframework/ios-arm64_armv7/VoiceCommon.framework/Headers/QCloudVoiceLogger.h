//
//  CloudVoiceLogger.h
//  voice_common_ios
//
//  Created by sunnydu on 2025/4/29.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#define VOICE_LOG_DEBUG(format_string, ...) \
[QCloudVoiceLogger logDebug:[NSString stringWithFormat:format_string,##__VA_ARGS__]]

#define VOICE_LOG_INFO(format_string, ...) \
[QCloudVoiceLogger logInfo:[NSString stringWithFormat:format_string,##__VA_ARGS__]]

#define VOICE_LOG_WARN(format_string, ...) \
[QCloudVoiceLogger logWarnning:[NSString stringWithFormat:format_string,##__VA_ARGS__]]

#define VOICE_LOG_ERROR(format_string, ...) \
[QCloudVoiceLogger logError:[NSString stringWithFormat:format_string,##__VA_ARGS__]]

/// YtSDKLoggerLevel
typedef NS_ENUM(NSInteger, VoiceLoggerLevel)
{
    /// ERROR 级别
    VOICE_SDK_ERROR_LEVEL = 0,
    /// WARN 级别
    VOICE_SDK_WARN_LEVEL,
    /// INFO 级别
    VOICE_SDK_INFO_LEVEL,
    /// DEBUG 基本
    VOICE_SDK_DEBUG_LEVEL
};

typedef void (^OnLoggerEventBlock)(VoiceLoggerLevel loggerLevel, NSString * _Nonnull logInfo);
@interface QCloudVoiceLogger : NSObject
+ (void)registerLoggerListener:(OnLoggerEventBlock _Nullable)listener withNativeLog:(BOOL)needNative;
+ (BOOL)needNativeLog;
+ (void)logDebug:(NSString* _Nonnull)message;
+ (void)logInfo:(NSString* _Nonnull)message;
+ (void)logWarnning:(NSString* _Nonnull)message;
+ (void)logError:(NSString* _Nonnull)message;
+ (void)needLogFile:(BOOL) needLogFile;
+ (void)setLoggerLevel:(VoiceLoggerLevel)level;
@end

NS_ASSUME_NONNULL_END
