/*
 * cait gui: a small window over the feed (macOS). Pick an agent, then one of
 * its reports, and stop the agent, undo that report, or send the main agent
 * a correction. Only this window is Objective-C; the rest of cait is C.
 */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <unistd.h>

#include "cait.h"

@interface Report : NSObject
@property NSString *rid, *agent, *status, *file, *counts, *text, *when, *mdPath;
@property NSArray<NSString *> *paths;
@property NSInteger mdIndex;	/* mdPath: first document the preview can show (Markdown, Mermaid, image) */
@end
@implementation Report
@end

@interface Agent : NSObject
@property NSString *name, *text, *state, *doing;	/* state/doing: from cait status */
@property NSInteger count, idle, unreported;
@property BOOL final, acked;
@property BOOL stopped;
@property NSInteger rank;	/* 0 needs you now, 1 needs a look, 2 running, 3 finished */
@property NSString *label, *sub;	/* the state in words, and one line of what is going on */
@property NSColor *color;
@end
@implementation Agent
@end

@interface App : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property NSWindow *win;
@property NSTableView *agentTable, *reportTable;
@property NSTextField *header;
@property NSString *counts;
@property NSArray<Agent *> *agents;
@property NSArray<Report *> *shown;	/* the selected agent's reports */
@property NSMutableArray<Report *> *all;
@property NSData *lastFeed, *lastStop;
@property BOOL busy;
@property NSMutableSet<NSString *> *asked;	/* permission requests already announced */
@property NSView *permBar;	/* shown above the tables while an agent waits for the user's OK */
@property NSTextField *permLabel;
@property NSString *permId;
@property NSMutableDictionary<NSString *, NSNumber *> *pendingBy;	/* agent name -> requests waiting */
@property NSString *permShown;
@property BOOL showHidden;
@property NSPopUpButton *guardMenu;
@property NSScrollView *agentScroll, *reportScroll;
@property NSMutableDictionary *askedReport;
@property NSScrollView *previewScroll;
@property WKWebView *web;
@property NSTextView *previewText;
@property NSString *previewKey;
@property BOOL permUp, previewShown, previewWeb;	/* previewWeb: drawn by the web view (diagrams, images) */
@property NSButton *askReportBtn, *ackBtn, *stopBtn, *undoBtn, *forgetBtn, *forgetAllBtn, *fixBtn;
@property NSTextField *emptyLabel, *noReports;
@property NSMutableDictionary<NSString *, NSString *> *permInfo;	/* agent name -> its first waiting request */
@property BOOL allStop;
@property NSMutableSet<NSString *> *seen;	/* "needs a look" agents the user has already looked at */
@end

static NSData *read_dir_file(const char *name)
{
	char path[4200];

	path_in_dir(path, sizeof(path), name);
	return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
}

/* Run this same program with other arguments; returns its output. */
static NSString *run_cait(NSArray<NSString *> *args, int *status)
{
	NSTask *t = [NSTask new];
	NSPipe *pipe = [NSPipe pipe];
	NSData *data;

	t.executableURL = [NSURL fileURLWithPath:NSBundle.mainBundle.executablePath];
	t.arguments = args;
	t.standardOutput = pipe;
	t.standardError = pipe;
	[t launchAndReturnError:nil];
	data = [pipe.fileHandleForReading readDataToEndOfFile];
	[t waitUntilExit];
	if (status)
		*status = t.terminationStatus;
	return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}

static NSString *friendly_tool(NSString *t)
{
	if ([t isEqualToString:@"Bash"])
		return @"명령 실행";
	if ([t isEqualToString:@"Edit"] || [t isEqualToString:@"MultiEdit"])
		return @"파일 수정";
	if ([t isEqualToString:@"Write"])
		return @"파일 작성";
	return t;
}

/* cait status, by agent name */
static NSDictionary<NSString *, NSDictionary *> *load_status(NSMutableArray<NSString *> *order)
{
	agent_info_t *st = calloc(64, sizeof(*st));
	NSMutableDictionary *by = [NSMutableDictionary dictionary];
	int n = st ? status_load(st, 64) : 0;

	for (int i = 0; i < n; i++) {
		NSString *name = [NSString stringWithUTF8String:st[i].name];
		NSString *state = [NSString stringWithUTF8String:st[i].state];
		NSString *doing = @"";

		if (st[i].tool[0] && ![state isEqualToString:@"done"]) {
			char say[700];

			activity_label(st[i].tool, st[i].target, say, sizeof(say));
			doing = [NSString stringWithUTF8String:say];
		}
		by[name] = @{ @"state" : state, @"doing" : doing, @"idle" : @(st[i].idle), @"hidden" : @(st[i].hidden),
			@"unrep" : @(st[i].unreported), @"final" : @(st[i].final_ok), @"acked" : @(st[i].acked) };
		[order addObject:name];
	}
	free(st);
	return by;
}


/* ---- Markdown preview: a small renderer so a changed .md file can be read as a document ---- */

static NSAttributedString *md_inline(NSString *src, NSFont *base, NSColor *color)
{
	NSAttributedStringMarkdownParsingOptions *o = [NSAttributedStringMarkdownParsingOptions new];
	NSMutableAttributedString *a;
	NSFontManager *fm = NSFontManager.sharedFontManager;
	NSError *err = nil;

	o.interpretedSyntax = NSAttributedStringMarkdownInterpretedSyntaxInlineOnlyPreservingWhitespace;
	a = [[NSMutableAttributedString alloc] initWithMarkdownString:src options:o baseURL:nil error:&err] ?
		    : [[NSMutableAttributedString alloc] initWithString:src];
	[a addAttribute:NSFontAttributeName value:base range:NSMakeRange(0, a.length)];
	[a addAttribute:NSForegroundColorAttributeName value:color range:NSMakeRange(0, a.length)];
	[a enumerateAttribute:NSInlinePresentationIntentAttributeName
		      inRange:NSMakeRange(0, a.length)
		      options:0
		   usingBlock:^(id v, NSRange r, BOOL *stop) {
			   (void)stop;
			   NSUInteger i = [v unsignedIntegerValue];
			   NSFont *f = base;

			   if (!v)
				   return;
			   if (i & NSInlinePresentationIntentStronglyEmphasized)
				   f = [fm convertFont:f toHaveTrait:NSBoldFontMask];
			   if (i & NSInlinePresentationIntentEmphasized)
				   f = [fm convertFont:f toHaveTrait:NSItalicFontMask];
			   if (i & NSInlinePresentationIntentCode) {
				   f = [NSFont monospacedSystemFontOfSize:base.pointSize - 1 weight:NSFontWeightRegular];
				   [a addAttribute:NSBackgroundColorAttributeName value:NSColor.quaternaryLabelColor range:r];
			   }
			   if (i & NSInlinePresentationIntentStrikethrough)
				   [a addAttribute:NSStrikethroughStyleAttributeName value:@(NSUnderlineStyleSingle) range:r];
			   [a addAttribute:NSFontAttributeName value:f range:r];
		   }];
	return a;
}

static NSColor *md_mark;	/* set while the current source line is new or changed */

