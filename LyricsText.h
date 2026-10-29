#pragma once
#import <Foundation/Foundation.h>
static NSString *DateLyricsMusixmatchNormalizedString(NSString *value) {
    if (![value isKindOfClass:NSString.class]) return @"";
    static NSCache<NSString *, NSString *> *cache;
    static NSLocale *locale;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSCache new];
        cache.countLimit = 256;
        cache.totalCostLimit = 128 * 1024;
        locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    });
    NSString *cached = [cache objectForKey:value];
    if (cached) return cached;
    // expand & and + to "and" before folding so that e.g.
    // "simon & garfunkel" and "simon and garfunkel" normalise identically.
    NSMutableString *prep = [value mutableCopy];
    [prep replaceOccurrencesOfString:@"&" withString:@" and " options:0 range:NSMakeRange(0, prep.length)];
    [prep replaceOccurrencesOfString:@"+" withString:@" and " options:0 range:NSMakeRange(0, prep.length)];
    NSString *normalized = [prep stringByFoldingWithOptions:
        NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch | NSWidthInsensitiveSearch
        locale:locale];
    NSMutableString *result = [NSMutableString string];
    NSCharacterSet *alphanumeric = [NSCharacterSet alphanumericCharacterSet];
    __block BOOL needsSpace = NO;
    [normalized enumerateSubstringsInRange:NSMakeRange(0, normalized.length)
        options:NSStringEnumerationByComposedCharacterSequences
        usingBlock:^(NSString *substring, __unused NSRange substringRange, __unused NSRange enclosingRange, __unused BOOL *stop) {
        // apostrophes may be omitted by a provider. keep supplementary unicode
        // letters intact rather than treating each utf-16 surrogate as punctuation.
        if ([substring isEqualToString:@"'"] || [substring isEqualToString:@"’"]) return;
        if ([substring rangeOfCharacterFromSet:alphanumeric].location != NSNotFound) {
            if (needsSpace && result.length > 0) [result appendString:@" "];
            [result appendString:substring];
            needsSpace = NO;
        } else {
            needsSpace = YES;
        }
    }];
    NSString *output = [result copy];
    // cap retained metadata and copy keys because callers can pass mutable strings.
    if (value.length <= 1024)
        [cache setObject:output forKey:[value copy] cost:(value.length + output.length) * sizeof(unichar)];
    return output;
}

static BOOL DateLyricsTextsAreSimilar(NSString *left, NSString *right) {
    NSString *a = DateLyricsMusixmatchNormalizedString(left);
    NSString *b = DateLyricsMusixmatchNormalizedString(right);
    if (a.length == 0 || b.length == 0) return NO;
    if ([a isEqualToString:b]) return YES;

    // normalization already guarantees single spaces and nonempty tokens.
    NSArray<NSString *> *aWords = [a componentsSeparatedByString:@" "];
    NSArray<NSString *> *bWords = [b componentsSeparatedByString:@" "];
    // a single word needs exact equality, handled above.
    if (aWords.count == 1 || bWords.count == 1) return NO;

    // short lines also need the same opening word before timing can be merged.
    if (aWords.count <= 3 || bWords.count <= 3) {
        if (![aWords.firstObject isEqualToString:bWords.firstObject]) return NO;
    }

    NSCountedSet<NSString *> *aBag = [[NSCountedSet alloc] initWithArray:aWords];
    NSCountedSet<NSString *> *bBag = [[NSCountedSet alloc] initWithArray:bWords];

    // multiset overlap respects repeated words. when one side is contained in
    // the other, this already reduces to shorter-count / longer-count.
    NSMutableSet<NSString *> *allKeys = [NSMutableSet setWithArray:aWords];
    [allKeys addObjectsFromArray:bWords];
    NSUInteger interSize = 0, unionSize = 0;
    for (NSString *key in allKeys) {
        NSUInteger ac = [aBag countForObject:key];
        NSUInteger bc = [bBag countForObject:key];
        interSize += MIN(ac, bc);
        unionSize += MAX(ac, bc);
    }
    if (unionSize == 0) return NO;

    return (double)interSize / (double)unionSize >= 0.50;
}
