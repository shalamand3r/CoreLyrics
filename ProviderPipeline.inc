// mark: - optional musixmatch rich-sync fallback

// token flow adapted from github.com/ybenyedder/musixmatch-freeapi (mit).
// online lookups remain isolated from apple's lyric queue; network
// callbacks return to glyricsqueue before touching shared state.
static NSString *DateLyricsMusixmatchPercentEncode(NSString *value) {
    if (![value isKindOfClass:NSString.class]) return @"";
    NSMutableCharacterSet *allowed = [[NSCharacterSet alphanumericCharacterSet] mutableCopy];
    [allowed addCharactersInString:@"-._~"];
    return [value stringByAddingPercentEncodingWithAllowedCharacters:allowed] ?: @"";
}

static void DateLyricsCancelProviderRequest(NSString *taskID) {
    if (taskID.length == 0) return;
    NSURLSessionDataTask *request = gDateLyricsProviderRequests[taskID];
    [gDateLyricsProviderRequests removeObjectForKey:taskID];
    [request cancel];
}

static void DateLyricsCancelAllProviderRequests(void) {
    NSArray<NSURLSessionDataTask *> *requests = gDateLyricsProviderRequests.allValues;
    [gDateLyricsProviderRequests removeAllObjects];
    for (NSURLSessionDataTask *request in requests) [request cancel];
}

static void DateLyricsMusixmatchRequest(DateLyricsMusixmatchTask *owner, NSString *urlString, NSDictionary<NSString *, NSString *> *headers, void (^completion)(NSData *, NSInteger, NSError *)) {
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        if (completion) completion(nil, 0, [NSError errorWithDomain:@"DateLyricsProvider" code:-1 userInfo:nil]);
        return;
    }
    if (!gDateLyricsProviderSession) {
        NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        // datelyrics owns the finite wall-clock deadline below. unlimited removes
        // that deadline; transport errors and system limits still apply.
        configuration.HTTPMaximumConnectionsPerHost = 2;
        configuration.timeoutIntervalForRequest = 365.0 * 24.0 * 60.0 * 60.0;
        configuration.timeoutIntervalForResource = configuration.timeoutIntervalForRequest;
        gDateLyricsProviderSession = [NSURLSession sessionWithConfiguration:configuration];
        gDateLyricsProviderRequests = [NSMutableDictionary dictionary];
    }
    NSString *requestKey = owner.taskID ?: @"token";
    DateLyricsCancelProviderRequest(requestKey);
    NSInteger timeout = gDateLyricsRequestTimeout;
    if (owner) {
        ++gLookupIDs[@(owner.storeID)].requestCount;
    } else {
        NSMutableSet<DLCoreLookup *> *requestLookups = [NSMutableSet set];
        for (DLCoreProviderTask *task in gMusixmatchTokenWaiters) {
            DLCoreLookup *lookup = gLookupIDs[@(task.storeID)];
            if (lookup) [requestLookups addObject:lookup];
        }
        for (DLCoreLookup *lookup in requestLookups) ++lookup.requestCount;
    }
    CFTimeInterval startedAt = (gDateLyricsDebugLogging || gPerformanceLogging) ? CACurrentMediaTime() : 0.0;
    DateLyricsDebugLog(@"[Provider:HTTP] begin host=%@ timeout=%@", url.host ?: @"?", DateLyricsRequestTimeoutLabel(timeout));
    NSMutableURLRequest *request = DateLyricsProviderURLRequest(url);
    request.timeoutInterval = 365.0 * 24.0 * 60.0 * 60.0;
    request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [headers enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSString *value, __unused BOOL *stop) {
        [request setValue:value forHTTPHeaderField:key];
    }];

    __block dispatch_source_t deadline = nil;
    __block BOOL expired = NO;
    __block __weak NSURLSessionDataTask *weakNetworkTask;
    NSURLSessionDataTask *networkTask = [gDateLyricsProviderSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(gLyricsQueue, ^{
            if (deadline) { dispatch_source_cancel(deadline); deadline = nil; }
            NSURLSessionDataTask *finished = weakNetworkTask;
            if (!finished || gDateLyricsProviderRequests[requestKey] != finished) return;
            [gDateLyricsProviderRequests removeObjectForKey:requestKey];
            NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
            NSError *resultError = expired ? [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorTimedOut userInfo:nil] : error;
            DateLyricsDebugLog(@"[Provider:HTTP] end host=%@ status=%ld bytes=%lu seconds=%.3f errorCode=%ld",
                               url.host ?: @"?", (long)http.statusCode, (unsigned long)data.length,
                               startedAt > 0.0 ? CACurrentMediaTime() - startedAt : -1.0,
                               (long)resultError.code);
            if (gPerformanceLogging) CLDiagnosticAppend(DLCCacheRoot(), @"CoreLyrics", YES, [NSString stringWithFormat:@"event=http-finish host=%@ status=%ld bytes=%lu seconds=%.3f errorCode=%ld", url.host, (long)http.statusCode, (unsigned long)data.length, CACurrentMediaTime() - startedAt, (long)resultError.code]);
            if (resultError || http.statusCode == 429 || http.statusCode >= 500) {
                NSArray *affected = owner ? @[owner] : [gMusixmatchTokenWaiters copy];
                DLCProviderFailed(owner.provider ?: @"musixmatch", http);
                for (DLCoreProviderTask *task in affected) task.hadTransportFailure = YES;
                if (owner) {
                    DLCFinishTask(owner, @"Provider temporarily unavailable");
                    return;
                }
            }
            if (completion) completion(expired ? nil : data, http.statusCode, resultError);
        });
    }];
    weakNetworkTask = networkTask;
    gDateLyricsProviderRequests[requestKey] = networkTask;
    if (timeout != DateLyricsUnlimitedRequestTimeout) {
        deadline = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gLyricsQueue);
        dispatch_source_set_timer(deadline, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC)), DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(deadline, ^{
            expired = YES;
            [weakNetworkTask cancel];
        });
        dispatch_resume(deadline);
    }
    [networkTask resume];
}

static id DateLyricsMusixmatchFindJSONValue(id object, NSString *key) {
    if ([object isKindOfClass:NSDictionary.class]) {
        NSDictionary *dictionary = (NSDictionary *)object;
        id direct = dictionary[key];
        if (direct) return direct;
        for (id value in dictionary.allValues) {
            id found = DateLyricsMusixmatchFindJSONValue(value, key);
            if (found) return found;
        }
    } else if ([object isKindOfClass:NSArray.class]) {
        for (id value in (NSArray *)object) {
            id found = DateLyricsMusixmatchFindJSONValue(value, key);
            if (found) return found;
        }
    }
    return nil;
}



// remove catalog annotations, while retaining subtitles and performance versions
// (live, acoustic, remix, clean, sped up...) that can change lyrics or timing.
static BOOL DateLyricsIsRemovableMetadataAnnotation(NSString *annotation, BOOL isTitle) {
    NSString *normalized = DateLyricsMusixmatchNormalizedString(annotation);
    static NSRegularExpression *feature;
    static NSRegularExpression *edition;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        feature = [NSRegularExpression regularExpressionWithPattern:@"^(?:feat|featuring|ft)\\s+.+$" options:0 error:nil];
        NSString *descriptor = @"(?:\\d{4}\\s+)?remaster(?:ed)?(?:\\s+\\d{4})?|"
            @"(?:deluxe|special|platinum)(?:\\s+edition)?|"
            @"(?:\\d+(?:st|nd|rd|th)?\\s+)?anniversary(?:\\s+edition)?";
        edition = [NSRegularExpression regularExpressionWithPattern:
            [NSString stringWithFormat:@"^(?:%@)(?:\\s+(?:%@))*$", descriptor, descriptor] options:0 error:nil];
    });
    NSRange range = NSMakeRange(0, normalized.length);
    return [feature firstMatchInString:normalized options:0 range:range] != nil ||
        (isTitle && [edition firstMatchInString:normalized options:0 range:range] != nil);
}

static NSString *DateLyricsCleanMetadata(NSString *value, BOOL isTitle) {
    if (![value isKindOfClass:NSString.class] || value.length == 0) return @"";
    NSMutableString *cleaned = [value mutableCopy];
    for (NSValue *group in [DateLyricsBracketedRanges(cleaned, YES) reverseObjectEnumerator]) {
        NSRange range = group.rangeValue;
        NSString *annotation = [cleaned substringWithRange:NSMakeRange(range.location + 1, range.length - 2)];
        if (DateLyricsIsRemovableMetadataAnnotation(annotation, isTitle)) {
            [cleaned replaceCharactersInRange:range withString:@" "];
        }
    }

    static NSRegularExpression *feature;
    static NSRegularExpression *dash;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        feature = [NSRegularExpression regularExpressionWithPattern:@"\\s+(?:[-–—]\\s+)?(?:feat(?:uring)?\\.?|ft\\.?)\\s+[^()\\[\\]{}]+$"
            options:NSRegularExpressionCaseInsensitive error:nil];
        dash = [NSRegularExpression regularExpressionWithPattern:@"\\s+[-–—]\\s+" options:0 error:nil];
    });
    [feature replaceMatchesInString:cleaned options:0 range:NSMakeRange(0, cleaned.length) withTemplate:@""];
    while (isTitle) {
        NSTextCheckingResult *separator = [dash matchesInString:cleaned options:0 range:NSMakeRange(0, cleaned.length)].lastObject;
        if (!separator || !DateLyricsIsRemovableMetadataAnnotation([cleaned substringFromIndex:NSMaxRange(separator.range)], YES)) break;
        [cleaned deleteCharactersInRange:NSMakeRange(separator.range.location, cleaned.length - separator.range.location)];
    }
    NSString *trimmed = DateLyricsCollapseWhitespace(cleaned);
    return trimmed.length > 0 ? trimmed : DateLyricsCollapseWhitespace(value);
}