static void md_add(NSMutableAttributedString *out, NSAttributedString *line, CGFloat indent, CGFloat hang, CGFloat after)
{
	NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
	NSMutableAttributedString *m = [line mutableCopy];

	ps.firstLineHeadIndent = indent;
	ps.headIndent = indent + hang;
	ps.paragraphSpacing = after;
	[m appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
	[m addAttribute:NSParagraphStyleAttributeName value:ps range:NSMakeRange(0, m.length)];
	if (md_mark && line.length)
		[m addAttribute:NSBackgroundColorAttributeName value:md_mark range:NSMakeRange(0, m.length - 1)];
	[out appendAttributedString:m];
}

/* Which lines of `after` are new or changed compared with `before` (0-based). Plain LCS on lines, no diff output. */
static NSIndexSet *changed_lines(NSString *before, NSString *after)
{
	NSArray<NSString *> *a = [before componentsSeparatedByString:@"\n"], *b = [after componentsSeparatedByString:@"\n"];
	NSMutableIndexSet *out = [NSMutableIndexSet indexSet];
	NSMutableDictionary<NSString *, NSNumber *> *ids = [NSMutableDictionary dictionary];
	NSUInteger lo = 0, ea = a.count, eb = b.count;
	uint32_t *dp;
	size_t w;

	NSUInteger (^idOf)(NSString *) = ^NSUInteger(NSString *l) {
		NSString *k = [l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
		NSNumber *n = ids[k];

		if (!n) {
			n = @(ids.count);
			ids[k] = n;
		}
		return n.unsignedIntegerValue;
	};
	NSUInteger *ia = calloc(a.count + 1, sizeof(NSUInteger)), *ib = calloc(b.count + 1, sizeof(NSUInteger));

	for (NSUInteger i = 0; i < a.count; i++)
		ia[i] = idOf(a[i]);
	for (NSUInteger i = 0; i < b.count; i++)
		ib[i] = idOf(b[i]);
	while (lo < ea && lo < eb && ia[lo] == ib[lo])
		lo++;
	while (ea > lo && eb > lo && ia[ea - 1] == ib[eb - 1]) {
		ea--;
		eb--;
	}
	/* lo..ea of a and lo..eb of b are what differs; lines outside are unchanged */
	if ((ea - lo) * (eb - lo) > 6000000) {	/* too big to compare: say nothing rather than colour everything */
		free(ia);
		free(ib);
		return out;
	}
	w = eb - lo + 1;
	dp = calloc((ea - lo + 1) * w, sizeof(uint32_t));
	for (NSUInteger i = ea - lo; i-- > 0;)
		for (NSUInteger j = eb - lo; j-- > 0;)
			dp[i * w + j] = ia[lo + i] == ib[lo + j] ? dp[(i + 1) * w + j + 1] + 1
								  : MAX(dp[(i + 1) * w + j], dp[i * w + j + 1]);
	for (NSUInteger i = 0, j = 0; j < eb - lo;) {
		if (i < ea - lo && ia[lo + i] == ib[lo + j]) {
			i++;
			j++;
		} else if (i < ea - lo && dp[(i + 1) * w + j] >= dp[i * w + j + 1]) {
			i++;
		} else {
			[out addIndex:lo + j];
			j++;
		}
	}
	free(dp);
	free(ia);
	free(ib);
	return out;
}

static NSAttributedString *render_markdown(NSString *md, NSIndexSet *changed)
{
	NSMutableAttributedString *out = [NSMutableAttributedString new];
	NSFont *body = [NSFont systemFontOfSize:13];
	NSFont *mono = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
	NSRegularExpression *head = [NSRegularExpression regularExpressionWithPattern:@"^(#{1,6})\\s+(.*)$" options:0 error:nil];
	NSRegularExpression *bullet = [NSRegularExpression regularExpressionWithPattern:@"^(\\s*)[-*+]\\s+(.*)$" options:0 error:nil];
	NSRegularExpression *num = [NSRegularExpression regularExpressionWithPattern:@"^(\\s*)(\\d+[.)])\\s+(.*)$" options:0 error:nil];
	BOOL code = NO;
	NSUInteger lineno = (NSUInteger)-1;
	NSColor *mark = [NSColor.systemGreenColor colorWithAlphaComponent:0.22];

	for (NSString *raw in [md componentsSeparatedByString:@"\n"]) {
		NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\r"]];
		NSString *t = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSTextCheckingResult *m;
		NSRange all = NSMakeRange(0, line.length);

		lineno++;
		md_mark = [changed containsIndex:lineno] ? mark : nil;
		if ([t hasPrefix:@"```"] || [t hasPrefix:@"~~~"]) {
			code = !code;
			continue;
		}
		if (code) {
			NSMutableAttributedString *c = [[NSMutableAttributedString alloc] initWithString:line.length ? line : @" "
					attributes:@{ NSFontAttributeName : mono, NSForegroundColorAttributeName : NSColor.labelColor,
						      NSBackgroundColorAttributeName : NSColor.quaternaryLabelColor }];

			md_add(out, c, 8, 0, 0);
			continue;
		}
		if (t.length == 0) {
			md_add(out, [[NSAttributedString alloc] initWithString:@"" attributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:5] }], 0, 0, 0);
			continue;
		}
		if ((m = [head firstMatchInString:line options:0 range:all])) {
			NSUInteger lv = [m rangeAtIndex:1].length;
			CGFloat size = lv == 1 ? 22 : lv == 2 ? 18 : lv == 3 ? 15 : 13;

			md_add(out, md_inline([line substringWithRange:[m rangeAtIndex:2]], [NSFont boldSystemFontOfSize:size], NSColor.labelColor),
			       0, 0, lv <= 2 ? 6 : 2);
		} else if ([t isEqualToString:@"---"] || [t isEqualToString:@"***"] || [t isEqualToString:@"___"]) {
			md_add(out, [[NSAttributedString alloc] initWithString:@"────────────────────────────────"
					attributes:@{ NSFontAttributeName : body, NSForegroundColorAttributeName : NSColor.tertiaryLabelColor }], 0, 0, 4);
		} else if ((m = [bullet firstMatchInString:line options:0 range:all])) {
			CGFloat ind = 6 + [m rangeAtIndex:1].length * 8;
			NSMutableAttributedString *l = [[NSMutableAttributedString alloc] initWithString:@"•  " attributes:@{ NSFontAttributeName : body }];

			[l appendAttributedString:md_inline([line substringWithRange:[m rangeAtIndex:2]], body, NSColor.labelColor)];
			md_add(out, l, ind, 14, 1);
		} else if ((m = [num firstMatchInString:line options:0 range:all])) {
			CGFloat ind = 6 + [m rangeAtIndex:1].length * 8;
			NSMutableAttributedString *l = [[NSMutableAttributedString alloc]
				initWithString:[[line substringWithRange:[m rangeAtIndex:2]] stringByAppendingString:@"  "]
				    attributes:@{ NSFontAttributeName : body }];

			[l appendAttributedString:md_inline([line substringWithRange:[m rangeAtIndex:3]], body, NSColor.labelColor)];
			md_add(out, l, ind, 18, 1);
		} else if ([t hasPrefix:@">"]) {
			NSString *q = [[t substringFromIndex:1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
			NSMutableAttributedString *l = [[NSMutableAttributedString alloc] initWithString:@"▎ "
					attributes:@{ NSFontAttributeName : body, NSForegroundColorAttributeName : NSColor.systemBlueColor }];

			[l appendAttributedString:md_inline(q, body, NSColor.secondaryLabelColor)];
			md_add(out, l, 6, 14, 2);
		} else if ([t hasPrefix:@"|"]) {
			NSString *bare = [[t stringByReplacingOccurrencesOfString:@"|" withString:@""]
				stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" -:"]];

			if (bare.length == 0 && [t containsString:@"-"])	/* the ---|--- row */
				continue;
			md_add(out, [[NSAttributedString alloc] initWithString:t attributes:@{ NSFontAttributeName : mono }], 4, 0, 0);
		} else {
			md_add(out, md_inline(t, body, NSColor.labelColor), 0, 0, 4);
		}
	}
	md_mark = nil;
	return out;
}

@implementation App

- (NSButton *)button:(NSString *)title x:(CGFloat)x w:(CGFloat)w action:(SEL)sel
{
	NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];

	b.frame = NSMakeRect(x, 8, w, 28);
	b.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
	return b;
}

