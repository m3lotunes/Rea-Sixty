// The 360 key on the UAD Console layer (Frank 04.11.2026: "wenn man den 360
// Knopf drückt, dass dann die UAD-Konsole aufgeht"): UAD Console to the front,
// started if it is not running; when it is already in front, REAPER comes back,
// as 360 toggles the mixer on the REAPER layer.
//
// Called from the UF8 input path, which is not the main thread; AppKit is, so
// the work goes to the main queue.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

namespace uf8 {

static NSString* const kUadConsoleBundle = @"com.uaudio.console3";

void macosUadConsoleToggle()
{
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            NSWorkspace* ws = [NSWorkspace sharedWorkspace];
            NSRunningApplication* front = [ws frontmostApplication];
            if (front && [[front bundleIdentifier] isEqualToString:kUadConsoleBundle]) {
                [NSApp activateIgnoringOtherApps:YES];   // back to REAPER
                return;
            }
            NSURL* url = [ws URLForApplicationWithBundleIdentifier:kUadConsoleBundle];
            if (!url) {
                NSLog(@"Rea-Sixty: UAD Console (%@) is not installed", kUadConsoleBundle);
                return;
            }
            // Starts it, or brings the running one to the front.
            NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
            cfg.activates = YES;
            [ws openApplicationAtURL:url configuration:cfg completionHandler:nil];
        }
    });
}

}  // namespace uf8