static NSString *DateLyricsCleanTrackTitle(NSString *title) {
    return DateLyricsCleanMetadata(title, YES);
}

static NSString *DateLyricsCleanArtist(NSString *artist) {
    return DateLyricsCleanMetadata(artist, NO);
}

// a spaced slash is a list separator; an embedded slash belongs to the act's
// name (e.g. ac/dc). ampersands remain part of the identity of a group.
static NSArray<NSString *> *DateLyricsSplitArtists(NSString *artist) {
    NSString *separated = [artist stringByReplacingOccurrencesOfString:@" / " withString:@";"];
    NSArray<NSString *> *parts = [separated componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@",;"]];
    NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:parts.count];
    for (NSString *part in parts) {
        NSString *trimmed = [part stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (trimmed.length > 0) [result addObject:trimmed];
    }
    return result;
}

static BOOL DateLyricsTitleMatches(NSString *rawWanted, NSString *rawCandidate) {
    if (![rawWanted isKindOfClass:NSString.class] || ![rawCandidate isKindOfClass:NSString.class]) return NO;
    if (rawWanted.length == 0 || rawCandidate.length == 0) return NO;

    NSString *wanted = DateLyricsMusixmatchNormalizedString(DateLyricsCleanTrackTitle(rawWanted));
    NSString *candidate = DateLyricsMusixmatchNormalizedString(DateLyricsCleanTrackTitle(rawCandidate));
    // overlapping lyric text is useful when merging timing, but overlapping
    // title words are not evidence that two catalog entries are the same song.
    return wanted.length > 0 && [wanted isEqualToString:candidate];
}

static BOOL DateLyricsArtistMatches(NSString *rawWanted, NSString *rawCandidate) {
    if (![rawWanted isKindOfClass:NSString.class] || rawWanted.length == 0) return NO;
    if (![rawCandidate isKindOfClass:NSString.class] || rawCandidate.length == 0) return NO;

    NSString *cleanWanted = DateLyricsCleanArtist(rawWanted);
    NSString *cleanCandidate = DateLyricsCleanArtist(rawCandidate);

    NSString *normWanted = DateLyricsMusixmatchNormalizedString(cleanWanted);
    NSString *normCandidate = DateLyricsMusixmatchNormalizedString(cleanCandidate);
    if (normWanted.length == 0) return NO;
    if (normCandidate.length == 0) return NO;

    if ([normWanted isEqualToString:normCandidate]) return YES;

    // multi-artist splitting: if either side lists multiple artists separated
    // by , ; or /, check whether any individual segment from one side matches
    // any segment from the other.  handles "swae lee, post malone" vs
    // "post malone" regardless of ordering.
    NSArray<NSString *> *wantedSegments = DateLyricsSplitArtists(cleanWanted);
    NSArray<NSString *> *candidateSegments = DateLyricsSplitArtists(cleanCandidate);
    if (wantedSegments.count > 1 || candidateSegments.count > 1) {
        for (NSString *ws in wantedSegments) {
            NSString *normWS = DateLyricsMusixmatchNormalizedString(ws);
            if (normWS.length == 0) continue;
            for (NSString *cs in candidateSegments) {
                NSString *normCS = DateLyricsMusixmatchNormalizedString(cs);
                if (normCS.length == 0) continue;
                if ([normWS isEqualToString:normCS]) return YES;
            }
        }
    }

    return NO;
}

// bounded edit distance on composed characters, after the existing metadata
// normalization. this measures spelling similarity, not recording confidence.
static double DateLyricsNameSimilarity(NSString *left, NSString *right) {
    if (left.length == 0 || right.length == 0 || left.length > 192 || right.length > 192) return 0.0;
    NSMutableArray<NSString *> *a = [NSMutableArray array], *b = [NSMutableArray array];
    for (NSArray *pair in @[@[left, a], @[right, b]]) {
        NSString *text = pair[0];
        NSMutableArray *characters = pair[1];
        [text enumerateSubstringsInRange:NSMakeRange(0, text.length) options:NSStringEnumerationByComposedCharacterSequences
            usingBlock:^(NSString *part, __unused NSRange range, __unused NSRange enclosing, __unused BOOL *stop) {
                [characters addObject:part];
            }];
    }
    if (a.count > 96 || b.count > 96) return 0.0;
    NSUInteger previous[97], current[97];
    for (NSUInteger j = 0; j <= b.count; j++) previous[j] = j;
    for (NSUInteger i = 1; i <= a.count; i++) {
        current[0] = i;
        for (NSUInteger j = 1; j <= b.count; j++) {
            NSUInteger substitute = previous[j - 1] + (![a[i - 1] isEqualToString:b[j - 1]]);
            current[j] = MIN(substitute, MIN(previous[j] + 1, current[j - 1] + 1));
        }
        memcpy(previous, current, (b.count + 1) * sizeof(NSUInteger));
    }
    return 1.0 - (double)previous[b.count] / MAX(a.count, b.count);
}

static BOOL DateLyricsFuzzyArtistMatches(NSString *wanted, NSString *candidate) {
    NSArray *left = DateLyricsSplitArtists(DateLyricsCleanArtist(wanted));
    NSArray *right = DateLyricsSplitArtists(DateLyricsCleanArtist(candidate));
    for (NSString *a in left) for (NSString *b in right) {
        NSString *na = DateLyricsMusixmatchNormalizedString(a), *nb = DateLyricsMusixmatchNormalizedString(b);
        if (MIN(na.length, nb.length) < 8) continue;
        if ([na componentsSeparatedByString:@" "].count != [nb componentsSeparatedByString:@" "].count) continue;
        if (DateLyricsNameSimilarity(na, nb) >= 0.82) return YES;
    }
    return NO;
}

// keep meaningful annotations, performance words and numbers exact even when
// a longer title has a typo. "part 1" and "part 2" cannot become fuzzy matches.
static NSArray<NSString *> *DateLyricsTitleVersionSignature(NSString *title) {
    NSMutableArray *signature = [NSMutableArray array];
    NSString *clean = DateLyricsCleanTrackTitle(title);
    for (NSValue *value in DateLyricsBracketedRanges(clean, YES)) {
        [signature addObject:DateLyricsMusixmatchNormalizedString([clean substringWithRange:value.rangeValue])];
    }
    static NSRegularExpression *versions, *suffixes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        versions = [NSRegularExpression regularExpressionWithPattern:@"\\b(?:live|acoustic|remix|mix|instrumental|karaoke|cover|demo|clean|explicit|sped up|slowed|reverb|edit|extended|version)\\b|[0-9]+" options:0 error:nil];
        suffixes = [NSRegularExpression regularExpressionWithPattern:@"\\s+[-–—]\\s+(.+)$" options:0 error:nil];
    });
    NSString *normalized = DateLyricsMusixmatchNormalizedString(clean);
    for (NSTextCheckingResult *match in [versions matchesInString:normalized options:0 range:NSMakeRange(0, normalized.length)]) {
        [signature addObject:[normalized substringWithRange:match.range]];
    }
    NSTextCheckingResult *suffix = [suffixes firstMatchInString:clean options:0 range:NSMakeRange(0, clean.length)];
    if (suffix) [signature addObject:DateLyricsMusixmatchNormalizedString([clean substringWithRange:[suffix rangeAtIndex:1]])];
    return signature;
}

static BOOL DateLyricsFuzzyTitleMatches(NSString *wanted, NSString *candidate) {
    NSString *left = DateLyricsMusixmatchNormalizedString(DateLyricsCleanTrackTitle(wanted));
    NSString *right = DateLyricsMusixmatchNormalizedString(DateLyricsCleanTrackTitle(candidate));
    if (MIN(left.length, right.length) < 10 || [left componentsSeparatedByString:@" "].count < 2 ||
        [right componentsSeparatedByString:@" "].count < 2) return NO;
    if (![DateLyricsTitleVersionSignature(wanted) isEqual:DateLyricsTitleVersionSignature(candidate)]) return NO;
    return DateLyricsNameSimilarity(left, right) >= 0.90;
}

static BOOL DateLyricsMetadataMatchesStrictly(NSString *title, NSString *artist, DateLyricsMusixmatchTask *task) {
    return DateLyricsTitleMatches(task.title, title) && DateLyricsArtistMatches(task.artist, artist);
}