- (NSTableView *)tableWithColumns:(NSArray<NSArray *> *)spec
{
	NSTableView *t = [[NSTableView alloc] initWithFrame:NSZeroRect];

	for (NSArray *c in spec) {
		NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:c[0]];

		col.title = c[1];
		col.width = [c[2] doubleValue];
		[t addTableColumn:col];
	}
	t.dataSource = self;
	t.delegate = self;
	t.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
	return t;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	NSView *root;
	NSScrollView *as, *rs;

	_all = [NSMutableArray array];
	_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1134, 560)
					   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
						     NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
					     backing:NSBackingStoreBuffered
					       defer:NO];
	_win.title = @"cait";
	_win.delegate = self;
	[_win center];
	root = _win.contentView;

	_header = [NSTextField labelWithString:@""];
	_header.frame = NSMakeRect(12, 508, 1110, 44);
	_header.maximumNumberOfLines = 2;
	_header.lineBreakMode = NSLineBreakByTruncatingTail;
	_header.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
	[root addSubview:_header];

	_agentTable = [self tableWithColumns:@[ @[ @"name", @"에이전트", @186 ], @[ @"state", @"상태", @104 ] ]];
	_agentTable.rowHeight = 38;
	as = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 44, 300, 460)];
	as.documentView = _agentTable;
	as.hasVerticalScroller = YES;
	as.borderType = NSBezelBorder;
	as.autoresizingMask = NSViewHeightSizable;
	[root addSubview:as];
	_agentScroll = as;

	_reportTable = [self tableWithColumns:@[ @[ @"when", @"시간", @62 ], @[ @"status", @"상태", @72 ],
						 @[ @"file", @"파일", @120 ], @[ @"text", @"요약", @500 ] ]];
	_reportTable.rowHeight = 40;
	rs = [[NSScrollView alloc] initWithFrame:NSMakeRect(320, 44, 802, 460)];
	rs.documentView = _reportTable;
	rs.hasVerticalScroller = YES;
	rs.borderType = NSBezelBorder;
	rs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	[root addSubview:rs];
	_reportScroll = rs;

	_previewScroll = [NSTextView scrollableTextView];
	_previewText = _previewScroll.documentView;
	_previewText.editable = NO;
	_previewText.textContainerInset = NSMakeSize(12, 10);
	_previewScroll.borderType = NSBezelBorder;
	_previewScroll.hidden = YES;
	[root addSubview:_previewScroll];
	_web = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:[WKWebViewConfiguration new]];
	_web.hidden = YES;
	[root addSubview:_web];

	_emptyLabel = [NSTextField wrappingLabelWithString:@"아직 보이는 에이전트가 없습니다.\n\n메인 에이전트가 서브에이전트에게 일을 시키면 여기에 나타나고, 파일을 바꿀 때마다 쉬운 말로 요약이 올라옵니다."];
	_emptyLabel.frame = NSMakeRect(28, 330, 256, 120);
	_emptyLabel.textColor = NSColor.secondaryLabelColor;
	_emptyLabel.alignment = NSTextAlignmentCenter;
	[root addSubview:_emptyLabel];

	_noReports = [NSTextField wrappingLabelWithString:@""];
	_noReports.frame = NSMakeRect(360, 300, 722, 120);
	_noReports.textColor = NSColor.secondaryLabelColor;
	_noReports.alignment = NSTextAlignmentCenter;
	_noReports.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
	_noReports.hidden = YES;
	[root addSubview:_noReports];

	_permBar = [[NSView alloc] initWithFrame:NSMakeRect(320, 440, 708, 64)];
	_permBar.wantsLayer = YES;
	_permBar.layer.backgroundColor = [[NSColor.systemYellowColor colorWithAlphaComponent:0.28] CGColor];
	_permBar.layer.cornerRadius = 6;
	_permBar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
	_permBar.hidden = YES;
	_permLabel = [NSTextField labelWithString:@""];
	_permLabel.frame = NSMakeRect(10, 4, 520, 56);
	_permLabel.maximumNumberOfLines = 3;
	_permLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
	_permLabel.autoresizingMask = NSViewWidthSizable;
	[_permBar addSubview:_permLabel];
	{
		NSButton *ok = [NSButton buttonWithTitle:@"허용" target:self action:@selector(allowPerm:)];
		NSButton *no = [NSButton buttonWithTitle:@"거부" target:self action:@selector(denyPerm:)];

		ok.frame = NSMakeRect(708 - 80 - 10, 18, 80, 28);
		no.frame = NSMakeRect(708 - 160 - 16, 18, 80, 28);
		ok.bezelColor = NSColor.systemGreenColor;
		ok.autoresizingMask = no.autoresizingMask = NSViewMinXMargin;
		[_permBar addSubview:no];
		[_permBar addSubview:ok];
	}
	[root addSubview:_permBar];

	_stopBtn = [self button:@"에이전트 중지" x:12 w:110 action:@selector(stopAgent:)];
	_stopBtn.toolTip = @"선택한 에이전트를 멈추게 합니다. 에이전트는 다음 보고나 도구 호출 때 멈춥니다.";
	[root addSubview:_stopBtn];
	{
		NSButton *clr = [self button:@"중지 해제" x:128 w:80 action:@selector(clearStops:)];

		clr.toolTip = @"걸어 둔 중지 요청을 모두 풉니다. 새 서브에이전트를 시키기 전에 누르세요.";
		[root addSubview:clr];
	}
	_askReportBtn = [self button:@"보고 요청" x:214 w:96 action:@selector(askReport:)];
	_askReportBtn.toolTip = @"'확인 필요' 에이전트에게 빠진 보고를 하게 하라고 메인 에이전트에게 전달합니다.";
	[root addSubview:_askReportBtn];
	_ackBtn = [self button:@"확인했어요" x:316 w:92 action:@selector(ackAgent:)];
	_ackBtn.toolTip = @"결과를 직접 확인했다고 표시합니다. 이 에이전트가 더 일하기 전까지 '확인 필요'에서 빠집니다.";
	[root addSubview:_ackBtn];
	_undoBtn = [self button:@"이 보고 되돌리기" x:414 w:130 action:@selector(undoReport:)];
	_undoBtn.toolTip = @"선택한 보고의 파일 변경을 되돌립니다. 되돌리기 전 파일은 백업됩니다.";
	[root addSubview:_undoBtn];
	_fixBtn = [self button:@"수정 요청…" x:550 w:100 action:@selector(askFix:)];
	_fixBtn.toolTip = @"고칠 점을 적으면 메인 에이전트가 받아서 다시 지시합니다.";
	[root addSubview:_fixBtn];

	_forgetBtn = [self button:@"치우기" x:656 w:64 action:@selector(forgetSelected:)];
	_forgetBtn.toolTip = @"끝난 에이전트를 목록에서 치웁니다. 보고 기록은 남습니다.";
	[root addSubview:_forgetBtn];
	_forgetAllBtn = [self button:@"끝난 것 모두 치우기" x:724 w:130 action:@selector(forgetDone:)];
	_forgetAllBtn.toolTip = @"끝난 에이전트를 모두 목록에서 치웁니다. 보고 기록은 남습니다.";
	[root addSubview:_forgetAllBtn];
	{
		NSButton *show = [NSButton checkboxWithTitle:@"치운 것 보기" target:self action:@selector(toggleHidden:)];
		NSString *cur = run_cait(@[ @"guard", @"mode" ], NULL);

		show.frame = NSMakeRect(864, 10, 110, 24);
		show.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
		[root addSubview:show];
		_guardMenu = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(978, 8, 148, 28) pullsDown:NO];
		[_guardMenu addItemsWithTitles:@[ @"승인: 위험한 것만", @"승인: 전부 묻기", @"승인: 끄기" ]];
		[_guardMenu itemAtIndex:0].representedObject = @"on";
		[_guardMenu itemAtIndex:1].representedObject = @"all";
		[_guardMenu itemAtIndex:2].representedObject = @"off";
		[_guardMenu selectItemAtIndex:[cur containsString:@"all"] ? 1 : [cur containsString:@"off"] ? 2 : 0];
		_guardMenu.target = self;
		_guardMenu.action = @selector(setGuard:);
		_guardMenu.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
		[root addSubview:_guardMenu];
	}

	[_win makeKeyAndOrderFront:nil];
	[NSTimer scheduledTimerWithTimeInterval:0.3
					 target:self
				       selector:@selector(refresh)
				       userInfo:nil
					repeats:YES];
	[NSTimer scheduledTimerWithTimeInterval:1.0
					 target:self
				       selector:@selector(tick)
				       userInfo:nil
					repeats:YES];
	[self refresh];
}

