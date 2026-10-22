// datelyrics owns its presentation and legacy score files; corelyrics owns fetching.
static NSMutableDictionary<NSNumber *, DLLyricsSubscription *> *gDateLyricsEngineSubscriptions;
static BOOL DateLyricsMusixmatchTaskIsCurrent(DateLyricsMusixmatchTask *task);
static BOOL DateLyricsShouldFetchMusixmatch(DateLyricsSyncTier appleTier) {
    if (!gDateLyricsMusixmatchEnabled) return NO;
    if (appleTier == DateLyricsSyncTierWord) return NO;
    if (appleTier == DateLyricsSyncTierLine) {
        if (!gDateLyricsWordHighlighting) return NO;
        return YES;
    }
    return YES;
}

static BOOL DateLyricsMusixmatchScoreWins(DateLyricsScore *incoming, DateLyricsSyncTier appleTier) {
    if (!incoming || incoming.lines.count == 0) return NO;
    if (appleTier == DateLyricsSyncTierLine) {
        return incoming.hasWordTiming;
    }
    return YES;
}

static void DateLyricsRemoveMusixmatchLoadingScore(NSInteger storeID) {
    if (storeID <= 0) return;
    NSString *path = DateLyricsActiveScorePath(storeID);
    NSData *data = DateLyricsStorageReadData(path);
    DateLyricsScore *score = data.length > 0 ? [[DateLyricsScore alloc] initWithJSONData:data] : nil;
    if (![score.source isEqualToString:@"musixmatch-loading"]) return;
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    DateLyricsPublishBestScore(storeID);
}

static void DateLyricsWriteMusixmatchLoadingScore(DateLyricsMusixmatchTask *task, NSInteger step, NSInteger total) {
    if (!task || task.storeID <= 0 || task.appleTier == DateLyricsSyncTierLine || task.hasUsableLines) return;
    NSTimeInterval end = task.duration > 1.0 ? task.duration : 3600.0;
    NSString *text = @"Loading…";
    DateLyricsWriteScoreJSON(task.storeID, @[@{
        @"begin": @0.0,
        @"end": @(end),
        @"text": text
    }], NO, @"musixmatch-loading");
}

static void DateLyricsScheduleMusixmatchLoadingScore(DateLyricsMusixmatchTask *task) {
    if (!task || task.storeID <= 0 || task.appleTier == DateLyricsSyncTierLine || task.hasUsableLines) return;

    NSInteger storeID = task.storeID;
    NSString *taskID = [task.taskID copy];
    DateLyricsSyncTier appleTier = task.appleTier;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), gLyricsQueue, ^{
        if (!DateLyricsMusixmatchServiceEnabled()) return;
        if (![gMusixmatchPendingTaskIDs[@(storeID)] isEqualToString:taskID]) return;
        if (appleTier == DateLyricsSyncTierLine || task.hasUsableLines) return;

        task.hasShownLoading = YES;
        DateLyricsWriteMusixmatchLoadingScore(task, task.currentProviderIndex, task.totalProviders);
        DateLyricsDebugLog(@"[Musixmatch] showing delayed loading indicator (%ld/%ld) storeID=%ld",
                           (long)task.currentProviderIndex, (long)task.totalProviders, (long)storeID);
    });
}