static BOOL DateLyricsProviderTrackIsTrusted(NSString *title, NSString *artist, id duration, DateLyricsMusixmatchTask *task) {
    BOOL titleMatches = DateLyricsTitleMatches(task.title, title);
    BOOL artistMatches = DateLyricsArtistMatches(task.artist, artist);
    double returnedDuration = DateLyricsJSONDouble(duration, 0.0);
    double difference = returnedDuration > 0.0 && task.duration > 0.0
        ? fabs(returnedDuration - task.duration) : DBL_MAX;
    BOOL fuzzyArtist = NO, fuzzyTitle = NO;
    // short titles (under 6 chars, e.g. "need", "red", "run") are prone to false positives; require exact title match even in relaxed mode.
    BOOL isShortTitle = DateLyricsMusixmatchNormalizedString(DateLyricsCleanTrackTitle(task.title)).length <= 5;
    if (gDateLyricsRelaxedMatching && difference <= 3.0 && (!titleMatches || !artistMatches)) {
        fuzzyArtist = titleMatches && DateLyricsFuzzyArtistMatches(task.artist, artist);
        fuzzyTitle = !isShortTitle && artistMatches && DateLyricsFuzzyTitleMatches(task.title, title);
    }
    if ((!titleMatches || !artistMatches) && !fuzzyArtist && !fuzzyTitle) {
        DateLyricsDebugLog(@"[Provider:MATCH] reject storeID=%ld title=%d artist=%d relaxed=%d isShortTitle=%d reason=identity",
            (long)task.storeID, titleMatches, artistMatches, gDateLyricsRelaxedMatching, isShortTitle);
        return NO;
    }
    // limit max duration difference across all services: reject if delta > 6.0s
    if (difference != DBL_MAX && difference > 6.0) {
        DateLyricsDebugLog(@"[Provider:MATCH] reject storeID=%ld wantedDuration=%.3f returnedDuration=%.3f delta=%.3f reason=duration",
            (long)task.storeID, task.duration, returnedDuration, difference);
        return NO;
    }
    BOOL trusted = (task.appleTier != DateLyricsSyncTierLine && !task.hasUsableLines) || difference <= 3.0;
    DateLyricsDebugLog(@"[Provider:MATCH] result=%d storeID=%ld wantedDuration=%.3f returnedDuration=%.3f delta=%.3f reason=%@",
        trusted, (long)task.storeID, task.duration, returnedDuration, difference,
        !trusted ? @"upgrade-recording-mismatch" : fuzzyArtist ? @"relaxed-artist-exact-title" : fuzzyTitle ? @"relaxed-title-exact-artist" : @"strict-identity");
    return trusted;
}

static NSArray<NSDictionary *> *DateLyricsProviderQueries(DateLyricsMusixmatchTask *task) {
    NSString *title = DateLyricsCleanTrackTitle(task.title);
    NSString *artist = DateLyricsCleanArtist(task.artist);
    NSString *primaryArtist = DateLyricsSplitArtists(artist).firstObject ?: artist;
    NSArray *variants = @[
        @{ @"title": task.title ?: @"", @"artist": task.artist ?: @"", @"album": task.album ?: @"" },
        @{ @"title": title, @"artist": primaryArtist, @"album": @"" },
        @{ @"title": title, @"artist": @"", @"album": @"" }
    ];
    NSMutableArray *queries = [NSMutableArray array];
    for (NSDictionary *query in variants) {
        if ([query[@"title"] length] > 0 && ![queries containsObject:query]) [queries addObject:query];
    }
    return queries;
}

static NSArray<NSDictionary *> *DateLyricsRankProviderCandidates(NSArray *candidates, DateLyricsMusixmatchTask *task) {
    NSMutableArray *trusted = [NSMutableArray array];
    for (id candidate in candidates) {
        if (![candidate isKindOfClass:NSDictionary.class]) continue;
        if (DateLyricsProviderTrackIsTrusted(candidate[@"title"], candidate[@"artist"], candidate[@"duration"], task)) [trusted addObject:candidate];
    }
    return [trusted sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        BOOL leftStrict = DateLyricsMetadataMatchesStrictly(left[@"title"], left[@"artist"], task);
        BOOL rightStrict = DateLyricsMetadataMatchesStrictly(right[@"title"], right[@"artist"], task);
        if (leftStrict != rightStrict) return leftStrict ? NSOrderedAscending : NSOrderedDescending;
        double ld = DateLyricsJSONDouble(left[@"duration"], 0.0), rd = DateLyricsJSONDouble(right[@"duration"], 0.0);
        double ldelta = ld > 0.0 && task.duration > 0.0 ? fabs(ld - task.duration) : DBL_MAX;
        double rdelta = rd > 0.0 && task.duration > 0.0 ? fabs(rd - task.duration) : DBL_MAX;
        if (ldelta != rdelta) return ldelta < rdelta ? NSOrderedAscending : NSOrderedDescending;
        NSString *wantedArtist = DateLyricsMusixmatchNormalizedString(DateLyricsCleanArtist(task.artist));
        BOOL la = [DateLyricsMusixmatchNormalizedString(DateLyricsCleanArtist(left[@"artist"])) isEqual:wantedArtist];
        BOOL ra = [DateLyricsMusixmatchNormalizedString(DateLyricsCleanArtist(right[@"artist"])) isEqual:wantedArtist];
        if (la != ra) return la ? NSOrderedAscending : NSOrderedDescending;
        NSString *wantedAlbum = DateLyricsMusixmatchNormalizedString(task.album);
        BOOL lalbum = wantedAlbum.length > 0 && [DateLyricsMusixmatchNormalizedString(left[@"album"]) isEqual:wantedAlbum];
        BOOL ralbum = wantedAlbum.length > 0 && [DateLyricsMusixmatchNormalizedString(right[@"album"]) isEqual:wantedAlbum];
        if (lalbum != ralbum) return lalbum ? NSOrderedAscending : NSOrderedDescending;
        return NSOrderedSame;
    }];
}