/*
 * An agent wants to run something that needs the user's OK. Its row in the agent list
 * gets a warning, and the request is shown, with allow/deny, above that agent's reports.
 */
- (void)updatePerm
{
	perm_t *q = calloc(8, sizeof(*q));
	int n = q ? perm_load(q, 8) : 0;
	NSMutableDictionary<NSString *, NSNumber *> *by = [NSMutableDictionary dictionary];
	NSString *sel = [self selectedAgent].name, *newcomer = nil;
	NSInteger sh = -1;

	if (!_asked)
		_asked = [NSMutableSet set];
	for (int i = 0; i < n; i++) {
		NSString *name = [NSString stringWithUTF8String:q[i].agent];
		NSString *rid = [NSString stringWithUTF8String:q[i].id];

		by[name] = @([by[name] integerValue] + 1);
		if (sel && [name isEqualToString:sel] && sh < 0)
			sh = i;
		if (![_asked containsObject:rid]) {
			[_asked addObject:rid];
			if (!newcomer)
				newcomer = name;
		}
	}
	if (newcomer) {	/* a new question: bring the window up and show whose it is */
		[NSApp activateIgnoringOtherApps:YES];
		[_win makeKeyAndOrderFront:nil];
		[NSApp requestUserAttention:NSCriticalRequest];
		for (NSUInteger i = 0; i < _agents.count; i++)
			if ([_agents[i].name isEqualToString:newcomer] && ![newcomer isEqualToString:sel]) {
				_busy = YES;
				[_agentTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
				[self showSelectedAgentReports:nil];
				_busy = NO;
				sel = newcomer;
				sh = -1;
				for (int k = 0; k < n; k++)
					if ([[NSString stringWithUTF8String:q[k].agent] isEqualToString:sel]) {
						sh = k;
						break;
					}
			}
	}
	{
		NSMutableDictionary<NSString *, NSString *> *info = [NSMutableDictionary dictionary];

		for (int i = 0; i < n; i++) {
			NSString *nm = [NSString stringWithUTF8String:q[i].agent];

			if (!info[nm])
				info[nm] = [NSString stringWithFormat:@"%@: %@", friendly_tool([NSString stringWithUTF8String:q[i].tool]),
								     [NSString stringWithUTF8String:q[i].detail]];
		}
		_permInfo = info;
	}
	if (!_pendingBy || ![_pendingBy isEqualToDictionary:by]) {
		_pendingBy = by;
		[self reloadAgents:sel];
	}

	_permBar.hidden = sh < 0;
	if (sh >= 0) {
		NSString *agent = [NSString stringWithUTF8String:q[sh].agent];
		NSString *tool = [NSString stringWithUTF8String:q[sh].tool];
		NSString *detail = [NSString stringWithUTF8String:q[sh].detail];
		NSInteger mine = [by[agent] integerValue];

		_permId = [NSString stringWithUTF8String:q[sh].id];
		NSString *why = [NSString stringWithUTF8String:q[sh].why];

		_permLabel.stringValue = [NSString stringWithFormat:@"⚠ 이 에이전트가 승인을 요청합니다%@\n%@: %@\n%@",
								    mine > 1 ? [NSString stringWithFormat:@" (대기 %ld건)", (long)mine] : @"",
								    friendly_tool(tool), detail,
								    why.length ? [@"이유: " stringByAppendingString:why] : @""];
	} else {
		_permId = nil;
	}
	_permUp = sh >= 0;
	[self layoutPanes];
	free(q);
}

/* Agents on the left; on the right the reports, with a Markdown preview below them when one is selected. */
- (void)layoutPanes
{
	CGFloat h = _win.contentView.bounds.size.height, w = _win.contentView.bounds.size.width, top = h - 52;
	CGFloat rtop = top - (_permUp ? 70 : 0), room = rtop - 44, ph = _previewShown ? MAX(150, room * 0.55) : 0;

	_permBar.frame = NSMakeRect(320, top - 64, w - 332, 64);
	_agentScroll.frame = NSMakeRect(12, 44, 300, top - 44);
	_reportScroll.frame = NSMakeRect(320, 44 + ph + (ph ? 6 : 0), w - 332, room - ph - (ph ? 6 : 0));
	_previewScroll.frame = NSMakeRect(320, 44, w - 332, ph);
	_web.frame = _previewScroll.frame;
	_previewScroll.hidden = !_previewShown || _previewWeb;
	_web.hidden = !_previewShown || !_previewWeb;
}

- (void)windowDidResize:(NSNotification *)n
{
	[self layoutPanes];
}

/* Diagrams are drawn by Mermaid in the web view; with no network the source is shown instead. */
static NSString *web_page(NSString *md, NSString *title)
{
	NSData *j = [NSJSONSerialization dataWithJSONObject:@[ md ?: @"" ] options:0 error:nil];
	NSString *arr = j ? [[NSString alloc] initWithData:j encoding:NSUTF8StringEncoding] : @"[\"\"]";
	NSString *lit = [[arr substringWithRange:NSMakeRange(1, arr.length - 2)] stringByReplacingOccurrencesOfString:@"</" withString:@"<\\/"];

	return [NSString stringWithFormat:@"<!doctype html><meta charset=utf-8><style>"
		":root{color-scheme:light dark}body{font:13px -apple-system,sans-serif;margin:14px 18px;line-height:1.55}"
		"h1,h2,h3{margin:.9em 0 .4em}pre{background:rgba(128,128,128,.15);padding:8px 10px;border-radius:6px;overflow:auto}"
		"code{background:rgba(128,128,128,.15);padding:1px 4px;border-radius:3px}pre code{background:none;padding:0}"
		"blockquote{margin:0;padding-left:10px;border-left:3px solid #4a8af4;opacity:.85}"
		"table{border-collapse:collapse}td,th{border:1px solid rgba(128,128,128,.4);padding:3px 8px}"
		".mermaid{text-align:center;margin:12px 0}.note{opacity:.6;font-size:11px}.title{opacity:.6;font-size:11px}</style>"
		"<div class=title>%@  ·  미리보기</div><div id=out></div>"
		"<script src=\"https://cdn.jsdelivr.net/npm/marked@12.0.2/marked.min.js\"></script>"
		"<script src=\"https://cdn.jsdelivr.net/npm/mermaid@10.9.1/dist/mermaid.min.js\"></script>"
		"<script>var src=%@;var out=document.getElementById('out');"
		"function esc(t){return t.replace(/&/g,'&amp;').replace(/</g,'&lt;')}"
		"if(typeof marked==='undefined'){out.innerHTML='<p class=note>다이어그램과 문서를 그리려면 인터넷 연결이 필요합니다. 원본을 보여 줍니다.</p><pre>'+esc(src)+'</pre>';}"
		"else{out.innerHTML=marked.parse(src);"
		"var blocks=out.querySelectorAll('pre code.language-mermaid');"
		"if(typeof mermaid==='undefined'){if(blocks.length){var n=document.createElement('p');n.className='note';n.textContent='다이어그램을 그리려면 인터넷 연결이 필요합니다. 원본을 보여 줍니다.';out.prepend(n);}}"
		"else{blocks.forEach(function(c){var d=document.createElement('div');d.className='mermaid';d.textContent=c.textContent;c.parentNode.replaceWith(d)});"
		"mermaid.initialize({startOnLoad:false,theme:matchMedia('(prefers-color-scheme: dark)').matches?'dark':'default'});"
		"mermaid.run({querySelector:'.mermaid'}).catch(function(e){var p=document.createElement('p');p.className='note';p.textContent='다이어그램 문법 오류: '+e;out.prepend(p)});}}"
		"</script>", title, lit];
}

/*
 * What the file looked like before this report: the snapshot taken just before the agent's edits, else the
 * version right after the previous report of the same file. nil when the file is new (nothing to compare).
 */
- (NSString *)textBefore:(Report *)r
{
	NSData *d = read_dir_file([[NSString stringWithFormat:@"snap/%@/%ld.before", r.rid, (long)r.mdIndex] UTF8String]);
	NSUInteger at = [_all indexOfObject:r];

	if (d)
		return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
	if (read_dir_file([[NSString stringWithFormat:@"snap/%@/%ld.before.deleted", r.rid, (long)r.mdIndex] UTF8String]))
		return nil;
	for (NSUInteger q = at == NSNotFound ? 0 : at; q-- > 0;) {
		Report *p = _all[q];
		NSUInteger fi = [p.paths indexOfObject:r.mdPath];

		if (fi == NSNotFound || [p.status isEqualToString:@"되돌림"])
			continue;
		d = read_dir_file([[NSString stringWithFormat:@"snap/%@/%lu", p.rid, (unsigned long)fi] UTF8String]);
		return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil;
	}
	return nil;
}

/* Show the selected report's document: Markdown as text, Mermaid and images in the web view. */
- (void)updatePreview
{
	Report *r = [self selectedReport];
	NSString *key = nil, *ext = r.mdPath.pathExtension.lowercaseString;
	BOOL show = NO, web = NO;
	BOOL image = [@[ @"png", @"jpg", @"jpeg", @"gif", @"svg", @"webp" ] containsObject:ext];
	BOOL mmd = [ext isEqualToString:@"mmd"] || [ext isEqualToString:@"mermaid"];
	NSString *text = nil;

	if (r.mdPath.length) {
		NSDictionary *at = [NSFileManager.defaultManager attributesOfItemAtPath:r.mdPath error:nil];

		if (at && [at fileSize] < (image ? 20000000 : 600000)) {
			key = [NSString stringWithFormat:@"%@:%@:%@:%llu", r.rid, r.mdPath, at.fileModificationDate, at.fileSize];
			show = YES;
		}
	}
	if (show && !image) {
		text = [NSString stringWithContentsOfFile:r.mdPath encoding:NSUTF8StringEncoding error:nil];
		if (mmd)
			text = [NSString stringWithFormat:@"```mermaid\n%@\n```\n", text ?: @""];
		web = mmd || [text containsString:@"```mermaid"];
	}
	web = web || image;
	if (show && (![key isEqualToString:_previewKey] || web != _previewWeb)) {
		NSString *name = r.mdPath.lastPathComponent;

		if (image) {
			[_web loadFileURL:[NSURL fileURLWithPath:r.mdPath] allowingReadAccessToURL:[NSURL fileURLWithPath:r.mdPath.stringByDeletingLastPathComponent]];
		} else if (web) {
			[_web loadHTMLString:web_page(text, name) baseURL:nil];
		} else {
			NSMutableAttributedString *doc = [NSMutableAttributedString new];
			NSString *before = [self textBefore:r];
			NSIndexSet *changed = before && text ? changed_lines(before, text) : [NSIndexSet indexSet];

			[doc appendAttributedString:[[NSAttributedString alloc] initWithString:
					[name stringByAppendingString:changed.count ? @"  ·  미리보기  ·  바뀐 부분은 초록색\n" : @"  ·  미리보기\n"]
					attributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : NSColor.secondaryLabelColor }]];
			[doc appendAttributedString:render_markdown(text ?: @"(파일을 읽을 수 없습니다)", changed)];
			[_previewText.textStorage setAttributedString:doc];
			[_previewText scrollRangeToVisible:NSMakeRange(0, 0)];
		}
		_previewKey = key;
	}
	if (!show)
		_previewKey = nil;
	if (show != _previewShown || web != _previewWeb) {
		_previewShown = show;
		_previewWeb = web;
		[self layoutPanes];
	}
}

