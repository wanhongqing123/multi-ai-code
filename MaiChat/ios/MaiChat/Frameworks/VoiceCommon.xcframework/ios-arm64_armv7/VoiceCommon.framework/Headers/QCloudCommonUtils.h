//
//  QCloudCommonUtils.h
//  voice_common_ios
//
//  Created by sunnydu on 2025/4/29.
//
#ifndef QCloudCommonUtils_h
#define QCloudCommonUtils_h

#pragma once
#ifndef WeakRef
#define WeakRef(weakVar, strongVar) __weak __typeof(&*strongVar) weakVar = strongVar
#endif
#ifndef WeakSelf
#define WeakSelf() WeakRef(weakSelf, self)
#endif
#ifndef StrongRef
#define StrongRef(strongVar, weakVar) __strong __typeof(&*weakVar) strongVar = weakVar
#endif
#ifndef StrongSelf
#define StrongSelf() StrongRef(strongSelf,weakSelf)
#endif

#endif /* QCloudCommonUtils_h */
