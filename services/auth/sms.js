'use strict';

function createAliyunSmsSender(env = process.env) {
  const accessKeyId = env.MAICHAT_SMS_ACCESS_KEY_ID || '';
  const accessKeySecret = env.MAICHAT_SMS_ACCESS_KEY_SECRET || '';
  const signName = env.MAICHAT_SMS_SIGN_NAME || '';
  const templateCode = env.MAICHAT_SMS_TEMPLATE_CODE || '';
  if (!accessKeyId || !accessKeySecret || !signName || !/^SMS_[0-9]+$/.test(templateCode))
    throw new Error('SMS credentials, sign, or template are missing');
  const Dysmsapi = require('@alicloud/dysmsapi20170525');
  const OpenApi = require('@alicloud/openapi-client');
  const client = new Dysmsapi.default(new OpenApi.Config({
    accessKeyId, accessKeySecret, endpoint: 'dysmsapi.aliyuncs.com'
  }));
  return {
    async send(phone, code) {
      // 号码和验证码只传阿里云接口，不记录请求体、供应商响应全文或登录密钥。
      const response = await client.sendSms(new Dysmsapi.SendSmsRequest({
        phoneNumbers: phone,
        signName,
        templateCode,
        templateParam: JSON.stringify({ code })
      }));
      if (response.body?.code !== 'OK' || !response.body?.bizId)
        throw new Error('SMS provider did not accept the message');
    }
  };
}

module.exports = { createAliyunSmsSender };