static NSArray<DateLyricsTimedLine *> *DateLyricsMusixmatchLinesFromBody(id body, NSTimeInterval trackDuration) {
    id parsedBody = body;
    if ([parsedBody isKindOfClass:NSString.class]) {
        NSData *data = [(NSString *)parsedBody dataUsingEncoding:NSUTF8StringEncoding];
        parsedBody = data.length ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    }
    if (![parsedBody isKindOfClass:NSArray.class]) {
        DateLyricsDebugLog(@"[Parser:RichSync] reject reason=body-type class=%@", NSStringFromClass([parsedBody class]) ?: @"nil");
        return nil;
    }

    NSMutableArray<DateLyricsTimedLine *> *lines = [NSMutableArray array];
    NSArray *rawLines = (NSArray *)parsedBody;
    for (NSUInteger lineIndex = 0; lineIndex < rawLines.count; lineIndex++) {
        id rawLine = rawLines[lineIndex];
        if (![rawLine isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *lineDictionary = (NSDictionary *)rawLine;
        double begin = DateLyricsJSONDouble(lineDictionary[@"ts"], -1.0);
        double end = DateLyricsJSONDouble(lineDictionary[@"te"], -1.0);
        if (begin < 0.0 || (isfinite(trackDuration) && trackDuration > 0.0 && begin >= trackDuration)) continue;
        if (end <= begin) {
            for (NSUInteger nextIndex = lineIndex + 1; nextIndex < rawLines.count; nextIndex++) {
                id nextLine = rawLines[nextIndex];
                if (![nextLine isKindOfClass:NSDictionary.class]) continue;
                double nextBegin = DateLyricsJSONDouble(nextLine[@"ts"], -1.0);
                if (nextBegin > begin) {
                    end = nextBegin;
                    break;
                }
            }
        }
        if (end <= begin && trackDuration > begin) end = trackDuration;
        if (end <= begin) end = begin + 5.0;
        if (isfinite(trackDuration) && trackDuration > 0.0) end = MIN(end, trackDuration);
        if (!isfinite(end) || end <= begin) continue;

        NSArray *rawWords = [lineDictionary[@"l"] isKindOfClass:NSArray.class] ? lineDictionary[@"l"] : @[];
        NSMutableArray<DateLyricsTimedWord *> *words = [NSMutableArray array];
        NSMutableString *assembledText = [NSMutableString string];
        NSMutableString *pendingSeparator = [NSMutableString string];
        BOOL canonicalMappingFailed = NO;
        BOOL invalidWordTiming = NO;
        double previousOffset = -1.0;
        for (NSUInteger index = 0; index < rawWords.count; index++) {
            id rawWord = rawWords[index];
            if (![rawWord isKindOfClass:NSDictionary.class]) continue;
            NSString *text = [rawWord[@"c"] isKindOfClass:NSString.class] ? rawWord[@"c"] : @"";
            if (text.length == 0) continue;

            // rich-sync represents spaces as their own timed fragments.  keep
            // those characters in the line text, but store them as a separator
            // instead of a highlightable word; otherwise the active range can
            // land on a blank and long-line splitting counts spaces as words.
            BOOL isWhitespace = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]].length == 0;
            if (isWhitespace) {
                [pendingSeparator appendString:text];
                continue;
            }

            NSString *canonicalLine = [lineDictionary[@"x"] isKindOfClass:NSString.class]
                ? [lineDictionary[@"x"] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
                : nil;
            NSString *separator = [pendingSeparator copy];
            if (canonicalLine.length > 0) {
                NSUInteger cursor = assembledText.length;
                NSRange found = [canonicalLine rangeOfString:text options:0 range:NSMakeRange(MIN(cursor, canonicalLine.length), canonicalLine.length - MIN(cursor, canonicalLine.length))];
                if (found.location != NSNotFound) {
                    separator = [canonicalLine substringWithRange:NSMakeRange(cursor, found.location - cursor)];
                } else {
                    canonicalMappingFailed = YES;
                }
            } else if (separator.length == 0 && words.count > 0) {
                separator = @" ";
            }
            [assembledText appendString:separator];
            [assembledText appendString:text];

            double offset = DateLyricsJSONDouble(rawWord[@"o"], -1.0);
            double wordBegin = begin + offset;
            if (offset < 0.0 || offset < previousOffset || !isfinite(wordBegin) || wordBegin >= end) invalidWordTiming = YES;
            previousOffset = offset;
            double nextOffset = end - begin;

            for (NSUInteger nextIndex = index + 1; nextIndex < rawWords.count; nextIndex++) {
                NSDictionary *nextWord =
                    [rawWords[nextIndex] isKindOfClass:NSDictionary.class]
                        ? rawWords[nextIndex]
                        : nil;
                if (!nextWord) continue;

                NSString *nextText =
                    [nextWord[@"c"] isKindOfClass:NSString.class]
                        ? nextWord[@"c"]
                        : @"";

                if ([[nextText stringByTrimmingCharactersInSet:
                        [NSCharacterSet whitespaceAndNewlineCharacterSet]] length] == 0) {
                    continue;
                }

                double candidate = DateLyricsJSONDouble(nextWord[@"o"], -1.0);
                if (candidate > offset) {
                    nextOffset = candidate;
                    break;
                }
            }

            double wordEnd = MIN(end, begin + nextOffset);
            if (wordEnd <= wordBegin) {
                wordEnd = MIN(end, wordBegin + 0.05);
            }

            DateLyricsTimedWord *word = [DateLyricsTimedWord new];
            word.begin = wordBegin;
            word.end = wordEnd;
            word.text = text;
            word.separatorBefore = separator;
            [pendingSeparator setString:@""];
            word.background = NO;
            [words addObject:word];
        }

        NSString *lineText = [lineDictionary[@"x"] isKindOfClass:NSString.class]
            ? [lineDictionary[@"x"] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
            : [assembledText stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (lineText.length == 0) continue;

        DateLyricsTimedLine *line = [DateLyricsTimedLine new];
        line.begin = begin;
        line.end = end;
        line.text = lineText;
        // a partial timing map must never be advertised as complete karaoke.
        if (![assembledText isEqualToString:lineText]) canonicalMappingFailed = YES;
        if (canonicalMappingFailed || invalidWordTiming) {
            DateLyricsDebugLog(@"[Parser:RichSync] downgrade begin=%.3f end=%.3f words=%lu mappingInvalid=%d timingInvalid=%d",
                begin, end, (unsigned long)words.count, canonicalMappingFailed, invalidWordTiming);
        }
        line.words = (canonicalMappingFailed || invalidWordTiming) ? @[] : words;
        line.canonicalMapped = [lineDictionary[@"x"] isKindOfClass:NSString.class] && !canonicalMappingFailed;
        [lines addObject:line];
    }

    [lines sortUsingComparator:^NSComparisonResult(DateLyricsTimedLine *left, DateLyricsTimedLine *right) {
        return left.begin < right.begin ? NSOrderedAscending : (left.begin > right.begin ? NSOrderedDescending : NSOrderedSame);
    }];

    for (DateLyricsTimedLine *line in lines) {
        if (!line.canonicalMapped) DateLyricsNormalizeWordLines(@[line]);
        DateLyricsAssignCharacterRanges(line);
    }
    DateLyricsDebugLog(@"[Parser:RichSync] complete inputLines=%lu usableLines=%lu duration=%.3f",
        (unsigned long)rawLines.count, (unsigned long)lines.count, trackDuration);
    return lines.count > 0 ? [lines copy] : nil;
}

static NSArray<DateLyricsTimedLine *> *DateLyricsMusixmatchLinesFromLRC(NSString *body, NSTimeInterval trackDuration) {
    if (![body isKindOfClass:NSString.class] || body.length == 0) return nil;
    static NSRegularExpression *timestampExpression = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        timestampExpression = [NSRegularExpression regularExpressionWithPattern:@"\\[([0-9]+):([0-9]{1,2}(?:\\.[0-9]+)?)\\]" options:0 error:nil];
    });

    NSMutableArray<DateLyricsTimedLine *> *lines = [NSMutableArray array];
    __block __unused NSUInteger inputLines = 0;
    __block __unused NSUInteger rejectedTimestamps = 0;
    __block __unused NSUInteger repeatedTimestampLines = 0;
    [body enumerateLinesUsingBlock:^(NSString *rawLine, __unused BOOL *stop) {
        inputLines++;
        NSString *input = [rawLine stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if ([input hasPrefix:@"\uFEFF"]) input = [input substringFromIndex:1];
        NSMutableArray<NSNumber *> *timestamps = [NSMutableArray array];
        NSUInteger cursor = 0;
        while (cursor < input.length) {
            NSTextCheckingResult *match = [timestampExpression firstMatchInString:input options:NSMatchingAnchored
                range:NSMakeRange(cursor, input.length - cursor)];
            if (!match) break;
            double minutes = [[input substringWithRange:[match rangeAtIndex:1]] doubleValue];
            double seconds = [[input substringWithRange:[match rangeAtIndex:2]] doubleValue];
            double begin = minutes * 60.0 + seconds;
            if (seconds < 60.0 && isfinite(begin) &&
                (!isfinite(trackDuration) || trackDuration <= 0.0 || begin < trackDuration)) {
                [timestamps addObject:@(begin)];
            } else {
                rejectedTimestamps++;
            }
            cursor = NSMaxRange(match.range);
        }
        NSString *text = [[input substringFromIndex:cursor] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (timestamps.count > 1) repeatedTimestampLines++;
        for (NSNumber *timestamp in timestamps) {
            DateLyricsTimedLine *line = [DateLyricsTimedLine new];
            line.begin = timestamp.doubleValue;
            line.text = text;
            line.words = @[];
            [lines addObject:line];
        }
    }];
    [lines sortUsingComparator:^NSComparisonResult(DateLyricsTimedLine *left, DateLyricsTimedLine *right) {
        return left.begin < right.begin ? NSOrderedAscending : (left.begin > right.begin ? NSOrderedDescending : NSOrderedSame);
    }];
    // empty timestamped entries are intentional silence boundaries. keep them
    // until ends are assigned, and give simultaneous entries the same next edge.
    NSTimeInterval nextBegin = isfinite(trackDuration) ? trackDuration : 0.0;
    NSTimeInterval laterBegin = DBL_MAX;
    for (DateLyricsTimedLine *line in lines.reverseObjectEnumerator) {
        if (laterBegin < DBL_MAX && laterBegin > line.begin) nextBegin = laterBegin;
        line.end = nextBegin > line.begin ? nextBegin : line.begin + 5.0;
        laterBegin = line.begin;
    }
    NSMutableArray *usable = [NSMutableArray array];
    for (DateLyricsTimedLine *line in lines) {
        if (line.text.length > 0 && isfinite(line.end) && line.end > line.begin) [usable addObject:line];
    }
    DateLyricsDebugLog(@"[Parser:LRC] complete inputLines=%lu timedEntries=%lu usableLines=%lu rejectedTimestamps=%lu repeatedTimestampLines=%lu duration=%.3f",
        (unsigned long)inputLines, (unsigned long)lines.count, (unsigned long)usable.count,
        (unsigned long)rejectedTimestamps, (unsigned long)repeatedTimestampLines, trackDuration);
    return usable.count > 0 ? [usable copy] : nil;
}



static BOOL DateLyricsSerializeMusixmatchLines(NSInteger storeID, NSArray<DateLyricsTimedLine *> *lines, DateLyricsSyncTier appleTier, BOOL relaxedMatch) {
    return DLCPublishLines(storeID, lines, relaxedMatch);
}


static BOOL DateLyricsMusixmatchTaskIsCurrent(DateLyricsMusixmatchTask *task);

static void DateLyricsMusixmatchFinishTask(DateLyricsMusixmatchTask *task, NSString *reason);
static void DateLyricsMusixmatchEnsureToken(DateLyricsMusixmatchTask *task, BOOL forceRefresh);
static NSDictionary *DateLyricsMusixmatchJSON(NSData *data);
static void DateLyricsStepFallback(DateLyricsMusixmatchTask *task, NSString *reason);

static NSString *DateLyricsUnescapeLRC(NSString *lrc) {
    if (!lrc || lrc.length == 0) return @"";
    NSString *clean = [lrc stringByReplacingOccurrencesOfString:@"&apos;" withString:@"'"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&quot;" withString:@"\""];
    clean = [clean stringByReplacingOccurrencesOfString:@"&amp;" withString:@"&"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&lt;" withString:@"<"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&gt;" withString:@">"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&#39;" withString:@"'"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&#38;" withString:@"&"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&#32;" withString:@" "];
    clean = [clean stringByReplacingOccurrencesOfString:@"&#58;" withString:@":"];
    clean = [clean stringByReplacingOccurrencesOfString:@"&#10;" withString:@"\n"];
    return clean;
}

static void DateLyricsStepFallback(DateLyricsMusixmatchTask *task, NSString *reason) {
    DLCFinishTask(task, reason);
}
static void DateLyricsMusixmatchFinishTask(DateLyricsMusixmatchTask *task, NSString *reason) {
    DLCFinishTask(task, reason);
}
static void DateLyricsProviderSearch(DateLyricsMusixmatchTask *task);
static void DateLyricsStartProvider(DateLyricsMusixmatchTask *task) {
    task.searchQueries = DateLyricsProviderQueries(task);
    task.searchedURLs = [NSMutableSet set];
    task.attemptedCandidates = [NSMutableSet set];
    DateLyricsDebugLog(@"[Provider:START] storeID=%ld provider=%@", (long)task.storeID, task.provider);
    if ([task.provider isEqualToString:@"musixmatch"]) DateLyricsMusixmatchEnsureToken(task, NO);
    else DateLyricsProviderSearch(task);
}

// providers share discovery, ranking and a three-candidate fetch budget.
static NSDictionary *DateLyricsProviderHeaders(NSString *provider) {
    if ([provider isEqualToString:@"lrclib"]) {
        NSString *client = @"DateLyrics v4.0.8 (https://github.com/shalamand3r/DateLyrics)";
        return @{ @"User-Agent": client, @"Lrclib-Client": client, @"Accept": @"application/json" };
    }
    if ([provider isEqualToString:@"megalobiz"]) {
        return @{ @"User-Agent": @"DateLyrics/4.0.8 (iOS Lock Screen Lyrics)", @"Accept": @"application/json" };
    }
    return @{ @"Referer": [provider isEqualToString:@"netease"] ? @"https://music.163.com/" : @"https://y.qq.com/",
              @"User-Agent": @"Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15",
              @"Accept": @"application/json" };
}

static NSString *DateLyricsProviderSearchURL(NSString *provider, NSDictionary *query, NSTimeInterval duration, BOOL exact) {
    NSString *title = DateLyricsMusixmatchPercentEncode(query[@"title"]);
    NSString *artist = DateLyricsMusixmatchPercentEncode(query[@"artist"]);
    if ([provider isEqualToString:@"lrclib"]) {
        NSMutableString *url = [NSMutableString stringWithFormat:@"https://lrclib.net/api/%@?track_name=%@", exact ? @"get" : @"search", title];
        if (artist.length > 0) [url appendFormat:@"&artist_name=%@", artist];
        if (exact) {
            NSString *album = DateLyricsMusixmatchPercentEncode(query[@"album"]);
            if (album.length > 0) [url appendFormat:@"&album_name=%@", album];
            if (duration > 0.0) [url appendFormat:@"&duration=%ld", (long)llround(duration)];
        }
        return url;
    }
    NSString *words = [query[@"artist"] length] > 0
        ? [NSString stringWithFormat:@"%@ %@", query[@"title"], query[@"artist"]] : query[@"title"];
    if ([provider isEqualToString:@"megalobiz"]) {
        return [NSString stringWithFormat:@"https://api.megalobiz.com/api/public/v1/search?q=%@", DateLyricsMusixmatchPercentEncode(words)];
    }
    if ([provider isEqualToString:@"netease"]) {
        return [NSString stringWithFormat:@"https://music.163.com/api/cloudsearch/pc?s=%@&type=1&limit=10", DateLyricsMusixmatchPercentEncode(words)];
    }
    NSDictionary *payload = @{
        @"comm": @{ @"ct": @26, @"cv": @0 },
        @"req": @{ @"module": @"music.search.SearchCgiService", @"method": @"DoSearchForQQMusicDesktop",
                   @"param": @{ @"search_type": @0, @"query": words, @"page_num": @1, @"num_per_page": @10 } }
    };
    NSData *data = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    NSString *json = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
    return [NSString stringWithFormat:@"https://u.y.qq.com/cgi-bin/musicu.fcg?data=%@", DateLyricsMusixmatchPercentEncode(json)];
}

static NSArray<NSDictionary *> *DateLyricsProviderCandidates(NSString *provider, id json, BOOL exact) {
    NSArray *songs;
    if ([provider isEqualToString:@"lrclib"]) {
        songs = exact ? (DateLyricsJSONDictionary(json) ? @[json] : @[]) : DateLyricsJSONArray(json);
    } else if ([provider isEqualToString:@"megalobiz"]) {
        songs = DateLyricsJSONArray(DateLyricsJSONDictionary(json)[@"results"]);
    } else if ([provider isEqualToString:@"netease"]) {
        songs = DateLyricsJSONArray(DateLyricsJSONDictionary(DateLyricsJSONDictionary(json)[@"result"])[@"songs"]);
    } else {
        NSDictionary *root = DateLyricsJSONDictionary(json);
        NSDictionary *req = DateLyricsJSONDictionary(root[@"req"]);
        NSDictionary *data = DateLyricsJSONDictionary(req[@"data"]);
        NSDictionary *body = DateLyricsJSONDictionary(data[@"body"]);
        songs = DateLyricsJSONArray(DateLyricsJSONDictionary(body[@"song"])[@"list"]);
        if (songs.count == 0) {
            NSDictionary *legacy = DateLyricsJSONDictionary(root[@"data"]);
            songs = DateLyricsJSONArray(DateLyricsJSONDictionary(legacy[@"song"])[@"list"]);
        }
    }
    NSMutableArray *candidates = [NSMutableArray array];
    for (id value in songs) {
        NSDictionary *song = DateLyricsJSONDictionary(value);
        if (!song) continue;
        if ([provider isEqualToString:@"lrclib"]) {
            if (DateLyricsJSONNumber(song[@"instrumental"]).boolValue) continue;
            [candidates addObject:@{ @"title": DateLyricsJSONString(song[@"trackName"]) ?: @"",
                @"artist": DateLyricsJSONString(song[@"artistName"]) ?: @"", @"album": DateLyricsJSONString(song[@"albumName"]) ?: @"",
                @"duration": @(DateLyricsJSONDouble(song[@"duration"], 0.0)), @"synced": DateLyricsJSONString(song[@"syncedLyrics"]) ?: @"" }];
            continue;
        }
        if ([provider isEqualToString:@"megalobiz"]) {
            double songId = DateLyricsJSONDouble(song[@"id"], 0.0);
            if (songId <= 0.0 || songId >= 0x1p53 || floor(songId) != songId) continue;
            [candidates addObject:@{ @"title": DateLyricsJSONString(song[@"title"]) ?: @"",
                @"artist": DateLyricsJSONString(song[@"artist"]) ?: @"",
                @"duration": @(DateLyricsJSONDouble(song[@"duration"], 0.0)),
                @"id": [NSString stringWithFormat:@"%.0f", songId] }];
            continue;
        }
        BOOL netease = [provider isEqualToString:@"netease"];
        NSMutableArray *artists = [NSMutableArray array];
        for (id entry in DateLyricsJSONArray(song[netease ? @"ar" : @"singer"])) {
            NSString *name = DateLyricsJSONString(DateLyricsJSONDictionary(entry)[@"name"]);
            if (name.length > 0) [artists addObject:name];
        }
        NSString *identifier = nil;
        if (netease) {
            double number = DateLyricsJSONDouble(song[@"id"], 0.0);
            if (number > 0.0 && number < 0x1p53 && floor(number) == number) identifier = [NSString stringWithFormat:@"%lld", (long long)number];
        } else {
            identifier = DateLyricsJSONString(song[@"mid"]) ?: DateLyricsJSONString(song[@"songmid"]);
        }
        if (identifier.length == 0) continue;
        // qq's canonical name excludes display-only content-rating labels.
        NSString *title = DateLyricsJSONString(song[@"name"]) ?: DateLyricsJSONString(song[@"title"])
            ?: DateLyricsJSONString(song[@"songname"]) ?: @"";
        NSDictionary *album = DateLyricsJSONDictionary(song[netease ? @"al" : @"album"]);
        [candidates addObject:@{ @"title": title, @"artist": [artists componentsJoinedByString:@"; "],
            @"album": DateLyricsJSONString(album[@"name"]) ?: @"", @"id": identifier,
            @"duration": @(DateLyricsJSONDouble(song[netease ? @"dt" : @"interval"], 0.0) / (netease ? 1000.0 : 1.0)) }];
    }
    return candidates;
}

static void DateLyricsProviderSearch(DateLyricsMusixmatchTask *task);
static void DateLyricsProviderTryCandidate(DateLyricsMusixmatchTask *task);

static NSArray<DateLyricsTimedLine *> *DateLyricsProviderTimedLines(NSArray<NSDictionary *> *entries) {
    NSMutableArray *lines = [NSMutableArray array];
    for (NSDictionary *entry in entries) {
        DateLyricsTimedLine *line = [DateLyricsTimedLine new];
        line.begin = [entry[@"begin"] doubleValue];
        line.end = [entry[@"end"] doubleValue];
        line.text = entry[@"text"];
        NSMutableArray *words = [NSMutableArray array];
        for (NSDictionary *item in entry[@"words"]) {
            DateLyricsTimedWord *word = [DateLyricsTimedWord new];
            word.begin = [item[@"b"] doubleValue]; word.end = [item[@"e"] doubleValue];
            word.text = item[@"t"]; word.separatorBefore = item[@"s"]; word.background = NO;
            [words addObject:word];
        }
        line.words = words;
        DateLyricsAssignCharacterRanges(line);
        [lines addObject:line];
    }
    return lines;
}

static BOOL DateLyricsPublishProviderLines(DateLyricsMusixmatchTask *task, NSArray<DateLyricsTimedLine *> *lines) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task) || !lines.count || (task.relaxedMatch && !gDateLyricsRelaxedMatching)) return NO;
    BOOL hasWords = NO;
    for (DateLyricsTimedLine *line in lines) hasWords |= line.words.count > 0;
    if (task.hasWordTiming || (task.hasUsableLines && !hasWords)) return NO;
    if (!DateLyricsSerializeMusixmatchLines(task.storeID, lines, task.appleTier, task.relaxedMatch)) return NO;
    task.hasUsableLines = YES;
    task.hasWordTiming |= hasWords;
    return YES;
}

static void DateLyricsProviderFetchWordTiming(DateLyricsMusixmatchTask *task, NSDictionary *candidate) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    BOOL netease = [task.provider isEqualToString:@"netease"];
    NSString *url = netease ? DateLyricsNetEaseWordURL(candidate[@"id"]) : DateLyricsQQWordURL(candidate[@"id"]);
    if (!url.length) { DateLyricsProviderTryCandidate(task); return; }
    DateLyricsMusixmatchRequest(task, url, DateLyricsProviderHeaders(task.provider), ^(NSData *data, NSInteger status, NSError *error) {
        dispatch_async(gLyricsQueue, ^{
            if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
            NSDictionary *json = DateLyricsMusixmatchJSON(data);
            NSArray *entries = nil;
            if (!error && status == 200) {
                if (netease && DateLyricsJSONDouble(json[@"code"], 0) == 200) {
                    entries = DateLyricsNetEaseWordLines(DateLyricsJSONDictionary(json[@"yrc"])[@"lyric"], task.duration);
                } else if (!netease) {
                    NSDictionary *response = DateLyricsJSONDictionary(json[@"req_0"]);
                    NSDictionary *body = DateLyricsJSONDictionary(response[@"data"]);
                    if (DateLyricsJSONDouble(json[@"code"], -1) == 0 && DateLyricsJSONDouble(response[@"code"], -1) == 0 && DateLyricsJSONDouble(body[@"qrc"], 0) == 1)
                        entries = DateLyricsQQWordLines(body[@"lyric"], task.duration);
                }
            }
            NSArray *lines = DateLyricsProviderTimedLines(entries);
            BOOL hasWords = NO;
            for (DateLyricsTimedLine *line in lines) hasWords |= line.words.count > 0;
            DateLyricsDebugLog(@"[Provider:WORDS] provider=%@ status=%ld lines=%lu wordTiming=%d", task.provider, (long)status, (unsigned long)lines.count, hasWords);
            if (hasWords && DateLyricsPublishProviderLines(task, lines)) { DateLyricsStepFallback(task, nil); return; }
            // newer netease responses can also carry usable line lyrics.
            if (netease && !task.hasUsableLines && !error && status == 200)
                DateLyricsPublishProviderLines(task, DateLyricsMusixmatchLinesFromLRC(DateLyricsUnescapeLRC(DateLyricsJSONString(DateLyricsJSONDictionary(json[@"lrc"])[@"lyric"])), task.duration));
            DateLyricsProviderTryCandidate(task);
        });
    });
}