- (void)answerPerm:(BOOL)allow
{
	if (!_permId)
		return;
	perm_answer([_permId UTF8String], allow, "");
	[self updatePerm];
}

- (void)allowPerm:(id)s
{
	[self answerPerm:YES];
}

- (void)denyPerm:(id)s
{
	[self answerPerm:NO];
}

/* What agents are doing changes without touching the feed: re-read it every second. */
- (void)tick
{
	NSMutableArray<NSString *> *order = [NSMutableArray array];
	NSDictionary *by = load_status(order);
	NSString *selName = [self selectedAgent].name;

	if (_busy)
		return;
	[self updatePerm];
	for (NSString *n in order) {
		BOOL known = NO;

		if (!_showHidden && [by[n][@"hidden"] boolValue])
			continue;
		for (Agent *a in _agents)
			if ([a.name isEqualToString:n])
				known = YES;
		if (!known) {	/* activity from an agent with no report yet: refresh adds its row */
			_lastFeed = nil;
			[self refresh];
			return;
		}
	}
	for (Agent *a in _agents)
		if (!_showHidden && [by[a.name][@"hidden"] boolValue]) {	/* cleared from elsewhere (cait forget) */
			_lastFeed = nil;
			[self refresh];
			return;
		}
	for (Agent *a in _agents) {
		a.state = by[a.name][@"state"];
		a.doing = by[a.name][@"doing"];
		a.idle = [by[a.name][@"idle"] longValue];
		a.unreported = [by[a.name][@"unrep"] integerValue];
		a.final = [by[a.name][@"final"] boolValue];
		a.acked = [by[a.name][@"acked"] boolValue];
	}
	[self reloadAgents:selName];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a
{
	return YES;
}

- (Agent *)selectedAgent
{
	NSInteger i = _agentTable.selectedRow;

	return i >= 0 && i < (NSInteger)_agents.count ? _agents[(NSUInteger)i] : nil;
}

- (Report *)selectedReport
{
	NSInteger i = _reportTable.selectedRow;

	return i >= 0 && i < (NSInteger)_shown.count ? _shown[(NSUInteger)i] : nil;
}

- (void)refresh
{
	NSData *feed = read_dir_file("feed.tsv"), *stop = read_dir_file("stop");
	NSMutableArray<Agent *> *agents = [NSMutableArray array];
	NSMutableDictionary<NSString *, Agent *> *byName = [NSMutableDictionary dictionary];
	NSMutableSet<NSString *> *stopped = [NSMutableSet set];
	NSMutableArray<Report *> *all = [NSMutableArray array];
	NSDateFormatter *df = [[NSDateFormatter alloc] init];
	NSDictionary *labels = @{
		@"created" : @"생성됨", @"updated" : @"업데이트됨", @"deleted" : @"삭제됨", @"reverted" : @"되돌림", @"final" : @"완료 보고"
	};
	NSString *selName = [self selectedAgent].name;
	NSString *selRid = [self selectedReport].rid;
	BOOL all_stop = NO;

	if (_busy || ((feed == _lastFeed || [feed isEqualToData:_lastFeed]) &&
		      (stop == _lastStop || [stop isEqualToData:_lastStop])))
		return;
	_lastFeed = feed;
	_lastStop = stop;
	df.dateFormat = @"HH:mm:ss";

	for (NSString *l in [(stop ? [[NSString alloc] initWithData:stop encoding:NSUTF8StringEncoding] : @"")
				componentsSeparatedByString:@"\n"]) {
		NSArray *f = [l componentsSeparatedByString:@"\t"];

		if (f.count < 3)
			continue;
		if ([f[1] isEqualToString:@"*"])
			all_stop = YES;
		else
			[stopped addObject:f[1]];
	}

	for (NSString *l in [(feed ? [[NSString alloc] initWithData:feed encoding:NSUTF8StringEncoding] : @"")
				componentsSeparatedByString:@"\n"]) {
		rec_t r;
		char *buf = strdup([l UTF8String] ?: ""), key[24];
		Report *rp;
		NSString *files;
		NSInteger nf = 1;
		Agent *a;

		if (!l.length || !parse_rec(buf, &r)) {
			free(buf);
			continue;
		}
		rec_key(r.time, r.agent, r.files, key, sizeof(key));
		rp = [Report new];
		rp.rid = [NSString stringWithUTF8String:key];
		rp.agent = [NSString stringWithUTF8String:r.agent];
		rp.text = [NSString stringWithUTF8String:r.text];
		rp.status = [NSString stringWithUTF8String:r.status];
		files = [NSString stringWithUTF8String:r.files];
		for (NSUInteger i = 0; i < files.length; i++)
			if ([files characterAtIndex:i] == 0x1f)
				nf++;
		rp.paths = [files componentsSeparatedByString:@"\x1f"];
		for (NSString *one in rp.paths) {
			NSString *ext = one.pathExtension.lowercaseString;

			if ([@[ @"md", @"markdown", @"mmd", @"mermaid", @"png", @"jpg", @"jpeg", @"gif", @"svg", @"webp" ]
				    containsObject:ext]) {
				rp.mdPath = one;
				rp.mdIndex = (NSInteger)[rp.paths indexOfObject:one];
				break;
			}
		}
		rp.file = nf > 1 ? [NSString stringWithFormat:@"%ld개 파일", (long)nf] : files.lastPathComponent;
		rp.counts = r.added >= 0 && r.removed >= 0
				    ? [NSString stringWithFormat:@"+%ld −%ld", r.added, r.removed]
				    : @"";
		rp.when = [df stringFromDate:[NSDate dateWithTimeIntervalSince1970:r.time]];
		free(buf);
		[all addObject:rp];

		a = byName[rp.agent];
		if (!a) {
			a = [Agent new];
			a.name = rp.agent;
			byName[rp.agent] = a;
			[agents addObject:a];
		}
		a.count++;
		a.text = rp.text;
	}
	for (Report *r in all)
		r.status = labels[r.status] ?: @"변경됨";
	{
		NSMutableArray<NSString *> *order = [NSMutableArray array];
		NSDictionary *by = load_status(order);

		for (NSString *n in order)
			if (!byName[n] && (_showHidden || ![by[n][@"hidden"] boolValue])) {	/* working, but has not reported yet */
				Agent *a = [Agent new];

				a.name = n;
				a.text = @"";
				byName[n] = a;
				[agents addObject:a];
			}
		for (Agent *a in agents) {
			a.state = by[a.name][@"state"];
			a.doing = by[a.name][@"doing"];
			a.idle = [by[a.name][@"idle"] longValue];
			a.unreported = [by[a.name][@"unrep"] integerValue];
			a.final = [by[a.name][@"final"] boolValue];
			a.acked = [by[a.name][@"acked"] boolValue];
		}
		if (!_showHidden)	/* cleared from the list; their reports stay in the feed and on cait log */
			[agents filterUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(Agent *a, NSDictionary *u) {
				(void)u;
				return ![by[a.name][@"hidden"] boolValue];
			}]];
	}
	for (Agent *a in agents)
		a.stopped = all_stop || [stopped containsObject:a.name];

	_agents = agents;
	_all = all;
	_allStop = all_stop;
	[self reloadAgents:selName];
	_busy = YES;	/* our own selection changes must not trigger another refresh */
	[self showSelectedAgentReports:selRid];
	_busy = NO;
}

