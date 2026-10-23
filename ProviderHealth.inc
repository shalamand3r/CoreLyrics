// provider health is shared by callers in this process and confined to glyricsqueue.
@interface DLCProviderHealth : NSObject
@property (nonatomic) NSUInteger failures;
@property (nonatomic) NSTimeInterval retryAt;
@property (nonatomic, copy) NSString *probeID;
@end
@implementation DLCProviderHealth
@end
static NSMutableDictionary<NSString *, DLCProviderHealth *> *gProviderHealth;

static DLCProviderHealth *DLCHealth(NSString *provider) {
    if (!gProviderHealth) gProviderHealth = [NSMutableDictionary dictionary];
    DLCProviderHealth *health = gProviderHealth[provider];
    if (!health) { health = [DLCProviderHealth new]; gProviderHealth[provider] = health; }
    return health;
}
static NSTimeInterval DLCRetryDelay(NSString *value) {
    if (!value.length) return 0;
    NSString *trimmed = [value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSScanner *scanner = [NSScanner scannerWithString:trimmed];
    double seconds;
    if ([scanner scanDouble:&seconds] && scanner.isAtEnd && isfinite(seconds) && seconds >= 0) return seconds;
    NSDateFormatter *formatter = [NSDateFormatter new];
    formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    formatter.dateFormat = @"EEE, dd MMM yyyy HH:mm:ss zzz";
    NSDate *date = [formatter dateFromString:trimmed];
    return date ? MAX(0, date.timeIntervalSinceNow) : 0;
}
static void DLCProviderFailed(NSString *provider, NSHTTPURLResponse *response) {
    DLCProviderHealth *health = DLCHealth(provider);
    health.failures = MIN(health.failures + 1, 2);
    NSString *retryAfter;
    for (NSString *key in response.allHeaderFields)
        if ([key caseInsensitiveCompare:@"Retry-After"] == NSOrderedSame) retryAfter = [response.allHeaderFields[key] description];
    NSTimeInterval delay = MAX(DLCRetryDelay(retryAfter), health.failures >= 2 ? 300 : 0);
    if (delay > 0) health.retryAt = MAX(health.retryAt, CACurrentMediaTime() + delay);
    DateLyricsDebugLog(@"[Provider:HEALTH] provider=%@ failures=%lu cooldown=%.0fs", provider, (unsigned long)health.failures, delay);
}
static BOOL DLCProviderAcquire(NSString *provider, NSString *taskID) {
    DLCProviderHealth *health = DLCHealth(provider);
    if (health.retryAt > CACurrentMediaTime() || health.probeID) return NO;
    if (health.retryAt > 0) {
        health.probeID = taskID;
        DateLyricsDebugLog(@"[Provider:HEALTH] recovery probe provider=%@", provider);
    }
    return YES;
}
static void DLCProviderHealthy(NSString *provider) {
    DLCProviderHealth *health = DLCHealth(provider);
    // an older in-flight response must not override a server's retry-after.
    if (health.retryAt > CACurrentMediaTime()) return;
    health.failures = 0; health.retryAt = 0; health.probeID = nil;
}
static void DLCProviderRelease(DLCoreProviderTask *task) {
    DLCProviderHealth *health = gProviderHealth[task.provider];
    if ([health.probeID isEqualToString:task.taskID]) health.probeID = nil;
}