static void DateLyricsProviderTryCandidate(DateLyricsMusixmatchTask *task) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    BOOL lrclib = [task.provider isEqualToString:@"lrclib"];
    while (task.candidateIndex < task.candidates.count) {
        NSDictionary *candidate = task.candidates[task.candidateIndex++];
        task.relaxedMatch = !DateLyricsMetadataMatchesStrictly(candidate[@"title"], candidate[@"artist"], task);
        if (lrclib) {
            NSArray *lines = DateLyricsMusixmatchLinesFromLRC(candidate[@"synced"], task.duration);
            if (DateLyricsPublishProviderLines(task, lines)) { DateLyricsStepFallback(task, nil); return; }
            continue;
        }
        NSString *identifier = candidate[@"id"];
        if ([task.attemptedCandidates containsObject:identifier]) continue;
        if (task.candidateFetches >= 3) break;
        [task.attemptedCandidates addObject:identifier];
        task.candidateFetches++;
        if (task.hasUsableLines && gDateLyricsWordHighlighting && DateLyricsProviderHasWordTiming(task.provider)) {
            DateLyricsProviderFetchWordTiming(task, candidate);
            return;
        }
        BOOL netease = [task.provider isEqualToString:@"netease"];
        BOOL megalobiz = [task.provider isEqualToString:@"megalobiz"];
        NSString *url = nil;
        if (megalobiz) {
            url = [NSString stringWithFormat:@"https://api.megalobiz.com/api/public/v1/lrc/%@?format=lrc", DateLyricsMusixmatchPercentEncode(identifier)];
        } else if (netease) {
            url = [NSString stringWithFormat:@"https://music.163.com/api/song/lyric?id=%@&lv=1&kv=1&tv=-1", identifier];
        } else {
            url = [NSString stringWithFormat:@"https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=%@&format=json&nobase64=1", DateLyricsMusixmatchPercentEncode(identifier)];
        }
        DateLyricsMusixmatchRequest(task, url, DateLyricsProviderHeaders(task.provider), ^(NSData *data, NSInteger status, NSError *error) {
            dispatch_async(gLyricsQueue, ^{
                if (!DateLyricsMusixmatchTaskIsCurrent(task)) { DateLyricsDebugLog(@"[Provider] stale callback ignored"); return; }
                if (error || (status != 200 && status != 404)) {
                    DateLyricsStepFallback(task, @"Provider lyric request failed");
                    return;
                }
                NSString *raw = nil;
                if (megalobiz) {
                    raw = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
                } else {
                    NSDictionary *json = DateLyricsMusixmatchJSON(data);
                    raw = DateLyricsJSONString(netease ? DateLyricsJSONDictionary(json[@"lrc"])[@"lyric"] : json[@"lyric"]);
                    if (!netease && raw.length > 0 && ![raw containsString:@"["]) {
                        NSData *decoded = [[NSData alloc] initWithBase64EncodedString:raw options:NSDataBase64DecodingIgnoreUnknownCharacters];
                        if (decoded) raw = [[NSString alloc] initWithData:decoded encoding:NSUTF8StringEncoding];
                    }
                }
                NSArray *lines = DateLyricsMusixmatchLinesFromLRC(DateLyricsUnescapeLRC(raw), task.duration);
                if (status == 200) DateLyricsPublishProviderLines(task, lines);
                if (gDateLyricsWordHighlighting && DateLyricsProviderHasWordTiming(task.provider)) {
                    DateLyricsProviderFetchWordTiming(task, candidate);
                    return;
                }
                if (task.hasUsableLines) { DateLyricsStepFallback(task, nil); return; }
                DateLyricsProviderTryCandidate(task);
            });
        });
        return;
    }
    if (!lrclib && task.candidateFetches >= 3) { DateLyricsStepFallback(task, @"Provider candidate limit reached"); return; }
    DateLyricsProviderSearch(task);
}

