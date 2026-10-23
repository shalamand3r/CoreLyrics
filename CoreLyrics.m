#import "CoreLyrics.h"
#import "LyricsPreferences.h"
#import "ProviderModels.h"
#import "LyricsText.h"
#import "DateLyricsJSON.h"
#import "DateLyricsProviderOrder.h"
#import "DateLyricsFetchSettings.h"
#import "DateLyricsProviderTiming.h"
#import "CoreLyricsPaths.h"
#import "CoreLyricsDiagnostics.h"
#import <CommonCrypto/CommonDigest.h>
#import <QuartzCore/QuartzCore.h>
#import <ctype.h>
#import <float.h>
#import <sys/file.h>
#import <fcntl.h>
#import <unistd.h>
#import <sys/resource.h>

#define DateLyricsMusixmatchTask DLCoreProviderTask
#define DateLyricsTimedLine DLCoreLine
#define DateLyricsTimedWord DLCoreWord
#define DateLyricsDebugLog(...) do { if (gDateLyricsDebugLogging) DLCLog(__VA_ARGS__); } while (0)

@interface DLLyricsResult ()
@property (nonatomic, copy) NSArray<NSDictionary *> *lines;
@property (nonatomic, copy) NSString *provider;
@property (nonatomic) BOOL hasWordTiming;
@property (nonatomic) BOOL relaxedMatch;
@property (nonatomic) BOOL savedOffline;
@end
@interface DLLyricsSubscription ()
@property (nonatomic, copy) dispatch_block_t cancellation;
@end
@class DLCSharedLease;
@interface DLCoreLookup : NSObject
@property (nonatomic, strong) DLCSharedLease *sharedLease;
@property (nonatomic, copy) NSString *cacheEpoch;
@property (nonatomic, strong) DLLyricsTrack *track;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, DLCoreProviderTask *> *tasks;
@property (nonatomic, copy) NSArray<NSString *> *providers;
@property (nonatomic) NSUInteger nextProvider;
@property (nonatomic) NSTimeInterval startedAt;
@property (nonatomic) double startedCPU;
@property (nonatomic) BOOL expandedWindow;
@property (nonatomic) NSUInteger requestCount;
@property (nonatomic) BOOL hasPublishedLines;
@property (nonatomic) BOOL waitingForRetry;
@property (nonatomic) NSUInteger retryRound;
@property (nonatomic, strong) NSMutableSet<NSString *> *retryProviders;
@property (nonatomic) NSUInteger hedgeGeneration;
@property (nonatomic) BOOL hadTransportFailure;
@property (nonatomic, copy) NSString *failureReason;
@property (nonatomic, strong) DLLyricsResult *result;
@property (nonatomic, strong) NSMutableDictionary<NSString *, DLLyricsUpdate> *subscribers;
@end
@implementation DLCoreLookup
@end

static dispatch_queue_t gLyricsQueue;
static NSMutableDictionary<NSString *, DLCoreLookup *> *gLookups;
static NSMutableDictionary<NSNumber *, DLCoreLookup *> *gLookupIDs;
static NSMutableDictionary<NSString *, NSNumber *> *gRetryAfter;
static NSMutableDictionary<NSNumber *, NSString *> *gMusixmatchPendingTaskIDs;
static NSMutableArray<DLCoreProviderTask *> *gMusixmatchTokenWaiters;
static NSMutableDictionary<NSString *, NSURLSessionDataTask *> *gDateLyricsProviderRequests;
static NSURLSession *gDateLyricsProviderSession;
static BOOL gMusixmatchTokenRequestInFlight, gMusixmatchTokenCacheLoaded;
static NSString *gMusixmatchUserToken, *gDateLyricsMusixmatchCustomUserToken;
static NSTimeInterval gMusixmatchTokenFetchedAt;
static NSUInteger gDateLyricsMusixmatchGeneration, gTokenGeneration;
static NSInteger gNextLookupID;
static BOOL gDateLyricsDebugLogging = NO;
static BOOL gPerformanceLogging;
static BOOL gDateLyricsMusixmatchEnabled = YES, gDateLyricsWordHighlighting = YES;
static BOOL gDateLyricsRelaxedMatching;
static NSInteger gDateLyricsRequestTimeout = DateLyricsDefaultRequestTimeout;
static NSArray<NSString *> *gDateLyricsProviderOrder;
static NSDictionary *gSavedProviderPreferences;

