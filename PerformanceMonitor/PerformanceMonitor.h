//
//  PerformanceMonitor.h
//  SuperApp
//
//  Created by qianjianeng on 15/11/12.
//  Copyright © 2015年 Tencent. All rights reserved.
//

#import <Foundation/Foundation.h>

@interface PerformanceMonitor : NSObject

+ (instancetype)sharedInstance;

- (void)startMonitor;
- (void)stopMonitor;

@end