/* Work out how each agent looks in the list: its state in words, colour and one line of detail. */
- (void)decorate:(Agent *)a
{
	NSString *st = a.state ?: @"", *last = a.text.length ? a.text : @"";
	NSInteger waiting = [_pendingBy[a.name] integerValue];

	a.color = NSColor.secondaryLabelColor;
	if (waiting) {
		a.rank = 0;
		a.label = [NSString stringWithFormat:@"⚠ 승인 대기 %ld", (long)waiting];
		a.color = NSColor.systemRedColor;
		a.sub = _permInfo[a.name] ?: @"허락을 기다립니다";
	} else if (a.stopped) {
		a.rank = 1;
		a.label = @"중단됨";
		a.color = NSColor.systemRedColor;
		a.sub = last.length ? [@"마지막 보고: " stringByAppendingString:last] : @"중단 요청이 걸려 있습니다";
	} else if ([st isEqualToString:@"done"] && (a.unreported || !a.final)) {
		BOOL asked = [_askedReport[a.name] isEqualToString:ask_key(a)];

		a.rank = 1;
		a.label = asked ? @"보고 요청 중" : @"확인 필요";
		a.color = NSColor.systemOrangeColor;
		a.sub = asked ? @"보고를 요청했습니다. 에이전트가 답하면 여기에 올라옵니다"
		      : a.unreported ? [NSString stringWithFormat:@"마지막 보고 뒤 수정 %ld건이 보고되지 않았습니다", (long)a.unreported]
				     : @"끝났지만 완료 보고가 오지 않았습니다";
	} else if ([st isEqualToString:@"done"]) {
		a.rank = 3;
		a.label = @"완료";
		a.sub = a.acked ? (last.length ? [@"확인함 · " stringByAppendingString:last] : @"확인함")
				: last.length ? last : @"완료 보고가 도착했습니다";
	} else if ([st isEqualToString:@"reported"]) {
		a.rank = 3;
		a.label = @"보고 있음";
		a.sub = last;
	} else if ([st isEqualToString:@"quiet"]) {
		a.rank = 2;
		a.label = @"잠시 멈춤";
		a.color = NSColor.systemOrangeColor;
		a.sub = [NSString stringWithFormat:@"%ld초째 조용함%@", (long)a.idle,
						   a.doing.length ? [@" · 마지막: " stringByAppendingString:a.doing] : @""];
	} else {
		a.rank = 2;
		a.label = @"진행 중";
		a.color = NSColor.systemGreenColor;
		a.sub = a.doing.length ? a.doing : @"작업을 시작하는 중";
	}
}

/*
 * Redraw the agent list: needs-you-now first, then needs-a-look, running, finished.
 * Keeps the same agent selected, and updates the summary, Dock badge and buttons.
 */
- (void)reloadAgents:(NSString *)selName
{
	NSInteger n[4] = { 0, 0, 0, 0 };
	NSMutableArray<NSString *> *parts = [NSMutableArray array];
	NSUInteger ai = NSNotFound;

	for (Agent *a in _agents)
		[self decorate:a];
	_agents = [_agents sortedArrayUsingComparator:^NSComparisonResult(Agent *x, Agent *y) {
		if (x.rank != y.rank)
			return x.rank < y.rank ? NSOrderedAscending : NSOrderedDescending;
		if (x.idle != y.idle)
			return x.idle < y.idle ? NSOrderedAscending : NSOrderedDescending;
		return NSOrderedSame;
	}];
	for (Agent *a in _agents)
		n[a.rank]++;
	for (NSUInteger i = 0; i < _agents.count; i++)
		if ([_agents[i].name isEqualToString:selName])
			ai = i;
	_busy = YES;
	[_agentTable reloadData];
	if (_agents.count)
		[_agentTable selectRowIndexes:[NSIndexSet indexSetWithIndex:ai == NSNotFound ? 0 : ai] byExtendingSelection:NO];
	_busy = NO;
	_emptyLabel.hidden = _agents.count > 0;

	[parts addObject:[NSString stringWithFormat:@"에이전트 %lu", (unsigned long)_agents.count]];
	if (n[0])
		[parts addObject:[NSString stringWithFormat:@"승인 대기 %ld", (long)n[0]]];
	if (n[1])
		[parts addObject:[NSString stringWithFormat:@"확인 필요 %ld", (long)n[1]]];
	if (n[2])
		[parts addObject:[NSString stringWithFormat:@"진행 중 %ld", (long)n[2]]];
	if (n[3])
		[parts addObject:[NSString stringWithFormat:@"완료 %ld", (long)n[3]]];
	_counts = [[parts componentsJoinedByString:@"  ·  "] stringByAppendingString:_allStop ? @"   ⛔ 전체 중단 요청됨" : @""];
	[self updateBadge];
	[self updateHeader];
}

