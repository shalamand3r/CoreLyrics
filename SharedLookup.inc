// leases share in-flight work without keeping a service running between songs.
@interface DLCSharedLease : NSObject
@property (nonatomic, copy) NSString *directory;
@property (nonatomic, copy) NSString *participant;
@property (nonatomic, copy) NSString *revision;
@property (nonatomic, strong) NSMutableDictionary *status;
@property (nonatomic) int ownerFD;
@property (nonatomic) int participantFD;
@property (nonatomic, strong) dispatch_source_t timer;
@end
@implementation DLCSharedLease
- (instancetype)init {
    if ((self = [super init])) { _ownerFD = -1; _participantFD = -1; }
    return self;
}
- (void)dealloc {
    if (_timer) dispatch_source_cancel(_timer);
    if (_ownerFD >= 0) close(_ownerFD);
    if (_participantFD >= 0) close(_participantFD);
}
@end

static void DLCSharedTick(DLCoreLookup *lookup);
static BOOL DLCSharedIsFollower(DLCoreLookup *lookup) {
    return lookup.sharedLease && lookup.sharedLease.ownerFD < 0;
}
static int DLCSharedOpen(NSString *path) {
    return open(path.fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
}
static NSDictionary *DLCSharedRead(DLCSharedLease *lease) {
    NSString *path = [lease.directory stringByAppendingPathComponent:@"state.json"];
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (![attributes.fileType isEqual:NSFileTypeRegular] || attributes.fileSize > 4096) return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    return data ? DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:data options:0 error:nil]) : nil;
}
static void DLCSharedWrite(DLCSharedLease *lease) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:lease.status options:0 error:nil];
    if (data.length && data.length <= 4096)
        [data writeToFile:[lease.directory stringByAppendingPathComponent:@"state.json"] options:NSDataWritingAtomic error:nil];
}
// called under the cache gate so joining, checking and removing a lease cannot race.
static BOOL DLCSharedPeers(DLCSharedLease *lease) {
    BOOL live = NO;
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:lease.directory error:nil]) {
        if (![name hasSuffix:@".peer"]) continue;
        NSString *path = [lease.directory stringByAppendingPathComponent:name];
        if ([path isEqual:lease.participant] && lease.participantFD >= 0) { live = YES; continue; }
        int fd = open(path.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        else if (errno == EWOULDBLOCK) live = YES;
        close(fd);
    }
    return live;
}
static void DLCSharedClean(DLCSharedLease *lease) {
    int fd = DLCSharedOpen([lease.directory stringByAppendingPathComponent:@"owner.lock"]);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0 && !DLCSharedPeers(lease)) {
        // never recurse through unexpected files or links in the shared directory.
        for (NSString *name in @[@"state.json", @"owner.lock"])
            [NSFileManager.defaultManager removeItemAtPath:[lease.directory stringByAppendingPathComponent:name] error:nil];
        rmdir(lease.directory.fileSystemRepresentation);
    }
    close(fd);
}
static void DLCSharedDropParticipantLocked(DLCSharedLease *lease) {
    if (lease.participantFD < 0) return;
    close(lease.participantFD); lease.participantFD = -1;
    [NSFileManager.defaultManager removeItemAtPath:lease.participant error:nil];
    lease.participant = nil;
}
static void DLCSharedDropParticipant(DLCoreLookup *lookup) {
    DLCSharedLease *lease = lookup.sharedLease;
    if (!lease) return;
    int gate = DLCLockCache(DLCCacheRoot());
    if (gate < 0) return;
    DLCSharedDropParticipantLocked(lease);
    DLCUnlockCache(gate);
}
static BOOL DLCSharedAddParticipantLocked(DLCSharedLease *lease) {
    if (lease.participantFD >= 0) return YES;
    NSString *path = [lease.directory stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@".peer"]];
    int fd = DLCSharedOpen(path);
    if (fd < 0) return NO;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return NO; }
    lease.participant = path; lease.participantFD = fd;
    return YES;
}
static void DLCSharedAddParticipant(DLCoreLookup *lookup) {
    if (!lookup.sharedLease) return;
    int gate = DLCLockCache(DLCCacheRoot());
    if (gate < 0) return;
    DLCSharedAddParticipantLocked(lookup.sharedLease);
    DLCUnlockCache(gate);
}
static BOOL DLCSharedHasConsumers(DLCoreLookup *lookup) {
    if (lookup.subscribers.count) return YES;
    if (!lookup.sharedLease || DLCSharedIsFollower(lookup)) return NO;
    int gate = DLCLockCache(DLCCacheRoot());
    if (gate < 0) return NO;
    BOOL live = DLCSharedPeers(lookup.sharedLease);
    DLCUnlockCache(gate);
    return live;
}
static void DLCSharedClose(DLCoreLookup *lookup) {
    DLCSharedLease *lease = lookup.sharedLease;
    if (!lease) return;
    if (lease.timer) { dispatch_source_cancel(lease.timer); lease.timer = nil; }
    int gate = DLCLockCache(DLCCacheRoot());
    if (gate >= 0) {
        DLCSharedDropParticipantLocked(lease);
        if (lease.ownerFD >= 0) { close(lease.ownerFD); lease.ownerFD = -1; }
        DLCSharedClean(lease);
        DLCUnlockCache(gate);
    }
    lookup.sharedLease = nil;
}
static void DLCSharedAbort(DLCoreLookup *lookup) {
    lookup.result = nil;
    DLCNotify(lookup, YES, [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]);
    DLCCancelLookup(lookup);
    [lookup.subscribers removeAllObjects];
}
static void DLCSharedPublish(DLCoreLookup *lookup, BOOL finished, NSString *reason) {
    DLCSharedLease *lease = lookup.sharedLease;
    if (!lease || lease.ownerFD < 0) return;
    lease.status = [@{@"revision":NSUUID.UUID.UUIDString, @"heartbeat":@(CACurrentMediaTime()),
        @"finished":@(finished), @"finishedAt":@(finished ? CACurrentMediaTime() : 0),
        @"result":@(lookup.result != nil), @"words":@(lookup.result.hasWordTiming),
        @"failed":@(reason.length && !lookup.result), @"transport":@(lookup.hadTransportFailure)} mutableCopy];
    DLCSharedWrite(lease);
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(), CFSTR("com.shalamand3r.corelyrics/LookupUpdated"), NULL, NULL, YES);
}
static void DLCSharedBeginOwner(DLCoreLookup *lookup) {
    DateLyricsDebugLog(@"[Lookup:SHARED] role=owner");
    DLCSharedPublish(lookup, NO, nil);
    DLCArmHedge(lookup); DLCFillProviders(lookup);
}
static void DLCSharedDeliver(DLCoreLookup *lookup, NSDictionary *state, BOOL finished) {
    DLLyricsResult *cached = DLCReadCache(lookup.track);
    if (cached && (!lookup.result || (!lookup.result.hasWordTiming && cached.hasWordTiming))) {
        lookup.result = cached; lookup.hasPublishedLines = YES;
        DLCPerformance(@"shared-result", lookup);
        if (!finished) DLCNotify(lookup, NO, nil);
    }
    if (!finished) return;
    BOOL failed = [DateLyricsJSONNumber(state[@"failed"]) boolValue];
    lookup.hadTransportFailure = [DateLyricsJSONNumber(state[@"transport"]) boolValue];
    if (failed && !lookup.hadTransportFailure && !lookup.result)
        gRetryAfter[lookup.track.cacheKey] = @(NSDate.date.timeIntervalSince1970 + 900);
    NSError *error = failed && !lookup.result ? [NSError errorWithDomain:@"CoreLyrics" code:1 userInfo:@{NSLocalizedDescriptionKey:@"All lyric providers exhausted"}] : nil;
    DLCPerformance(@"shared-finished", lookup);
    DLCNotify(lookup, YES, error);
    DLCCancelLookup(lookup);
    [lookup.subscribers removeAllObjects];
}
static void DLCSharedTick(DLCoreLookup *lookup) {
    if (gLookups[lookup.track.cacheKey] != lookup || !lookup.sharedLease) return;
    if (![lookup.cacheEpoch isEqual:DLCCacheEpoch()]) { DLCSharedAbort(lookup); return; }
    DLCSharedLease *lease = lookup.sharedLease;
    if (!DLCSharedHasConsumers(lookup)) { DLCCancelLookup(lookup); return; }
    if (lease.ownerFD >= 0) {
        lease.status[@"heartbeat"] = @(CACurrentMediaTime());
        DLCSharedWrite(lease);
        return;
    }
    int gate = DLCLockCache(DLCCacheRoot());
    if (gate < 0) return;
    int fd = DLCSharedOpen([lease.directory stringByAppendingPathComponent:@"owner.lock"]);
    BOOL claimed = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
    NSDictionary *state = DLCSharedRead(lease);
    if (claimed) lease.ownerFD = fd;
    else if (fd >= 0) close(fd);
    DLCUnlockCache(gate);
    BOOL finished = claimed && [DateLyricsJSONNumber(state[@"finished"]) boolValue] &&
        DateLyricsJSONDouble(state[@"finishedAt"], 0) >= lookup.startedAt;
    if (finished) {
        DLLyricsResult *cached = DLCReadCache(lookup.track);
        // a failed cache write must not turn a successful owner result into an empty reply.
        if (![DateLyricsJSONNumber(state[@"result"]) boolValue] ||
            (cached && (![DateLyricsJSONNumber(state[@"words"]) boolValue] || cached.hasWordTiming))) {
            DLCSharedDeliver(lookup, state, YES); return;
        }
    }
    if (claimed) {
        DLLyricsResult *cached = DLCReadCache(lookup.track);
        if (cached.hasWordTiming) {
            lookup.result = cached;
            DLCSharedPublish(lookup, YES, nil);
            DLCSharedDeliver(lookup, nil, YES);
            return;
        }
        DLCSharedBeginOwner(lookup); return;
    }
    NSString *revision = DateLyricsJSONString(state[@"revision"]);
    if (revision && ![revision isEqual:lease.revision]) {
        lease.revision = revision;
        DLCSharedDeliver(lookup, state, NO);
    }
    NSTimeInterval heartbeat = DateLyricsJSONDouble(state[@"heartbeat"], lookup.startedAt);
    if (CACurrentMediaTime() - heartbeat > 6) {
        // a suspended owner still holds its lock; use independent work rather than wait forever.
        DateLyricsDebugLog(@"[Lookup:SHARED] fallback=unresponsive-owner");
        DLCSharedClose(lookup);
        DLCArmHedge(lookup); DLCFillProviders(lookup);
    }
}
static void DLCSharedChanged(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef info) {
    dispatch_async(gLyricsQueue, ^{
        for (DLCoreLookup *lookup in [gLookups.allValues copy])
            if (DLCSharedIsFollower(lookup)) DLCSharedTick(lookup);
    });
}
static void DLCSharedStart(DLCoreLookup *lookup) {
    int gate = DLCLockCache(DLCCacheRoot());
    lookup.cacheEpoch = DLCCacheEpoch();
    if (gate < 0) { DLCArmHedge(lookup); DLCFillProviders(lookup); return; }
    NSData *identity = [NSJSONSerialization dataWithJSONObject:@[@1, lookup.track.cacheKey, DLCFetchConfiguration(), lookup.cacheEpoch] options:0 error:nil];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(identity.bytes, (CC_LONG)identity.length, digest);
    NSMutableString *key = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [key appendFormat:@"%02x", digest[i]];
    NSString *root = [DLCCacheRoot() stringByAppendingPathComponent:@".shared-lookups"];
    [NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    static NSTimeInterval nextPrune;
    if (CACurrentMediaTime() >= nextPrune) {
        nextPrune = CACurrentMediaTime() + 3600;
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:nil]) {
            if (name.length != 64 || [name rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location != NSNotFound) continue;
            DLCSharedLease *stale = [DLCSharedLease new]; stale.directory = [root stringByAppendingPathComponent:name];
            NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:stale.directory error:nil];
            if ([attributes.fileType isEqual:NSFileTypeDirectory]) DLCSharedClean(stale);
        }
    }
    DLCSharedLease *lease = [DLCSharedLease new];
    lease.directory = [root stringByAppendingPathComponent:key];
    BOOL ready = [NSFileManager.defaultManager createDirectoryAtPath:lease.directory withIntermediateDirectories:YES attributes:nil error:nil] && DLCSharedAddParticipantLocked(lease);
    DLCUnlockCache(gate);
    if (!ready) { DLCArmHedge(lookup); DLCFillProviders(lookup); return; }
    lookup.sharedLease = lease;
    DateLyricsDebugLog(@"[Lookup:SHARED] joined");
    __weak DLCoreLookup *weakLookup = lookup;
    lease.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gLyricsQueue);
    dispatch_source_set_timer(lease.timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(lease.timer, ^{ DLCoreLookup *active = weakLookup; if (active) DLCSharedTick(active); });
    dispatch_resume(lease.timer);
    DLCSharedTick(lookup);
}