static void DateLyricsProviderSearch(DateLyricsMusixmatchTask *task) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    while (task.searchIndex < task.searchQueries.count) {
        NSUInteger stage = task.searchIndex++;
        BOOL exact = stage == 0;
        NSString *url = DateLyricsProviderSearchURL(task.provider, task.searchQueries[stage], task.duration, exact);
        if ([task.searchedURLs containsObject:url]) continue;
        [task.searchedURLs addObject:url];
        DateLyricsDebugLog(@"[Provider:SEARCH] provider=%@ stage=%lu", task.provider, (unsigned long)(stage + 1));
        DateLyricsMusixmatchRequest(task, url, DateLyricsProviderHeaders(task.provider), ^(NSData *data, NSInteger status, NSError *error) {
            dispatch_async(gLyricsQueue, ^{
                if (!DateLyricsMusixmatchTaskIsCurrent(task)) { DateLyricsDebugLog(@"[Provider] stale callback ignored"); return; }
                if (error || (status != 200 && status != 404)) {
                    DateLyricsStepFallback(task, @"Provider search request failed");
                    return;
                }
                id json = data.length ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
                NSArray *candidates = status == 200 ? DateLyricsProviderCandidates(task.provider, json, exact) : @[];
                DateLyricsDebugLog(@"[Provider:SEARCH] provider=%@ candidates=%lu", task.provider, (unsigned long)candidates.count);
                task.candidates = DateLyricsRankProviderCandidates(candidates, task);
                task.candidateIndex = 0;
                DateLyricsProviderTryCandidate(task);
            });
        });
        return;
    }
    DateLyricsStepFallback(task, @"Provider has no trustworthy synchronized lyrics");
}

static BOOL DateLyricsMusixmatchTaskIsCurrent(DateLyricsMusixmatchTask *task) {
    return task && task.musixmatchGeneration == gDateLyricsMusixmatchGeneration &&
        [gMusixmatchPendingTaskIDs[@(task.storeID)] isEqualToString:task.taskID];
}

static void DateLyricsCancelProviderLookup(NSInteger storeID) {
    DLCoreLookup *lookup = gLookupIDs[@(storeID)];
    DLCoreProviderTask *task = lookup.tasks[@(storeID)];
    if (task) DLCProviderRelease(task);
    DateLyricsCancelProviderRequest(gMusixmatchPendingTaskIDs[@(storeID)]);
    [gMusixmatchPendingTaskIDs removeObjectForKey:@(storeID)];
    for (DateLyricsMusixmatchTask *waiter in [gMusixmatchTokenWaiters copy])
        if (!DateLyricsMusixmatchTaskIsCurrent(waiter)) [gMusixmatchTokenWaiters removeObject:waiter];
    if (!gMusixmatchTokenWaiters.count && gMusixmatchTokenRequestInFlight) {
        DateLyricsCancelProviderRequest(@"token");
        gMusixmatchTokenRequestInFlight = NO;
        ++gTokenGeneration;
    }
}