/* a request stays "in progress" until the agent does something (its counts change) */
static NSString *ask_key(Agent *a)
{
	return [NSString stringWithFormat:@"%@:%@:%ld:%d", a.name, a.state, (long)a.unreported, a.final];
}

static NSString *seen_key(Agent *a)
{
	return [NSString stringWithFormat:@"%@:%ld:%d", a.name, (long)a.unreported, a.final];
}

/* Looking at an agent in the window counts as having seen what needs a look. */
- (void)markSeen
{
	Agent *a = [self selectedAgent];

	if (!_seen)
		_seen = [NSMutableSet set];
	if (a && a.rank == 1 && _win.isKeyWindow)
		[_seen addObject:seen_key(a)];
}

/* Dock badge and title: what waits for an answer, plus what has not been looked at yet. */
- (void)updateBadge
{
	NSInteger att = 0;

	[self markSeen];
	for (Agent *a in _agents)
		if (a.rank == 0 || (a.rank == 1 && ![_seen containsObject:seen_key(a)]))
			att++;
	NSApp.dockTile.badgeLabel = att ? [NSString stringWithFormat:@"%ld", (long)att] : nil;
	_win.title = att ? [NSString stringWithFormat:@"cait — 확인할 것 %ld", (long)att] : @"cait";
}

/* Summary on the first line; on the second, what the selected agent was asked and what it does now. */
- (void)updateHeader
{
	Agent *sel = [self selectedAgent];
	NSMutableArray<NSString *> *parts = [NSMutableArray array];
	NSMutableAttributedString *as = [NSMutableAttributedString new];
	NSDictionary *big = @{ NSFontAttributeName : [NSFont boldSystemFontOfSize:14] };
	NSDictionary *small = @{ NSFontAttributeName : [NSFont systemFontOfSize:12],
				 NSForegroundColorAttributeName : NSColor.secondaryLabelColor };

	if (sel) {
		NSString *rel = [@"brief/" stringByAppendingString:[sel.name stringByReplacingOccurrencesOfString:@"/" withString:@"_"]];
		NSData *d = read_dir_file([rel UTF8String]);
		NSString *t = d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil;

		if (t.length)
			[parts addObject:[@"시킨 일: " stringByAppendingString:[t componentsSeparatedByString:@"\n"][0]]];
		if (sel.sub.length)
			[parts addObject:sel.sub];
	} else {
		[parts addObject:@"왼쪽에서 에이전트를 고르면 시킨 일과 보고가 보입니다."];
	}
	[as appendAttributedString:[[NSAttributedString alloc] initWithString:_counts ?: @"" attributes:big]];
	[as appendAttributedString:[[NSAttributedString alloc]
		initWithString:[@"\n" stringByAppendingString:[parts componentsJoinedByString:@"   ·   "]] attributes:small]];
	_header.attributedStringValue = as;
	[self updateButtons];
}

/* Only offer what makes sense for the selection. */
- (void)updateButtons
{
	Agent *a = [self selectedAgent];
	Report *r = [self selectedReport];
	BOOL running = a && ([a.state isEqualToString:@"working"] || [a.state isEqualToString:@"quiet"]);

	_stopBtn.enabled = running && !a.stopped;
	_forgetBtn.enabled = a && !running;
	_undoBtn.enabled = r && ![r.status isEqualToString:@"되돌림"] && ![r.status isEqualToString:@"완료 보고"];
	_fixBtn.enabled = r != nil;
	BOOL isAsked = a && [_askedReport[a.name] isEqualToString:ask_key(a)];

	_askReportBtn.title = isAsked ? @"요청 진행 중…" : @"보고 요청";
	_askReportBtn.enabled = a && (a.unreported || !a.final) && !running && !isAsked;
	[self updatePreview];
	_ackBtn.enabled = a && [a.state isEqualToString:@"done"] && (a.unreported || !a.final);
}

/* The user looked at the result: stop asking for a look until the agent does more. */
- (void)ackAgent:(id)sender
{
	Agent *a = [self selectedAgent];

	if (a && ack_agent([a.name UTF8String]))
		[self refresh];
}

/* An agent finished without reporting everything: have the main agent ask it to. */
- (void)askReport:(id)sender
{
	Agent *a = [self selectedAgent];
	NSString *msg;
	int st;

	if (!a)
		return;
	msg = [NSString stringWithFormat:@"%@ 에이전트가 보고 없이 파일을 고쳤거나 완료 보고 없이 끝났습니다(보고 안 한 수정 %ld번). "
					 @"그 에이전트에게 지금까지 바꾼 파일과 내용을 cait report로 보고하고 --final로 마무리하라고 "
					 @"다시 지시해 주세요.",
					 a.name, (long)a.unreported];
	run_cait(@[ @"tell", @"--agent", a.name, msg ], &st);
	if (st == 0) {
		if (!_askedReport)
			_askedReport = [NSMutableDictionary dictionary];
		_askedReport[a.name] = ask_key(a);
	}
	[self reloadAgents:a.name];
}

/* Say why the report list is empty instead of leaving a blank table. */
- (void)updateNoReports
{
	Agent *a = [self selectedAgent];
	NSString *t = nil;

	if (a && _shown.count == 0) {
		if (a.unreported)
			t = [NSString stringWithFormat:@"이 에이전트는 보고 없이 파일을 %ld번 고쳤습니다.\n\n무엇을 바꿨는지 에이전트가 알려 주지 않아 여기서는 볼 수 없습니다.\n위쪽 \"보고 요청\" 버튼을 누르면 메인 에이전트가 이 에이전트에게 보고를 다시 시킵니다.", (long)a.unreported];
		else if ([a.state isEqualToString:@"working"] || [a.state isEqualToString:@"quiet"])
			t = @"아직 보고가 없습니다.\n\n파일을 바꾸면 여기에 쉬운 말로 올라옵니다.";
		else
			t = @"보고 없이 끝났습니다.\n\n파일을 바꾸지 않고 읽거나 찾기만 했을 수 있습니다.";
	}
	_noReports.stringValue = t ?: @"";
	_noReports.hidden = t == nil;
}

/* Fill the report table for the selected agent, newest at the bottom. */
- (void)showSelectedAgentReports:(NSString *)keepRid
{
	NSString *name = [self selectedAgent].name;
	NSMutableArray<Report *> *mine = [NSMutableArray array];
	NSUInteger keep = NSNotFound;

	for (Report *r in _all)
		if ([r.agent isEqualToString:name])
			[mine addObject:r];
	_shown = mine;
	[self updateNoReports];
	[self updateHeader];
	[_reportTable reloadData];
	for (NSUInteger i = 0; keepRid && i < mine.count; i++)
		if ([mine[i].rid isEqualToString:keepRid])
			keep = i;
	if (keep != NSNotFound) {
		[_reportTable selectRowIndexes:[NSIndexSet indexSetWithIndex:keep] byExtendingSelection:NO];
	} else if (mine.count) {
		[_reportTable scrollRowToVisible:(NSInteger)mine.count - 1];
		[_reportTable selectRowIndexes:[NSIndexSet indexSetWithIndex:mine.count - 1] byExtendingSelection:NO];
	}
}

- (void)tableViewSelectionDidChange:(NSNotification *)n
{
	if (_busy)
		return;
	if (n.object != _agentTable) {
		[self updateButtons];
		return;
	}
	[self showSelectedAgentReports:nil];
	[self updateBadge];
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
	[self updateBadge];
}

