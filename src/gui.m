/*
 * cait gui: a small window over the feed (macOS). Pick an agent, then one of
 * its reports, and stop the agent, undo that report, or send the main agent
 * a correction. Only this window is Objective-C; the rest of cait is C.
 */
#import <Cocoa/Cocoa.h>
#include <unistd.h>

#include "cait.h"

@interface Report : NSObject
@property NSString *rid, *agent, *status, *file, *counts, *text, *when;
@end
@implementation Report
@end

@interface Agent : NSObject
@property NSString *name, *text, *state, *doing;	/* state/doing: from cait status */
@property NSInteger count, idle, unreported;
@property BOOL final;
@property BOOL stopped;
@end
@implementation Agent
@end

@interface App : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate>
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
@property NSScrollView *agentScroll, *reportScroll;
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

		if (st[i].tool[0] && ![state isEqualToString:@"done"])
			doing = [NSString stringWithFormat:@"%@ %@", [NSString stringWithUTF8String:st[i].tool],
						       [NSString stringWithUTF8String:st[i].target]];
		by[name] = @{ @"state" : state, @"doing" : doing, @"idle" : @(st[i].idle),
			@"unrep" : @(st[i].unreported), @"final" : @(st[i].final) };
		[order addObject:name];
	}
	free(st);
	return by;
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
	_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 860, 560)
					   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
						     NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
					     backing:NSBackingStoreBuffered
					       defer:NO];
	_win.title = @"cait";
	[_win center];
	root = _win.contentView;

	_header = [NSTextField labelWithString:@""];
	_header.frame = NSMakeRect(12, 528, 836, 22);
	_header.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
	_header.font = [NSFont boldSystemFontOfSize:13];
	[root addSubview:_header];

	_agentTable = [self tableWithColumns:@[ @[ @"name", @"에이전트", @90 ], @[ @"count", @"건수", @34 ],
						@[ @"state", @"상태", @130 ] ]];
	as = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 44, 300, 476)];
	as.documentView = _agentTable;
	as.hasVerticalScroller = YES;
	as.borderType = NSBezelBorder;
	as.autoresizingMask = NSViewHeightSizable;
	[root addSubview:as];
	_agentScroll = as;

	_reportTable = [self tableWithColumns:@[ @[ @"when", @"시간", @70 ], @[ @"status", @"상태", @80 ],
						 @[ @"file", @"파일", @140 ], @[ @"counts", @"변경", @70 ],
						 @[ @"text", @"요약", @400 ] ]];
	rs = [[NSScrollView alloc] initWithFrame:NSMakeRect(320, 44, 528, 476)];
	rs.documentView = _reportTable;
	rs.hasVerticalScroller = YES;
	rs.borderType = NSBezelBorder;
	rs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	[root addSubview:rs];
	_reportScroll = rs;

	_permBar = [[NSView alloc] initWithFrame:NSMakeRect(320, 470, 528, 46)];
	_permBar.wantsLayer = YES;
	_permBar.layer.backgroundColor = [[NSColor.systemYellowColor colorWithAlphaComponent:0.28] CGColor];
	_permBar.layer.cornerRadius = 6;
	_permBar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
	_permBar.hidden = YES;
	_permLabel = [NSTextField labelWithString:@""];
	_permLabel.frame = NSMakeRect(10, 4, 340, 38);
	_permLabel.maximumNumberOfLines = 2;
	_permLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
	_permLabel.autoresizingMask = NSViewWidthSizable;
	[_permBar addSubview:_permLabel];
	{
		NSButton *ok = [NSButton buttonWithTitle:@"허용" target:self action:@selector(allowPerm:)];
		NSButton *no = [NSButton buttonWithTitle:@"거부" target:self action:@selector(denyPerm:)];

		ok.frame = NSMakeRect(528 - 80 - 10, 9, 80, 28);
		no.frame = NSMakeRect(528 - 160 - 16, 9, 80, 28);
		ok.bezelColor = NSColor.systemGreenColor;
		ok.autoresizingMask = no.autoresizingMask = NSViewMinXMargin;
		[_permBar addSubview:no];
		[_permBar addSubview:ok];
	}
	[root addSubview:_permBar];

	[root addSubview:[self button:@"에이전트 중지" x:12 w:110 action:@selector(stopAgent:)]];
	[root addSubview:[self button:@"중지 해제" x:128 w:80 action:@selector(clearStops:)]];
	[root addSubview:[self button:@"이 보고 되돌리기" x:320 w:130 action:@selector(undoReport:)]];
	[root addSubview:[self button:@"수정 요청…" x:456 w:100 action:@selector(askFix:)]];

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
	CGFloat h = _win.contentView.bounds.size.height, w = _win.contentView.bounds.size.width, top = h - 40;

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
	if (!_pendingBy || ![_pendingBy isEqualToDictionary:by]) {
		NSInteger row = _agentTable.selectedRow;

		_pendingBy = by;
		_busy = YES;
		[_agentTable reloadData];
		if (row >= 0 && row < (NSInteger)_agents.count)
			[_agentTable selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
		_busy = NO;
	}

	_permBar.hidden = sh < 0;
	if (sh >= 0) {
		NSString *agent = [NSString stringWithUTF8String:q[sh].agent];
		NSString *tool = [NSString stringWithUTF8String:q[sh].tool];
		NSString *detail = [NSString stringWithUTF8String:q[sh].detail];
		NSInteger mine = [by[agent] integerValue];

		_permId = [NSString stringWithUTF8String:q[sh].id];
		_permLabel.stringValue = [NSString stringWithFormat:@"⚠ 이 에이전트가 승인을 요청합니다%@\n%@  %@",
								    mine > 1 ? [NSString stringWithFormat:@" (대기 %ld건)", (long)mine] : @"",
								    tool, detail];
	} else {
		_permId = nil;
	}
	_permBar.frame = NSMakeRect(320, top - 46, w - 332, 46);
	_agentScroll.frame = NSMakeRect(12, 44, 300, top - 44);
	_reportScroll.frame = NSMakeRect(320, 44, w - 332, top - 44 - (sh >= 0 ? 52 : 0));
	free(q);
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
	NSInteger row = _agentTable.selectedRow;

	if (_busy)
		return;
	[self updatePerm];
	for (NSString *n in order) {
		BOOL known = NO;

		for (Agent *a in _agents)
			if ([a.name isEqualToString:n])
				known = YES;
		if (!known) {	/* activity from an agent with no report yet: refresh adds its row */
			_lastFeed = nil;
			[self refresh];
			return;
		}
	}
	for (Agent *a in _agents) {
		a.state = by[a.name][@"state"];
		a.doing = by[a.name][@"doing"];
		a.idle = [by[a.name][@"idle"] longValue];
		a.unreported = [by[a.name][@"unrep"] integerValue];
		a.final = [by[a.name][@"final"] boolValue];
	}
	_busy = YES;
	[_agentTable reloadData];
	if (row >= 0 && row < (NSInteger)_agents.count && [_agents[(NSUInteger)row].name isEqualToString:selName])
		[_agentTable selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
	_busy = NO;
	[self updateHeader];
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
			if (!byName[n]) {	/* working, but has not reported yet */
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
		}
	}
	for (Agent *a in agents)
		a.stopped = all_stop || [stopped containsObject:a.name];

	_busy = YES;	/* our own selection changes must not trigger another refresh */
	_agents = agents;
	_all = all;
	[_agentTable reloadData];
	NSUInteger ai = 0;
	for (NSUInteger i = 0; i < agents.count; i++)
		if ([agents[i].name isEqualToString:selName])
			ai = i;
	if (agents.count)
		[_agentTable selectRowIndexes:[NSIndexSet indexSetWithIndex:ai] byExtendingSelection:NO];
	[self showSelectedAgentReports:selRid];
	_busy = NO;

	_counts = [NSString stringWithFormat:@"에이전트 %lu  보고 %lu%@", (unsigned long)agents.count,
							 (unsigned long)all.count, all_stop ? @"   ⛔ 전체 중단 요청됨" : @""];
	[self updateHeader];
}