static BOOL DateLyricsMusixmatchTaskIsCurrent(DateLyricsMusixmatchTask *task) {
    return task && task.cacheGeneration == gDateLyricsCacheGeneration &&
        task.musixmatchGeneration == gDateLyricsMusixmatchGeneration &&
        gDateLyricsEnabled && DateLyricsMusixmatchAllowedForTrack(task.storeID) &&
        (!DateLyricsIsExternalTrack(task.storeID) || task.storeID == gDateLyricsSelectedExternalTrack) &&
        [gMusixmatchPendingTaskIDs[@(task.storeID)] isEqualToString:task.taskID];
}
static void DateLyricsCancelProviderLookup(NSInteger trackID) {
    [gDateLyricsEngineSubscriptions[@(trackID)] cancel];
    [gDateLyricsEngineSubscriptions removeObjectForKey:@(trackID)];
    [gMusixmatchPendingTaskIDs removeObjectForKey:@(trackID)];
    DateLyricsRemoveMusixmatchLoadingScore(trackID);
}
static void DateLyricsCancelAllProviderRequests(void) {
    for (DLLyricsSubscription *subscription in gDateLyricsEngineSubscriptions.allValues) [subscription cancel];
    [gDateLyricsEngineSubscriptions removeAllObjects];
}
static void DateLyricsShareCachedScore(NSInteger storeID, NSString *title, NSString *artist, NSString *album, NSTimeInterval duration) {
    NSData *data = DateLyricsStorageReadData(DateLyricsMusixmatchScorePath(storeID));
    if (!data.length || data.length > 4 * 1024 * 1024) return;
    NSDictionary *score = DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:data options:0 error:nil]);
    if (![DateLyricsJSONString(score[@"kind"]) isEqualToString:@"score"] || DateLyricsJSONTrackID(score[@"trackId"]) != storeID) return;
    DLLyricsTrack *track = [DLLyricsTrack trackWithTitle:title artist:artist duration:duration];
    track.album = album ?: @"";
    [DLLyricsEngine.sharedEngine importCachedLines:DateLyricsJSONArray(score[@"lines"]) forTrack:track relaxedMatch:[DateLyricsJSONString(score[@"source"]) isEqualToString:@"musixmatch-relaxed"]];
}
static void DateLyricsBeginSharedLookup(DateLyricsMusixmatchTask *task) {
    if (!gDateLyricsEngineSubscriptions) gDateLyricsEngineSubscriptions = [NSMutableDictionary dictionary];
    DLLyricsTrack *track = [DLLyricsTrack trackWithTitle:task.title artist:task.artist duration:task.duration];
    track.album = task.album ?: @"";
    track.hasNativeLineTiming = task.appleTier == DateLyricsSyncTierLine;
    if (!DateLyricsIsExternalTrack(task.storeID)) track.catalogID = task.storeID;
    DLLyricsSubscription *subscription = [DLLyricsEngine.sharedEngine fetchTrack:track client:@"DateLyrics" update:^(DLLyricsResult *result, BOOL finished, NSError *error) {
        dispatch_async(gLyricsQueue, ^{
            if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
            if (result.lines.count && (!result.relaxedMatch || gDateLyricsRelaxedMatching)) {
                DateLyricsScore *apple = DateLyricsScoreAtPath(DateLyricsAppleScorePath(task.storeID));
                DateLyricsScore *cached = DateLyricsScoreAtPath(DateLyricsMusixmatchScorePath(task.storeID));
                BOOL wins = !apple.hasWordTiming && (!apple.lines.count || result.hasWordTiming) &&
                    (!cached.hasWordTiming || result.hasWordTiming);
                if (wins && (result.hasWordTiming || !task.hasUsableLines)) {
                    task.hasUsableLines = YES; task.hasWordTiming = result.hasWordTiming;
                    DateLyricsWriteScoreJSON(task.storeID, result.lines, result.hasWordTiming,
                        result.relaxedMatch ? @"musixmatch-relaxed" : @"musixmatch");
                }
            }
            if (!finished && task.hasUsableLines && !gDateLyricsWordHighlighting) {
                DateLyricsCancelProviderLookup(task.storeID);
                return;
            }
            if (!finished) return;
            [gMusixmatchPendingTaskIDs removeObjectForKey:@(task.storeID)];
            [gDateLyricsEngineSubscriptions removeObjectForKey:@(task.storeID)];
            if (!task.hasUsableLines && error) {
                gMusixmatchFailureRetryAfter[@(task.storeID)] = @(NSDate.date.timeIntervalSince1970 + 30);
                DateLyricsRemoveMusixmatchLoadingScore(task.storeID);
            }
        });
    }];
    gDateLyricsEngineSubscriptions[@(task.storeID)] = subscription;
}
