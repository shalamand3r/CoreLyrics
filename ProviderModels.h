#pragma once
#import <Foundation/Foundation.h>
#import <math.h>

typedef NS_ENUM(NSInteger, DateLyricsSyncTier) { DateLyricsSyncTierNone, DateLyricsSyncTierLine, DateLyricsSyncTierWord };
@interface DLCoreProviderTask : NSObject
@property (nonatomic, assign) NSInteger storeID;
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *artist;
@property (nonatomic, copy) NSString *album;
@property (nonatomic, assign) NSTimeInterval duration;
@property (nonatomic, copy) NSString *subtitleBody;
@property (nonatomic, assign) BOOL refreshedToken;
@property (nonatomic, assign) DateLyricsSyncTier appleTier;
@property (nonatomic, assign) NSInteger commontrackID;

@property (nonatomic, assign) NSUInteger cacheGeneration;
@property (nonatomic, assign) NSUInteger musixmatchGeneration;
@property (nonatomic, copy) NSString *taskID;
@property (nonatomic, strong) NSMutableSet<NSString *> *attemptedProviders;
@property (nonatomic, assign) BOOL hasShownLoading;
@property (nonatomic, assign) NSInteger currentProviderIndex;
@property (nonatomic, assign) NSInteger totalProviders;
@property (nonatomic, copy) NSString *provider;
@property (nonatomic, copy) NSArray<NSDictionary *> *searchQueries;
@property (nonatomic, assign) NSUInteger searchIndex;
@property (nonatomic, strong) NSMutableSet<NSString *> *searchedURLs;
@property (nonatomic, copy) NSArray<NSDictionary *> *candidates;
@property (nonatomic, assign) NSUInteger candidateIndex;
@property (nonatomic, assign) NSUInteger candidateFetches;
@property (nonatomic, strong) NSMutableSet<NSString *> *attemptedCandidates;
@property (nonatomic, assign) BOOL hasUsableLines;
@property (nonatomic, assign) BOOL hasWordTiming;
@property (nonatomic, assign) BOOL hadTransportFailure;
@property (nonatomic, copy) NSDictionary *matchedTrack;
@property (nonatomic, assign) BOOL relaxedMatch;
@end

@interface DLCoreWord : NSObject
@property (nonatomic, assign) NSTimeInterval begin;
@property (nonatomic, assign) NSTimeInterval end;
@property (nonatomic, copy) NSString *text;
@property (nonatomic, copy) NSString *separatorBefore;
@property (nonatomic, assign, getter=isBackground) BOOL background;
@end

@interface DLCoreLine : NSObject
@property (nonatomic, assign) NSTimeInterval begin;
@property (nonatomic, assign) NSTimeInterval end;
@property (nonatomic, copy) NSString *text;
@property (nonatomic, strong) NSArray<DLCoreWord *> *words;
@property (nonatomic, assign) BOOL canonicalMapped;
@end