/* Counts, plus the selected agent's task: the first line of what the main agent asked it. */
- (void)updateHeader
{
	NSString *name = [self selectedAgent].name, *task = @"";

	if (name) {
		NSString *rel = [@"brief/" stringByAppendingString:[name stringByReplacingOccurrencesOfString:@"/" withString:@"_"]];
		NSData *d = read_dir_file([rel UTF8String]);
		NSString *t = d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : nil;

		if (t.length)
			task = [@"   —   " stringByAppendingString:[t componentsSeparatedByString:@"\n"][0]];
	}
	Agent *sel = [self selectedAgent];
	NSString *now = sel.doing.length ? [NSString stringWithFormat:@"   ·   %@ (%lds 전)", sel.doing, (long)sel.idle] : @"";

	_header.stringValue = [[(_counts ?: @"") stringByAppendingString:task] stringByAppendingString:now];
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
	if (_busy || n.object != _agentTable)
		return;
	[self showSelectedAgentReports:nil];
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

		if ([id isEqualToString:@"name"]) {
			f.stringValue = a.name;
		} else if ([id isEqualToString:@"count"]) {
			f.stringValue = [NSString stringWithFormat:@"%ld", (long)a.count];
		} else {
			NSString *st = a.state ?: @"";
			NSInteger waiting = [_pendingBy[a.name] integerValue];

			if (waiting) {
				f.stringValue = [NSString stringWithFormat:@"⚠ 승인 대기 %ld", (long)waiting];
				f.textColor = NSColor.systemRedColor;
				f.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
			} else if (a.stopped) {
				f.stringValue = @"중단됨";
				f.textColor = NSColor.systemRedColor;
			} else if ([st isEqualToString:@"done"]) {
				f.stringValue = a.unreported ? [NSString stringWithFormat:@"끝남·미보고 수정 %ld", (long)a.unreported]
							     : a.final ? @"완료 보고됨" : @"끝남·완료 보고 없음";
				f.textColor = a.final && !a.unreported ? NSColor.secondaryLabelColor : NSColor.systemOrangeColor;
			} else if ([st isEqualToString:@"quiet"]) {
				f.stringValue = @"조용함";
				f.textColor = NSColor.systemOrangeColor;
			} else if ([st isEqualToString:@"reported"]) {
				f.stringValue = @"보고만";
				f.textColor = NSColor.secondaryLabelColor;
			} else {
				f.stringValue = @"진행 중";
				f.textColor = NSColor.systemGreenColor;
			}
		}
	} else {
		Report *p = _shown[(NSUInteger)r];

		if ([id isEqualToString:@"when"])
			f.stringValue = p.when;
		else if ([id isEqualToString:@"status"])
			f.stringValue = p.status;
		else if ([id isEqualToString:@"file"])
			f.stringValue = p.file;
		else if ([id isEqualToString:@"counts"])
			f.stringValue = p.counts;
		else
			f.stringValue = p.text;
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
