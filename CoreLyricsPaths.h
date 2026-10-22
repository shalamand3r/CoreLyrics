#pragma once
#import <Foundation/Foundation.h>

@protocol CoreLyricsApplicationProxy <NSObject>
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (NSURL *)dataContainerURL;
- (BOOL)isInstalled;
@end

static inline NSString *DLCMusicCacheDirectory(Class proxyClass) {
    SEL selector = @selector(applicationProxyForIdentifier:);
    if (![proxyClass respondsToSelector:selector]) return nil;
    id<CoreLyricsApplicationProxy> proxy = [(id)proxyClass applicationProxyForIdentifier:@"com.apple.Music"];
    if ([proxy respondsToSelector:@selector(isInstalled)] && ![proxy isInstalled]) return nil;
    NSURL *container = [proxy respondsToSelector:@selector(dataContainerURL)] ? [proxy dataContainerURL] : nil;
    if (![container isKindOfClass:NSURL.class] || !container.isFileURL || !container.path.length) return nil;
    return [container.path stringByAppendingPathComponent:@"Library/CoreLyrics"];
}

static inline NSString *DLCLocalCacheDirectory(void) {
    NSString *library = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    return [library stringByAppendingPathComponent:@"CoreLyrics"];
}