- (void)alert:(NSString *)title info:(NSString *)info
{
	NSAlert *a = [NSAlert new];

	a.messageText = title;
	a.informativeText = info;
	[a runModal];
}

- (void)stopAgent:(id)s
{
	Agent *a = [self selectedAgent];

	if (!a)
		return;
	stop_add([a.name UTF8String], "stopped from gui");
	_lastStop = nil;
	[self refresh];
}

- (void)clearStops:(id)s
{
	char path[4200];

	path_in_dir(path, sizeof(path), "stop");
	unlink(path);
	_lastStop = nil;
	[self refresh];
}

- (void)undoReport:(id)s
{
	Report *r = [self selectedReport];
	NSString *plan;
	NSAlert *a;
	int st;

	if (!r)
		return;
	if ([r.status isEqualToString:@"되돌림"]) {
		[self alert:@"되돌릴 수 없는 항목입니다" info:@"이 항목은 이미 되돌린 기록입니다."];
		return;
	}
	if ([r.status isEqualToString:@"완료 보고"]) {
		[self alert:@"되돌릴 변경이 없습니다" info:@"완료 보고는 에이전트가 마지막에 남긴 요약이라 파일 변경이 없습니다. 되돌리려면 변경이 있는 보고를 고르세요."];
		return;
	}
	plan = run_cait(@[ @"revert", @"--id", r.rid, @"--dry-run" ], &st);
	a = [NSAlert new];
	a.messageText = [NSString stringWithFormat:@"이 보고를 되돌릴까요?\n%@", r.text];
	a.informativeText = [NSString stringWithFormat:@"%@\n되돌리기 전 파일은 ~/.cait/undo에 백업됩니다.", plan];
	[a addButtonWithTitle:@"되돌리기"];
	[a addButtonWithTitle:@"취소"];
	if (st != 0 || [a runModal] != NSAlertFirstButtonReturn) {
		if (st != 0)
			[self alert:@"되돌릴 수 없습니다" info:plan];
		return;
	}
	plan = run_cait(@[ @"revert", @"--id", r.rid ], &st);
	if (st != 0)
		[self alert:@"일부 파일을 되돌리지 못했습니다" info:plan];
	_lastFeed = nil;
	[self refresh];
}

- (void)forgetSelected:(id)s
{
	Agent *a = [self selectedAgent];
	int running = 0;

	if (!a)
		return;
	forget_agents([a.name UTF8String], &running);
	if (running)
		[self alert:@"아직 실행 중입니다" info:@"실행 중인 에이전트는 치울 수 없습니다. 먼저 중지하세요."];
	_lastFeed = nil;
	[self refresh];
}

- (void)forgetDone:(id)s
{
	forget_agents(NULL, NULL);
	_lastFeed = nil;
	[self refresh];
}

- (void)toggleHidden:(NSButton *)b
{
	_showHidden = b.state == NSControlStateValueOn;
	_lastFeed = nil;
	[self refresh];
}

- (void)setGuard:(NSPopUpButton *)b
{
	run_cait(@[ @"guard", @"mode", b.selectedItem.representedObject ], NULL);
}

- (void)askFix:(id)s
{
	Report *r = [self selectedReport];
	NSAlert *a = [NSAlert new];
	NSTextField *in = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 60)];
	NSButton *stopToo = [NSButton checkboxWithTitle:@"이 에이전트도 중지" target:nil action:nil];
	NSStackView *box = [NSStackView stackViewWithViews:@[ in, stopToo ]];
	NSString *msg;
	int st;

	if (!r)
		return;
	in.placeholderString = @"어떻게 고치라고 할지 적어 주세요";
	stopToo.state = NSControlStateValueOn;
	box.orientation = NSUserInterfaceLayoutOrientationVertical;
	box.alignment = NSLayoutAttributeLeading;
	box.frame = NSMakeRect(0, 0, 380, 90);
	a.messageText = [NSString stringWithFormat:@"%@ — %@", r.agent, r.text];
	a.informativeText = @"적은 내용은 메인 에이전트에게 전달되어 다시 지시를 내립니다.";
	a.accessoryView = box;
	[a addButtonWithTitle:@"전달"];
	[a addButtonWithTitle:@"취소"];
	[a.window setInitialFirstResponder:in];
	if ([a runModal] != NSAlertFirstButtonReturn || in.stringValue.length == 0)
		return;
	msg = [NSString stringWithFormat:@"%@ 에이전트의 보고 \"%@\" (%@)에 대한 수정 요청: %@", r.agent, r.text,
					 r.file, in.stringValue];
	run_cait(@[ @"tell", @"--agent", r.agent, msg ], &st);
	if (stopToo.state == NSControlStateValueOn)
		stop_add([r.agent UTF8String], "correction requested from gui");
	_lastStop = nil;
	[self refresh];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t
{
	return (NSInteger)(t == _agentTable ? _agents.count : _shown.count);
}

- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)c row:(NSInteger)r
{
	NSTextField *f = [NSTextField labelWithString:@""];
	NSString *id = c.identifier;

	f.lineBreakMode = NSLineBreakByTruncatingTail;
	if (t == _agentTable) {
		Agent *a = _agents[(NSUInteger)r];

		if ([id isEqualToString:@"name"]) {	/* two lines: the name, then what is going on */
			NSTableCellView *v = [t makeViewWithIdentifier:@"agentcell" owner:self];
			NSTextField *top, *bottom;

			if (!v) {
				v = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 186, 38)];
				v.identifier = @"agentcell";
				top = [NSTextField labelWithString:@""];
				top.tag = 1;
				top.frame = NSMakeRect(2, 19, 182, 17);
				top.font = [NSFont boldSystemFontOfSize:13];
				top.lineBreakMode = NSLineBreakByTruncatingTail;
				top.autoresizingMask = NSViewWidthSizable;
				bottom = [NSTextField labelWithString:@""];
				bottom.tag = 2;
				bottom.frame = NSMakeRect(2, 2, 182, 15);
				bottom.font = [NSFont systemFontOfSize:11];
				bottom.textColor = NSColor.secondaryLabelColor;
				bottom.lineBreakMode = NSLineBreakByTruncatingTail;
				bottom.autoresizingMask = NSViewWidthSizable;
				[v addSubview:top];
				[v addSubview:bottom];
			}
			((NSTextField *)[v viewWithTag:1]).stringValue = a.name;
			bottom = [v viewWithTag:2];
			bottom.stringValue = a.sub ?: @"";
			bottom.toolTip = a.sub;
			return v;
		}
		f.stringValue = a.label ?: @"";
		f.textColor = a.color ?: NSColor.labelColor;
		if (a.rank <= 1)
			f.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
	} else {
		Report *p = _shown[(NSUInteger)r];

		if ([id isEqualToString:@"when"])
			f.stringValue = p.when;
		else if ([id isEqualToString:@"status"])
			f.stringValue = p.status;
		else if ([id isEqualToString:@"file"])
			f.stringValue = p.file;
		else
			f.stringValue = p.text;
		if ([id isEqualToString:@"file"]) {
			f.toolTip = p.counts.length ? [NSString stringWithFormat:@"%@  (%@)", p.file, p.counts] : p.file;
		} else if ([id isEqualToString:@"text"]) {
			f.lineBreakMode = NSLineBreakByWordWrapping;
			f.maximumNumberOfLines = 2;
			f.usesSingleLineMode = NO;
			f.cell.wraps = YES;
			f.toolTip = p.text;
		}
		if ([p.status isEqualToString:@"되돌림"])
			f.textColor = NSColor.secondaryLabelColor;
	}
	return f;
}

@end

int cmd_gui(int argc, char **argv)
{
	(void)argv;
	if (argc > 0) {
		fprintf(stderr, "usage: cait gui\n");
		return 1;
	}
	ensure_dir();
	@autoreleasepool {
		App *app = [App new];

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		NSApp.delegate = app;
		[NSApp activateIgnoringOtherApps:YES];
		[NSApp run];
	}
	return 0;
}