static void DateLyricsCancelMusixmatchTask(DateLyricsMusixmatchTask *task) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    DateLyricsCancelProviderLookup(task.storeID);
    DateLyricsDebugLog(@"[Musixmatch] Cancelled storeID=%ld", (long)task.storeID);
}

static NSString *DateLyricsMusixmatchURL(NSString *endpoint, NSDictionary<NSString *, id> *parameters) {
    if (endpoint.length == 0) return nil;
    NSMutableDictionary<NSString *, id> *query = [@{
        @"app_id": @"mac-ios-v2.0",
        @"format": @"json"
    } mutableCopy];
    [query addEntriesFromDictionary:parameters ?: @{}];
    NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithCapacity:query.count];
    NSArray<NSString *> *keys = [[query allKeys] sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *key in keys) {
        id value = query[key];
        NSString *stringValue = [value isKindOfClass:NSString.class] ? value : [value description];
        [parts addObject:[NSString stringWithFormat:@"%@=%@",
                          DateLyricsMusixmatchPercentEncode(key),
                          DateLyricsMusixmatchPercentEncode(stringValue ?: @"")]];
    }
    return [NSString stringWithFormat:@"https://apic-appmobile.musixmatch.com/ws/1.1/%@?%@",
            endpoint, [parts componentsJoinedByString:@"&"]];
}

static NSDictionary<NSString *, NSString *> *DateLyricsMusixmatchHeaders(void) {
    return @{
        @"User-Agent": @"Musixmatch/13812 CFNetwork/1498.700.2 Darwin/23.6.0",
        @"Accept": @"application/json",
        @"Cookie": @"AWSELBCORS=0; AWSELB=0"
    };
}

static NSDictionary *DateLyricsMusixmatchJSON(NSData *data) {
    if (data.length == 0) return nil;
    const uint8_t *bytes = (const uint8_t *)data.bytes;
    NSUInteger index = 0;
    while (index < data.length && isspace(bytes[index])) index++;
    if (index < data.length && bytes[index] == '<') return nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [json isKindOfClass:NSDictionary.class] ? json : nil;
}

static NSString *DateLyricsMusixmatchTokenCachePath(void) {
    return [DLCCacheRoot() stringByAppendingPathComponent:@"musixmatch-token.json"];
}

static BOOL DateLyricsMusixmatchIsTokenValid(NSString *token) {
    if (![token isKindOfClass:NSString.class] || token.length == 0) return NO;
    if ([token hasPrefix:@"UpgradeOnly"] || [token hasPrefix:@"UpgradeRequired"]) return NO;
    if ([token hasPrefix:@"00000000"]) return NO;
    return YES;
}

static void DateLyricsMusixmatchLoadTokenCache(void) {
    if (gMusixmatchTokenCacheLoaded) return;
    gMusixmatchTokenCacheLoaded = YES;
    NSData *cacheData = DLCReadTokenCache();
    NSDictionary *cache = cacheData.length > 0
        ? DateLyricsJSONDictionary([NSJSONSerialization JSONObjectWithData:cacheData options:0 error:nil])
        : nil;
    NSString *token = [cache[@"v"] isKindOfClass:NSString.class] ? cache[@"v"] : nil;
    if (DateLyricsMusixmatchIsTokenValid(token)) {
        gMusixmatchUserToken = token;
        gMusixmatchTokenFetchedAt = DateLyricsJSONDouble(cache[@"t"], 0.0);
    }
}

static void DateLyricsMusixmatchSaveToken(void) {
    if (gMusixmatchUserToken.length == 0) return;
    NSDictionary *cache = @{@"v": gMusixmatchUserToken, @"t": @(gMusixmatchTokenFetchedAt)};
    NSData *data = [NSJSONSerialization dataWithJSONObject:cache options:0 error:nil];
    [data writeToFile:DateLyricsMusixmatchTokenCachePath() atomically:YES];
}

static BOOL DateLyricsMusixmatchTrackIsTrusted(NSDictionary *track, DateLyricsMusixmatchTask *task) {
    if (![track isKindOfClass:NSDictionary.class] || task.title.length == 0) return NO;
    return DateLyricsProviderTrackIsTrusted(track[@"track_name"], track[@"artist_name"], track[@"track_length"], task);
}

static void DateLyricsMusixmatchFetchMacro(DateLyricsMusixmatchTask *task);
static void DateLyricsMusixmatchBeginTokenLoad(void);

static void DateLyricsMusixmatchFinishTokenLoad(NSString *token, NSString *reason) {
    gMusixmatchTokenRequestInFlight = NO;
    // a token entered while the anonymous request was in flight takes priority.
    if (gDateLyricsMusixmatchCustomUserToken.length > 0 && DateLyricsMusixmatchServiceEnabled()) {
        DateLyricsDebugLog(@"[Token] custom token takes priority over anonymous response waiters=%lu",
            (unsigned long)gMusixmatchTokenWaiters.count);
        gMusixmatchUserToken = [gDateLyricsMusixmatchCustomUserToken copy];
    } else if (DateLyricsMusixmatchIsTokenValid(token) && DateLyricsMusixmatchServiceEnabled()) {
        gMusixmatchUserToken = [token copy];
        gMusixmatchTokenFetchedAt = [[NSDate date] timeIntervalSince1970];
        DateLyricsMusixmatchSaveToken();
        DateLyricsDebugLog(@"[Musixmatch] anonymous token loaded and cached");
    } else {
        gMusixmatchUserToken = nil;
        gMusixmatchTokenFetchedAt = 0.0;
        DateLyricsDebugLog(@"[Musixmatch] anonymous token unavailable");
    }

    NSArray<DateLyricsMusixmatchTask *> *tasks = [gMusixmatchTokenWaiters copy];
    [gMusixmatchTokenWaiters removeAllObjects];
    for (DateLyricsMusixmatchTask *task in tasks) {
        if (DateLyricsMusixmatchTaskIsCurrent(task) && DateLyricsMusixmatchServiceEnabled() && gMusixmatchUserToken.length > 0) {
            DateLyricsMusixmatchFetchMacro(task);
        } else if (!DateLyricsMusixmatchServiceEnabled()) {
            DateLyricsCancelMusixmatchTask(task);
        } else if (!DateLyricsMusixmatchTaskIsCurrent(task)) {
            continue;
        } else {
            DateLyricsMusixmatchFinishTask(task, reason ?: @"Unable to obtain anonymous token");
        }
    }
}

static void DateLyricsMusixmatchBeginTokenLoad(void) {
    if (gMusixmatchTokenRequestInFlight) return;
    gMusixmatchTokenRequestInFlight = YES;
    // one acquisition per lookup: long retry backoffs block every later service.
    NSUInteger generation = gTokenGeneration;
    DateLyricsDebugLog(@"[Musixmatch] requesting anonymous token");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 0), gLyricsQueue, ^{
        if (generation != gTokenGeneration) {
            DateLyricsDebugLog(@"[Token] stale timer ignored generation=%lu current=%lu",
                (unsigned long)generation, (unsigned long)gTokenGeneration);
            return;
        }
        if (!DateLyricsMusixmatchServiceEnabled() || gMusixmatchTokenWaiters.count == 0) {
            DateLyricsMusixmatchFinishTokenLoad(nil, @"Fallback disabled");
            return;
        }
        NSTimeInterval milliseconds = [[NSDate date] timeIntervalSince1970] * 1000.0;
        NSString *url = DateLyricsMusixmatchURL(@"token.get", @{@"t": [NSString stringWithFormat:@"%.0f", milliseconds]});
        DateLyricsMusixmatchRequest(nil, url, DateLyricsMusixmatchHeaders(), ^(NSData *data, NSInteger status, NSError *error) {
            dispatch_async(gLyricsQueue, ^{
                if (generation != gTokenGeneration) {
                    DateLyricsDebugLog(@"[Token] stale response ignored generation=%lu current=%lu",
                        (unsigned long)generation, (unsigned long)gTokenGeneration);
                    return;
                }
                NSDictionary *json = DateLyricsMusixmatchJSON(data);
                NSString *token = DateLyricsMusixmatchFindJSONValue(json, @"user_token");
                if (!error && status >= 200 && status < 300 && DateLyricsMusixmatchIsTokenValid(token)) {
                    DateLyricsMusixmatchFinishTokenLoad(token, nil);
                } else {
                    DateLyricsMusixmatchFinishTokenLoad(nil,
                        error.localizedDescription ?: [NSString stringWithFormat:@"HTTP %ld", (long)status]);
                }
            });
        });
    });
}

static void DateLyricsMusixmatchEnsureToken(DateLyricsMusixmatchTask *task, BOOL forceRefresh) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    if (gDateLyricsMusixmatchCustomUserToken.length > 0) {
        if (forceRefresh) {
            DateLyricsDebugLog(@"[Musixmatch] custom user token was rejected (HTTP 401)");
            DateLyricsMusixmatchFinishTask(task, @"Custom user token invalid or expired");
            return;
        }
        gMusixmatchUserToken = gDateLyricsMusixmatchCustomUserToken;
        DateLyricsDebugLog(@"[Musixmatch] using custom user token");
        DateLyricsMusixmatchFetchMacro(task);
        return;
    }
    DateLyricsMusixmatchLoadTokenCache();
    NSTimeInterval age = [[NSDate date] timeIntervalSince1970] - gMusixmatchTokenFetchedAt;
    if (!forceRefresh && gMusixmatchUserToken.length > 0 && age >= 0.0 && age < 6.0 * 60.0 * 60.0) {
        DateLyricsDebugLog(@"[Musixmatch] using cached anonymous token age=%.0fs", age);
        DateLyricsMusixmatchFetchMacro(task);
        return;
    }
    if (![gMusixmatchTokenWaiters containsObject:task]) [gMusixmatchTokenWaiters addObject:task];
    if (forceRefresh) {
        gMusixmatchUserToken = nil;
        gMusixmatchTokenFetchedAt = 0.0;
    }
    DateLyricsMusixmatchBeginTokenLoad();
}