static NSString *DLCCacheRoot(void);
static NSData *DLCReadTokenCache(void);
static void DLCLog(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void DLCPerformance(NSString *event, DLCoreLookup *lookup);
static void DLCPruneCache(void);
static BOOL DLCPublishLines(NSInteger lookupID, NSArray<DLCoreLine *> *lines, BOOL relaxed);
static void DLCFinishTask(DLCoreProviderTask *task, NSString *reason);
static void DLCCompleteLookup(DLCoreLookup *lookup, NSString *reason);
static void DLCFillProviders(DLCoreLookup *lookup);
static void DLCArmHedge(DLCoreLookup *lookup);
static void DLCCancelLookup(DLCoreLookup *lookup);
static NSArray *DLCFetchConfiguration(void);
static void DateLyricsAssignCharacterRanges(DLCoreLine *line) { (void)line; }
static BOOL DateLyricsMusixmatchServiceEnabled(void) { return [gDateLyricsProviderOrder containsObject:@"musixmatch"]; }
static NSArray *DateLyricsActiveProviders(void) { return gDateLyricsMusixmatchEnabled ? gDateLyricsProviderOrder : @[]; }
#include "ProviderText.inc"
#include "ProviderHealth.inc"
#include "ProviderPipeline.inc"

@implementation DLLyricsTrack
+ (instancetype)trackWithTitle:(NSString *)title artist:(NSString *)artist duration:(NSTimeInterval)duration {
    DLLyricsTrack *track = [self new];
    track.title = title ?: @""; track.artist = artist ?: @""; track.album = @"";
    track.duration = isfinite(duration) && duration > 0 ? duration : 0;
    return track;
}
- (id)copyWithZone:(NSZone *)zone {
    DLLyricsTrack *track = [self.class trackWithTitle:self.title artist:self.artist duration:self.duration];
    track.album = self.album; track.catalogID = self.catalogID;
    track.hasNativeLineTiming = self.hasNativeLineTiming;
    return track;
}
- (NSString *)cacheKey {
    NSData *data = [NSJSONSerialization dataWithJSONObject:@[
        DateLyricsMusixmatchNormalizedString(self.title), DateLyricsMusixmatchNormalizedString(self.artist),
        @(isfinite(self.duration) && self.duration > 0 ? round(self.duration) : 0)] options:0 error:nil];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *key = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [key appendFormat:@"%02x", digest[i]];
    return key;
}
@end

static NSString *DLCXMLText(NSString *text) {
    NSMutableString *safe = [NSMutableString stringWithCapacity:text.length];
    for (NSUInteger index = 0; index < text.length; index++) {
        unichar character = [text characterAtIndex:index];
        if ((character < 0x20 && character != '\t' && character != '\n' && character != '\r') || character >= 0xfffe) continue;
        [safe appendFormat:@"%C", character];
    }
    text = safe;
    for (NSArray *pair in @[@[@"&", @"&amp;"], @[@"<", @"&lt;"], @[@">", @"&gt;"], @[@"\"", @"&quot;"], @[@"'", @"&apos;"]])
        text = [text stringByReplacingOccurrencesOfString:pair[0] withString:pair[1]];
    return text ?: @"";
}
@implementation DLLyricsResult
- (NSData *)TTMLData {
    double duration = 0;
    for (NSDictionary *line in self.lines) duration = MAX(duration, [line[@"end"] doubleValue]);
    NSMutableString *xml = [NSMutableString stringWithFormat:@"<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:itunes=\"http://music.apple.com/lyric-ttml-internal\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" xml:lang=\"und\" itunes:timing=\"%@\"><head><metadata><ttm:agent type=\"person\" xml:id=\"v1\"/></metadata></head><body dur=\"%.3f\"><div begin=\"0.000\" end=\"%.3f\">", self.hasWordTiming ? @"Word" : @"Line", duration, duration];
    NSUInteger index = 0;
    for (NSDictionary *line in self.lines) {
        [xml appendFormat:@"<p begin=\"%.3f\" end=\"%.3f\" itunes:key=\"L%lu\" ttm:agent=\"v1\">", [line[@"begin"] doubleValue], [line[@"end"] doubleValue], (unsigned long)++index];
        NSArray *words = line[@"words"];
        if (!words.count) [xml appendString:DLCXMLText(line[@"text"])];
        BOOL background = NO;
        for (NSDictionary *word in words) {
            BOOL nextBackground = [DateLyricsJSONNumber(word[@"bg"]) boolValue];
            if (background != nextBackground) {
                [xml appendString:background ? @"</span>" : @"<span ttm:role=\"x-bg\">"];
                background = nextBackground;
            }
            [xml appendString:DLCXMLText(word[@"s"] ?: @"")];
            [xml appendFormat:@"<span begin=\"%.3f\" end=\"%.3f\">%@</span>", [word[@"b"] doubleValue], [word[@"e"] doubleValue], DLCXMLText(word[@"t"])];
        }
        if (background) [xml appendString:@"</span>"];
        [xml appendString:@"</p>"];
    }
    [xml appendString:@"</div></body></tt>"];
    return [xml dataUsingEncoding:NSUTF8StringEncoding];
}
@end
@implementation DLLyricsSubscription
- (void)cancel {
    dispatch_block_t cancellation;
    @synchronized (self) { cancellation = self.cancellation; self.cancellation = nil; }
    if (cancellation) cancellation();
}
- (void)dealloc { [self cancel]; }
@end

static NSString *DLCCacheRoot(void) {
#ifdef CORELYRICS_TESTING
    NSString *test = NSProcessInfo.processInfo.environment[@"CORELYRICS_TEST_CACHE"];
    NSCAssert(test.length, @"Test cache directory is required");
    return test;
#else
    NSString *music = DLCMusicCacheDirectory(NSClassFromString(@"LSApplicationProxy"));
    NSString *library = music.length ? music.stringByDeletingLastPathComponent : NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    return [library stringByAppendingPathComponent:@"CoreLyrics"];
#endif
}
static NSString *DLCCachePath(DLLyricsTrack *track) { return [DLCCacheRoot() stringByAppendingPathComponent:[track.cacheKey stringByAppendingString:@".json"]]; }
static NSData *DLCReadTokenCache(void) {
    NSString *music = DLCMusicCacheDirectory(NSClassFromString(@"LSApplicationProxy"));
    NSMutableArray *roots = [NSMutableArray arrayWithArray:@[DLCCacheRoot(),
        @"/var/mobile/Library/CoreLyrics", @"/var/jb/var/mobile/Library/CoreLyrics",
        @"/var/mobile/Library/DateLyrics", @"/var/jb/var/mobile/Library/DateLyrics",
        @"/var/mobile/Library/DateLyrics/External"]];
    if (music.length) [roots addObject:[music.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"DateLyrics"]];
    for (NSString *root in roots) {
        if (!root.length) continue;
        NSString *path = [root stringByAppendingPathComponent:@"musixmatch-token.json"];
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
        if (![attributes.fileType isEqualToString:NSFileTypeRegular] || attributes.fileSize > 8192) continue;
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (data.length) return data;
    }
    return nil;
}
static void DLCLog(NSString *format, ...) {
    va_list arguments; va_start(arguments, format);
    NSString *message = [[NSString alloc] initWithFormat:format arguments:arguments]; va_end(arguments);
    CLDiagnosticAppend(DLCCacheRoot(), @"CoreLyrics", NO, message);
}
static double DLCProcessCPU(void) {
    struct rusage usage; if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec/1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec/1e6;
}
static void DLCPerformance(NSString *event, DLCoreLookup *lookup) {
    if (!gPerformanceLogging) return;
    CLDiagnosticAppend(DLCCacheRoot(), @"CoreLyrics", YES,
        [NSString stringWithFormat:@"event=%@ elapsed=%.3f activeProviders=%lu subscribers=%lu lines=%lu words=%d requests=%lu retries=%lu processCpuDelta=%.6f", event,
            lookup.startedAt > 0 ? CACurrentMediaTime() - lookup.startedAt : 0,
            (unsigned long)lookup.tasks.count, (unsigned long)lookup.subscribers.count,
            (unsigned long)lookup.result.lines.count, lookup.result.hasWordTiming, (unsigned long)lookup.requestCount, (unsigned long)lookup.retryRound, lookup.startedCPU > 0 ? MAX(0, DLCProcessCPU()-lookup.startedCPU) : 0]);
}
static NSArray *DLCValidatedLines(id raw) {
    if (![raw isKindOfClass:NSArray.class] || ![raw count] || [raw count] > 4000) return nil;
    NSMutableArray *lines = [NSMutableArray array];
    for (id value in raw) {
        NSDictionary *line = DateLyricsJSONDictionary(value);
        double begin = DateLyricsJSONDouble(line[@"begin"], -1), end = DateLyricsJSONDouble(line[@"end"], -1);
        NSString *text = DateLyricsJSONString(line[@"text"]);
        if (!text.length || text.length > 8192 || begin < 0 || end <= begin) return nil;
        NSMutableDictionary *valid = [@{@"begin":@(begin), @"end":@(end), @"text":text} mutableCopy];
        id rawWords = line[@"words"];
        if (rawWords && ![rawWords isKindOfClass:NSArray.class]) return nil;
        NSMutableArray *words = [NSMutableArray array]; NSMutableString *assembled = [NSMutableString string];
        if ([rawWords count] > 4000) return nil;
        for (id item in rawWords) {
            NSDictionary *word = DateLyricsJSONDictionary(item);
            NSString *fragment = DateLyricsJSONString(word[@"t"]), *separator = DateLyricsJSONString(word[@"s"]) ?: @"";
            double b = DateLyricsJSONDouble(word[@"b"], -1), e = DateLyricsJSONDouble(word[@"e"], -1);
            if (!fragment.length || b < begin || e <= b || e > end) return nil;
            [assembled appendString:separator]; [assembled appendString:fragment];
            [words addObject:@{@"b":@(b), @"e":@(e), @"t":fragment, @"s":separator, @"bg":@([DateLyricsJSONNumber(word[@"bg"]) boolValue])}];
        }
        if (words.count) {
            if (![assembled isEqualToString:text]) return nil;
            valid[@"words"] = words;
        }
        [lines addObject:valid];
    }
    return lines;
}
static DLLyricsResult *DLCReadCache(DLLyricsTrack *track) {
    NSString *path = DLCCachePath(track);
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (![attributes.fileType isEqualToString:NSFileTypeRegular] || attributes.fileSize > 4 * 1024 * 1024) return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    NSDictionary *json = data ? DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:data options:0 error:nil]) : nil;
    if (![DateLyricsJSONString(json[@"key"]) isEqualToString:track.cacheKey] || ![json[@"schema"] isEqual:@1]) return nil;
    NSArray *lines = DLCValidatedLines(json[@"lines"]);
    if (!lines.count || (!gDateLyricsRelaxedMatching && [DateLyricsJSONNumber(json[@"relaxed"]) boolValue])) return nil;
    DLLyricsResult *result = [DLLyricsResult new]; result.lines = lines;
    result.provider = DateLyricsJSONString(json[@"provider"]) ?: @"";
    result.relaxedMatch = [DateLyricsJSONNumber(json[@"relaxed"]) boolValue];
    result.savedOffline = [DateLyricsJSONNumber(json[@"saved"]) boolValue];
    for (NSDictionary *line in lines) result.hasWordTiming |= [line[@"words"] count] > 0;
    return result;
}
static int DLCLockCache(NSString *root) {
    if (![NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]) return -1;
    int lock = open([root stringByAppendingPathComponent:@".write-lock"].fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return -1;
    if (flock(lock, LOCK_EX) != 0) { close(lock); return -1; }
    return lock;
}
static void DLCUnlockCache(int lock) { flock(lock, LOCK_UN); close(lock); }
static NSString *DLCCacheEpoch(void) {
    return [NSString stringWithContentsOfFile:[DLCCacheRoot() stringByAppendingPathComponent:@".fetch-generation"] encoding:NSUTF8StringEncoding error:nil] ?: @"0";
}
static void DLCAdvanceCacheEpoch(void) {
    [NSUUID.UUID.UUIDString writeToFile:[DLCCacheRoot() stringByAppendingPathComponent:@".fetch-generation"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
static BOOL DLCWriteCacheForEpoch(DLLyricsTrack *track, DLLyricsResult *result, BOOL preserveSaved, NSString *epoch) {
    int lock = DLCLockCache(DLCCacheRoot());
    if (lock < 0) return NO;
    if (epoch && ![epoch isEqual:DLCCacheEpoch()]) { DLCUnlockCache(lock); return NO; }
    DLLyricsResult *existing = DLCReadCache(track);
    if (preserveSaved && existing.savedOffline) result.savedOffline = YES;
    if (existing.hasWordTiming && !result.hasWordTiming) {
        result.lines = existing.lines; result.provider = existing.provider;
        result.hasWordTiming = YES; result.relaxedMatch = existing.relaxedMatch;
    }
    NSDictionary *json = @{@"schema":@1, @"key":track.cacheKey, @"title":track.title, @"artist":track.artist,
        @"duration":@(track.duration), @"provider":result.provider, @"relaxed":@(result.relaxedMatch),
        @"saved":@(result.savedOffline), @"lines":result.lines};
    NSData *data = [NSJSONSerialization dataWithJSONObject:json options:0 error:nil];
    BOOL written = data.length <= 4 * 1024 * 1024 && [data writeToFile:DLCCachePath(track) options:NSDataWritingAtomic error:nil];
    DLCUnlockCache(lock);
    return written;
}
static BOOL DLCWriteCache(DLLyricsTrack *track, DLLyricsResult *result, BOOL preserveSaved) {
    return DLCWriteCacheForEpoch(track, result, preserveSaved, nil);
}
static void DLCNotify(DLCoreLookup *lookup, BOOL finished, NSError *error) {
    DLLyricsResult *result = lookup.result;
    for (DLLyricsUpdate update in lookup.subscribers.allValues)
        dispatch_async(dispatch_get_main_queue(), ^{ update(result, finished, error); });
}
#include "SharedLookup.inc"
static BOOL DLCPublishLines(NSInteger lookupID, NSArray<DLCoreLine *> *lines, BOOL relaxed) {
    DLCoreLookup *lookup = gLookupIDs[@(lookupID)];
    if (!lookup || !DLCSharedHasConsumers(lookup)) return NO;
    if (![lookup.cacheEpoch isEqual:DLCCacheEpoch()]) { DLCSharedAbort(lookup); return NO; }
    NSMutableArray *serialized = [NSMutableArray array];
    for (DLCoreLine *line in lines) {
        NSMutableDictionary *item = [@{@"begin":@(line.begin), @"end":@(line.end), @"text":line.text ?: @""} mutableCopy];
        NSMutableArray *words = [NSMutableArray array];
        for (DLCoreWord *word in line.words)
            [words addObject:@{@"b":@(word.begin), @"e":@(word.end), @"t":word.text ?: @"", @"s":word.separatorBefore ?: @"", @"bg":@(word.isBackground)}];
        if (words.count) item[@"words"] = words;
        [serialized addObject:item];
    }
    NSArray *valid = DLCValidatedLines(serialized);
    if (!valid.count) return NO;
    DLLyricsResult *result = [DLLyricsResult new]; result.lines = valid; result.provider = lookup.tasks[@(lookupID)].provider ?: @"";
    result.relaxedMatch = relaxed; result.savedOffline = lookup.result.savedOffline;
    for (NSDictionary *line in valid) result.hasWordTiming |= [line[@"words"] count] > 0;
    if (lookup.result.hasWordTiming || (lookup.result && !result.hasWordTiming)) return NO;
    lookup.result = result;
    lookup.hasPublishedLines = YES;
    DLCPerformance(result.hasWordTiming ? @"word-upgrade" : @"first-lines", lookup);
    BOOL written = DLCWriteCacheForEpoch(lookup.track, result, YES, lookup.cacheEpoch);
    if (!written && ![lookup.cacheEpoch isEqual:DLCCacheEpoch()]) { DLCSharedAbort(lookup); return NO; }
    if (written) {
        DLCSharedPublish(lookup, NO, nil);
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(), CFSTR("com.shalamand3r.corelyrics/CacheUpdated"), NULL, NULL, YES);
    }
    DLCNotify(lookup, NO, nil);
    for (DLCoreProviderTask *task in lookup.tasks.allValues) {
        task.hasUsableLines = YES;
        task.hasWordTiming = result.hasWordTiming;
    }
    DLCProviderHealthy(result.provider);
    if (result.hasWordTiming) DLCCompleteLookup(lookup, nil);
    static NSTimeInterval nextPrune;
    if (NSDate.date.timeIntervalSince1970 >= nextPrune) {
        nextPrune = NSDate.date.timeIntervalSince1970 + 3600;
        DLCPruneCache();
    }
    if (!result.hasWordTiming) {
        DLCArmHedge(lookup);
        // another line-only response cannot improve the result already on screen.
        for (DLCoreProviderTask *task in [lookup.tasks.allValues copy]) {
            if (DateLyricsProviderHasWordTiming(task.provider)) continue;
            DateLyricsCancelProviderLookup(task.storeID);
            [lookup.tasks removeObjectForKey:@(task.storeID)];
            [gLookupIDs removeObjectForKey:@(task.storeID)];
        }
        DLCFillProviders(lookup);
    }
    return YES;
}
static void DLCCancelLookup(DLCoreLookup *lookup) {
    DLCPerformance(@"cancel-remaining", lookup);
    for (DLCoreProviderTask *task in lookup.tasks.allValues) {
        DateLyricsCancelProviderLookup(task.storeID);
        [gLookupIDs removeObjectForKey:@(task.storeID)];
    }
    [lookup.tasks removeAllObjects];
    DLCSharedClose(lookup);
    [gLookups removeObjectForKey:lookup.track.cacheKey];
}
static BOOL DLCScheduleRetry(DLCoreLookup *lookup) {
    if (lookup.retryRound >= 2 || !lookup.retryProviders.count || lookup.result.hasWordTiming || !DLCSharedHasConsumers(lookup)) return NO;
    NSMutableArray<NSString *> *providers = [NSMutableArray array];
    NSTimeInterval now = CACurrentMediaTime(), earliest = DBL_MAX;
    for (NSString *provider in lookup.providers) {
        if (![lookup.retryProviders containsObject:provider]) continue;
        if (lookup.result.lines.count && !DateLyricsProviderHasWordTiming(provider)) continue;
        [providers addObject:provider];
        DLCProviderHealth *health = DLCHealth(provider);
        earliest = MIN(earliest, MAX(now, health.retryAt));
    }
    if (!providers.count) return NO;
    NSTimeInterval delay = MAX(lookup.retryRound ? 8 : 2, earliest - now);
    lookup.waitingForRetry = YES;
    ++lookup.retryRound; ++lookup.hedgeGeneration;
    lookup.providers = providers; lookup.nextProvider = 0;
    [lookup.retryProviders removeAllObjects];
    DateLyricsDebugLog(@"[Lookup:RETRY] scheduled round=%lu providers=%lu delay=%.1fs", (unsigned long)lookup.retryRound, (unsigned long)providers.count, delay);
    DLCPerformance(@"retry-scheduled", lookup);
    __weak DLCoreLookup *weakLookup = lookup;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(MIN(delay, 86400.0) * NSEC_PER_SEC)), gLyricsQueue, ^{
        DLCoreLookup *active = weakLookup;
        if (!active || gLookups[active.track.cacheKey] != active || !DLCSharedHasConsumers(active)) return;
        active.waitingForRetry = NO; active.expandedWindow = NO;
        DLLyricsResult *cached = DLCReadCache(active.track);
        if (cached && (cached.hasWordTiming || !active.result)) {
            active.result = cached; active.hasPublishedLines = YES;
            DLCNotify(active, NO, nil);
            if (cached.hasWordTiming) { DLCCompleteLookup(active, nil); return; }
        }
        DLCPerformance(@"retry-start", active);
        DLCArmHedge(active); DLCFillProviders(active);
    });
    return YES;
}
static void DLCCompleteLookup(DLCoreLookup *lookup, NSString *reason) {
    if (!lookup || gLookups[lookup.track.cacheKey] != lookup) return;
    if (lookup.waitingForRetry || DLCScheduleRetry(lookup)) return;
    DLCSharedPublish(lookup, YES, reason);
    if (reason.length && !lookup.result) {
        if (lookup.hadTransportFailure) [gRetryAfter removeObjectForKey:lookup.track.cacheKey];
        else gRetryAfter[lookup.track.cacheKey] = @(NSDate.date.timeIntervalSince1970 + 900);
    }
    NSError *error = reason.length && !lookup.result ? [NSError errorWithDomain:@"CoreLyrics" code:1 userInfo:@{NSLocalizedDescriptionKey:reason}] : nil;
    DLCPerformance(error ? @"finished-no-result" : @"finished", lookup);
    DateLyricsDebugLog(@"[Lookup:FINISH] result=%d transportFailure=%d", lookup.result != nil, lookup.hadTransportFailure);
    DLCNotify(lookup, YES, error);
    DLCCancelLookup(lookup);
    [lookup.subscribers removeAllObjects];
}
static void DLCArmHedge(DLCoreLookup *lookup) {
    if (lookup.expandedWindow) return;
    NSUInteger generation = ++lookup.hedgeGeneration;
    __weak DLCoreLookup *weakLookup = lookup;
    // two providers start immediately; a third joins only if work is still pending.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 350 * NSEC_PER_MSEC), gLyricsQueue, ^{
        DLCoreLookup *active = weakLookup;
        if (!active || gLookups[active.track.cacheKey] != active || active.hedgeGeneration != generation) return;
        DLCPerformance(@"start-overlap", active);
        active.expandedWindow = YES;
        DLCFillProviders(active);
    });
}
static void DLCFillProviders(DLCoreLookup *lookup) {
    if (gLookups[lookup.track.cacheKey] != lookup || lookup.waitingForRetry || DLCSharedIsFollower(lookup)) return;
    // keep existing word requests, but give their fast upgrade a head start before adding more.
    NSUInteger limit = lookup.expandedWindow ? 3 : (lookup.hasPublishedLines ? 1 : 2);
    while (lookup.tasks.count < limit && lookup.nextProvider < lookup.providers.count) {
        NSString *provider = lookup.providers[lookup.nextProvider++];
        if (lookup.result.lines.count && !DateLyricsProviderHasWordTiming(provider)) continue;
        DLCoreProviderTask *task = [DLCoreProviderTask new]; task.storeID = ++gNextLookupID;
        task.title = lookup.track.title; task.artist = lookup.track.artist; task.album = lookup.track.album;
        task.duration = lookup.track.duration;
        // each consumer decides whether external lines improve its own apple result.
        task.appleTier = DateLyricsSyncTierNone;
        task.hasUsableLines = lookup.result.lines.count > 0; task.hasWordTiming = lookup.result.hasWordTiming;
        task.musixmatchGeneration = gDateLyricsMusixmatchGeneration; task.taskID = NSUUID.UUID.UUIDString;
        task.provider = provider;
        if (!DLCProviderAcquire(provider, task.taskID)) {
            lookup.hadTransportFailure = YES;
            [lookup.retryProviders addObject:provider];
            DateLyricsDebugLog(@"[Provider:COOLDOWN] skipped provider=%@", provider);
            continue;
        }
        lookup.tasks[@(task.storeID)] = task; gLookupIDs[@(task.storeID)] = lookup;
        gMusixmatchPendingTaskIDs[@(task.storeID)] = task.taskID;
        DateLyricsStartProvider(task);
        if (gLookups[lookup.track.cacheKey] != lookup) return;
    }
    if (!lookup.tasks.count)
        DLCCompleteLookup(lookup, lookup.result ? nil : (lookup.failureReason ?: @"All lyric providers exhausted"));
}
static void DLCFinishTask(DLCoreProviderTask *task, NSString *reason) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    DLCoreLookup *lookup = gLookupIDs[@(task.storeID)];
    DateLyricsDebugLog(@"[Provider:FINISH] request=%ld provider=%@ failed=%d transportFailure=%d", (long)task.storeID, task.provider, reason.length > 0, task.hadTransportFailure);
    lookup.hadTransportFailure |= task.hadTransportFailure;
    if (task.hadTransportFailure) [lookup.retryProviders addObject:task.provider];
    if (!task.hadTransportFailure) DLCProviderHealthy(task.provider);
    if (reason.length) lookup.failureReason = reason;
    DateLyricsCancelProviderLookup(task.storeID);
    [lookup.tasks removeObjectForKey:@(task.storeID)]; [gLookupIDs removeObjectForKey:@(task.storeID)];
    DLCFillProviders(lookup);
}

