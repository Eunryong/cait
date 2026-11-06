/* Markdown and Mermaid preview support for the cait window: render, compare, and wrap documents. */
#import <Cocoa/Cocoa.h>

#import "gui_markdown.h"

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
NSIndexSet *changed_lines(NSString *before, NSString *after)
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

NSAttributedString *render_markdown(NSString *md, NSIndexSet *changed)
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

/* Diagrams are drawn by Mermaid in the web view; with no network the source is shown instead. */
NSString *web_page(NSString *md, NSString *title)
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