static BOOL DateLyricsMusixmatchPublishSubtitle(DateLyricsMusixmatchTask *task) {
    if (task.hasUsableLines || task.appleTier == DateLyricsSyncTierLine) return NO;
    NSArray *lines = DateLyricsMusixmatchLinesFromLRC(task.subtitleBody, task.duration);
    BOOL published = DateLyricsPublishProviderLines(task, lines);
    if (published) DateLyricsDebugLog(@"[Musixmatch] showing line timing while checking word timing storeID=%ld", (long)task.storeID);
    return published;
}

static BOOL DateLyricsMusixmatchCompleteWithSubtitle(DateLyricsMusixmatchTask *task, NSString *failureReason) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return NO;
    BOOL published = DateLyricsMusixmatchPublishSubtitle(task);
    if (task.hasUsableLines && !gDateLyricsWordHighlighting) {
        DateLyricsMusixmatchFinishTask(task, nil);
    } else if (++task.searchIndex < task.searchQueries.count) {
        DateLyricsMusixmatchFetchMacro(task);
    } else {
        DateLyricsMusixmatchFinishTask(task, failureReason ?: @"No synchronized upgrade available");
    }
    return published;
}

static void DateLyricsMusixmatchFetchRichsync(DateLyricsMusixmatchTask *task) {
    NSString *url = DateLyricsMusixmatchURL(@"track.richsync.get", @{
        @"commontrack_id": @(task.commontrackID),
        @"usertoken": gMusixmatchUserToken ?: @""
    });
    DateLyricsDebugLog(@"[Musixmatch] requesting rich-sync request=%ld", (long)task.storeID);
    DateLyricsMusixmatchRequest(task, url, DateLyricsMusixmatchHeaders(), ^(NSData *data, NSInteger status, NSError *error) {
        dispatch_async(gLyricsQueue, ^{
            if (!DateLyricsMusixmatchTaskIsCurrent(task)) {
                DateLyricsDebugLog(@"[Musixmatch] Ignoring stale rich-sync task storeID=%ld", (long)task.storeID);
                return;
            }
            if (!DateLyricsMusixmatchServiceEnabled()) {
                DateLyricsCancelMusixmatchTask(task);
                return;
            }
            NSDictionary *json = DateLyricsMusixmatchJSON(data);
            id body = DateLyricsMusixmatchFindJSONValue(json, @"richsync_body");
            NSArray<DateLyricsTimedLine *> *lines = DateLyricsMusixmatchLinesFromBody(body, task.duration);
            BOOL hasWordTiming = NO;
            for (DateLyricsTimedLine *line in lines) {
                if (line.words.count > 0) {
                    hasWordTiming = YES;
                    break;
                }
            }
            DateLyricsDebugLog(@"[Musixmatch] rich-sync response status=%ld parsedLines=%lu",
                               (long)status, (unsigned long)lines.count);
            if (!error && status >= 200 && status < 300 && lines.count > 0 && hasWordTiming &&
                DateLyricsPublishProviderLines(task, lines)) {
                DateLyricsDebugLog(@"[Musixmatch] loaded %lu word-synced lines for storeID=%ld",
                                   (unsigned long)lines.count, (long)task.storeID);
                DateLyricsMusixmatchFinishTask(task, nil);
                return;
            }
            DateLyricsMusixmatchCompleteWithSubtitle(task, @"No usable rich-sync or subtitle");
        });
    });
}

static void DateLyricsMusixmatchFetchMacro(DateLyricsMusixmatchTask *task) {
    if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
    if (!task.searchQueries) task.searchQueries = DateLyricsProviderQueries(task);
    if (gMusixmatchUserToken.length == 0 || task.searchIndex >= task.searchQueries.count) {
        DateLyricsMusixmatchFinishTask(task, @"No remaining Musixmatch query");
        return;
    }
    NSDictionary *query = task.searchQueries[task.searchIndex];
    NSString *queryTitle = query[@"title"], *queryArtist = query[@"artist"];
    task.subtitleBody = nil;
    task.matchedTrack = nil;
    task.commontrackID = 0;
    NSDictionary *parameters = @{
        @"namespace": @"lyrics_richsynched",
        @"subtitle_format": @"lrc",
        @"q_track": queryTitle,
        @"q_artist": queryArtist,
        @"q_album": query[@"album"],
        @"q_duration": task.duration > 0.0 ? @((NSInteger)llround(task.duration)) : @"",
        @"usertoken": gMusixmatchUserToken
    };
    DateLyricsDebugLog(@"[Musixmatch] matching request=%ld duration=%.0f",
                       (long)task.storeID, task.duration);
    DateLyricsMusixmatchRequest(task, DateLyricsMusixmatchURL(@"macro.subtitles.get", parameters),
                                DateLyricsMusixmatchHeaders(),
                                ^(NSData *data, NSInteger status, NSError *error) {
        dispatch_async(gLyricsQueue, ^{
            if (!DateLyricsMusixmatchTaskIsCurrent(task)) {
                DateLyricsDebugLog(@"[Musixmatch] Ignoring stale task storeID=%ld", (long)task.storeID);
                return;
            }
            if (!DateLyricsMusixmatchServiceEnabled()) {
                DateLyricsCancelMusixmatchTask(task);
                return;
            }
            NSDictionary *json = DateLyricsMusixmatchJSON(data);
            double apiStatus = DateLyricsJSONDouble(DateLyricsMusixmatchFindJSONValue(json, @"status_code"), 0.0);
            DateLyricsDebugLog(@"[Musixmatch] match response http=%ld api=%.0f bytes=%lu",
                               (long)status, apiStatus, (unsigned long)data.length);
            if ((status == 401 || apiStatus == 401) && !task.refreshedToken) {
                task.refreshedToken = YES;
                DateLyricsDebugLog(@"[Musixmatch] token exhausted; refreshing once");
                DateLyricsMusixmatchEnsureToken(task, YES);
                return;
            }
            if (error || status < 200 || status >= 300 || !json) {
                DateLyricsMusixmatchFinishTask(task,
                    error.localizedDescription ?: [NSString stringWithFormat:@"HTTP %ld", (long)status]);
                return;
            }

            id matcher = DateLyricsMusixmatchFindJSONValue(json, @"matcher.track.get");
            NSDictionary *track = DateLyricsMusixmatchFindJSONValue(matcher, @"track");
            if (![track isKindOfClass:NSDictionary.class] ||
                !DateLyricsMusixmatchTrackIsTrusted(track, task) ||
                DateLyricsJSONNumber(track[@"instrumental"]).boolValue) {
                // say which check failed, and what musixmatch actually returned, so a rejected
                // match can be told apart from "musixmatch has nothing" in the log.
                if (![track isKindOfClass:NSDictionary.class]) {
                    DateLyricsDebugLog(@"[Musixmatch] reject storeID=%ld reason=no-track-in-response",
                                       (long)task.storeID);
                } else if (DateLyricsJSONNumber(track[@"instrumental"]).boolValue) {
                    DateLyricsDebugLog(@"[Musixmatch] reject request=%ld reason=flagged-instrumental", (long)task.storeID);
                } else {
                    DateLyricsDebugLog(@"[Musixmatch] reject request=%ld reason=untrusted-match duration=%.0f", (long)task.storeID, task.duration);
                }
                DateLyricsMusixmatchFinishTask(task, @"No trustworthy non-instrumental match");
                return;
            }
            task.matchedTrack = track;
            task.relaxedMatch = !DateLyricsMetadataMatchesStrictly(track[@"track_name"], track[@"artist_name"], task);
            task.commontrackID = DateLyricsJSONTrackID(track[@"commontrack_id"]);
            id subtitleBody = DateLyricsMusixmatchFindJSONValue(json, @"subtitle_body");
            task.subtitleBody = [subtitleBody isKindOfClass:NSString.class] ? subtitleBody : nil;
            BOOL hasRichsync = DateLyricsJSONNumber(track[@"has_richsync"]).boolValue;
            DateLyricsDebugLog(@"[Musixmatch] matched request=%ld richsync=%d subtitle=%d", (long)task.storeID, hasRichsync, task.subtitleBody.length > 0);
            DateLyricsMusixmatchPublishSubtitle(task);
            if (!DateLyricsMusixmatchTaskIsCurrent(task)) return;
            NSString *candidateID = [NSString stringWithFormat:@"%ld", (long)task.commontrackID];
            if (hasRichsync && task.commontrackID > 0 && (!task.hasUsableLines || gDateLyricsWordHighlighting) &&
                ![task.attemptedCandidates containsObject:candidateID]) {
                [task.attemptedCandidates addObject:candidateID];
                DateLyricsMusixmatchFetchRichsync(task);
            } else {
                DateLyricsMusixmatchCompleteWithSubtitle(task, @"Matched track has no synchronized lyrics");
            }
        });
    });
}
