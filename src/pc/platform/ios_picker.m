/* The iPhone's document picker (MEMORIES_IOS), which SDL has none of on
 * iOS: Platform_SelectDisc (sdl.c) asks for the disc image with it. The
 * picker copies the chosen document into the app (asCopy), and from there
 * it moves to its place, so no lasting right to read it is needed. Host
 * code only (tools/pc/build_arm64.py ORDINARY), with ARC. */
#ifdef MEMORIES_IOS
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdio.h>

@interface IosPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, strong) UIWindow *holder;
@property(nonatomic, strong) NSURL *chosen;
@property(nonatomic, strong) dispatch_semaphore_t finished;
@property(atomic) BOOL done;
@end

@implementation IosPicker
- (void)finish
{
    self.holder.hidden = YES;
    self.holder = nil;
    self.done = YES;
    dispatch_semaphore_signal(self.finished);
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    self.chosen = urls.firstObject;
    [self finish];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    [self finish];
}
@end

/* On the main thread: a window of its own over everything (there may be
 * no game window yet), whose controller shows the picker. Any document:
 * a .bin has no type of its own, and the game checks what it holds. */
static void present(IosPicker *picking)
{
    UIWindowScene *scene = nil;
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    for (UIScene *each in UIApplication.sharedApplication.connectedScenes)
        if ([each isKindOfClass:UIWindowScene.class]) {
            scene = (UIWindowScene *)each;
            break;
        }
    picking.holder = scene ? [[UIWindow alloc] initWithWindowScene:scene]
                           : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    picking.holder.rootViewController = [UIViewController new];
    picking.holder.windowLevel = UIWindowLevelAlert;
    [picking.holder makeKeyAndVisible];
    picker.delegate = picking;
    picker.allowsMultipleSelection = NO;
    [picking.holder.rootViewController presentViewController:picker animated:YES completion:nil];
}

int Ios_PickDocument(const char *target, char *why, size_t why_size)
{
    @autoreleasepool {
        IosPicker *picking = [IosPicker new];
        NSFileManager *files = NSFileManager.defaultManager;
        NSURL *destination = [NSURL fileURLWithPath:[NSString stringWithUTF8String:target]];
        NSError *error = nil;
        picking.finished = dispatch_semaphore_create(0);
        if (NSThread.isMainThread) {
            /* the answer comes on this thread's run loop, as SDL's message
             * box waits for its own (SDL_uikitmessagebox.m) */
            present(picking);
            while (!picking.done) {
                @autoreleasepool {
                    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                }
            }
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{ present(picking); });
            dispatch_semaphore_wait(picking.finished, DISPATCH_TIME_FOREVER);
        }
        if (!picking.chosen) {
            fprintf(stderr, "memories-pc: no document chosen\n");
            return 0;
        }
        [files removeItemAtURL:destination error:nil];
        if (![files moveItemAtURL:picking.chosen toURL:destination error:&error]) {
            if (![files copyItemAtURL:picking.chosen toURL:destination error:&error]) {
                snprintf(why, why_size, "Could not copy the ROM into the game: %s",
                         error.localizedDescription.UTF8String);
                [files removeItemAtURL:picking.chosen error:nil];
                return -1;
            }
            [files removeItemAtURL:picking.chosen error:nil];
        }
        fprintf(stderr, "memories-pc: %s copied to %s\n", picking.chosen.lastPathComponent.UTF8String, target);
        return 1;
    }
}
#endif