static id DLCPreference(NSString *key) {
    // one provider-settings domain keeps the sister tweaks consistent.
    return DLLyricsPreferenceValue(@"com.shalamand3r.datelyrics", key, gSavedProviderPreferences);
}
static BOOL DLCPreferenceBool(NSString *key, BOOL fallback) {
    id value = DLCPreference(key);
    return [value isKindOfClass:NSNumber.class] ? [value boolValue] : fallback;
}
static void DLCLoadPreferences(void) {
    gSavedProviderPreferences = DLLyricsSavedPreferences(@"com.shalamand3r.datelyrics");
    NSMutableArray *active = [NSMutableArray array];
    NSDictionary *keys = @{@"lrclib":@"LRCLIBEnabled", @"megalobiz":@"MegalobizEnabled", @"netease":@"NetEaseEnabled", @"qqmusic":@"QQMusicEnabled"};
    for (NSString *provider in DateLyricsSanitizedProviderOrder(DLCPreference(@"LyricsProviderOrder")))
        if (!keys[provider] || DLCPreferenceBool(keys[provider], YES)) [active addObject:provider];
    gDateLyricsProviderOrder = active;
    gDateLyricsMusixmatchEnabled = DLCPreferenceBool(@"MusixmatchEnabled", YES);
    gDateLyricsRelaxedMatching = DLCPreferenceBool(@"RelaxedMatching", NO);
    gDateLyricsRequestTimeout = DateLyricsSanitizedRequestTimeout(DLCPreference(@"LyricsRequestTimeout"));
    NSDictionary *bridge = DLLyricsSavedPreferences(@"com.shalamand3r.lyricbridge");
    id bridgeDebug = DLLyricsPreferenceValue(@"com.shalamand3r.lyricbridge", @"DebugLogging", bridge);
    gDateLyricsDebugLogging = DLCPreferenceBool(@"DebugLogging", NO) || ([bridgeDebug isKindOfClass:NSNumber.class] && [bridgeDebug boolValue]);
    gPerformanceLogging = gDateLyricsDebugLogging;
    NSString *token = DateLyricsJSONString(DLCPreference(@"MusixmatchUserToken"));
    token = [token stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (![token ?: @"" isEqualToString:gDateLyricsMusixmatchCustomUserToken ?: @""]) {
        gDateLyricsMusixmatchCustomUserToken = token.length ? token : nil;
        gMusixmatchUserToken = nil; gMusixmatchTokenCacheLoaded = NO;
    }
}
static NSArray *DLCFetchConfiguration(void) {
    return @[gDateLyricsProviderOrder ?: @[], @(gDateLyricsMusixmatchEnabled),
        @(gDateLyricsRelaxedMatching), @(gDateLyricsRequestTimeout), gDateLyricsMusixmatchCustomUserToken ?: @""];
}
static void DLCPreferencesChanged(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef info) {
    [DLLyricsEngine.sharedEngine reloadPreferences];
}
static BOOL DLCCacheFilename(NSString *name) {
    if (name.length != 69 || ![name hasSuffix:@".json"]) return NO;
    return [[name substringToIndex:64] rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location == NSNotFound;
}
static void DLCPruneCache(void) {
    NSString *root = DLCCacheRoot(); NSMutableArray *files = [NSMutableArray array];
    int lock = DLCLockCache(root);
    if (lock < 0) return;
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:nil]) {
        if (!DLCCacheFilename(name)) continue;
        NSString *path = [root stringByAppendingPathComponent:name];
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
        if (![attributes.fileType isEqual:NSFileTypeRegular] || attributes.fileSize > 4 * 1024 * 1024) continue;
        NSDictionary *json = DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] ?: NSData.data options:0 error:nil]);
        if ([DateLyricsJSONNumber(json[@"saved"]) boolValue]) continue;
        [files addObject:@{@"path":path, @"date":attributes[NSFileModificationDate] ?: NSDate.distantPast}];
    }
    [files sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [b[@"date"] compare:a[@"date"]]; }];
    for (NSUInteger index = 100; index < files.count; index++) [NSFileManager.defaultManager removeItemAtPath:files[index][@"path"] error:nil];
    DLCUnlockCache(lock);
}
static void DLCInvalidateLookups(void) {
    ++gDateLyricsMusixmatchGeneration; ++gTokenGeneration;
    DateLyricsCancelAllProviderRequests();
    for (DLCProviderHealth *health in gProviderHealth.allValues) health.probeID = nil;
    [gMusixmatchPendingTaskIDs removeAllObjects]; [gMusixmatchTokenWaiters removeAllObjects];
    gMusixmatchTokenRequestInFlight = NO;
    NSError *error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil];
    for (DLCoreLookup *lookup in gLookups.allValues) { DLCSharedClose(lookup); lookup.result = nil; DLCNotify(lookup, YES, error); }
    [gLookups removeAllObjects]; [gLookupIDs removeAllObjects]; [gRetryAfter removeAllObjects];
}
static void DLCClearCacheFiles(NSString *root, BOOL keepSaved) {
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:nil]) {
        if (!DLCCacheFilename(name)) continue;
        NSString *path = [root stringByAppendingPathComponent:name];
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
        if (![attributes.fileType isEqual:NSFileTypeRegular] || attributes.fileSize > 4 * 1024 * 1024) continue;
        if (keepSaved) {
            NSDictionary *json = DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] ?: NSData.data options:0 error:nil]);
            if ([DateLyricsJSONNumber(json[@"saved"]) boolValue]) continue;
        }
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
}
static NSUInteger DLCCacheGeneration(id value) {
    return [value isKindOfClass:NSNumber.class] ? [value unsignedIntegerValue] : 0;
}
static void DLCApplyRequestedCacheReset(void) {
    NSUInteger requested = DLCCacheGeneration(DLCPreference(@"CacheGeneration"));
    NSString *root = DLCCacheRoot();
    int lock = DLCLockCache(root);
    if (lock < 0) return;
    NSString *marker = [root stringByAppendingPathComponent:@".cache-generation.plist"];
    NSDictionary *state = [NSDictionary dictionaryWithContentsOfFile:marker];
    NSUInteger processed = state ? DLCCacheGeneration(state[@"generation"]) :
        DLCCacheGeneration(DLCPreference(@"MusicProcessedCacheGeneration"));
    BOOL reset = requested > processed;
    if (reset) {
        DLCAdvanceCacheEpoch();
        DLCClearCacheFiles(root, NO);
    }
    [@{@"generation":@(MAX(requested, processed))} writeToFile:marker atomically:YES];
    DLCUnlockCache(lock);
    if (reset) DLCInvalidateLookups();
}
static void DLCCacheResetRequested(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef info) {
    [DLLyricsEngine.sharedEngine reloadPreferences];
}
@implementation DLLyricsEngine
+ (instancetype)sharedEngine {
    static DLLyricsEngine *engine; static dispatch_once_t once;
    dispatch_once(&once, ^{
        engine = [DLLyricsEngine new];
        gLyricsQueue = dispatch_queue_create("com.shalamand3r.corelyrics.providers", DISPATCH_QUEUE_SERIAL);
        gLookups = [NSMutableDictionary dictionary]; gLookupIDs = [NSMutableDictionary dictionary]; gRetryAfter = [NSMutableDictionary dictionary];
        gMusixmatchPendingTaskIDs = [NSMutableDictionary dictionary]; gMusixmatchTokenWaiters = [NSMutableArray array];
        DLCLoadPreferences();
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, DLCSharedChanged, CFSTR("com.shalamand3r.corelyrics/LookupUpdated"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        dispatch_async(gLyricsQueue, ^{ DLCApplyRequestedCacheReset(); });
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, DLCCacheResetRequested, CFSTR("com.shalamand3r.datelyrics/ClearCaches"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, DLCPreferencesChanged, CFSTR("com.shalamand3r.datelyrics/ReloadPrefs"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, DLCPreferencesChanged, CFSTR("com.shalamand3r.corelyrics/ReloadPrefs"), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    });
    return engine;
}
- (DLLyricsSubscription *)fetchTrack:(DLLyricsTrack *)track client:(NSString *)client update:(DLLyricsUpdate)update {
    DLLyricsSubscription *subscription = [DLLyricsSubscription new];
    NSString *token = NSUUID.UUID.UUIDString;
    DLLyricsTrack *selection = [track copy]; NSString *key = selection.cacheKey;
    __weak DLLyricsSubscription *weakSubscription = subscription;
    DLLyricsUpdate delivery = ^(DLLyricsResult *result, BOOL finished, NSError *error) {
        DLLyricsSubscription *active = weakSubscription;
        BOOL deliver;
        @synchronized (active) { deliver = active.cancellation != nil; }
        if (deliver) update(result, finished, error);
    };
    subscription.cancellation = ^{
        dispatch_async(gLyricsQueue, ^{
            DLCoreLookup *lookup = gLookups[key];
            if (!lookup) return;
            [lookup.subscribers removeObjectForKey:token];
            if (!lookup.subscribers.count) {
                DLCSharedDropParticipant(lookup);
                if (!DLCSharedHasConsumers(lookup)) DLCCancelLookup(lookup);
            }
        });
    };
    dispatch_async(gLyricsQueue, ^{
        NSTimeInterval requestedAt = CACurrentMediaTime();
        DLLyricsSubscription *active = weakSubscription;
        @synchronized (active) { if (!active.cancellation) return; }
        if (!DateLyricsMusixmatchNormalizedString(selection.title).length || !DateLyricsMusixmatchNormalizedString(selection.artist).length) {
            dispatch_async(dispatch_get_main_queue(), ^{ delivery(nil, YES, [NSError errorWithDomain:@"CoreLyrics" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Song title and artist are required."}]); });
            return;
        }
        DLCoreLookup *lookup = gLookups[key];
        if (lookup) {
            lookup.subscribers[token] = delivery;
            DLCSharedAddParticipant(lookup);
            if (lookup.result) dispatch_async(dispatch_get_main_queue(), ^{ delivery(lookup.result, NO, nil); });
            return;
        }
        DLLyricsResult *cached = DLCReadCache(selection);
        DateLyricsDebugLog(@"[Cache:READ] hit=%d words=%d durationKnown=%d", cached != nil, cached.hasWordTiming, selection.duration > 0);
        if (cached.hasWordTiming || (!gDateLyricsMusixmatchEnabled && cached)) {
            dispatch_async(dispatch_get_main_queue(), ^{ delivery(cached, YES, nil); }); return;
        }
        if ([gRetryAfter[key] doubleValue] > NSDate.date.timeIntervalSince1970 || !gDateLyricsMusixmatchEnabled) {
            dispatch_async(dispatch_get_main_queue(), ^{ delivery(cached, YES, cached ? nil : [NSError errorWithDomain:@"CoreLyrics" code:3 userInfo:@{NSLocalizedDescriptionKey:@"No cached lyrics are available."}]); }); return;
        }
        lookup = [DLCoreLookup new]; lookup.startedAt = requestedAt; lookup.startedCPU = gPerformanceLogging ? DLCProcessCPU() : 0; lookup.track = selection; lookup.result = cached;
        DLCPerformance(@"lookup-start", lookup);
        lookup.subscribers = [NSMutableDictionary dictionaryWithObject:delivery forKey:token];
        lookup.retryProviders = [NSMutableSet set];
        lookup.tasks = [NSMutableDictionary dictionary]; lookup.providers = [DateLyricsActiveProviders() copy];
        gLookups[key] = lookup;
        if (cached) DLCNotify(lookup, NO, nil);
        DLCSharedStart(lookup);
    });
    return subscription;
}
- (void)cachedTrack:(DLLyricsTrack *)track completion:(void (^)(DLLyricsResult *))completion {
    DLLyricsTrack *selection = [track copy];
    dispatch_async(gLyricsQueue, ^{ DLLyricsResult *cached = DLCReadCache(selection); dispatch_async(dispatch_get_main_queue(), ^{ completion(cached); }); });
}
- (void)importCachedLines:(NSArray<NSDictionary *> *)lines forTrack:(DLLyricsTrack *)track relaxedMatch:(BOOL)relaxed {
    DLLyricsTrack *selection = [track copy]; NSArray *snapshot = [lines copy];
    dispatch_async(gLyricsQueue, ^{
        if (relaxed && !gDateLyricsRelaxedMatching) return;
        NSArray *valid = DLCValidatedLines(snapshot);
        if (!valid.count) return;
        DLLyricsResult *existing = DLCReadCache(selection);
        DLLyricsResult *result = [DLLyricsResult new]; result.lines = valid;
        result.provider = @"cached"; result.relaxedMatch = relaxed; result.savedOffline = existing.savedOffline;
        for (NSDictionary *line in valid) result.hasWordTiming |= [line[@"words"] count] > 0;
        if (existing.hasWordTiming || (existing && !result.hasWordTiming)) return;
        DLCWriteCache(selection, result, YES);
    });
}
- (void)setSavedOffline:(BOOL)saved forTrack:(DLLyricsTrack *)track completion:(void (^)(BOOL))completion {
    DLLyricsTrack *selection = [track copy];
    dispatch_async(gLyricsQueue, ^{
        DLLyricsResult *cached = DLCReadCache(selection); cached.savedOffline = saved;
        BOOL success = cached && DLCWriteCache(selection, cached, NO);
        DLCoreLookup *active = gLookups[selection.cacheKey];
        if (success && active.result) {
            DLLyricsResult *replacement = [DLLyricsResult new];
            replacement.lines = active.result.lines; replacement.provider = active.result.provider;
            replacement.hasWordTiming = active.result.hasWordTiming;
            replacement.relaxedMatch = active.result.relaxedMatch; replacement.savedOffline = saved;
            active.result = replacement;
        }
        dispatch_async(dispatch_get_main_queue(), ^{ completion(success); });
    });
}
- (void)clearCacheKeepingSaved:(BOOL)keepSaved completion:(dispatch_block_t)completion {
    dispatch_async(gLyricsQueue, ^{
        DLCInvalidateLookups();
        NSString *root = DLCCacheRoot();
        int lock = DLCLockCache(root);
        if (lock < 0) { dispatch_async(dispatch_get_main_queue(), completion); return; }
        DLCAdvanceCacheEpoch();
        DLCClearCacheFiles(root, keepSaved);
        DLCUnlockCache(lock);
        dispatch_async(dispatch_get_main_queue(), completion);
    });
}
- (void)reloadPreferences {
    dispatch_async(gLyricsQueue, ^{
        NSArray *previous = DLCFetchConfiguration();
        DLCLoadPreferences();
        if (![previous isEqual:DLCFetchConfiguration()]) DLCInvalidateLookups();
        DLCApplyRequestedCacheReset();
    });
}
@end
