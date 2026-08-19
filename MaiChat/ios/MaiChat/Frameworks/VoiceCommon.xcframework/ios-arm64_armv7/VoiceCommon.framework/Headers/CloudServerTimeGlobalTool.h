//
//  CloudServerTimeGlobleTool.h
//  voice_common_ios
//
//  Created by sunnydu on 2025/4/29.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface CloudServerTimeGlobalTool : NSObject
+(CloudServerTimeGlobalTool *)getInstance;

@property (nonatomic, assign) double serverTimeDifference;
@property (nonatomic, assign) NSInteger requestTimeoutInterval;//配置全局网络超时时间，默认20s
- (void)calibrationServerTime;
@end

NS_ASSUME_NONNULL_END
