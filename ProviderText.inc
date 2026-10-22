static NSArray<NSValue *> *DateLyricsBracketedRanges(NSString *text, BOOL includeBraces) {
    NSMutableArray<NSNumber *> *stack = [NSMutableArray array];
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    for (NSUInteger index = 0; index < text.length; index++) {
        unichar character = [text characterAtIndex:index];
        if (character == '(' || character == '[' || (includeBraces && character == '{')) {
            [stack addObject:@(index)];
            continue;
        }
        unichar opener = character == ')' ? '(' : character == ']' ? '[' : (includeBraces && character == '}') ? '{' : 0;
        if (opener == 0 || stack.count == 0) continue;
        NSUInteger start = stack.lastObject.unsignedIntegerValue;
        if ([text characterAtIndex:start] != opener) {
            [stack removeAllObjects];
            continue;
        }
        [stack removeLastObject];
        if (stack.count == 0) [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, index - start + 1)]];
    }
    return ranges;
}

static NSString *DateLyricsCollapseWhitespace(NSString *text) {
    static NSRegularExpression *spaces;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        spaces = [NSRegularExpression regularExpressionWithPattern:@"\\s+" options:0 error:nil];
    });
    NSString *collapsed = [spaces stringByReplacingMatchesInString:text options:0
        range:NSMakeRange(0, text.length) withTemplate:@" "];
    return [collapsed stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static void DateLyricsNormalizeWordLines(NSArray<DateLyricsTimedLine *> *lines) {
    for (DateLyricsTimedLine *line in lines) {
        if (line.words.count == 0) continue;

        NSMutableString *reconstructed = [NSMutableString string];
        for (NSUInteger i = 0; i < line.words.count; i++) {
            DateLyricsTimedWord *word = line.words[i];
            if (i == 0) word.separatorBefore = @"";
            [reconstructed appendString:word.separatorBefore ?: @""];
            [reconstructed appendString:word.text ?: @""];
        }

        line.text = [reconstructed copy];
    }
}
