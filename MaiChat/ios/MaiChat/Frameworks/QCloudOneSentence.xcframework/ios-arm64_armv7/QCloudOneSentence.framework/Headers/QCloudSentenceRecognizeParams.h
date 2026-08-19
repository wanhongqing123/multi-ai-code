//
//  QCloudOneSentenceRecognitionParameters.h
//  QCloudSDK
//
//  Created by Sword on 2019/2/26.
//  Copyright © 2019 Tencent. All rights reserved.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN


typedef NS_ENUM(NSInteger, QCloudAudioSourceType) {
    QCloudAudioSourceTypeNone = -1,          //未知
    QCloudAudioSourceTypeUrl = 0,            //语音url
    QCloudAudioSourceTypeAudioData,          //语音数据
};


@interface QCloudSentenceRecognizeParams : NSObject


@property(nonatomic, strong) NSString *appid;
@property(nonatomic, assign) NSInteger projectId;   //optional 腾讯云项目 ID，可填 0，总长度不超过 1024 字节。
@property(nonatomic, strong) NSString *action;
@property(nonatomic, strong) NSString *region;
@property(nonatomic, assign) NSInteger timestamp;
@property(nonatomic, assign) NSInteger nonce;
@property(nonatomic, strong) NSString *secretId;
@property(nonatomic, strong) NSString *secretKey;
@property(nonatomic, strong) NSString *token; //临时鉴权token
@property(nonatomic, strong) NSString *signature;
@property(nonatomic, strong) NSString *version;
@property(nonatomic, strong) NSString *signatureMethod;


@property(nonatomic, copy) NSString *engSerViceType;     //引擎类型。8k_zh：电话 8k 通用模型；16k_zh：16k 中文通用模型。只支持单声道音频识别。更多的引擎见API文档 https://cloud.tencent.com/document/product/1093/35646
@property(nonatomic, copy) NSString *voiceFormat; //识别音频的音频格式，支持格式以API文档为准，后端会持续增加更多格式类型 https://cloud.tencent.com/document/product/1093/35646
@property(nonatomic, assign) QCloudAudioSourceType sourceType;  //语音数据来源。0：语音 URL；1：语音数据（post body）。

@property(nonatomic, strong) NSString *url; //语音URL，公网可下载。当 SourceType 值为 QCloudAudioSourceTypeUrl 时须填写该字段，为 QCloudAudioSourceTypeAudioData 时不填；URL 的长度小于2048字符，音频时间长度要小于60s
@property(nonatomic, strong) NSData *data; //语音数据，语音数据格式必须与voiceFormat匹配。当SourceType为QCloudAudioSourceTypeAudioData时必须填写，为QCloudAudioSourceTypeUrl可不写。

@property(nonatomic, strong) NSString *usrAudioKey;                    //用户端对此任务的唯一标识，用户自助生成，用于用户查找识别结果
@property(nonatomic, assign) NSInteger subServiceType;                 //子服务类型。2:一句话识别，固定值。

@property(nonatomic, assign) NSInteger filterDirty;  //是否过滤脏词（目前支持中文普通话引擎）。0：不过滤脏词；1：过滤脏词；2：将脏词替换为 * 。默认值为 0。
@property(nonatomic, assign) NSInteger filterModal; //是否过语气词（目前支持中文普通话引擎）。0：不过滤语气词；1：部分过滤；2：严格过滤 。默认值为 0。
@property(nonatomic, assign) NSInteger filterPunc; //是否过滤标点符号（目前支持中文普通话引擎）。 0：不过滤，1：过滤句末标点，2：过滤所有标点。默认值为 0。
@property(nonatomic, assign) NSInteger convertNumMode; //是否进行阿拉伯数字智能转换。0：不转换，直接输出中文数字，1：根据场景智能转换为阿拉伯数字。默认值为1。
@property(nonatomic, strong) NSString *hotwordId;    //热词id.
@property(nonatomic, assign) NSInteger wordInfo;   //是否显示词级别时间戳。0：不显示；1：显示，不包含标点时间戳，2：显示，包含标点时间戳。默认值为 0。
@property(nonatomic, assign) NSInteger reinforceHotword  __attribute__((deprecated("该属性即将过期，可通过在控制台配置热词列表里的热词权重为100，并将热词列表ID通过hotwordID属性传入SDK，实现增强热词的功能"))); // 热词增强功能。1:开启后（仅支持8k_zh,16k_zh），将开启同音替换功能，同音字、词在热词中配置。举例：热词配置“蜜制”并开启增强功能后，与“蜜制”同拼音（mizhi）的“秘制”、“蜜汁”的识别结果会被强制替换成“蜜制”。因此建议客户根据自己的实际情况开启该功能。


+ (instancetype)defaultRequestParams;

/**
 V3鉴权通用参数
 @return V3通用参数
 */
- (NSDictionary *)commonParamsForV3Authentication;

/**
 V1鉴权通用参数

 @return V1通用参数
 */
- (NSDictionary *)commonParamsForV1Authentication;


/**
 是否使用V3接口鉴权，默认NO

 @return 返回一个Bool值表示是否使用V3鉴权
 */
- (BOOL)usingV3Authentication;

@end

NS_ASSUME_NONNULL_END
