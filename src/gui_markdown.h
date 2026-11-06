#import <Cocoa/Cocoa.h>

/* Markdown text as a styled document; lines in `changed` (0-based) get a coloured background. */
NSAttributedString *render_markdown(NSString *md, NSIndexSet *changed);
/* Which lines of `after` are new or changed compared with `before`. */
NSIndexSet *changed_lines(NSString *before, NSString *after);
/* A web page that draws Mermaid blocks in `md`; shows the source when offline. */
NSString *web_page(NSString *md, NSString *title);
