//
//  QCloudSentenceRecognizer.h
//  QCloudSDK
//
//  Created by Sword on 2019/2/27.
//  Copyright © 2019 Tencent. All rights reserved.
//


NS_ASSUME_NONNULL_BEGIN

@class QCloudSentenceRecognizer;

@protocol QCloudSentenceRecognizerDelegate <NSObject>

@required
/**
 * 一句话识别回调delegate
 * @param text 识别结果文本，error=nil此字段才存在值
 * @param error 错误信息，详细错误信息见error.domain和error.userInfo字段
 * @param resultData 识别原始数据
 */
- (void)oneSentenceRecognizerDidRecognize:(QCloudSentenceRecognizer *)recognizer text:(nullable NSString *)text error:(nullable NSError *)error resultData:(nullable NSDictionary *)resultData;

@optional
/**
 * 开始录音回调
 */
- (void)oneSentenceRecognizerDidStartRecord:(QCloudSentenceRecognizer *)recognizer error:(nullable NSError *)error;
/**
 * 结束录音回调, SDK通过此方法回调后内部开始上报语音数据进行识别
 */
- (void)oneSentenceRecognizerDidEndRecord:(QCloudSentenceRecognizer *)recognizer audioFilePath:(NSString *)audioFilePath;
/**
 * 录音音量实时回调用
 * @param recognizer 实时语音识别实例
 * @param volume 声音音量，取值范围（-40-0)
 */
- (void)oneSentenceRecognizerDidUpdateVolume:(QCloudSentenceRecognizer *)recognizer volume:(float)volume;
@end

@class QCloudSentenceRecognizeParams;


@class QCloudOneSentenceAudioRecorder;
@class QCloudOneSentenceConfig;

@interface QCloudSentenceRecognizer : NSObject



@property (nonatomic, strong, readonly) QCloudOneSentenceConfig *config;

/**
 * 初始化方法，使用内置录音器采集音频
 * @param config 配置参数，详见QCloudConfig定义
 */
- (instancetype)initWithConfig:(QCloudOneSentenceConfig *)config;
/**
 * 通过appId secretId secretKey初始化
 * @param appid     腾讯云appId        基本概念见https://cloud.tencent.com/document/product/441/6194
 * @param secretId  腾讯云secretId     基本概念见https://cloud.tencent.com/document/product/441/6194
 * @param secretKey 腾讯云secretKey    基本概念见https://cloud.tencent.com/document/product/441/6194
 */
- (instancetype)initWithAppId:(NSString *)appid secretId:(NSString *)secretId secretKey:(NSString *)secretKey;

/**
 * 通过appId secretId secretKey初始化, 临时鉴权
 * @param appid     腾讯云appId        基本概念见https://cloud.tencent.com/document/product/441/6194
 * @param secretId  腾讯云secretId     基本概念见https://cloud.tencent.com/document/product/441/6194
 * @param secretKey 腾讯云secretKey    基本概念见https://cloud.tencent.com/document/product/441/6194
 */
- (instancetype)initWithAppId:(NSString *)appid secretId:(NSString *)secretId secretKey:(NSString *)secretKey token:(NSString *)token;

- (void)validateConfig;


@property (nonatomic, weak) id<QCloudSentenceRecognizerDelegate> delegate;
@property (nonatomic, assign, readonly) float volume;
/**
 * 通过sdk内置录音器开始一句话识别，录音时长超过60秒或音频数据超过3m，服务器会返回识别失败
 * 开始录音后，可以直接调用stopRecognizeWithRecorder结束录音, SDK会回调识别结果
 * @param engSerViceType  引擎模型，内置录音器仅支持16k的模型，具体类型参数详见官网API文档
 * https://cloud.tencent.com/document/product/1093/35646
 */
- (void)startRecognizeWithRecorder:(NSString *)engSerViceType;
/**
 * 通过sdk内置录音器开始一句话识别，调用此方法主动停止录音后，SDK会自动上报录音语音数据进行识别
 */
- (void)stopRecognizeWithRecorder;


/**
 * 通过语音url进行一句话识别的快捷入口
 * @param url 资源url 如http://www.qq.music/hello.mp3
 * @param voiceFormat 语音数据格式，取值见kQCloudVoiceFormat定义
 * @param  engSerViceType  引擎模型，内置录音器仅支持16k的模型，具体类型参数详见官网API文档
 * https://cloud.tencent.com/document/product/1093/35646
 * @return YES 本地参数校验通过，成功发起请求 NO:参数校验不通过
 */
- (BOOL)recoginizeWithUrl:(NSString *)url voiceFormat:(NSString* )voiceFormat EngSerViceType:(NSString* )engSerViceType;
/**
 * 通过语音数据进行一句话识别的快捷入口
 * @param audioData 语音数据
 * @param voiceFormat 语音数据格式，取值见kQCloudVoiceFormat定义
 * @param  engSerViceType  引擎模型，内置录音器仅支持16k的模型，具体类型参数详见官网API文档
 * https://cloud.tencent.com/document/product/1093/35646
 * @return YES 本地参数校验通过，成功发起请求 NO:参数校验不通过
 */
- (BOOL)recoginizeWithData:(NSData *)audioData voiceFormat:(NSString*)voiceFormat EngSerViceType:(NSString* )engSerViceType;
/**
 * 获取通用参数，调用者只需关注需要修改的参数
 */
- (QCloudSentenceRecognizeParams *)defaultRecognitionParams;
/**
 * 通过QCloudOneSentenceRecognitionParams调用一句话识别, 调用[QCloudSentenceRecognizeParams defaultRequestParams]方法获取默认参数，然后根据需求设置参数
 * @return YES:本地参数校验通过，成功发起请求 NO:参数校验不通过
 */
- (BOOL)recognizeWithParams:(QCloudSentenceRecognizeParams *)params;

/// 设置自定义参数,可使用该方法控制请求时的参数
/// @param value nil时将删除参数,否则会在请求中添加参数
- (void)setApiParam:(NSString *_Nonnull)key value:(NSObject *_Nullable)value;

/**
 * 获取SDK版本号
 */
+ (NSString*) getVersion;
@end

NS_ASSUME_NONNULL_END
